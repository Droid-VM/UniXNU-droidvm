// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// One physically contiguous region a virtio transport takes its rings and buffers from, allocated when the device starts, for a
// board whose kernel data has no room for them as static arrays (DROIDVM_DEVICE_DYNAMIC: the iPadOS 27 fileset, where the native
// __DATA must fit the original kernel's slot). A buffer's device address is the region's plus its offset, as ml_static_vtop
// gives it for a static one; the region is wired and never freed.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vm/vm_kern_xnu.h>

extern uint64_t kvtophys(vm_offset_t va);

struct droidvm_dma {
    vm_offset_t base;
    uint64_t pa, size;
};

static inline bool droidvm_dma_alloc(struct droidvm_dma *region, uint64_t size) {
    vm_offset_t address = 0;
    if (region->base) return true;
    size = (size + 4095) & ~(uint64_t)4095;
    if (kmem_alloc_contig(kernel_map, &address, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) !=
        KERN_SUCCESS) return false;
    region->pa = kvtophys(address);
    region->size = size;
    region->base = address;
    return true;
}

static inline uint64_t droidvm_dma_pa(const struct droidvm_dma *region, const void *address) {
    return region->pa + ((vm_offset_t)address - region->base);
}
