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
 *   DRM_IOCTL_MSM_GEM_SUBMIT   -> Submit (one buffer entry per cmd stream)
 *   DRM_IOCTL_MSM_WAIT_FENCE   -> WaitFence
 *   DRM_IOCTL_MSM_GEM_CLOSE    -> BoFree
 *
 * GPU addresses are the client's: the ABI reports NativeAdrenoDeviceClientIova and
 * BoMap takes the iova (freedreno's has_set_iova path).
 *
 * STATUS: written against the ABI header and the msm_drm.h layouts (bo: flags/handle/
 * presumed; cmd: submit_idx/submit_offset/size; wait: drm_msm_timespec). NOT yet run
 * on hardware. The submit translation is the part to check first: our ABI takes whole
 * buffers {handle, iova, bytes} while MSM names cmd streams by (bo index, offset, size),
 * so each cmd becomes one buffer entry starting at presumed + submit_offset.
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
	uint64_t handle;
	uint64_t bytes;
	uint64_t iova;
	uint32_t cpu_token;
	void *cpu_addr;
	uint64_t cpu_size;
};
static struct bo bos[MAX_BOS];
static unsigned n_bos;

static struct bo *bo_by_full(uint64_t h)
{
	for (unsigned i = 0; i < n_bos; i++) {
		if (bos[i].handle == h) {
			return &bos[i];
		}
	}
	return NULL;
}

