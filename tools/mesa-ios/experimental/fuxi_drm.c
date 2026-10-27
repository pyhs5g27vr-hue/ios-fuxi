/*
 * fuxi_drm.c — the libdrm of xf86drm.h for fuxi.
 *
 * Linux's DRM requests are carried to the kernel's GPU driver as calls of the
 * NativeAdreno user client (xnu/fuxi-adreno/NativeAdrenoABI.h), the same shape as
 * hackios-xnu-userland's redwood_drm.c (a meaningless file descriptor, one connection
 * per process, the request number as the selector) with our ABI behind it.
 *
 *   DRM_IOCTL_MSM_GET_PARAM    -> GetDeviceInfo + Query     (chip id, GMEM, UBWC, VA)
 *   DRM_IOCTL_MSM_GEM_NEW      -> BoAlloc
 *   DRM_IOCTL_MSM_GEM_INFO     -> bookkeeping (we keep the sizes we handed out)
 *   DRM_IOCTL_MSM_GEM_CPU_PREP -> BoCpuMap / BoCpuUnmap
 *   DRM_IOCTL_MSM_GEM_MMAP     -> BoCpuMap token + IOConnectMapMemory64
 *   DRM_IOCTL_MSM_GEM_SUBMIT   -> Submit (buffers carry handle/iova/bytes)
 *   DRM_IOCTL_MSM_WAIT_FENCE   -> WaitFence
 *   DRM_IOCTL_MSM_GEM_CLOSE    -> BoFree
 *
 * GPU addresses are the client's: the ABI reports NativeAdrenoDeviceClientIova and
 * BoMap takes the iova (freedreno's has_set_iova path).
 *
 * STATUS: written against the ABI header, NOT yet compiled or run on hardware.
 * The submit translation is the part to check first: our ABI submits whole buffers
 * {handle, iova, bytes}, while MSM submits a per-BO reloc table plus IB chunks, so
 * the cmds[] -> buffers[] reduction below is an approximation.
 *
 * NOT built by tools/mesa-ios/build.sh: the freedreno-darwin patch compiles
 * redwood_drm.c by name; this file is here to be switched in when it is ready.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/msm_drm.h"
#include "NativeAdrenoABI.h"

#define FUXI_SERVICE_CLASS "NativeAdrenoGpuPublished"
#define FUXI_SERVICE_NAME  "NativeAdrenoGpu"

static io_connect_t connection = MACH_PORT_NULL;
static int connection_fd = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- buffer objects -------------------------------------------------- */

#define MAX_BOS 8192
struct bo {
	uint64_t handle;      /* NativeAdreno handle */
	uint64_t bytes;
	uint64_t iova;        /* assigned when mapped */
	bool mapped;
	uint32_t cpu_token;   /* BoCpuMap token, 0 = not CPU-mapped */
	void *cpu_addr;
	uint64_t cpu_size;
};
static struct bo bos[MAX_BOS];
static unsigned n_bos;

static struct bo *bo_by_handle(uint64_t h)
{
	for (unsigned i = 0; i < n_bos; i++) {
		if (bos[i].handle == h) {
			return &bos[i];
		}
	}
	return NULL;
}

static struct bo *bo_new(uint64_t handle, uint64_t bytes)
{
	if (n_bos >= MAX_BOS) {
		return NULL;
	}
	struct bo *b = &bos[n_bos++];
	memset(b, 0, sizeof(*b));
	b->handle = handle;
	b->bytes = bytes;
	return b;
}

static void bo_del(uint64_t handle)
{
	for (unsigned i = 0; i < n_bos; i++) {
		if (bos[i].handle == handle) {
			bos[i] = bos[--n_bos];
			return;
		}
	}
}

/* ---- ABI plumbing ---------------------------------------------------- */

