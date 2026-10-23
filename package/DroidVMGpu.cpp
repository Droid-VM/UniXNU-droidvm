// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Paravirtual Adreno for a DroidVM guest: NativeAdreno's user-client ABI, IOKit surface (NativeAdrenoPublish.cpp) and execbuf
// (NativeAdrenoSubmit.cpp) unchanged, over the host phone's own GPU. crosvm's droidvm-gpu device (--droidvm-gpu, wire in
// DroidVMGpuWire.h) runs every request against the host's KGSL, so this file replaces only the parts of NativeAdreno that own
// hardware: the GPU address space (NativeAdrenoVm: SMMU tables there, KGSL imports bound into the device's one VBO here), the
// command processor ring (NativeAdrenoRing: the CP ring and its fence page there, KGSL submissions with the guest's own
// timestamps here) and the always-on counter (NativeAdrenoTimer). NativeAdrenoVm.cpp, NativeAdrenoRing.cpp and
// NativeAdrenoTimer.cpp are not linked; their classes' declarations are, so the private state named there is used as it is
// documented, and what has no field there lives in this file (one device, one instance of each).
//
// A BO is guest memory, which a pseudo-unprotected VM shares with the host: the device makes a udmabuf of the guest physical
// ranges and KGSL imports it IO-coherent (as drm2kgsl imports a Linux guest's BOs), so no CPU cache maintenance is needed when
// KGSL keeps the flag. User BOs come from one physically contiguous pool (NativeAdrenoMemory::reservePool), which is imported
// once: a pooled BO is a bind of a range of that object. Other memory (an IOSurface a client imports) is imported per mapping,
// at most DROIDVM_GPU_MAX_RANGES physical ranges per KGSL object.
//
// The ring: NativeAdrenoSubmit frames each submission as CP_INDIRECT_BUFFER packets; submitFramed() reads the IBs back out of
// them and submits them with fence = seqno, which KGSL (user-generated timestamps) retires in order on the device's context.
//
// droidvm-gpu=1 starts it from the board's GPU hook (sheng_gpu_power_probe, called by the late display probe before launchd):
// attach, pool, publish the service. droidvm-gpu-metal=1 also registers the AGXAccelerator for the original Metal discovery,
// as sheng-gpu-metal=1 does on sheng. droidvm-gpu-selftest=1 runs one CP_MEM_WRITE through the VM and execbuf first.
// droidvm-gpu-pool-mib=N sizes the pool (default 512). Progress goes to the kernel log ("droidvm-gpu: ...").
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <string.h>
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoMemory.h"
#include "AdrenoPM4.h"
#include "DroidVMGpuWire.h"

extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" unsigned native_adreno_accelerator_instance_size(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" volatile uint32_t native_adreno_wc_enabled;
extern "C" vm_offset_t ml_io_map(vm_offset_t physical, vm_size_t size);   // machine_routines.h, outside IOKit's view

// NativeAdrenoVm.cpp's definitions, which the linked NativeAdreno sources use: IO coherence of imported memory (NativeAdrenoUserClient
// maps an imported BO write-back inner-shareable when it is set), the VM's clock and the board's memory readers (none here).
extern "C" {
volatile uint32_t native_adreno_io_coherent;
uint64_t (*native_adreno_vm_now_us)(void);   // the VM's phase clock, which NativeAdrenoUserClient sets; this VM times nothing
int (*native_adreno_dram_read32)(uint64_t, uint32_t *);
int (*native_adreno_cached_read32)(uint64_t, uint32_t *);
int (*native_adreno_line_audit)(uint64_t, size_t, uint32_t *, uint32_t *);
}

// For the host rig: state 0 off, 1 attached, 2 published, 3 failed (why); the device's facts; counters; the self-test.
extern "C" {
volatile uint32_t droidvm_gpu_state;
const char *volatile droidvm_gpu_why = "";
struct droidvm_gpu_info droidvm_gpu_facts;
// [0] requests [1] failed requests [2] imports [3] binds [4] unbinds [5] frees [6] submits [7] waits [8] last op [9] last status
// [10] transport failures [11] pool MiB [12] pool coherent [13] publish status [14] metal stage [15] metal status
volatile uint64_t droidvm_gpu_stats[16];
// [0] result (1 pass, 2 fail) [1] the value read back [2] the fence [3] the step that failed [4] its status
volatile uint32_t droidvm_gpu_selftest[5];
}

namespace {

uint32_t bootArg(const char *name, uint32_t fallback)
{
    uint32_t value = fallback;
    PE_parse_boot_argn(name, &value, sizeof value);
    return value;
}

void fail(const char *why)
{
    droidvm_gpu_why = why;
    droidvm_gpu_state = 3;
    IOLog("droidvm-gpu: %s\n", why);
}

// ---- virtio-mmio transport: one polled request at a time, as DroidVMDataStore.c --------------------------------------------
// The queue page: descriptors at 0, the driver ring at 128, the device ring at 256. The request (header + payload) and the
// response are bounce buffers in the kernel's static memory, which is physically contiguous.
enum : uint32_t {
    kQueueSize = 4,
    kRequestBytes = 20480,                // header + an IMPORT of DROIDVM_GPU_MAX_RANGES ranges
    kResponseBytes = 4096,
    kTimeoutMs = 5000,
    kTransportFailed = 0x80000000u,       // status of a request the device never answered
};
static_assert(sizeof(droidvm_gpu_request) + sizeof(droidvm_gpu_import) + DROIDVM_GPU_MAX_RANGES * sizeof(droidvm_gpu_range) <=
              kRequestBytes, "request buffer");
struct Desc {uint64_t address; uint32_t length; uint16_t flags, next;};
struct UsedElement {uint32_t id, length;};

uint8_t gQueue[4096] __attribute__((aligned(4096)));
uint8_t gRequest[kRequestBytes] __attribute__((aligned(4096)));
uint8_t gResponse[kResponseBytes] __attribute__((aligned(4096)));
uintptr_t gRegisters;
uint64_t gQueuePhysical, gRequestPhysical, gResponsePhysical;
uint16_t gAvailable, gUsedIndex;
lck_grp_t *gLockGroup;
lck_mtx_t *gLock;
bool gBroken;

// MMIO through single base-register accesses: Gunyah aborts a post-indexed access (no syndrome to emulate it from).
uint32_t rd(unsigned offset)
{
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(gRegisters + offset) : "memory");
    return value;
}
void wr(unsigned offset, uint32_t value)
{
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" : : "r"(value), "r"(gRegisters + offset) : "memory");
}
void wr64(unsigned offset, uint64_t value)
{
    wr(offset, uint32_t(value));
    wr(offset + 4, uint32_t(value >> 32));
}
inline void barrier() {__asm__ volatile("dmb osh" ::: "memory");}

