/* NativeAdrenoABI v2 userspace lane. Minimal Metal discovery metaclass only.
 * ABI and framing credit: UniXNU NativeAdreno contributors (MIT).
 * See NativeAdrenoABI.h and LICENSE.NativeAdreno for copied material.
 */
#include <pexpert/arm64/board_config.h>
#if defined(FUXI)
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include "fuxi_gpu_abi.h"
#include "fuxi_abi_wire.h"

/* A subrange keeps the BO's managed-page descriptor/pager identity. Never
 * reconstruct a physical descriptor over wired managed RAM. The extra backing
 * reference owns the matched prepare until the final CPU mapping disappears.
 * Both mappings keep the buffer's Normal NC/write-combine cache policy. */
class FuxiAdrenoBoView : public IOSubMemoryDescriptor {
 OSDeclareDefaultStructors(FuxiAdrenoBoView)
 IOBufferMemoryDescriptor *backing_ = nullptr;
public:
 bool bind(IOBufferMemoryDescriptor *backing) {
  if (!initSubRange(backing,0,backing->getLength(),kIODirectionInOut)) return false;
  backing_=backing; backing_->retain(); return true;
 }
 void free() override {
  IOBufferMemoryDescriptor *backing=backing_;
  IOSubMemoryDescriptor::free();
  if (backing) { backing->complete(); backing->release(); }
 }
};
OSDefineMetaClassAndStructors(FuxiAdrenoBoView,IOSubMemoryDescriptor)

static IOLock *openLock;
static bool connected;
static bool execswapTest;
static unsigned testPasses, traceLines;
static bool abiExecswapTest(const char *swap) {
 size_t n=strnlen(swap,192);
 if (!n || n>=191) return false;
 const char *colon=strchr(swap,':');
 if (!colon || colon==swap || colon-swap>31) return false;
 /* Match sweet_execswap_parse's basename rules; only this test path counts. */
 for (const char *p=swap; p<colon; ++p)
  if (*p<=' ' || *p==',' || *p=='/') return false;
 return !strcmp(colon+1,"/usr/local/bin/fuxi-gpu-abi-test");
}
/* openLock serializes the boot-wide quota and trace budget (including opens
 * denied after the test). No user stdout or root-image update is needed. */