static int kern_errno(kern_return_t r)
{
	switch (r) {
	case KERN_SUCCESS: return 0;
	case kIOReturnBadArgument: return EINVAL;
	case kIOReturnNoMemory: return ENOMEM;
	case kIOReturnNoResources: return EBUSY;
	case kIOReturnUnsupported: return ENOSYS;
	case kIOReturnNotPermitted: return EACCES;
	case kIOReturnNotFound: return ENOENT;
	case kIOReturnTimeout: return ETIMEDOUT;
	case kIOReturnBusy: return EBUSY;
	default: return EIO;
	}
}

/* struct-in / struct-out; returns 0 or -errno */
static int abi_call(unsigned sel, void *in, size_t in_size, void *out, size_t out_cap, size_t *out_size)
{
	if (connection == MACH_PORT_NULL) {
		return -ENODEV;
	}
	size_t have = out_cap;
	kern_return_t r = IOConnectCallStructMethod(connection, sel, in, in_size, out, &have);
	if (out_size) {
		*out_size = have;
	}
	return -kern_errno(r);
}

static int ensure_open(void)
{
	pthread_mutex_lock(&lock);
	if (connection_fd < 0) {
		io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching(FUXI_SERVICE_CLASS));
		if (service == MACH_PORT_NULL) {
			service = IOServiceGetMatchingService(0, IOServiceMatching(FUXI_SERVICE_NAME));
		}
		if (service != MACH_PORT_NULL) {
			io_connect_t conn = MACH_PORT_NULL;
			if (IOServiceOpen(service, mach_task_self(), 0, &conn) == KERN_SUCCESS) {
				connection = conn;
				connection_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
				if (connection_fd < 0) {
					IOServiceClose(conn);
					connection = MACH_PORT_NULL;
				}
			}
			IOObjectRelease(service);
		}
	}
	int fd = connection_fd;
	pthread_mutex_unlock(&lock);
	return fd;
}

/* ---- parameters ------------------------------------------------------ */

static int query_device(struct NativeAdrenoAbiDeviceInfo *info, struct NativeAdrenoAbiParams *params)
{
	if (info) {
		memset(info, 0, sizeof(*info));
		if (abi_call(NativeAdrenoMethodGetDeviceInfo, NULL, 0, info, sizeof(*info), NULL) != 0) {
			return -1;
		}
	}
	if (params) {
		memset(params, 0, sizeof(*params));
		if (abi_call(NativeAdrenoMethodQuery, NULL, 0, params, sizeof(*params), NULL) != 0) {
			return -1;
		}
	}
	return 0;
}

/* MSM param ids (drm-uapi/msm_drm.h): the ones freedreno asks for. */
#define MSM_PARAM_GPU_ID        0x01
#define MSM_PARAM_GMEM_SIZE     0x02
#define MSM_PARAM_GMEM_BASE     0x03
#define MSM_PARAM_CHIP_ID       0x04
#define MSM_PARAM_MAX_FREQ      0x05
#define MSM_PARAM_VA_START      0x08
#define MSM_PARAM_VA_SIZE       0x09
#define MSM_PARAM_UBWC_SWIZZLE  0x0d
#define MSM_PARAM_MACROTILE_MODE 0x0f
#define MSM_PARAM_HAS_SET_IOVA  0x10

static int get_param(uint32_t param, uint32_t *value)
{
	struct NativeAdrenoAbiDeviceInfo info;
	struct NativeAdrenoAbiParams params;
	if (query_device(&info, &params) != 0) {
		return -ENODEV;
	}
	switch (param) {
	case MSM_PARAM_CHIP_ID:      *value = info.chip_id; break;
	case MSM_PARAM_GPU_ID:       *value = info.chip_id >> 24; break;   /* family */
	case MSM_PARAM_GMEM_SIZE:    *value = info.gmem_bytes; break;
	case MSM_PARAM_GMEM_BASE:    *value = (uint32_t)(info.gmem_base >> 32); break;
	case MSM_PARAM_VA_START:     *value = (uint32_t)(info.va_start >> 32); break;
	case MSM_PARAM_VA_SIZE:      *value = (uint32_t)(info.va_size >> 32); break;
	case MSM_PARAM_UBWC_SWIZZLE: *value = info.ubwc_swizzle; break;
	case MSM_PARAM_MACROTILE_MODE: *value = info.macrotile_mode; break;
	case MSM_PARAM_HAS_SET_IOVA: *value = 1; break;   /* client picks the iova */
	default: return -EINVAL;
	}
	return 0;
}