bool contiguous(const void *buffer, size_t bytes, uint64_t *physical)
{
    const vm_offset_t start = vm_offset_t(buffer);
    *physical = ml_static_vtop(start);
    for (size_t at = 4096; at < bytes; at += 4096)
        if (ml_static_vtop(start + at) != *physical + at) return false;
    return *physical != 0;
}

uint64_t nowUs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

const char *attachDevice()
{
    DTEntry armio, node;
    const void *value;
    unsigned int size;
    uint64_t ranges[3], reg[2];
    if (SecureDTLookupEntry(nullptr, "/arm-io", &armio) != kSuccess ||
        SecureDTGetProperty(armio, "ranges", &value, &size) != kSuccess || size < sizeof ranges) return "no /arm-io ranges";
    memcpy(ranges, value, sizeof ranges);
    if (SecureDTLookupEntry(nullptr, "/arm-io/gpu", &node) != kSuccess) return "no /arm-io/gpu";
    if (SecureDTGetProperty(node, "reg", &value, &size) != kSuccess || size != sizeof reg) return "no gpu reg";
    memcpy(reg, value, sizeof reg);
    if (reg[0] < ranges[0] || reg[1] < 0x100 || reg[1] > ranges[2] || reg[0] - ranges[0] > ranges[2] - reg[1])
        return "gpu reg outside /arm-io";
    gRegisters = ml_io_map(vm_offset_t(reg[0] - ranges[0] + ranges[1]), 0x1000);
    if (!gRegisters) return "cannot map the gpu registers";
    if (rd(0x00) != 0x74726976 || rd(0x04) != 2 || rd(0x08) != DROIDVM_GPU_DEVICE_ID) return "no droidvm-gpu device";
    if (!contiguous(gQueue, sizeof gQueue, &gQueuePhysical) || !contiguous(gRequest, sizeof gRequest, &gRequestPhysical) ||
        !contiguous(gResponse, sizeof gResponse, &gResponsePhysical)) return "bounce buffers not contiguous";
    wr(0x70, 0);
    for (unsigned waited = 0; rd(0x70); ++waited) {
        if (waited > 1000) return "device reset failed";
        IODelay(1000);
    }
    wr(0x70, 1);
    wr(0x70, 3);
    wr(0x14, 1);
    if (!(rd(0x10) & 1)) return "no VIRTIO_F_VERSION_1";
    wr(0x24, 0);
    wr(0x20, 0);
    wr(0x24, 1);
    wr(0x20, 1);
    wr(0x70, 11);
    if (!(rd(0x70) & 8)) return "features refused";
    wr(0x30, 0);
    if (rd(0x44) || rd(0x34) < kQueueSize) return "no usable queue 0";
    memset(gQueue, 0, sizeof gQueue);
    *reinterpret_cast<volatile uint16_t *>(gQueue + 128) = 1;   // VIRTQ_AVAIL_F_NO_INTERRUPT: this driver polls
    gAvailable = gUsedIndex = 0;
    barrier();
    wr(0x38, kQueueSize);
    wr64(0x80, gQueuePhysical);
    wr64(0x90, gQueuePhysical + 128);
    wr64(0xa0, gQueuePhysical + 256);
    wr(0x44, 1);
    wr(0x70, 15);
    if ((rd(0x70) & 0x4f) != 15) return "device not ready";
    gLockGroup = lck_grp_alloc_init("droidvm-gpu", LCK_GRP_ATTR_NULL);
    gLock = gLockGroup ? lck_mtx_alloc_init(gLockGroup, LCK_ATTR_NULL) : nullptr;
    return gLock ? nullptr : "no lock";
}

// One request: header + a + b in, response payload (at most `capacity` bytes) out. Returns the device's status (0 or a negative
// host errno) or kTransportFailed, after which the device is not used again.
int32_t call(uint32_t op, const void *a, uint32_t a_bytes, const void *b, uint32_t b_bytes, void *reply, uint32_t capacity)
{
    const uint32_t bytes = a_bytes + b_bytes;
    if (sizeof(droidvm_gpu_request) + bytes > kRequestBytes || sizeof(droidvm_gpu_response) + capacity > kResponseBytes)
        return int32_t(kTransportFailed);
    lck_mtx_lock(gLock);
    if (gBroken) {lck_mtx_unlock(gLock); return int32_t(kTransportFailed);}
    const droidvm_gpu_request header = {op, bytes};
    memcpy(gRequest, &header, sizeof header);
    if (a_bytes) memcpy(gRequest + sizeof header, a, a_bytes);
    if (b_bytes) memcpy(gRequest + sizeof header + a_bytes, b, b_bytes);
    droidvm_gpu_response *response = reinterpret_cast<droidvm_gpu_response *>(gResponse);
    response->status = int32_t(kTransportFailed);
    response->bytes = 0;
    Desc *descriptors = reinterpret_cast<Desc *>(gQueue);
    descriptors[0] = {gRequestPhysical, uint32_t(sizeof header + bytes), 1, 1};
    descriptors[1] = {gResponsePhysical, uint32_t(sizeof *response + capacity), 2, 0};
    volatile uint16_t *available = reinterpret_cast<volatile uint16_t *>(gQueue + 128);
    available[2 + gAvailable % kQueueSize] = 0;
    barrier();
    ++gAvailable;
    available[1] = gAvailable;
    barrier();
    wr(0x50, 0);
    volatile uint16_t *used = reinterpret_cast<volatile uint16_t *>(gQueue + 256);
    const uint64_t start = nowUs();
    bool answered = true;
    while (used[1] == gUsedIndex) {
        const uint64_t waited = nowUs() - start;
        if (waited >= uint64_t(kTimeoutMs) * 1000) {answered = false; break;}
        if (waited < 3000) IODelay(5);
        else IOSleep(1);
        barrier();
    }
    barrier();
    const volatile UsedElement *element = reinterpret_cast<const volatile UsedElement *>(gQueue + 260) + gUsedIndex % kQueueSize;
    int32_t status = int32_t(kTransportFailed);
    if (answered && used[1] == uint16_t(gUsedIndex + 1) && element->id == 0 && element->length >= sizeof *response &&
        response->bytes <= capacity && element->length >= sizeof *response + response->bytes) {
        status = response->status;
        if (!status && reply && capacity) memcpy(reply, gResponse + sizeof *response, response->bytes);
    }
    if (answered) ++gUsedIndex;
    const uint32_t interrupts = rd(0x60) & 3;
    if (interrupts) wr(0x64, interrupts);
    if (status == int32_t(kTransportFailed)) {
        gBroken = true;
        droidvm_gpu_stats[10] = droidvm_gpu_stats[10] + 1;
    }
    lck_mtx_unlock(gLock);
    droidvm_gpu_stats[0] = droidvm_gpu_stats[0] + 1;
    droidvm_gpu_stats[8] = op;
    droidvm_gpu_stats[9] = uint32_t(status);
    if (status && !(op == DROIDVM_GPU_OP_WAIT && status == -110 /* ETIMEDOUT: the normal answer of a wait */))
        droidvm_gpu_stats[1] = droidvm_gpu_stats[1] + 1;
    return status;
}