/* MSM structs carry a 32-bit handle; we keep the ABI's 64-bit one and map by low half. */
static struct bo *bo_by_u32(uint32_t h)
{
	for (unsigned i = 0; i < n_bos; i++) {
		if ((uint32_t)(bos[i].handle & 0xffffffffu) == h) {
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

static int abi_call(unsigned sel, void *in, size_t in_size, void *out, size_t out_cap)
{
	if (connection == MACH_PORT_NULL) {
		return -ENODEV;
	}
	size_t have = out_cap;
	kern_return_t r = IOConnectCallStructMethod(connection, sel, in, in_size, out, &have);
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

static int query_device(struct NativeAdrenoAbiDeviceInfo *info, struct NativeAdrenoAbiParams *params)
{
	if (info) {
		memset(info, 0, sizeof(*info));
		if (abi_call(NativeAdrenoMethodGetDeviceInfo, NULL, 0, info, sizeof(*info)) != 0) {
			return -1;
		}
	}
	if (params) {
		memset(params, 0, sizeof(*params));
		if (abi_call(NativeAdrenoMethodQuery, NULL, 0, params, sizeof(*params)) != 0) {
			return -1;
		}
	}
	return 0;
}

static int get_param(uint32_t param, uint64_t *value)
{
	struct NativeAdrenoAbiDeviceInfo info;
	struct NativeAdrenoAbiParams params;
	if (query_device(&info, &params) != 0) {
		return -ENODEV;
	}
	switch (param) {
	case MSM_PARAM_CHIP_ID:    *value = info.chip_id; break;
	case MSM_PARAM_GPU_ID:     *value = info.chip_id >> 24; break;
	case MSM_PARAM_GMEM_SIZE:  *value = info.gmem_bytes; break;
	case MSM_PARAM_GMEM_BASE:  *value = info.gmem_base; break;
	case MSM_PARAM_FAULTS:     *value = 0; break;
	case MSM_PARAM_TIMESTAMP: {
		struct NativeAdrenoAbiTimestamp ts = { 0 };
		if (abi_call(NativeAdrenoMethodGetTimestamp, NULL, 0, &ts, sizeof(ts)) != 0) {
			return -ENODEV;
		}
		*value = ts.ticks;
		break;
	}
	case MSM_PARAM_PP_PGTABLE: *value = 1; break;   /* our ABI maps per client */
	case MSM_PARAM_NR_RINGS:   *value = 1; break;   /* one queue per client */
#ifdef MSM_PARAM_VA_START
	case MSM_PARAM_VA_START:   *value = info.va_start; break;
#endif
#ifdef MSM_PARAM_VA_SIZE
	case MSM_PARAM_VA_SIZE:    *value = info.va_size; break;
#endif
#ifdef MSM_PARAM_UBWC_SWIZZLE
	case MSM_PARAM_UBWC_SWIZZLE: *value = info.ubwc_swizzle; break;
#endif
#ifdef MSM_PARAM_MACROTILE_MODE
	case MSM_PARAM_MACROTILE_MODE: *value = info.macrotile_mode; break;
#endif
#ifdef MSM_PARAM_HAS_SET_IOVA
	case MSM_PARAM_HAS_SET_IOVA: *value = 1; break;
#endif
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
	int r = abi_call(NativeAdrenoMethodBoCpuMap, &req, sizeof(req), &rep, sizeof(rep));
	if (r != 0) {
		return r;
	}
	uint64_t addr = 0;
	mach_vm_size_t size = (mach_vm_size_t)b->bytes;
	kern_return_t kr = IOConnectMapMemory64(connection, rep.token, mach_task_self(), &addr, &size, kIOMapAnywhere);
	if (kr != KERN_SUCCESS) {
		return -kern_errno(kr);
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
	abi_call(NativeAdrenoMethodBoCpuUnmap, &req, sizeof(req), NULL, 0);
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
	d->bustype = DRM_BUS_PLATFORM;
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
	d->bustype = DRM_BUS_PLATFORM;
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
	int r = abi_call(NativeAdrenoMethodBoAlloc, &req, sizeof(req), &rep, sizeof(rep));
	if (r != 0) {
		return r;
	}
	if (bo_new(rep.handle, a->size) == NULL) {
		struct NativeAdrenoAbiBoFree f = { .handle = rep.handle };
		abi_call(NativeAdrenoMethodBoFree, &f, sizeof(f), NULL, 0);
		return -ENOMEM;
	}
	a->handle = (uint32_t)(rep.handle & 0xffffffffu);
	return 0;
}

static int msm_gem_info(struct drm_msm_gem_info *a)
{
	struct bo *b = bo_by_u32(a->handle);
	if (b == NULL) {
		return -EINVAL;
	}
	switch (a->info) {
	case MSM_INFO_GET_OFFSET:
		a->value = b->handle;      /* opaque: only handed back to us for mmap */
		return 0;
	case MSM_INFO_GET_IOVA:
		a->value = b->iova;
		return 0;
	default:
		a->value = 0;
		return 0;
	}
}

static int msm_gem_cpu_prep(struct drm_msm_gem_cpu_prep *a)
{
	struct bo *b = bo_by_u32(a->handle);
	if (b == NULL) {
		return -EINVAL;
	}
	if (a->op & (MSM_PREP_READ | MSM_PREP_WRITE)) {
		return bo_cpu_map(b);
	}
	bo_cpu_unmap(b);
	return 0;
}

static int msm_submit(struct drm_msm_gem_submit *a)
{
	if (a->nr_bos > NativeAdrenoAbiMaxBuffers || a->nr_cmds > NativeAdrenoAbiMaxBuffers) {
		return -EINVAL;
	}
	const struct drm_msm_gem_submit_bo *sub_bos =
		(const struct drm_msm_gem_submit_bo *)(uintptr_t)a->bos;
	const struct drm_msm_gem_submit_cmd *cmds =
		(const struct drm_msm_gem_submit_cmd *)(uintptr_t)a->cmds;

	struct NativeAdrenoAbiSubmit sub;
	memset(&sub, 0, sizeof(sub));
	sub.queue = 0;
	sub.wait_fence = 0;

	unsigned n = 0;
	for (unsigned i = 0; i < a->nr_cmds && n < NativeAdrenoAbiMaxBuffers; i++, n++) {
		uint32_t idx = cmds[i].submit_idx;
		if (idx >= a->nr_bos) {
			return -EINVAL;
		}
		struct bo *b = bo_by_u32(sub_bos[idx].handle);
		if (b == NULL) {
			return -EINVAL;
		}
		uint64_t base = sub_bos[idx].presumed ? sub_bos[idx].presumed : b->iova;
		sub.buffers[n].handle = b->handle;
		sub.buffers[n].iova = base + cmds[i].submit_offset;
		sub.buffers[n].bytes = cmds[i].size;
	}
	if (n == 0) {
		/* no cmd streams: submit the bos as they are (rarely used) */
		for (unsigned i = 0; i < a->nr_bos && n < NativeAdrenoAbiMaxBuffers; i++, n++) {
			struct bo *b = bo_by_u32(sub_bos[i].handle);
			if (b == NULL) {
				return -EINVAL;
			}
			sub.buffers[n].handle = b->handle;
			sub.buffers[n].iova = sub_bos[i].presumed ? sub_bos[i].presumed : b->iova;
			sub.buffers[n].bytes = b->bytes;
		}
	}
	sub.buffer_count = n;
	if (n == 0) {
		return -EINVAL;
	}

	struct NativeAdrenoAbiSubmitReply rep;
	int r = abi_call(NativeAdrenoMethodSubmit, &sub, sizeof(sub), &rep, sizeof(rep));
	if (r != 0) {
		return r;
	}
	a->fence = (uint32_t)(rep.fence & 0xffffffffu);
	return 0;
}

static int msm_wait_fence(struct drm_msm_wait_fence *a)
{
	uint64_t ns = 0;
	if (a->timeout.tv_sec > 0) {
		ns = (uint64_t)a->timeout.tv_sec * 1000000000ull;
	}
	if (a->timeout.tv_nsec > 0) {
		ns += (uint64_t)a->timeout.tv_nsec;
	}
	struct NativeAdrenoAbiWaitFence req = { .fence = a->fence, .timeout_ns = ns };
	return abi_call(NativeAdrenoMethodWaitFence, &req, sizeof(req), NULL, 0);
}

static int msm_gem_close(uint32_t handle)
{
	struct bo *b = bo_by_u32(handle);
	if (b == NULL) {
		return -EINVAL;
	}
	uint64_t full = b->handle;
	bo_cpu_unmap(b);
	struct NativeAdrenoAbiBoFree f = { .handle = full };
	abi_call(NativeAdrenoMethodBoFree, &f, sizeof(f), NULL, 0);
	bo_del(full);
	return 0;
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (ensure_open() < 0) {
		return -ENODEV;
	}
	switch (request) {
	case DRM_IOCTL_MSM_GET_PARAM: {
		struct drm_msm_param *p = arg;
		uint64_t v = 0;
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
	(void)fd;
	(void)index;
	(void)data;
	(void)size;
	return -ENOSYS;
}

int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	(void)index;
	(void)data;
	(void)size;
	return -ENOSYS;
}

/* The two the freedreno-darwin patch adds. */
void *redwood_drm_map(int fd, uint32_t handle, uint64_t size)
{
	(void)fd;
	(void)size;
	struct bo *b = bo_by_u32(handle);
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
	struct bo *b = bo_by_u32(handle);
	if (b != NULL) {
		bo_cpu_unmap(b);
	}
}

int drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	(void)fd;
	(void)surface;
	(void)handle;
	return -ENOSYS;
}

/* MSM's GEM_CLOSE has no MSM ioctl on this path: freedreno calls close() on the handle. */
int drmCloseBufferHandle(int fd, uint32_t handle);
int drmCloseBufferHandle(int fd, uint32_t handle)
{
	(void)fd;
	return msm_gem_close(handle);
}

/* ---- what fuxi has no equivalent for ---------------------------------
 *
 * dmabufs and drm_syncobj timelines. The reference shim (redwood_drm.c) returns
 * -1 from the same calls and freedreno falls back to implicit fences, so these
 * keep that behaviour; the IOSurface import (BoImport in our ABI) is Turnip's
 * path and belongs to a winsys with surfaces, not to GL.
 */
#define UNUSED __attribute__((unused))

int drmPrimeHandleToFD(UNUSED int fd, UNUSED uint32_t handle, UNUSED uint32_t flags, UNUSED int *prime_fd)
{
	return -ENOSYS;
}

int drmSyncobjCreate(UNUSED int fd, UNUSED uint32_t flags, UNUSED uint32_t *handle)
{
	return -ENOSYS;
}

int drmSyncobjDestroy(UNUSED int fd, UNUSED uint32_t handle)
{
	return -ENOSYS;
}

int drmSyncobjReset(UNUSED int fd, UNUSED const uint32_t *handles, UNUSED uint32_t count)
{
	return -ENOSYS;
}

int drmSyncobjFDToHandle(UNUSED int fd, UNUSED int obj_fd, UNUSED uint32_t *handle)
{
	return -ENOSYS;
}

int drmSyncobjHandleToFD(UNUSED int fd, UNUSED uint32_t handle, UNUSED int *obj_fd)
{
	return -ENOSYS;
}

int drmSyncobjImportSyncFile(UNUSED int fd, UNUSED uint32_t handle, UNUSED int sync_file_fd)
{
	return -ENOSYS;
}

int drmSyncobjExportSyncFile(UNUSED int fd, UNUSED uint32_t handle, UNUSED int *sync_file_fd)
{
	return -ENOSYS;
}

int drmSyncobjWait(UNUSED int fd, UNUSED uint32_t *handles, UNUSED unsigned num_handles,
                   UNUSED int64_t timeout_nsec, UNUSED unsigned flags, UNUSED uint32_t *first_signaled)
{
	return -ENOSYS;
}

int drmSyncobjSignal(UNUSED int fd, UNUSED const uint32_t *handles, UNUSED uint32_t handle_count)
{
	return -ENOSYS;
}

int drmSyncobjTimelineSignal(UNUSED int fd, UNUSED const uint32_t *handles, UNUSED uint64_t *points,
                             UNUSED uint32_t handle_count)
{
	return -ENOSYS;
}

int drmSyncobjTimelineWait(UNUSED int fd, UNUSED uint32_t *handles, UNUSED uint64_t *points,
                           UNUSED unsigned num_handles, UNUSED int64_t timeout_nsec, UNUSED unsigned flags,
                           UNUSED uint32_t *first_signaled)
{
	return -ENOSYS;
}

int drmSyncobjQuery(UNUSED int fd, UNUSED uint32_t *handles, UNUSED uint64_t *points, UNUSED uint32_t handle_count)
{
	return -ENOSYS;
}

int drmSyncobjQuery2(UNUSED int fd, UNUSED uint32_t *handles, UNUSED uint64_t *points,
                     UNUSED uint32_t handle_count, UNUSED uint32_t flags)
{
	return -ENOSYS;
}

int drmSyncobjTransfer(UNUSED int fd, UNUSED uint32_t dst_handle, UNUSED uint64_t dst_point,
                       UNUSED uint32_t src_handle, UNUSED uint64_t src_point, UNUSED uint32_t flags)
{
	return -ENOSYS;
}

/* struct redwood_drm_surface is declared by the vendored xf86drm.h */
int redwood_drm_surface(UNUSED int fd, UNUSED struct redwood_drm_surface *surface)
{
	return -ENOSYS;
}