/* ---- CPU mapping ----------------------------------------------------- */

static int bo_cpu_map(struct bo *b)
{
	if (b->cpu_addr != NULL) {
		return 0;
	}
	struct NativeAdrenoAbiBoCpuMap req = { .handle = b->handle };
	struct NativeAdrenoAbiBoCpuMapReply rep;
	if (abi_call(NativeAdrenoMethodBoCpuMap, &req, sizeof(req), &rep, sizeof(rep), NULL) != 0) {
		return -ENODEV;
	}
	uint64_t addr = 0;
	size_t size = (size_t)b->bytes;
	kern_return_t r = IOConnectMapMemory64(connection, rep.token, mach_task_self(), &addr, &size, kIOMapAnywhere);
	if (r != KERN_SUCCESS) {
		return -kern_errno(r);
	}
	b->cpu_addr = (void *)(uintptr_t)addr;
	b->cpu_size = size;
	b->cpu_token = rep.token;
	return 0;
}

static void bo_cpu_unmap(struct bo *b)
{
	if (b->cpu_addr == NULL) {
		return;
	}
	IOConnectUnmapMemory64(connection, b->cpu_token, mach_task_self(), (mach_vm_address_t)(uintptr_t)b->cpu_addr);
	struct NativeAdrenoAbiBoCpuUnmap req = { .handle = b->handle, .token = b->cpu_token };
	abi_call(NativeAdrenoMethodBoCpuUnmap, &req, sizeof(req), NULL, 0, NULL);
	b->cpu_addr = NULL;
	b->cpu_size = 0;
	b->cpu_token = 0;
}

/* ---- libdrm surface -------------------------------------------------- */

int drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	if (name != NULL && strcmp(name, "msm") != 0) {
		return -1;
	}
	return ensure_open();
}

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	if (devices == NULL || max_devices <= 0) {
		return 1;
	}
	drmDevicePtr d = calloc(1, sizeof(*d));
	if (d == NULL) {
		return -ENOMEM;
	}
	d->bus_type = DRM_BUS_PLATFORM;
	d->available_nodes = 1 << DRM_NODE_RENDER;
	strncpy(d->nodes[DRM_NODE_RENDER], "/dev/null", sizeof(d->nodes[0]) - 1);
	devices[0] = d;
	return 1;
}

void drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; i < count; i++) {
		free(devices[i]);
	}
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	if (device == NULL) {
		return -EINVAL;
	}
	drmDevicePtr d = calloc(1, sizeof(*d));
	if (d == NULL) {
		return -ENOMEM;
	}
	d->bus_type = DRM_BUS_PLATFORM;
	d->available_nodes = 1 << DRM_NODE_RENDER;
	strncpy(d->nodes[DRM_NODE_RENDER], "/dev/null", sizeof(d->nodes[0]) - 1);
	*device = d;
	return 0;
}

void drmFreeDevice(drmDevicePtr *device)
{
	if (device && *device) {
		free(*device);
		*device = NULL;
	}
}

int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a == b;
}

drmVersionPtr drmGetVersion(int fd)
{
	(void)fd;
	drmVersionPtr v = calloc(1, sizeof(*v));
	if (v == NULL) {
		return NULL;
	}
	v->name = strdup("fuxi-nativeadreno");
	v->name_len = v->name ? (int)strlen(v->name) : 0;
	v->date = strdup("2026");
	v->date_len = v->date ? (int)strlen(v->date) : 0;
	v->desc = strdup("NativeAdreno ABI");
	v->desc_len = v->desc ? (int)strlen(v->desc) : 0;
	v->version_major = 2;
	return v;
}