IOReturn ioReturn(int32_t status)
{
    if (!status) return kIOReturnSuccess;
    if (status == int32_t(kTransportFailed)) return kIOReturnNotResponding;
    switch (-status) {
    case 12: return kIOReturnNoMemory;    // ENOMEM
    case 22: return kIOReturnBadArgument; // EINVAL
    case 16: return kIOReturnBusy;        // EBUSY
    case 110: return kIOReturnTimeout;    // ETIMEDOUT (Linux)
    default: return kIOReturnIOError;
    }
}

// ---- KGSL objects ---------------------------------------------------------------------------------------------------------
int32_t importRanges(const droidvm_gpu_range *ranges, uint32_t count, uint32_t flags, uint32_t *object, bool *coherent)
{
    const droidvm_gpu_import header = {count, flags};
    droidvm_gpu_import_reply reply = {};
    const int32_t status = call(DROIDVM_GPU_OP_IMPORT, &header, sizeof header, ranges, count * uint32_t(sizeof *ranges),
                                &reply, sizeof reply);
    if (status) return status;
    droidvm_gpu_stats[2] = droidvm_gpu_stats[2] + 1;
    *object = reply.object;
    *coherent = reply.coherent != 0;
    return 0;
}
int32_t bindRange(uint32_t object, uint32_t op, uint64_t offset, uint64_t iova, uint64_t bytes)
{
    const droidvm_gpu_bind bind = {object, op, offset, iova, bytes};
    const int32_t status = call(DROIDVM_GPU_OP_BIND, &bind, sizeof bind, nullptr, 0, nullptr, 0);
    if (!status) droidvm_gpu_stats[op == DROIDVM_GPU_BIND ? 3 : 4] = droidvm_gpu_stats[op == DROIDVM_GPU_BIND ? 3 : 4] + 1;
    return status;
}
int32_t freeObject(uint32_t object)
{
    const droidvm_gpu_free request = {object, 0};
    const int32_t status = call(DROIDVM_GPU_OP_FREE, &request, sizeof request, nullptr, 0, nullptr, 0);
    if (!status) droidvm_gpu_stats[5] = droidvm_gpu_stats[5] + 1;
    return status;
}

// The BO pool, imported once (pooled BOs are binds of ranges of it).
uint64_t gPoolPhysical, gPoolBytes;
uint32_t gPoolObject;
bool gPoolCoherent;

// What a mapping (index i of NativeAdrenoVm::mappings_) holds of the device: ranges of KGSL objects (`child` bytes into the object),
// each bound at the mapping's iova + `offset`. An imported piece is a whole object of its own; a pooled BO is one range of the pool's.
struct Piece {uint32_t object; uint64_t child, offset, bytes;};
struct Binding {Piece *pieces; uint32_t count, capacity; bool pooled, coherent;};
Binding gBindings[NativeAdrenoVmMaxObjects];

void dropPieces(Binding &binding, uint64_t iova, bool unbind)
{
    for (uint32_t i = binding.count; i-- > 0;) {
        const Piece &piece = binding.pieces[i];
        if (unbind) (void)bindRange(piece.object, DROIDVM_GPU_UNBIND, piece.child, iova + piece.offset, piece.bytes);
        if (!binding.pooled) (void)freeObject(piece.object);
    }
    if (binding.pieces) IODeleteData(binding.pieces, Piece, binding.capacity);
    binding = {};
}

