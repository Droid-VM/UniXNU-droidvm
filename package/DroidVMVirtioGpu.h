// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DROIDVM_VIRTIO_GPU_H
#define DROIDVM_VIRTIO_GPU_H
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

namespace DroidVMVirtioGpu {
struct Header { uint32_t type, flags; uint64_t fence; uint32_t context; uint8_t ring, padding[3]; };
struct Entry { uint64_t address; uint32_t length, padding; };
static_assert(sizeof(Header) == 24 && sizeof(Entry) == 16, "virtio-gpu wire ABI");
// MaxCommand: the longest command a slot holds, and the longest response. HugeRequest: the longest command at all, one at a time
// (DroidVMVirtioGpu.cpp hugeRequest) -- a blob over scattered pages, whose entries outgrow MaxCommand.
// MaxEntries: the most memory segments of a resource the display describes to the host -- 64 MiB in 4 KiB pages, whose physical
// pages are seldom adjacent: a 2048x2732 swap surface (21.3 MiB) took 5464 (4096 refused it, and every frame then took the CPU copy).
enum : uint32_t { MaxEntries = 16384, MaxCommand = 131072, HugeRequest = 1u << 20, FlagFence = 1, FlagRingIndex = 2 };
// One control queue shared by the display and 3D clients, with many commands in flight: the device returns a fenced command
// only when its fence signals (a 3D submission once the GPU has run it), so a waiting submission must not hold the queue.
// command() posts and waits, numbering the command on the device's global fence ring. post() returns a ticket at once and
// leaves a 3D command's fence fields (its context's ring) as the caller wrote them; poll() and wait() collect the response.
bool start();
bool ready();
uint64_t features();
IOReturn command(const void *, uint32_t, const void *, uint32_t, void *, uint32_t, uint32_t * = nullptr);
IOReturn post(const void *, uint32_t, const void *, uint32_t, uint32_t capacity, bool globalFence, uint32_t *ticket);
bool poll(uint32_t ticket, IOReturn *result, void *reply = nullptr, uint32_t capacity = 0, uint32_t *actual = nullptr);
IOReturn wait(uint32_t ticket, uint64_t timeoutUs, void *reply = nullptr, uint32_t capacity = 0, uint32_t *actual = nullptr);
uint32_t resourceId();
IOReturn unref(uint32_t);
// vgpu-selftest=1: one DRM context through the host's drm2kgsl at boot (DroidVMVirtioGpu3D.cpp).
void selftest3D();
}
extern "C" void droidvm_virtio_gpu_display_start(void);
extern "C" bool droidvm_virtio_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t);
extern "C" void droidvm_virtio_gpu_boot_dirty(void);
#endif