static void abiTrace(const char *step, int pid, IOReturn rc) {
 IOLockLock(openLock);
 if (traceLines < 64) {
  ++traceLines;
  IOLog("FUXI ABI: %s pid %d rc 0x%08x\n",step,pid,(unsigned)rc);
 }
 IOLockUnlock(openLock);
}
static const char *abiStep(unsigned selector) {
 static const char *steps[]={"invalid","query","alloc","map","free","submit","wait",
     "timestamp","status","set-metadata","get-metadata","create-queue","close-queue",
     "cpu-token","cpu-unmap","device-info","import"};
 return selector<=16 ? steps[selector] : "invalid";
}
class FuxiAdrenoUserClient : public IOUserClient {
 OSDeclareDefaultStructors(FuxiAdrenoUserClient)
 enum {MaxBos=FG_ABI_MAX_BOS};
 struct Bo {
  IOBufferMemoryDescriptor *memory;
  IOMemoryDescriptor *importMemory;
  IOMemoryMap *importMap;
  FuxiAdrenoBoView *view;
  uint64_t handle, bytes, allocated, iova;
  uint32_t token, leases, metadataBytes;
  char metadata[64];
 };
 /* 1024 entries no longer fit the 16 KiB OSObject size limit; kept separate. */
 struct BoRange {
  Bo *first, *last;
  Bo *begin() const { return first; } Bo *end() const { return last; }
  Bo &operator[](size_t i) const { return first[i]; }
 };
 Bo *boTable_ = nullptr;
 BoRange bos_ = {};
 task_t task_ = TASK_NULL;
 IOLock *lock_ = nullptr;
 bool live_ = false, diagnostic_ = false, resultChecked_ = false;
 int pid_ = 0;
 void result() {
  if (!diagnostic_ || resultChecked_) return;
  for (auto &b:bos_) if (b.memory && b.bytes==65536 && b.iova==0x20000000) {
   resultChecked_=true;
   volatile uint32_t *words=(volatile uint32_t *)b.memory->getBytesNoCopy();
   __asm__ volatile("dsb sy" ::: "memory");
   unsigned bad=0;
   uint32_t value=words[0x1000/4];
   for (unsigned i=0;i<256;i++) if (words[0x2000/4+i]!=0xff336699) ++bad;
   bool pass=value==0xa740cafe && !bad;
   IOLockLock(openLock);
   if (pass && testPasses<2) ++testPasses;
   if (traceLines<64) {
    ++traceLines;
    IOLog("FUXI ABI: result pid %d rc 0x%08x mem 0x%08x bad-pixels %u rounds %u/2\n",
        pid_,(unsigned)(pass ? kIOReturnSuccess : kIOReturnIOError),value,bad,testPasses);
   }
   IOLockUnlock(openLock);
   return;
  }
 }
 uint64_t nextHandle_ = 1, wired_ = 0, lastFence_ = 0, firstFence_ = 0;
 uint32_t nextToken_ = 1;
 bool queues_[8] = {true};
 Bo *find(uint64_t handle) {
  for (auto &b:bos_) if (handle && b.handle==handle) return &b;
  return nullptr;
 }
 IOReturn wait(uint64_t fence, uint64_t ns) {
  if (!fence || !firstFence_ || fence<firstFence_ || fence>lastFence_) return kIOReturnBadArgument;
  /* The hardware watchdog is independent and always bounds ownership at
   * 100 ms. A shorter user timeout does not itself lose the context. */
  uint64_t ticks=0;
  nanoseconds_to_absolutetime(ns>1000000000ULL ? 1000000000ULL : ns,&ticks);
  uint64_t deadline=mach_absolute_time()+ticks;
  for (;;) {
   int s=fuxi_gpu_abi_poll(fence);
   if (!s) return kIOReturnSuccess;
   if (s<0) return (s==-3 || s==-4) ? kIOReturnTimeout : kIOReturnIOError;
   if (mach_absolute_time()>=deadline) return kIOReturnTimeout;
   IOSleep(1);
  }
 }
 IOReturn drop(Bo &b) {
  if (b.iova) {
   int s=fuxi_gpu_abi_unmap(b.iova,b.bytes);
   if (s) return s==1 ? kIOReturnBusy : kIOReturnIOError;
   b.iova=0;
  }
  if (b.importMemory && b.importMap->wireRange(kIODirectionNone,0,b.bytes)!=kIOReturnSuccess)
   return kIOReturnIOError;
  wired_-=b.allocated;
  if (b.importMemory) {
   b.importMap->release();
   b.importMemory->complete(); b.importMemory->release();
  } else {
   b.view->release(); /* view owns the matched prepare + backing retain */
   b.memory->release();
  }
  bzero(&b,sizeof(b));
  return kIOReturnSuccess;
 }
 IOReturn allocate(const NativeAdrenoAbiBoAlloc &r, NativeAdrenoAbiBoAllocReply &out) {
  if (!fg_abi_alloc_valid(r.bytes,r.align_log2,r.contiguous)) return kIOReturnBadArgument;
  uint64_t allocated=(r.bytes+16383)&~UINT64_C(16383);
  if (wired_>FG_ABI_MAX_WIRED_BYTES || allocated>FG_ABI_MAX_WIRED_BYTES-wired_ ||
      nextToken_==UINT32_MAX || nextHandle_==UINT64_MAX) return kIOReturnNoResources;
  Bo *b=nullptr;
  for (auto &candidate:bos_) if (!candidate.handle) { b=&candidate; break; }
  if (!b) return kIOReturnNoResources;
  uint64_t mask=((UINT64_C(1)<<FG_ABI_PA_BITS)-1)&~((UINT64_C(1)<<r.align_log2)-1);
  IOOptionBits options=kIODirectionInOut|kIOMemoryMapperNone|kIOMemoryKernelUserShared|kIOMapWriteCombineCache;
  /* Contiguous-first, with a wired scattered fallback unless required. */
  IOBufferMemoryDescriptor *memory=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task,options|kIOMemoryPhysicallyContiguous,allocated,mask);
  if (!memory && !r.contiguous) memory=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,allocated,mask);
  if (!memory) return kIOReturnNoMemory;
  if (memory->prepare()!=kIOReturnSuccess) { memory->release(); return kIOReturnNoMemory; }
  FuxiAdrenoBoView *view=new FuxiAdrenoBoView;
  bool valid=view && memory->getBytesNoCopy() && memory->getLength()==allocated;
  uint64_t first=0;
  for (uint64_t off=0;valid && off<allocated;off+=16384) {
   IOByteCount length=0;
   uint64_t pa=memory->getPhysicalSegment(off,&length,kIOMemoryMapperNone);
   valid=pa && !(pa&16383) && length>=16384 && pa<(UINT64_C(1)<<FG_ABI_PA_BITS) &&
       (!off ? !(pa&((UINT64_C(1)<<r.align_log2)-1)) : !r.contiguous || pa==first+off);
   if (!valid) break;
   if (!off) first=pa;
  }
  if (valid) {
   bzero(memory->getBytesNoCopy(),allocated); /* includes 16K mapping tail */
   valid=view->bind(memory);
  }
  if (!valid) {
   if (view) view->release();
   memory->complete(); memory->release(); return kIOReturnNoMemory;
  }
  b->memory=memory; b->view=view; b->bytes=r.bytes; b->allocated=allocated;
  b->handle=nextHandle_++; b->token=nextToken_++; wired_+=allocated;
  out.handle=b->handle;
  return kIOReturnSuccess;
 }
 /* The persistent memory entry captures the caller's VM object, including
  * COW resolution, before wiring. Never translate a user VA with kernel_pmap,
  * change its WIMG/cache mode, or manufacture a physical descriptor alias. */
 IOReturn importHost(const NativeAdrenoAbiBoImport &r, NativeAdrenoAbiBoAllocReply &out) {
  if (current_task()!=task_ || task_==kernel_task || !fg_abi_import_valid(r.address,r.bytes))
   return kIOReturnBadArgument;
  if (wired_>FG_ABI_MAX_WIRED_BYTES || r.bytes>FG_ABI_MAX_WIRED_BYTES-wired_ ||
      nextHandle_==UINT64_MAX) return kIOReturnNoResources;
  Bo *b=nullptr;
  for (auto &candidate:bos_) if (!candidate.handle) { b=&candidate; break; }
  if (!b) return kIOReturnNoResources;
  IOMemoryDescriptor *memory=IOMemoryDescriptor::withAddressRange(r.address,r.bytes,
      kIODirectionInOut|kIOMemoryMapperNone|kIOMemoryPersistent,task_);
  if (!memory) return kIOReturnNoMemory;
  if (memory->prepare()!=kIOReturnSuccess) { memory->release(); return kIOReturnBadArgument; }
  bool valid=memory->getLength()==r.bytes;
  for (uint64_t off=0;valid && off<r.bytes;off+=NativeAdrenoAbiHostPageBytes) {
   IOByteCount length=0;
   uint64_t pa=memory->getPhysicalSegment(off,&length,kIOMemoryMapperNone);
   valid=pa && !(pa&(NativeAdrenoAbiHostPageBytes-1)) && length>=NativeAdrenoAbiHostPageBytes &&
       pa<(UINT64_C(1)<<FG_ABI_PA_BITS) && fuxi_adreno_host_page_ok(pa);
  }
  // DefaultCache inherits the memory entry's actual policy (WB or Normal NC).
  IOMemoryMap *mapping=valid ? memory->createMappingInTask(kernel_task,0,kIOMapAnywhere|kIOMapDefaultCache) : nullptr;
  if (!mapping || !mapping->getVirtualAddress() || mapping->getLength()!=r.bytes) {
   if (mapping) mapping->release();
   memory->complete(); memory->release(); return kIOReturnBadArgument;
  }
  // Fault/wire the kernel alias as well: pmap lookup and watchdog cache
  // maintenance must never depend on a pageable/demand-faulted KVA mapping.
  if (mapping->wireRange(kIODirectionInOut,0,r.bytes)!=kIOReturnSuccess) {
   mapping->release(); memory->complete(); memory->release(); return kIOReturnNoMemory;
  }
  b->importMemory=memory; b->importMap=mapping; b->bytes=b->allocated=r.bytes;
  b->handle=nextHandle_++; wired_+=r.bytes; out.handle=b->handle;
  return kIOReturnSuccess;
 }
 IOReturn run(unsigned method, const void *in, void *out) {
  if (method==NativeAdrenoMethodQuery) {
   *static_cast<NativeAdrenoAbiParams *>(out)={NativeAdrenoAbiVersion,31,FG_ABI_PA_BITS,NativeAdrenoParamsHostImport,FG_ABI_MAX_BYTES}; return kIOReturnSuccess;
  }
  if (method==NativeAdrenoMethodGetDeviceInfo) {
   /* A740 C510v2, SM8550 UBWC4; programmed UCHE ranges are checked at
    * publication. Catalog/UBWC configuration provenance is in metal.md. */
   *static_cast<NativeAdrenoAbiDeviceInfo *>(out)={0x43050a01,3u<<20,0x1000000,
       0x1fffffffff000ULL,FG_ABI_VA_START,FG_ABI_VA_SIZE,16,6,1,4,NativeAdrenoDeviceClientIova,0};
   return kIOReturnSuccess;
  }
  if (method==NativeAdrenoMethodWaitFence) {
   const auto &r=*static_cast<const NativeAdrenoAbiWaitFence *>(in); return wait(r.fence,r.timeout_ns);
  }
  if (method==NativeAdrenoMethodGetStatus) {
   auto &reply=*static_cast<NativeAdrenoAbiStatus *>(out);
   return fuxi_gpu_abi_status(&reply.faults,&reply.suspends) ? kIOReturnNotReady : kIOReturnSuccess;
  }
  if (method==NativeAdrenoMethodGetTimestamp) {
   int s=fuxi_gpu_abi_timestamp(&static_cast<NativeAdrenoAbiTimestamp *>(out)->ticks);
   return !s ? kIOReturnSuccess : s==-1 ? kIOReturnNotReady : s==-3 ? kIOReturnTimeout : kIOReturnIOError;
  }
  if (method==NativeAdrenoMethodBoCpuUnmap) {
   const auto &r=*static_cast<const NativeAdrenoAbiBoCpuUnmap *>(in);
   Bo *b=find(r.handle);
   if (!b || r.reserved || b->token!=r.token || !b->leases) return kIOReturnBadArgument;
   --b->leases; return kIOReturnSuccess;
  }
  if (method==NativeAdrenoMethodBoFree) {
   Bo *b=find(static_cast<const NativeAdrenoAbiBoFree *>(in)->handle);
   if (!b) return kIOReturnBadArgument;
   if (b->leases) return kIOReturnBusy;
   return drop(*b);
  }
  if (!fuxi_gpu_abi_available()) return kIOReturnNotReady;
  if (method==NativeAdrenoMethodBoImport)
   return importHost(*static_cast<const NativeAdrenoAbiBoImport *>(in),*static_cast<NativeAdrenoAbiBoAllocReply *>(out));
  if (method==NativeAdrenoMethodBoAlloc)
   return allocate(*static_cast<const NativeAdrenoAbiBoAlloc *>(in),*static_cast<NativeAdrenoAbiBoAllocReply *>(out));
  if (method==NativeAdrenoMethodBoMap) {
   const auto &r=*static_cast<const NativeAdrenoAbiBoMap *>(in);
   Bo *b=find(r.handle);
   if (!b || b->iova || (b->importMemory && (r.writeback || r.shareability!=2)) || !fg_abi_map_valid(r.iova,b->bytes,r.permissions,r.writeback,r.shareability,r.reserved))
    return kIOReturnBadArgument;
   uint64_t deadline; clock_interval_to_deadline(150,NSEC_PER_MSEC,&deadline);
   int s;
   do {
    s=b->importMemory ? fuxi_gpu_abi_map_import(r.iova,b->importMap->getVirtualAddress(),b->bytes,r.permissions) :
        fuxi_gpu_abi_map(r.iova,(uintptr_t)b->memory->getBytesNoCopy(),b->bytes,r.permissions,r.writeback,r.shareability);
    if (s!=1 || mach_absolute_time()>=deadline) break;
    IOSleep(1);
   } while (true);
   if (s) return s==1 ? kIOReturnBusy : kIOReturnBadArgument;
   b->iova=r.iova; static_cast<NativeAdrenoAbiBoMapReply *>(out)->iova=r.iova; return kIOReturnSuccess;
  }
  if (method==NativeAdrenoMethodBoCpuMap) {
   Bo *b=find(static_cast<const NativeAdrenoAbiBoCpuMap *>(in)->handle);
   if (!b || b->importMemory || b->leases==UINT32_MAX) return kIOReturnBadArgument;
   ++b->leases;
   *static_cast<NativeAdrenoAbiBoCpuMapReply *>(out)={b->token,0,b->allocated}; return kIOReturnSuccess;
  }
  if (method==NativeAdrenoMethodSubmit) {
   const auto &r=*static_cast<const NativeAdrenoAbiSubmit *>(in);
   if (!r.buffer_count || r.buffer_count>16 || r.reserved || r.queue>=8 || !queues_[r.queue]) return kIOReturnBadArgument;
   if (r.wait_fence) { IOReturn s=wait(r.wait_fence,r.wait_timeout_ns); if (s) return s; }
   uint64_t iova[16], bytes[16];
   for (unsigned i=0;i<r.buffer_count;i++) {
    const auto &ib=r.buffers[i]; Bo *b=find(ib.handle);
    if (!b || !b->iova || !fg_abi_ib_valid(b->iova,b->bytes,ib.iova,ib.bytes)) return kIOReturnBadArgument;
    iova[i]=ib.iova; bytes[i]=ib.bytes;
   }
   uint64_t deadline; clock_interval_to_deadline(150,NSEC_PER_MSEC,&deadline);
   for (;;) {
    uint64_t fence=0;
    int s=fuxi_gpu_abi_submit(iova,bytes,r.buffer_count,&fence);
    if (!s) { if (!firstFence_) firstFence_=fence; lastFence_=fence; static_cast<NativeAdrenoAbiSubmitReply *>(out)->fence=fence; return kIOReturnSuccess; }
    if (s<0) return kIOReturnIOError;
    if (mach_absolute_time()>=deadline) return kIOReturnBusy;
    IOSleep(1); /* Never hold the ring mutex during scheduler waits. */
   }
  }
  if (method==NativeAdrenoMethodSetMetadata || method==NativeAdrenoMethodGetMetadata) {
   const auto &r=*static_cast<const NativeAdrenoAbiMetadata *>(in); Bo *b=find(r.handle);
   if (!b || r.reserved || r.bytes>64) return kIOReturnBadArgument;
   if (method==NativeAdrenoMethodSetMetadata) {
    bcopy(r.text,b->metadata,r.bytes); b->metadataBytes=r.bytes;
    /* the client's own diagnosis, on the console: this is the only channel
     * a userland GPU client has when it refuses a command */
    if (r.bytes) printf("FUXI CLIENT: %.*s\n", (int)r.bytes, r.text);
   }
   else {
    if (r.bytes<b->metadataBytes) return kIOReturnBadArgument;
    auto &reply=*static_cast<NativeAdrenoAbiMetadata *>(out); reply={}; reply.handle=r.handle;
    reply.bytes=b->metadataBytes;
    bcopy(b->metadata,reply.text,reply.bytes);
   }
   return kIOReturnSuccess;
  }
  if (method==NativeAdrenoMethodCreateQueue || method==NativeAdrenoMethodCloseQueue) {
   const auto &r=*static_cast<const NativeAdrenoAbiQueue *>(in);
   if (method==NativeAdrenoMethodCreateQueue) {
    if (r.queue) return kIOReturnBadArgument;
    for (unsigned i=1;i<8;i++) if (!queues_[i]) { queues_[i]=true; static_cast<NativeAdrenoAbiQueueReply *>(out)->queue=i; return kIOReturnSuccess; }
    return kIOReturnNoResources;
   }
   if (!r.queue || r.queue>=8 || !queues_[r.queue]) return kIOReturnBadArgument;
   if (lastFence_ && fuxi_gpu_abi_poll(lastFence_)==1) return kIOReturnBusy;
   queues_[r.queue]=false; return kIOReturnSuccess;
  }
  return kIOReturnUnsupported;
 }
