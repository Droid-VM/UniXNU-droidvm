// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DROIDVM_VIRTIO_GPU_H
#define DROIDVM_VIRTIO_GPU_H
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

namespace DroidVMVirtioGpu {
struct Header { uint32_t type, flags; uint64_t fence; uint32_t context; uint8_t ring, padding[3]; };
struct Entry { uint64_t address; uint32_t length, padding; };
static_assert(sizeof(Header) == 24 && sizeof(Entry) == 16, "virtio-gpu wire ABI");
enum : uint32_t { MaxEntries = 4096, MaxCommand = 131072 };
// One serialized control queue, shared by display and future 3D clients. Commands receive a
// device fence and return only after the matching used-ring entry and response are validated.
bool start();
bool ready();
uint64_t features();
IOReturn command(const void *, uint32_t, const void *, uint32_t, void *, uint32_t, uint32_t * = nullptr);
uint32_t resourceId();
IOReturn unref(uint32_t);
}
extern "C" void droidvm_virtio_gpu_display_start(void);
extern "C" bool droidvm_virtio_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t);
extern "C" void droidvm_virtio_gpu_boot_dirty(void);
#endif