void drmFreeVersion(drmVersionPtr v)
{
	if (v == NULL) {
		return;
	}
	free(v->name);
	free(v->date);
	free(v->desc);
	free(v);
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	(void)value;
	return -EINVAL;
}

static int msm_gem_new(struct drm_msm_gem_new *a)
{
	struct NativeAdrenoAbiBoAlloc req = {
		.bytes = a->size,
		.align_log2 = 12,
		.contiguous = 0,
	};
	struct NativeAdrenoAbiBoAllocReply rep;
	int r = abi_call(NativeAdrenoMethodBoAlloc, &req, sizeof(req), &rep, sizeof(rep), NULL);
	if (r != 0) {
		return r;
	}
	struct bo *b = bo_new(rep.handle, a->size);
	if (b == NULL) {
		struct NativeAdrenoAbiBoFree f = { .handle = rep.handle };
		abi_call(NativeAdrenoMethodBoFree, &f, sizeof(f), NULL, 0, NULL);
		return -ENOMEM;
	}
	a->handle = (uint32_t)(rep.handle & 0xffffffffu);
	/* freedreno keeps the full handle where the kernel gave one; the MSM struct is 32-bit,
	 * so the low half travels and the table above maps it back. */
	return 0;
}

static uint64_t handle_from_u32(uint32_t h)
{
	for (unsigned i = 0; i < n_bos; i++) {
		if ((uint32_t)(bos[i].handle & 0xffffffffu) == h) {
			return bos[i].handle;
		}
	}
	return 0;
}

static int msm_gem_info(struct drm_msm_gem_info *a)
{
	struct bo *b = bo_by_handle(handle_from_u32(a->handle));
	if (b == NULL) {
		return -EINVAL;
	}
	a->offset = b->handle;   /* opaque; only handed back to us */
	return 0;
}

static int msm_gem_cpu_prep(struct drm_msm_gem_cpu_prep *a)
{
	struct bo *b = bo_by_handle(handle_from_u32(a->handle));
	if (b == NULL) {
		return -EINVAL;
	}
	/* MSM_PREP_READ/WRITE: the ABI's cpu map/unmap carries the cache maintenance */
	if (a->op & (1u << 0 | 1u << 1)) {
		return bo_cpu_map(b);
	}
	bo_cpu_unmap(b);   /* MSM_PREP_NOSYNC with no flags: drop the mapping */
	return 0;
}

static int msm_submit(struct drm_msm_gem_submit *a)
{
	if (a->nr_bos > NativeAdrenoAbiMaxBuffers || a->nr_cmds > NativeAdrenoAbiMaxBuffers) {
		return -EINVAL;
	}
	struct NativeAdrenoAbiSubmit sub;
	memset(&sub, 0, sizeof(sub));
	sub.queue = 0;   /* the open() queue */
	sub.buffer_count = a->nr_cmds ? a->nr_cmds : a->nr_bos;

	/* cmds[] name IB chunks: each becomes one buffer entry {handle, iova, bytes}.
	 * This is the approximation noted in the header comment. */
	const struct drm_msm_gem_submit_cmd *cmds =
		(const struct drm_msm_gem_submit_cmd *)(uintptr_t)a->cmds;
	const struct drm_msm_gem_submit_bo *sub_bos =
		(const struct drm_msm_gem_submit_bo *)(uintptr_t)a->bos;
	unsigned n = 0;
	for (unsigned i = 0; i < a->nr_cmds && n < NativeAdrenoAbiMaxBuffers; i++, n++) {
		uint32_t h32 = cmds[i].handle;
		struct bo *b = bo_by_handle(handle_from_u32(h32));
		if (b == NULL) {
			return -EINVAL;
		}
		sub.buffers[n].handle = b->handle;
		sub.buffers[n].iova = b->iova + cmds[i].base;
		sub.buffers[n].bytes = cmds[i].size;
	}
	if (a->nr_cmds == 0) {
		for (unsigned i = 0; i < a->nr_bos && n < NativeAdrenoAbiMaxBuffers; i++, n++) {
			struct bo *b = bo_by_handle(handle_from_u32(sub_bos[i].handle));
			if (b == NULL) {
				return -EINVAL;
			}
			sub.buffers[n].handle = b->handle;
			sub.buffers[n].iova = b->iova;
			sub.buffers[n].bytes = b->bytes;
		}
	}
	sub.buffer_count = n;
	if (n == 0) {
		return -EINVAL;
	}

	struct NativeAdrenoAbiSubmitReply rep;
	int r = abi_call(NativeAdrenoMethodSubmit, &sub, sizeof(sub), &rep, sizeof(rep), NULL);
	if (r != 0) {
		return r;
	}
	a->fence = (uint32_t)(rep.fence & 0xffffffffu);
	return 0;
}