public:
 bool initWithTask(task_t task, void *securityID, UInt32 type) override {
  if (!IOUserClient::initWithTask(task,securityID,type)) return false;
  task_=task; task_reference(task_);
  pid_=proc_selfpid();
  char name[64]={}; proc_name(pid_,name,sizeof(name));
  diagnostic_=execswapTest && type==0x4147 && !strncmp(name,"fuxi-gpu-abi-",13);
  boTable_=IONewZero(Bo,MaxBos);
  if (!boTable_) return false;
  bos_={boTable_,boTable_+MaxBos};
  lock_=IOLockAlloc(); return lock_!=nullptr;
 }
 bool mount() {
  IOLockLock(openLock);
  bool refused=diagnostic_ && testPasses>=2;
  bool ok=!refused && !connected && fuxi_gpu_abi_available();
  if (refused && traceLines<64) {
   ++traceLines;
   IOLog("FUXI ABI: open refused pid %d rc 0x%08x already passed 2/2 rounds this boot\n",pid_,(unsigned)kIOReturnBusy);
  }
  if (ok) live_=connected=true;
  IOLockUnlock(openLock); return ok;
 }
 IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *a,
     IOExternalMethodDispatch *, OSObject *, void *) override {
  if (!a || selector<1 || selector>16) { abiTrace("invalid",pid_,kIOReturnUnsupported); return kIOReturnUnsupported; }
  if (a->scalarInputCount || a->scalarOutputCount || a->asyncWakePort ||
      a->structureInputDescriptor || a->structureOutputDescriptor ||
      a->structureInputSize!=fg_abi_input_size(selector) || a->structureOutputSize!=fg_abi_output_size(selector) ||
      (a->structureInputSize && !a->structureInput) || (a->structureOutputSize && !a->structureOutput)) { abiTrace(abiStep(selector),pid_,kIOReturnBadArgument); return kIOReturnBadArgument; }
  IOLockLock(lock_);
  if (a->structureOutputSize) bzero(a->structureOutput,a->structureOutputSize);
  IOReturn s=live_ ? run(selector,a->structureInput,a->structureOutput) : kIOReturnNotOpen;
  abiTrace(abiStep(selector),pid_,s);
  if (selector==NativeAdrenoMethodWaitFence && s==kIOReturnSuccess) result();
  IOLockUnlock(lock_); return s;
 }
 IOReturn clientMemoryForType(UInt32 token, IOOptionBits *options, IOMemoryDescriptor **memory) override {
  if (!options || !memory) return kIOReturnBadArgument;
  *memory=nullptr;
  IOLockLock(lock_);
  if (live_) for (auto &b:bos_) if (b.handle && b.token==token && b.leases) {
   b.view->retain(); *memory=b.view; *options=kIOMapAnywhere|kIOMapWriteCombineCache; break;
  }
  IOReturn rc=*memory ? kIOReturnSuccess : kIOReturnNotFound;
  abiTrace("cpu-map",pid_,rc);
  IOLockUnlock(lock_); return rc;
 }
 IOReturn clientClose() override {
  if (!lock_) return kIOReturnSuccess;
  IOLockLock(lock_);
  bool wasLive=live_;
  if (live_) {
   if (lastFence_) (void)wait(lastFence_,200000000);
   for (auto &b:bos_) if (b.handle) {
    /* If drain failed, deliberately keep view, prepare and buffer references
     * until reset. A root daemon crash cannot release pages into live DMA. */
    if (drop(b)!=kIOReturnSuccess) IOLog("FUXI GPU ABI: quarantined BO %llu until reset\n",b.handle);
   }
   live_=false;
   IOLockLock(openLock); connected=false; IOLockUnlock(openLock);
  }
  IOLockUnlock(lock_);
  if (wasLive) { abiTrace("close",pid_,kIOReturnSuccess); terminate(); }
  return kIOReturnSuccess;
 }
 void free() override {
  (void)clientClose();
  if (lock_) IOLockFree(lock_);
  /* Quarantined BOs keep their own references; only the table goes. */
  if (boTable_) { IODelete(boTable_,Bo,MaxBos); boTable_=nullptr; bos_={}; }
  if (task_!=TASK_NULL) { task_deallocate(task_); task_=TASK_NULL; }
  IOUserClient::free();
 }
};
OSDefineMetaClassAndStructors(FuxiAdrenoUserClient,IOUserClient)
/* Name matches Turnip's direct unsandboxed backend. Type 0 is its default;
 * 0x4147 is the NativeAdreno plugin/raw-test open type. */