// Turns a BO's physical spans into bound KGSL objects at iova. A pooled BO (one span inside the pool) is a bind of the pool object.
IOReturn bindMemory(Binding &binding, const NativeAdrenoMemory *memory, uint64_t iova, bool read_only, bool cached)
{
    const NativeAdrenoPhysicalSpan *spans = memory->spans();
    const size_t count = memory->spanCount();
    if (gPoolObject && count == 1 && spans[0].physical >= gPoolPhysical && spans[0].bytes <= gPoolBytes &&
        spans[0].physical - gPoolPhysical <= gPoolBytes - spans[0].bytes) {
        binding.pieces = IONewZeroData(Piece, 1);
        if (!binding.pieces) return kIOReturnNoMemory;
        binding.capacity = 1;
        binding.pooled = true;
        binding.coherent = gPoolCoherent;
        const uint64_t offset = spans[0].physical - gPoolPhysical;
        const int32_t status = bindRange(gPoolObject, DROIDVM_GPU_BIND, offset, iova, spans[0].bytes);
        if (status) {dropPieces(binding, iova, false); return ioReturn(status);}
        binding.pieces[0] = {gPoolObject, offset, 0, spans[0].bytes};
        binding.count = 1;
        return kIOReturnSuccess;
    }
    // Physically adjacent spans merge; every DROIDVM_GPU_MAX_RANGES ranges make one object.
    size_t ranges = 0;
    for (size_t i = 0; i < count; ++i)
        if (!i || spans[i].physical != spans[i - 1].physical + spans[i - 1].bytes) ++ranges;
    const uint32_t pieces = uint32_t((ranges + DROIDVM_GPU_MAX_RANGES - 1) / DROIDVM_GPU_MAX_RANGES);
    binding.pieces = IONewZeroData(Piece, pieces);
    droidvm_gpu_range *list = IONewData(droidvm_gpu_range, DROIDVM_GPU_MAX_RANGES);
    if (!binding.pieces || !list) {
        if (list) IODeleteData(list, droidvm_gpu_range, DROIDVM_GPU_MAX_RANGES);
        if (binding.pieces) IODeleteData(binding.pieces, Piece, pieces);
        binding = {};
        return kIOReturnNoMemory;
    }
    binding.capacity = pieces;
    binding.coherent = true;
    const uint32_t flags = DROIDVM_GPU_IMPORT_COHERENT | (cached ? DROIDVM_GPU_IMPORT_CACHED : 0) |
                           (read_only ? DROIDVM_GPU_IMPORT_READ_ONLY : 0);
    size_t at = 0;
    IOReturn result = kIOReturnSuccess;
    while (at < count && !result) {
        uint32_t used = 0;
        const uint64_t offset = spans[at].offset;
        uint64_t bytes = 0;
        while (at < count) {
            const bool merges = used && spans[at].physical == list[used - 1].address + list[used - 1].bytes;
            if (!merges && used == DROIDVM_GPU_MAX_RANGES) break;
            if (merges) list[used - 1].bytes += spans[at].bytes;
            else list[used++] = {spans[at].physical, spans[at].bytes};
            bytes += spans[at].bytes;
            ++at;
        }
        uint32_t object = 0;
        bool coherent = false;
        int32_t status = importRanges(list, used, flags, &object, &coherent);
        if (!status) {
            binding.pieces[binding.count++] = {object, 0, offset, bytes};
            binding.coherent = binding.coherent && coherent;
            status = bindRange(object, DROIDVM_GPU_BIND, 0, iova + offset, bytes);
            if (status) {
                (void)freeObject(object);
                --binding.count;
            }
        }
        result = ioReturn(status);
    }
    IODeleteData(list, droidvm_gpu_range, DROIDVM_GPU_MAX_RANGES);
    if (result) dropPieces(binding, iova, true);
    return result;
}

}  // namespace