static int msm_wait_fence(struct drm_msm_wait_fence *a)
{
	struct NativeAdrenoAbiWaitFence req = {
		.fence = a->fence,
		.timeout_ns = (uint64_t)a->timeout,
	};
	return abi_call(NativeAdrenoMethodWaitFence, &req, sizeof(req), NULL, 0, NULL);
}

static int msm_gem_close(uint32_t handle)
{
	uint64_t full = handle_from_u32(handle);
	struct bo *b = bo_by_handle(full);
	if (b == NULL) {
		return -EINVAL;
	}
	bo_cpu_unmap(b);
	struct NativeAdrenoAbiBoFree f = { .handle = full };
	abi_call(NativeAdrenoMethodBoFree, &f, sizeof(f), NULL, 0, NULL);
	bo_del(full);
	return 0;
}

/* DRM_IOCTL_MSM_* numbers from drm-uapi/msm_drm.h are the selectors' identity here:
 * we compare the full request, as redwood_drm.c does. */
int drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (ensure_open() < 0) {
		return -ENODEV;
	}
	switch (request) {
	case DRM_IOCTL_MSM_GET_PARAM: {
		struct drm_msm_param *p = arg;
		uint32_t v = 0;
		int r = get_param(p->param, &v);
		if (r != 0) {
			return r;
		}
		p->value = v;
		return 0;
	}
	case DRM_IOCTL_MSM_GEM_NEW:
		return msm_gem_new(arg);
	case DRM_IOCTL_MSM_GEM_INFO:
		return msm_gem_info(arg);
	case DRM_IOCTL_MSM_GEM_CPU_PREP:
		return msm_gem_cpu_prep(arg);
	case DRM_IOCTL_MSM_GEM_SUBMIT:
		return msm_submit(arg);
	case DRM_IOCTL_MSM_WAIT_FENCE:
		return msm_wait_fence(arg);
	default:
		return -ENOSYS;
	}
}

int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)size;
	(void)data;
	(void)fd;
	(void)index;
	return -ENOSYS;
}

int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)size;
	(void)data;
	(void)fd;
	(void)index;
	return -ENOSYS;
}

/* The two the freedreno-darwin patch adds: a BO's memory as a CPU pointer, and the
 * IOSurface a texture is to show. The surface one is Turnip's path; GL does not use it. */
void *redwood_drm_map(int fd, uint32_t handle, uint64_t size)
{
	(void)fd;
	(void)size;
	struct bo *b = bo_by_handle(handle_from_u32(handle));
	if (b == NULL) {
		return NULL;
	}
	if (bo_cpu_map(b) != 0) {
		return NULL;
	}
	return b->cpu_addr;
}

void redwood_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	(void)address;
	struct bo *b = bo_by_handle(handle_from_u32(handle));
	if (b != NULL) {
		bo_cpu_unmap(b);
	}
}

int drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	(void)fd;
	(void)surface;
	(void)handle;
	return -ENOSYS;   /* IOSurface import is Turnip's BoImport path, not GL's */
}