/* IOServiceMatching("IOAcceleratorES") matches metaclass ancestry, not the
 * provider object or a mutable IOClass property. This owns only a discovery
 * name; it implements no Apple family/user-client ABI. The native root-only
 * ABI below remains the sole GPU connection contract. */
class IOAcceleratorES : public IOService {
 OSDeclareDefaultStructors(IOAcceleratorES)
};
OSDefineMetaClassAndStructors(IOAcceleratorES,IOService)
class NativeAdrenoGpuPublished : public IOAcceleratorES {
 OSDeclareDefaultStructors(NativeAdrenoGpuPublished)
public:
 IOReturn newUserClient(task_t task, void *securityID, UInt32 type,
     OSDictionary *, IOUserClient **handler) override {
  IOReturn rc=createClient(task,securityID,type,handler);
  abiTrace("open",proc_selfpid(),rc);
  return rc;
 }
 IOReturn createClient(task_t task, void *securityID, UInt32 type, IOUserClient **handler) {
  if (!handler) return kIOReturnBadArgument;
  *handler=nullptr;
  if (type!=0x4147 && type!=0) return kIOReturnUnsupported;
  if (IOUserClient::clientHasPrivilege(securityID,kIOClientPrivilegeAdministrator)!=kIOReturnSuccess) return kIOReturnNotPrivileged;
  auto *uc=new FuxiAdrenoUserClient;
  if (!uc) return kIOReturnNoMemory;
  if (!uc->initWithTask(task,securityID,type)) { uc->release(); return kIOReturnNoMemory; }
  if (!uc->attach(this)) { uc->release(); return kIOReturnNoResources; }
  if (!uc->start(this) || !uc->mount()) { uc->detach(this); uc->release(); return kIOReturnBusy; }
  *handler=uc; return kIOReturnSuccess;
 }
};
OSDefineMetaClassAndStructors(NativeAdrenoGpuPublished,IOAcceleratorES)
extern "C" void fuxi_gpu_abi_publish(void)
{
 char swap[192]={};
 execswapTest=PE_parse_boot_argn("sweetexecswap",swap,sizeof(swap)) &&
     abiExecswapTest(swap);
 openLock=IOLockAlloc();
 auto *service=new NativeAdrenoGpuPublished;
 if (!openLock || !service) { if (service) service->release(); return; }
 if (!service->init() || !service->attach(IOService::getPlatform()) || !service->start(IOService::getPlatform())) {
  service->release(); return;
 }
 service->setName("NativeAdrenoGpu");
 service->setProperty("NativeAdrenoABI",(uint64_t)NativeAdrenoAbiVersion,32);
 service->setProperty("MetalPluginName","FuxiMetal");
 service->setProperty("MetalPluginClassName","FuxiMTLDevice");
 // Verify the exact ancestry test used by IOService's matching path before
 // publishing a service which Metal will attempt to open.
 if (!service->metaCast("IOAcceleratorES")) {
  IOLog("FUXI MTL: IOAcceleratorES metaclass match FAIL\n");
  service->stop(IOService::getPlatform());service->detach(IOService::getPlatform());
  service->release();return;
 }
 IOLog("FUXI MTL: IOAcceleratorES metaclass match PASS plugin FuxiMetal/FuxiMTLDevice\n");
 service->registerService(); /* retained for boot lifetime */
 IOLog("FUXI GPU ABI: NativeAdrenoGpuPublished v2 HostImport 16K, open 0x4147, root only, watchdog 100 ms\n");
}
#endif