// ---- NativeAdrenoVm over KGSL -----------------------------------------------------------------------------------------------
// The SMMU arguments of the native driver have no meaning here; attach only allocates the mapping table. Spans are counted for
// spanCount() in lower_count_. The VA is the device's VBO (droidvm_gpu_facts.va_start/va_size).
IOReturn NativeAdrenoVm::attach(const ArmSmmu4KIo *, ArmSmmu4KContextConfig *, ArmSmmu4KStreamState *, const NativeAdrenoVmRegion *,
    const NativeAdrenoVmRegion *, size_t max_spans, unsigned)
{
    if (attached_) return kIOReturnBusy;
    mappings_ = IONewZeroData(Mapping, NativeAdrenoVmMaxObjects);
    if (!mappings_) return kIOReturnNoMemory;
    capacity_ = max_spans;
    lower_count_ = upper_count_ = objects_ = 0;
    attached_ = true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::detach()
{
    if (!attached_ || objects_) return kIOReturnBusy;
    IODeleteData(mappings_, Mapping, NativeAdrenoVmMaxObjects);
    mappings_ = nullptr;
    attached_ = false;
    return kIOReturnSuccess;
}
NativeAdrenoVm::Mapping *NativeAdrenoVm::find(uint64_t handle)
{
    if (!handle || !mappings_) return nullptr;
    for (size_t i = 0; i < NativeAdrenoVmMaxObjects; ++i)
        if (mappings_[i].memory && mappings_[i].handle == handle) return &mappings_[i];
    return nullptr;
}
IOReturn NativeAdrenoVm::map(NativeAdrenoMemory *memory, unsigned upper, uint64_t iova, unsigned permissions, unsigned writeback,
    unsigned shareability, uint64_t *handle)
{
    if (!handle) return kIOReturnBadArgument;
    *handle = 0;
    result_ = {};
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    const uint64_t start = droidvm_gpu_facts.va_start, size = droidvm_gpu_facts.va_size;
    if (!memory || upper || !memory->backed() || memory->quarantined() || !memory->bytes() || !memory->spanCount() ||
        !(permissions & ArmSmmu4KRead) || (permissions & ~15u) || writeback > 1 || (shareability != 2 && shareability != 3) ||
        (iova & 4095) || iova < start || iova - start >= size || memory->bytes() > size - (iova - start))
        return kIOReturnBadArgument;
    if (objects_ >= NativeAdrenoVmMaxObjects || memory->spanCount() > capacity_ - spanCount()) return kIOReturnNoResources;
    for (size_t i = 0; i < NativeAdrenoVmMaxObjects; ++i) {
        const Mapping &m = mappings_[i];
        if (m.memory && iova <= m.iova + (m.memory->bytes() - 1) && m.iova <= iova + (memory->bytes() - 1)) return kIOReturnBusy;
    }
    size_t index = 0;
    while (index < NativeAdrenoVmMaxObjects && mappings_[index].memory) ++index;
    if (index == NativeAdrenoVmMaxObjects) return kIOReturnNoResources;
    IOReturn status = memory->holdForDevice();
    if (status) return status;
    // Not coherent after all (KGSL dropped IOCOHERENT): the CPU caches are cleaned over it first, as the native driver does.
    status = bindMemory(gBindings[index], memory, iova, !(permissions & ArmSmmu4KWrite), writeback != 0);
    if (!status && !gBindings[index].coherent)
        status = memory->publishForDevice(0, memory->bytes(), permissions & ArmSmmu4KWrite, distrust_write_combined_);
    if (status) {
        if (gBindings[index].pieces) dropPieces(gBindings[index], iova, true);
        (void)memory->retireAfterQuiescence();
        return status;
    }
    mappings_[index] = {memory, next_handle_++, iova, memory->spanCount(), memory->spanCount(), permissions, writeback, shareability, 0};
    ++objects_;
    lower_count_ += memory->spanCount();
    *handle = mappings_[index].handle;
    result_.complete = true;
    result_.completed_spans = memory->spanCount();
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::unmap(uint64_t handle)
{
    result_ = {};
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    Mapping *record = find(handle);
    if (!record) return kIOReturnBadArgument;
    if (record->jobs) return kIOReturnBusy;
    Binding &binding = gBindings[record - mappings_];
    if (!binding.coherent && (record->permissions & ArmSmmu4KWrite) &&
        record->memory->acquireDeviceWrites(0, record->memory->bytes()) != kIOReturnSuccess) return kIOReturnIOError;
    dropPieces(binding, record->iova, true);
    if (record->memory->retireAfterQuiescence() != kIOReturnSuccess) {poisoned_ = true; return kIOReturnIOError;}
    lower_count_ -= record->spans;
    *record = {};
    --objects_;
    result_.complete = true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::describe(uint64_t handle, uint64_t *iova, size_t *bytes, unsigned *permissions) const
{
    if (!iova || !bytes || !permissions) return kIOReturnBadArgument;
    *iova = 0;
    *bytes = 0;
    *permissions = 0;
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    for (size_t m = 0; mappings_ && m < NativeAdrenoVmMaxObjects; ++m)
        if (mappings_[m].memory && mappings_[m].handle == handle) {
            *iova = mappings_[m].iova;
            *bytes = mappings_[m].memory->bytes();
            *permissions = mappings_[m].permissions;
            return kIOReturnSuccess;
        }
    return kIOReturnBadArgument;
}
IOReturn NativeAdrenoVm::holdJob(uint64_t handle)
{
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    Mapping *record = find(handle);
    if (!record) return kIOReturnBadArgument;
    if (record->jobs == UINT32_MAX) return kIOReturnNoResources;
    const IOReturn status = record->memory->holdForDevice();
    if (status) return status;
    ++record->jobs;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::retireJobAfterCompletion(uint64_t handle)
{
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    Mapping *record = find(handle);
    if (!record || !record->jobs) return kIOReturnBadArgument;
    const IOReturn status = record->memory->retireAfterQuiescence();
    if (status) return status;
    --record->jobs;
    return kIOReturnSuccess;
}
// KGSL owns the GPU's TLB.
IOReturn NativeAdrenoVm::invalidateTlb() {return attached_ && !poisoned_ ? kIOReturnSuccess : kIOReturnNotReady;}
IOReturn NativeAdrenoVm::publishMapped()
{
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    for (size_t i = 0; i < NativeAdrenoVmMaxObjects; ++i) {
        const Mapping &m = mappings_[i];
        if (!m.memory || gBindings[i].coherent) continue;
        if (m.memory->publishForDevice(0, m.memory->bytes(), true, distrust_write_combined_)) {poisoned_ = true; return kIOReturnIOError;}
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::acquireImported()
{
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    for (size_t i = 0; i < NativeAdrenoVmMaxObjects; ++i) {
        const Mapping &m = mappings_[i];
        if (!m.memory || !m.memory->imported() || !(m.permissions & ArmSmmu4KWrite) || gBindings[i].coherent) continue;
        if (m.memory->acquireDeviceWrites(0, m.memory->bytes())) {poisoned_ = true; return kIOReturnIOError;}
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::reassertWriteCombined(uint32_t *reset) const
{
    if (!reset) return kIOReturnBadArgument;
    *reset = 0;
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    for (size_t i = 0; i < NativeAdrenoVmMaxObjects; ++i) {
        if (!mappings_[i].memory) continue;
        uint32_t pages = 0;
        const IOReturn status = mappings_[i].memory->reassertWriteCombined(&pages);
        *reset += pages;
        if (status) return status;
    }
    return kIOReturnSuccess;
}
// No board reader of DRAM: the execbuf's write-combine audit and coherence samples have nothing to compare.
IOReturn NativeAdrenoVm::auditRange(uint64_t, size_t, uint32_t *lines, uint32_t *dirty, uint32_t *stale) const
{
    if (lines) *lines = 0;
    if (dirty) *dirty = 0;
    if (stale) *stale = 0;
    return kIOReturnUnsupported;
}
IOReturn NativeAdrenoVm::sampleWord(uint64_t, uint32_t *cached, uint32_t *dram) const
{
    if (!cached || !dram) return kIOReturnBadArgument;
    *cached = *dram = ~0u;
    return kIOReturnUnsupported;
}
IOReturn NativeAdrenoVm::peekWords(uint64_t iova, uint32_t *out, size_t count) const
{
    if (!out || !attached_ || poisoned_) return kIOReturnBadArgument;
    for (size_t k = 0; k < count; ++k) out[k] = 0;
    for (size_t i = 0; i < NativeAdrenoVmMaxObjects; ++i) {
        const Mapping &m = mappings_[i];
        if (!m.memory || !m.memory->mapping() || iova < m.iova || iova - m.iova >= m.memory->bytes()) continue;
        const size_t offset = size_t(iova - m.iova);
        for (size_t k = 0; k < count && offset + 4 * k + 4 <= m.memory->bytes(); ++k)
            out[k] = static_cast<const volatile uint32_t *>(m.memory->mapping())[offset / 4 + k];
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}

// ---- NativeAdrenoRing over KGSL ---------------------------------------------------------------------------------------------
// seqno_ is the last timestamp submitted, completed_ the last one a wait() saw retired; the fence of a submission is its timestamp.
namespace {
uint32_t gRetired;   // the newest retired timestamp the device reported (retirement is in order, so it is never ahead)
uint32_t gIbHeader;  // NativeAdrenoSubmit's CP_INDIRECT_BUFFER header
int32_t readRetired(uint32_t *value)
{
    droidvm_gpu_timestamp reply = {};
    const int32_t status = call(DROIDVM_GPU_OP_RETIRED, nullptr, 0, nullptr, 0, &reply, sizeof reply);
    if (!status) {
        *value = reply.timestamp;
        if (reply.timestamp > gRetired) gRetired = reply.timestamp;
    }
    return status;
}
}  // namespace

IOReturn NativeAdrenoRing::attach(const NativeAdrenoRingIo *, const NativeAdrenoRingCache *, void *, size_t, uint64_t, void *, size_t,
    uint64_t)
{
    if (attached_) return kIOReturnBusy;
    if (AdrenoPM4Type7(0x3f, 3, &gIbHeader) != AdrenoPM4Success) return kIOReturnInternalError;
    uint32_t retired = 0;
    if (readRetired(&retired)) return kIOReturnNotResponding;
    // A device that was used before (a guest reboot without a VM restart) continues its timeline.
    seqno_ = completed_ = retired;
    attached_ = true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::poison()
{
    poisoned_ = true;
    return kIOReturnIOError;
}
IOReturn NativeAdrenoRing::submitFramed(const uint32_t *words, size_t word_count, uint32_t *fence)
{
    if (!fence) return kIOReturnBadArgument;
    *fence = 0;
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    if (!words || !word_count || word_count % 4 || word_count / 4 > DROIDVM_GPU_MAX_IBS) return kIOReturnBadArgument;
    droidvm_gpu_ib ibs[DROIDVM_GPU_MAX_IBS];
    const uint32_t count = uint32_t(word_count / 4);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t *packet = words + 4 * i;
        if (packet[0] != gIbHeader || !packet[3] || packet[3] > 0xfffff) return kIOReturnBadArgument;
        ibs[i] = {uint64_t(packet[1]) | uint64_t(packet[2]) << 32, uint64_t(packet[3]) * 4};
    }
    const droidvm_gpu_submit header = {seqno_ + 1, count};
    const int32_t status = call(DROIDVM_GPU_OP_SUBMIT, &header, sizeof header, ibs, count * uint32_t(sizeof *ibs), nullptr, 0);
    droidvm_gpu_stats[6] = droidvm_gpu_stats[6] + 1;
    // KGSL refused the commands (they did not run, the timestamp is still free); a lost transport or an invalidated context
    // (EDEADLK, after a GPU fault) leaves nothing to submit to.
    if (status == int32_t(kTransportFailed) || status == -35 /* EDEADLK */ || status == -71 /* EPROTO */) {
        droidvm_gpu_facts.faults = droidvm_gpu_facts.faults + 1;
        return poison();
    }
    if (status) return ioReturn(status);
    reset_pending_ = false;   // KGSL frames each submission for its context itself
    ++seqno_;
    ++submissions_;
    *fence = seqno_;
    return kIOReturnSuccess;
}
NativeAdrenoRingFence NativeAdrenoRing::observeFence(uint32_t fence, const NativeAdrenoRingClock *clock, uint32_t timeout_us,
    uint32_t *observed)
{
    if (observed) *observed = 0;
    if (!attached_ || poisoned_) return NativeAdrenoRingFencePoisoned;
    if (!fence || fence > seqno_) return NativeAdrenoRingFenceArgument;
    if (fence <= gRetired) {
        if (observed) *observed = gRetired;
        return NativeAdrenoRingFenceComplete;
    }
    const uint64_t start = clock && clock->now_us ? clock->now_us(clock->cookie) : 0;
    for (;;) {
        uint32_t value = 0;
        if (readRetired(&value)) {poison(); return NativeAdrenoRingFencePoisoned;}
        if (observed) *observed = value;
        if (value > seqno_) {poison(); return NativeAdrenoRingFencePoisoned;}
        if (value >= fence) return NativeAdrenoRingFenceComplete;
        if (!clock || !clock->now_us) return NativeAdrenoRingFenceTimeout;
        const uint64_t waited = clock->now_us(clock->cookie) - start;
        if (waited >= timeout_us) return NativeAdrenoRingFenceTimeout;
        if (timeout_us - waited >= 1000) {
            // KGSL sleeps on the timestamp for up to a millisecond (its granularity); a timeout is the normal answer.
            const droidvm_gpu_timestamp wait = {fence, 1};
            const int32_t status = call(DROIDVM_GPU_OP_WAIT, &wait, sizeof wait, nullptr, 0, nullptr, 0);
            droidvm_gpu_stats[7] = droidvm_gpu_stats[7] + 1;
            if (status == int32_t(kTransportFailed)) {poison(); return NativeAdrenoRingFencePoisoned;}
        } else if (clock->idle && clock->idle(clock->cookie)) {
            return NativeAdrenoRingFenceTimeout;
        }
    }
}
NativeAdrenoRingFence NativeAdrenoRing::wait(uint32_t fence, const NativeAdrenoRingClock *clock, uint32_t timeout_us,
    uint32_t *observed)
{
    uint32_t value = 0;
    const NativeAdrenoRingFence result = observeFence(fence, clock, timeout_us, &value);
    if (observed) *observed = value;
    if (result == NativeAdrenoRingFenceComplete && value > completed_) completed_ = value;
    return result;
}

// ---- NativeAdrenoTimer: the host's counter --------------------------------------------------------------------------------
IOReturn NativeAdrenoTimer::attach(const NativeAdrenoTimeIo *, uint32_t source)
{
    if (attached_) return kIOReturnBusy;
    source_ = source;
    attached_ = true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoTimer::detach()
{
    if (!attached_) return kIOReturnNotReady;
    attached_ = false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoTimer::read(uint64_t *ticks) const
{
    if (!ticks) return kIOReturnBadArgument;
    *ticks = 0;
    if (!attached_) return kIOReturnNotReady;
    droidvm_gpu_ticks reply = {};
    const int32_t status = call(DROIDVM_GPU_OP_TICKS, nullptr, 0, nullptr, 0, &reply, sizeof reply);
    if (status) return ioReturn(status);
    *ticks = reply.ticks;
    return kIOReturnSuccess;
}

// ---- board: bring-up and publication ---------------------------------------------------------------------------------------
namespace {
NativeAdrenoVm gVm;
NativeAdrenoRing gRing;
NativeAdrenoSubmit gSubmit;
NativeAdrenoTimer gTimer;
NativeAdrenoAbiDeviceInfo gDeviceInfo;

uint64_t ringNow(void *) {return nowUs();}
int ringIdle(void *)
{
    IODelay(20);
    return 0;
}
const NativeAdrenoRingClock gClock = {nullptr, ringNow, ringIdle};

// The BO pool: the largest of droidvm-gpu-pool-mib, then halves down to 64 MiB, that the VM still has contiguous. Its physical
// base is that of the first pooled BO of a fresh pool (best fit from the one free extent takes offset 0).
void reservePool(unsigned output_bits)
{
    uint32_t mib = bootArg("droidvm-gpu-pool-mib", 512);
    while (mib >= 64 && NativeAdrenoMemory::reservePool(output_bits, size_t(mib) << 20)) mib /= 2;
    if (mib < 64) {IOLog("droidvm-gpu: no BO pool, every BO is imported on its own\n"); return;}
    NativeAdrenoMemory probe;
    if (probe.allocate(4096, output_bits, true, 4096, true) || probe.spanCount() != 1) {(void)probe.release(); return;}
    const uint64_t physical = probe.spans()[0].physical;
    (void)probe.release();
    const droidvm_gpu_range range = {physical, uint64_t(mib) << 20};
    uint32_t object = 0;
    bool coherent = false;
    const int32_t status = importRanges(&range, 1, DROIDVM_GPU_IMPORT_COHERENT, &object, &coherent);
    if (status) {IOLog("droidvm-gpu: the pool import failed (%d)\n", status); return;}
    gPoolPhysical = physical;
    gPoolBytes = range.bytes;
    gPoolObject = object;
    gPoolCoherent = coherent;
    if (native_adreno_wc_enabled) (void)NativeAdrenoMemory::writeCombinePool();
    droidvm_gpu_stats[11] = mib;
    droidvm_gpu_stats[12] = coherent;
    IOLog("droidvm-gpu: pool %u MiB at %#llx, KGSL object %u, %s\n", mib, physical, object, coherent ? "IO-coherent" : "not coherent");
}

// One CP_MEM_WRITE through the VM, the execbuf and the ring: the GPU writes a word into a second BO.
void selfTest(unsigned output_bits)
{
    enum : uint32_t {kValue = 0x0d201a75};
    auto step = [](unsigned at, IOReturn status) {
        if (status && !droidvm_gpu_selftest[3]) {droidvm_gpu_selftest[3] = at; droidvm_gpu_selftest[4] = uint32_t(status);}
        return status == kIOReturnSuccess;
    };
    NativeAdrenoMemory ib, target;
    uint64_t ibHandle = 0, targetHandle = 0;
    const uint64_t ibIova = droidvm_gpu_facts.va_start + 0x100000, targetIova = ibIova + 0x1000;
    uint32_t fence = 0;
    bool ok = step(1, ib.allocate(4096, output_bits, true, 4096, true)) && step(2, target.allocate(4096, output_bits, true, 4096, true)) &&
              ib.mapping() && target.mapping();
    if (ok) {
        volatile uint32_t *words = static_cast<volatile uint32_t *>(ib.mapping());
        volatile uint32_t *value = static_cast<volatile uint32_t *>(target.mapping());
        uint32_t header = 0;
        ok = step(3, AdrenoPM4Type7(0x3d, 3, &header) == AdrenoPM4Success ? kIOReturnSuccess : kIOReturnInternalError);
        words[0] = header;
        words[1] = uint32_t(targetIova);
        words[2] = uint32_t(targetIova >> 32);
        words[3] = kValue;
        value[0] = 0;
        __asm__ volatile("dsb sy" ::: "memory");
        ok = ok && step(4, gVm.map(&ib, 0, ibIova, ArmSmmu4KRead, 0, 2, &ibHandle)) &&
             step(5, gVm.map(&target, 0, targetIova, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2, &targetHandle));
        const NativeAdrenoSubmitBuffer buffer = {ibHandle, ibIova, 16};
        ok = ok && step(6, gSubmit.submit(&buffer, 1, 0, &fence)) && step(7, gSubmit.wait(fence, &gClock, 2000000));
        droidvm_gpu_selftest[1] = value[0];
        droidvm_gpu_selftest[2] = fence;
        ok = ok && step(8, value[0] == kValue ? kIOReturnSuccess : kIOReturnIOError);
    }
    if (targetHandle) (void)gVm.unmap(targetHandle);
    if (ibHandle) (void)gVm.unmap(ibHandle);
    (void)target.release();
    (void)ib.release();
    droidvm_gpu_selftest[0] = ok ? 1 : 2;
    IOLog("droidvm-gpu: self-test %s: read %#x, fence %u, step %u status %#x\n", ok ? "passed" : "FAILED", droidvm_gpu_selftest[1],
           fence, droidvm_gpu_selftest[3], droidvm_gpu_selftest[4]);
}

// The processes the accelerator's user client is granted to in place of the sandbox's verdict (NativeSandboxGrantProcess, a
// comma-separated list, patch zzw-native-adreno-sandbox-grant): the render server, and each process droidvm-gpu-clients gives a native
// Metal device of its own (DroidVMLaunchPolicy.c writes the same list for the plugin). Their own profiles refuse this driver's user
// client as well: without the grant WidgetRenderer_Default and iconservicesagent got no device (Vulkan -3), and CoreImage then crashed
// in its GL fallback, which left the widgets and every Settings icon blank. "*" (every process) gets no grant; only named processes
// pass the sandbox, a name longer than a process name's 16 characters by its first 16 (com.apple.WebKit.* names WebKit's processes).
OSString *grantedProcesses()
{
    char names[256] = "backboardd";
    char clients[200] = {};
    if (PE_parse_boot_argn("droidvm-gpu-clients", clients, sizeof clients - 1)) {
        char *name = clients;
        for (char *at = clients;; ++at) {
            if (*at && *at != ',') continue;
            const bool last = !*at;
            *at = 0;
            if (*name && strcmp(name, "*") != 0) {
                strlcat(names, ",", sizeof names);
                strlcat(names, name, sizeof names);
            }
            if (last) break;
            name = at + 1;
        }
    }
    return OSString::withCString(names);
}

// sheng's publishMetal (UniXNU-sheng GpuPowerProbe.cpp): the staged AGXAccelerator class registered after the original IOGPUFamily
// loaded, with the two keys the original Metal discovery reads and the sandbox grant (grantedProcesses).
void publishMetal()
{
    droidvm_gpu_stats[14] = 1;
    kern_return_t status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (status) {droidvm_gpu_stats[15] = uint32_t(status); return;}
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) {droidvm_gpu_stats[15] = uint32_t(kIOReturnNotFound); return;}
    droidvm_gpu_stats[14] = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {droidvm_gpu_stats[15] = uint32_t(kIOReturnNoResources); return;}
    native_adreno_accelerator_static_init();
    status = OSMetaClass::postModLoad(handle);
    if (status) {droidvm_gpu_stats[15] = uint32_t(status); return;}
    if (native_adreno_accelerator_instance_size() != 136) {droidvm_gpu_stats[15] = uint32_t(kIOReturnInvalid); return;}
    droidvm_gpu_stats[14] = 3;
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator) {droidvm_gpu_stats[15] = uint32_t(kIOReturnNoMemory); return;}
    if (!accelerator->init()) {accelerator->release(); droidvm_gpu_stats[15] = uint32_t(kIOReturnError); return;}
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    if (OSString *granted = grantedProcesses()) {
        accelerator->setProperty("NativeSandboxGrantProcess", granted);
        granted->release();
    } else {
        accelerator->setProperty("NativeSandboxGrantProcess", "backboardd");
    }
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    // OpenGL ES finds its renderer the same way: GLEngine loads the bundle this names. The stock accelerator
    // (AGXAcceleratorG9G) names AppleMetalGLRenderer, Apple's GL on Metal, which then runs on the process's Metal device.
    // Without it every EAGL context fails, and CoreImage's GL path calls through an empty dispatch table.
    accelerator->setProperty("IOGLESBundleName", "AppleMetalGLRenderer");
    droidvm_gpu_stats[14] = 4;
    if (!accelerator->attach(IOService::getPlatform())) {accelerator->release(); droidvm_gpu_stats[15] = uint32_t(kIOReturnNotAttached); return;}
    accelerator->registerService();
    droidvm_gpu_stats[14] = 5;
}

void gpuThread(void *, wait_result_t)
{
    const char *why = attachDevice();
    if (why) {fail(why); return;}
    droidvm_gpu_state = 1;
    if (call(DROIDVM_GPU_OP_INFO, nullptr, 0, nullptr, 0, &droidvm_gpu_facts, sizeof droidvm_gpu_facts)) {fail("INFO failed"); return;}
    const droidvm_gpu_info &info = droidvm_gpu_facts;
    if (info.version != DROIDVM_GPU_WIRE_VERSION || !info.chip_id || info.va_size < (UINT64_C(3) << 30) || (info.va_start & 4095)) {
        fail("unusable device facts");
        return;
    }
    IOLog("droidvm-gpu: KGSL chip %#x gmem %#x@%#llx hbb %u ubwc %u, VBO %#llx+%#llx\n", info.chip_id, info.gmem_bytes,
           info.gmem_base, info.highest_bank_bit, info.ubwc_mode, info.va_start, info.va_size);
    // GPU addresses below 2^input_bits; guest physical addresses (the 5 GiB VM's RAM ends below 2^33) below 2^40.
    const uint64_t end = info.va_start + info.va_size - 1;
    const unsigned input_bits = 64 - unsigned(__builtin_clzll(end)), output_bits = 40;
    {
        uint32_t wc = 1;
        PE_parse_boot_argn("droidvm-gpu-wc", &wc, sizeof wc);
        native_adreno_wc_enabled = wc ? 1 : 0;
    }
    reservePool(output_bits);
    native_adreno_io_coherent = gPoolObject && gPoolCoherent ? 1 : 0;
    IOReturn status = gVm.attach(nullptr, nullptr, nullptr, nullptr, nullptr, 1u << 20, 0);
    if (!status) status = gRing.attach(nullptr, nullptr, nullptr, 0, 0, nullptr, 0, 0);
    if (!status) status = gSubmit.attach(&gVm, &gRing);
    if (!status) status = gTimer.attach(nullptr, NativeAdrenoTimeCp);
    if (status) {fail("VM, ring or execbuf did not attach"); return;}
    // The Vulkan driver's facts: the chip, GMEM and trap base as KGSL reports and programmed them; UBWC 4.0 with swizzle levels 2+3
    // and eight-channel macrotiles (msm's sm8650 UBWC configuration, as sm8550's that sheng states). The VA is the VBO, which
    // NativeAdrenoPublish cuts into the clients' windows from its start (droidvm_gpu_window_base).
    gDeviceInfo = {info.chip_id, info.gmem_bytes, info.gmem_base, info.uche_trap_base, info.va_start, info.va_size,
                   info.highest_bank_bit ? info.highest_bank_bit : 16, 6, 1, 4, NativeAdrenoDeviceClientIova, 0};
    if (bootArg("droidvm-gpu-selftest", 0)) selfTest(output_bits);
    status = native_adreno_publish(&gVm, &gSubmit, &gRing, &gClock, &gTimer, &gDeviceInfo, input_bits, output_bits);
    droidvm_gpu_stats[13] = uint32_t(status);
    if (status) {fail("publish failed"); return;}
    droidvm_gpu_state = 2;
    IOLog("droidvm-gpu: published (VA bits %u)\n", input_bits);
    if (bootArg("droidvm-gpu-metal", 0) == 1) {
        publishMetal();
        IOLog("droidvm-gpu: Metal accelerator stage %llu status %#llx\n", droidvm_gpu_stats[14], droidvm_gpu_stats[15]);
    }
}
}  // namespace

// The first client window starts at the VBO (NativeAdrenoPublish.cpp's windows are offsets from it, see build-droidvm-gpu.py).
extern "C" uint64_t droidvm_gpu_window_base(void) {return droidvm_gpu_facts.va_start;}

// The board's GPU hook, from the late display probe (before launchd starts the render server).
extern "C" void sheng_gpu_power_probe(void)
{
    if (!bootArg("droidvm-gpu", 0)) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(gpuThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
