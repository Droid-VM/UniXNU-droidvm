// SPDX-License-Identifier: BSD-2-Clause-Patent
// Scanout client of the shared virtio GPU transport. Retain and prepare each IOSurface's
// memory until RESOURCE_UNREF completes; a timeout deliberately keeps the DMA pages pinned.
#include "DroidVMVirtioGpu.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
extern "C" void native_legacy_gpu_complete(void *, int);
extern "C" void native_legacy_frame_observed(unsigned, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" uint64_t droidvm_virtgpu_submitted(void);
extern "C" bool droidvm_virtgpu_finished(uint64_t);
extern "C" {
volatile uint32_t droidvm_virtio_display_state, droidvm_virtio_display_blob, droidvm_virtio_display_last_status;
volatile uint64_t droidvm_virtio_display_frames, droidvm_virtio_display_boot_frames, droidvm_virtio_display_resources;
// Every swap waits for the GPU work submitted before it, including the first swap after reboot.
// Setting this to 0 is an unsafe diagnostic A/B override, never the normal display path.
volatile uint32_t droidvm_virtio_present_wait = 1;
volatile uint64_t droidvm_virtio_present_probes, droidvm_virtio_present_busy, droidvm_virtio_present_wait_us;
// Swaps shown although the GPU work before them had not finished in 2 s (a torn frame rather than a stopped display).
volatile uint64_t droidvm_virtio_present_late;
struct DroidVMPresentSample { uint64_t number; uint32_t fence, retired, mode, status; uint64_t wait_us, flush_us; };
DroidVMPresentSample droidvm_virtio_present_samples[64];
}
namespace {
using namespace DroidVMVirtioGpu;
struct Rect { uint32_t x, y, width, height; };
// used: when the resource was last shown (useClock), so the cache gives up the one shown longest ago.
struct Resource {
    IOMemoryDescriptor *memory;
    uint64_t stride, used;
    uint32_t width, height, id;
    bool prepared, usable;
};
// work: the virtio GPU's fenced submissions made before the swap (droidvm_virtgpu_submitted). The frame is drawn once they have
// finished.
struct Frame { IOService *display; IOMemoryDescriptor *memory; uint64_t stride; uint32_t width, height; uint64_t work; };
// The compositor's surfaces: more than it keeps in flight, since during an animation it presents from a larger set than at rest
// (8 entries were recreated on almost every frame of a page drag).
Resource resources[32], boot;
uint64_t useClock;
// The guest kernel's page (arm64 iOS): a surface's memory is whole pages of it.
constexpr uint64_t GuestPage = 16384;
// Zeroed memory that ends a surface's blob when the surface's own last page has no room left (it ends on a page boundary):
// what the host's driver asks beyond stride x height, which its copy of the visible rows never reads.
IOBufferMemoryDescriptor *padding;
uint64_t paddingAddress;
Entry entries[MaxEntries]; // worker-owned scratch, never shared with another producer
IOLock *frameLock;
Frame pending;
bool started, compositor, bootDirty;
uint32_t activeResource;
uint64_t nowUs() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000; }

template <typename T> IOReturn send(T &cmd) {
    Header reply = {}; return command(&cmd, sizeof cmd, nullptr, 0, &reply, sizeof reply);
}
bool release(Resource &r) {
    if (r.id && unref(r.id)) return false;
    if (r.prepared) r.memory->complete(kIODirectionOut);
    OSSafeReleaseNULL(r.memory); r = {}; return true;
}
// surface: the memory is a compositor surface, whose last page is all its own (the boot framebuffer's is not known to be).
IOReturn create(Resource &r, IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height, bool surface = false) {
    if (!width || width > 16384 || !height || height > 16384 || stride < uint64_t(width) * 4 ||
        stride % 4 || stride > 65536 || memory->getLength() < stride * height) return kIOReturnBadArgument;
    r.memory = memory; memory->retain(); r.stride = stride; r.width = width; r.height = height;
    IOReturn status = memory->prepare(kIODirectionOut);
    if (status) { OSSafeReleaseNULL(r.memory); return status; }
    r.prepared = true;
    uint64_t bytes = stride * height, described = bytes;
    if (droidvm_virtio_display_blob) {
        bytes = described = (bytes + 4095) & ~4095ULL;
        if (bytes > memory->getLength()) { release(r); return kIOReturnBadArgument; }
        // The host may import the blob as an image of these dimensions, and its driver can ask for more memory than stride x
        // height: Turnip lays a LINEAR 1024x1366 image out over 1368 rows, and refused the 8 KiB smaller blob, so every frame
        // took the host's CPU copy. A surface's last page is its own; offer all of it.
        if (surface) bytes = (bytes + GuestPage - 1) & ~(GuestPage - 1);
    }
    uint32_t count = 0;
    for (uint64_t offset = 0; offset < described;) {
        IOByteCount length = 0;
        uint64_t pa = memory->getPhysicalSegment(offset, &length);
        if (length > described - offset) length = described - offset;
        if (!pa || !length || length > UINT32_MAX || count == MaxEntries ||
            (droidvm_virtio_display_blob && ((pa | length) & 4095))) { release(r); return kIOReturnBadArgument; }
        entries[count++] = {pa, uint32_t(length), 0}; offset += length;
    }
    if (bytes > described) {
        // The rest of the page the last segment ends in, where the descriptor stops short of it; or, with no room there (2 of
        // the compositor's 3 framebuffers), the padding.
        Entry &last = entries[count - 1];
        const uint64_t end = last.address + last.length, pageEnd = (end + GuestPage - 1) & ~(GuestPage - 1);
        if (pageEnd - end >= bytes - described) last.length += uint32_t(bytes - described);
        else if (paddingAddress && count < MaxEntries) entries[count++] = {paddingAddress, uint32_t(bytes - described), 0};
        else bytes = described;
    }
    r.id = resourceId();
    Header reply = {};
    bool created = false;
    if (droidvm_virtio_display_blob) {
        struct { Header h; uint32_t id, memory, flags, count; uint64_t blob, size; }
            cmd = {{0x10c, 0, 0, 0, 0, {}}, r.id, 1, 8, count, 0, bytes};
        status = command(&cmd, sizeof cmd, entries, count * sizeof(Entry), &reply, sizeof reply);
        created = !status;
    } else {
        struct { Header h; uint32_t id, format, width, height; } cmd = {{0x101, 0, 0, 0, 0, {}}, r.id, 2, uint32_t(stride / 4), height};
        status = command(&cmd, sizeof cmd, nullptr, 0, &reply, sizeof reply);
        created = !status;
        if (!status) {
            struct { Header h; uint32_t id, count; } attach = {{0x106, 0, 0, 0, 0, {}}, r.id, count};
            status = command(&attach, sizeof attach, entries, count * sizeof(Entry), &reply, sizeof reply);
        }
    }
    if (status) {
        // A valid error to CREATE means the resource doesn't exist. A transport failure is
        // ambiguous, so leave both the resource id and prepared pages owned until reboot.
        if (!created && ready() && reply.type >= 0x1200) r.id = 0;
        if (ready()) release(r);
        return status;
    }
    droidvm_virtio_display_resources = droidvm_virtio_display_resources + 1;
    r.usable = true;
    return kIOReturnSuccess;
}
IOReturn present(Resource &r) {
    IOReturn status;
    if (!droidvm_virtio_display_blob) {
        struct { Header h; Rect rect; uint64_t offset; uint32_t id, padding; }
            transfer = {{0x105, 0, 0, 0, 0, {}}, {0, 0, r.width, r.height}, 0, r.id, 0};
        status = send(transfer); if (status) return status;
    }
    if (activeResource != r.id) {
        if (droidvm_virtio_display_blob) {
            struct { Header h; Rect rect; uint32_t scanout, id, width, height, format, padding, strides[4], offsets[4]; }
                scanout = {{0x10d, 0, 0, 0, 0, {}}, {0, 0, r.width, r.height}, 0, r.id, r.width, r.height, 2, 0, {uint32_t(r.stride)}, {}};
            status = send(scanout);
        } else {
            struct { Header h; Rect rect; uint32_t scanout, id; } scanout = {{0x103, 0, 0, 0, 0, {}}, {0, 0, r.width, r.height}, 0, r.id};
            status = send(scanout);
        }
        if (status) return status;
        activeResource = r.id;
    }
    struct { Header h; Rect rect; uint32_t id, padding; } flush = {{0x104, 0, 0, 0, 0, {}}, {0, 0, r.width, r.height}, r.id, 0};
    return send(flush);
}
Resource *surface(const Frame &frame) {
    for (auto &r : resources) if (r.memory == frame.memory && r.usable && r.id && r.stride == frame.stride &&
        r.width == frame.width && r.height == frame.height) { r.used = ++useClock; return &r; }
    Resource *slot = nullptr;
    for (auto &r : resources) if (!r.memory) { slot = &r; break; }
    if (!slot) {
        Resource *oldest = nullptr;
        for (auto &r : resources) if (r.id != activeResource && (!oldest || r.used < oldest->used)) oldest = &r;
        if (oldest && release(*oldest)) slot = oldest;
    }
    if (!slot) return nullptr;
    IOReturn status = create(*slot, frame.memory, frame.stride, frame.width, frame.height, true);
    droidvm_virtio_display_last_status = status;
    if (status) return nullptr;
    slot->used = ++useClock;
    return slot;
}
void worker(void *, wait_result_t) {
    PE_Video video = {};
    if (PE_current_console(&video) || !video.v_baseAddr) { droidvm_virtio_display_state = 3; return; }
    IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, (video.v_rowBytes * video.v_height + 4095) & ~4095ULL);
    IOReturn status = memory ? create(boot, memory, video.v_rowBytes, video.v_width, video.v_height) : kIOReturnNoMemory;
    OSSafeReleaseNULL(memory);
    if (status) { droidvm_virtio_display_last_status = status; droidvm_virtio_display_state = 3; return; }
    droidvm_virtio_display_state = 2;
    unsigned idle = 100;
    for (;;) {
        IOLockLock(frameLock);
        Frame frame = pending; pending = {};
        bool showBoot = (!compositor && idle >= 100) || bootDirty;
        bootDirty = false;
        IOLockUnlock(frameLock);
        if (frame.memory) {
            const uint32_t mode = droidvm_virtio_present_wait;
            const uint64_t begin = nowUs();
            bool drawn = droidvm_virtgpu_finished(frame.work);
            const bool busy = !drawn;
            while (mode && !drawn && nowUs() - begin < 2000000) {
                IOSleep(1);   // GPU work: sleep, so the vCPU goes idle
                drawn = droidvm_virtgpu_finished(frame.work);
            }
            if (!drawn) droidvm_virtio_present_late = droidvm_virtio_present_late + 1;
            const uint64_t waited = nowUs() - begin;
            uint64_t sample = droidvm_virtio_present_probes;
            droidvm_virtio_present_probes = sample + 1;
            if (busy) droidvm_virtio_present_busy = droidvm_virtio_present_busy + 1;
            droidvm_virtio_present_wait_us = droidvm_virtio_present_wait_us + waited;
            Resource *r = surface(frame);
            const uint64_t flushBegin = nowUs();
            status = r ? present(*r) : kIOReturnIOError;
            // The sample's fence is the swap's work number, and retired the same once that work was drawn.
            droidvm_virtio_present_samples[sample % 64] = {sample + 1, uint32_t(frame.work), uint32_t(drawn ? frame.work : 0), mode,
                uint32_t(status), waited, nowUs() - flushBegin};
            droidvm_virtio_display_last_status = status;
            if (!ready()) {
                // An unacknowledged display DMA is not a completed frame. Keep this swap and its
                // references; recovery must quiesce the GPU before it can retire them. CPU
                // fallback would also race the writer.
                droidvm_virtio_display_state = 3;
                IOLog("virtio-gpu: scanout stopped with outstanding DMA, status %x\n", status);
                return;
            }
            if (!status) {
                droidvm_virtio_display_frames = droidvm_virtio_display_frames + 1;
                native_legacy_frame_observed(2, 0, 0, 0, frame.width, frame.height);
            }
            // The display workloop serializes this callback with offer(), so gpuRequest has
            // been recorded before completion even if the host answered immediately.
            native_legacy_gpu_complete(frame.display, status);
            frame.memory->release(); frame.display->release();
        } else if (showBoot && ready()) {
            status = present(boot); idle = 0;
            droidvm_virtio_display_last_status = status;
            if (!status) droidvm_virtio_display_boot_frames = droidvm_virtio_display_boot_frames + 1;
        }
        IOSleep(1); ++idle;
    }
}
}
extern "C" void droidvm_virtio_gpu_display_start(void) {
    if (started) return; started = true;
    if (!DroidVMVirtioGpu::start()) return;
    DroidVMVirtioGpu::selftest3D();
    unsigned blob = 1; PE_parse_boot_argn("droidvm-virtio-gpu-blob", &blob, sizeof blob);
    droidvm_virtio_display_blob = blob && ((DroidVMVirtioGpu::features() & 0x48) == 0x48);
    frameLock = IOLockAlloc(); if (!frameLock) { droidvm_virtio_display_state = 3; return; }
    padding = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), GuestPage, ~uint64_t(4095));
    if (padding && padding->prepare() == kIOReturnSuccess) {
        bzero(padding->getBytesNoCopy(), GuestPage);
        paddingAddress = padding->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    }
    droidvm_virtio_display_state = 1;
    thread_t thread;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else droidvm_virtio_display_state = 3;
}
extern "C" bool droidvm_virtio_gpu_offer(void *display, IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height) {
    if (droidvm_virtio_display_state != 2 || !DroidVMVirtioGpu::ready()) return false;
    IOLockLock(frameLock);
    bool accepted = !pending.memory;
    if (accepted) {
        auto service = static_cast<IOService *>(display);
        memory->retain(); service->retain(); pending = {service, memory, stride, width, height, droidvm_virtgpu_submitted()};
        compositor = true;
    }
    IOLockUnlock(frameLock); return accepted;
}
extern "C" void droidvm_virtio_gpu_boot_dirty(void) {
    if (!frameLock) return;
    IOLockLock(frameLock); bootDirty = true; IOLockUnlock(frameLock);
}
