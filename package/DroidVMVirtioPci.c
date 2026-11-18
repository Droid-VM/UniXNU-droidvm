// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// A virtio-pci (modern) device under the board's virtio-mmio drivers (DroidVMVirtioPci.h has the register map). The function is
// reached without IOPCIFamily's C++ interface: its configuration space through the PCI host's ECAM window (DarwinBoot's
// /arm-io/pcie, reg in arm-io's address space as every window there), its BARs at the addresses Apple's PCI configurator gave them
// (crosvm's PCI windows are identity mapped), and its MSI-X vector 0 written here to raise a free MBI SPI of /arm-io/gic-mbi through
// ArmCpu, as the virtio-mmio drivers' lines are. IOKit only finds the function (DroidVMVirtioPciNub.cpp).
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <mach/vm_param.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include "DroidVMVirtioPci.h"

extern int armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);
extern int armcpu_spi_set_enabled(uint32_t intid, bool enabled);

// Single loads and stores with a plain base register: crosvm emulates a trapped MMIO access from the syndrome, and a paired or
// post-indexed access has none (the guest would take a synchronous external abort).
static uint32_t rd32(uintptr_t a) { uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static uint16_t rd16(uintptr_t a) { uint32_t v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return (uint16_t)v; }
static uint8_t rd8(uintptr_t a) { uint32_t v; __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return (uint8_t)v; }
static void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(v), "r"(a) : "memory"); }
static void wr16(uintptr_t a, uint16_t v) { __asm__ volatile("strh %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)v), "r"(a) : "memory"); }
static void wr8(uintptr_t a, uint8_t v) { __asm__ volatile("strb %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)v), "r"(a) : "memory"); }

enum { CAP_VENDOR = 0x09, CAP_MSIX = 0x11, VIRTIO_COMMON = 1, VIRTIO_NOTIFY = 2, VIRTIO_ISR = 3, VIRTIO_DEVICE = 4 };
enum { NO_VECTOR = 0xffff };

// A window of arm-io (the node's reg, in arm-io's address space): its physical address and size.
static bool armio_window(const char *path, uint64_t *physical, uint64_t *size) {
    DTEntry armio, node;
    const void *value;
    unsigned int length;
    uint64_t ranges[3], reg[2];
    if (SecureDTLookupEntry(NULL, "/arm-io", &armio) != kSuccess ||
        SecureDTGetProperty(armio, "ranges", &value, &length) != kSuccess || length < sizeof(ranges)) return false;
    memcpy(ranges, value, sizeof(ranges));
    if (SecureDTLookupEntry(NULL, path, &node) != kSuccess ||
        SecureDTGetProperty(node, "reg", &value, &length) != kSuccess || length < sizeof(reg)) return false;
    memcpy(reg, value, sizeof(reg));
    if (reg[0] < ranges[0] || reg[1] == 0 || reg[1] > ranges[2] || reg[0] - ranges[0] > ranges[2] - reg[1]) return false;
    *physical = reg[0] - ranges[0] + ranges[1];
    *size = reg[1];
    return true;
}

// The physical address a memory BAR decodes (0 for an I/O or unassigned one).
static uint64_t bar_address(const struct droidvm_vio_pci *p, unsigned bar) {
    if (bar > 5) return 0;
    uint32_t low = rd32(p->config + 0x10 + 4 * bar);
    if (low & 1) return 0;
    uint64_t address = low & ~0xfULL;
    if (((low >> 1) & 3) == 2 && bar < 5) address |= (uint64_t)rd32(p->config + 0x14 + 4 * bar) << 32;
    return address;
}

// length bytes at offset of a BAR, mapped (the page around them).
static uintptr_t map_bar(const struct droidvm_vio_pci *p, unsigned bar, uint32_t offset, uint32_t length) {
    uint64_t base = bar_address(p, bar);
    if (!base || !length) return 0;
    uint64_t start = base + offset, page = trunc_page_64(start), end = round_page_64(start + length);
    vm_offset_t mapped = ml_io_map((vm_offset_t)page, (vm_size_t)(end - page));
    return mapped ? (uintptr_t)(mapped + (start - page)) : 0;
}

static uint8_t msix_capability(const struct droidvm_vio_pci *p) {
    uint8_t at = rd8(p->config + 0x34) & 0xfc;
    for (unsigned n = 0; at && n < 48; ++n, at = rd8(p->config + at + 1) & 0xfc)
        if (rd8(p->config + at) == CAP_MSIX) return at;
    return 0;
}

const char *droidvm_vio_pci_open(struct droidvm_vio_pci *p, uint8_t bus, uint8_t slot, uint8_t function) {
    memset(p, 0, sizeof(*p));
    p->bus = bus, p->slot = slot, p->function = function;
    uint64_t ecam, size, offset = ((uint64_t)bus << 20) | ((uint64_t)(slot & 31) << 15) | ((uint64_t)(function & 7) << 12);
    if (!armio_window("/arm-io/pcie", &ecam, &size)) return "no /arm-io/pcie";
    if (offset + 4096 > size) return "outside the ECAM window";
    p->config = (uintptr_t)ml_io_map((vm_offset_t)(ecam + offset), 4096);
    if (!p->config) return "ECAM not mapped";
    uint32_t id = rd32(p->config);
    if ((id & 0xffff) != 0x1af4 || (id >> 16) < 0x1041 || (id >> 16) > 0x107f) return "not a modern virtio device";
    p->device_type = (id >> 16) - 0x1040;
    if (!(rd16(p->config + 6) & 0x10)) return "no capabilities";
    uint8_t at = rd8(p->config + 0x34) & 0xfc;
    for (unsigned n = 0; at && n < 48; ++n, at = rd8(p->config + at + 1) & 0xfc) {
        if (rd8(p->config + at) != CAP_VENDOR || rd8(p->config + at + 2) < 16) continue;
        uint8_t type = rd8(p->config + at + 3), bar = rd8(p->config + at + 4);
        uint32_t where = rd32(p->config + at + 8), length = rd32(p->config + at + 12);
        if ((type == VIRTIO_COMMON && p->common) || (type == VIRTIO_NOTIFY && p->notify) || (type == VIRTIO_ISR && p->isr) ||
            (type == VIRTIO_DEVICE && p->device)) continue;   // the first of each kind (the spec's preferred one)
        uintptr_t mapped = (type >= VIRTIO_COMMON && type <= VIRTIO_DEVICE) ? map_bar(p, bar, where, length) : 0;
        if (type == VIRTIO_COMMON && length >= 0x38) p->common = mapped;
        else if (type == VIRTIO_NOTIFY && rd8(p->config + at + 2) >= 20) {
            p->notify = mapped;
            p->notify_multiplier = rd32(p->config + at + 16);
        } else if (type == VIRTIO_ISR) p->isr = mapped;
        else if (type == VIRTIO_DEVICE) p->device = mapped, p->device_length = length;
    }
    if (!p->common || !p->notify || !p->isr) return "virtio structures not found";
    wr16(p->config + 4, rd16(p->config + 4) | 0x6);   // memory decoding, bus mastering
    return NULL;
}

const char *droidvm_vio_pci_interrupt(struct droidvm_vio_pci *p, void (*handler)(void *), void *context) {
    uint8_t at = msix_capability(p);
    if (!at) return "no MSI-X";
    uint16_t control = rd16(p->config + at + 2);
    uint32_t table = rd32(p->config + at + 4);
    uintptr_t entry = map_bar(p, table & 7, table & ~7U, 16);
    if (!entry) return "MSI-X table not mapped";
    uint64_t frame, size;
    DTEntry node;
    const void *value;
    unsigned int length;
    uint32_t ranges[2];
    if (!armio_window("/arm-io/gic-mbi", &frame, &size) || SecureDTLookupEntry(NULL, "/arm-io/gic-mbi", &node) != kSuccess ||
        SecureDTGetProperty(node, "mbi-ranges", &value, &length) != kSuccess || length != sizeof(ranges)) return "no /arm-io/gic-mbi";
    memcpy(ranges, value, sizeof(ranges));
    // The MSI controller (DroidVMGicMsi) hands IOPCIFamily its vectors from the bottom of the range; one from the top, which ArmCpu
    // refuses when something already holds it.
    uint32_t intid = 0;
    for (uint32_t n = ranges[1]; n > 0 && !intid; --n)
        if (armcpu_spi_attach(ranges[0] + n - 1, true, handler, context, NULL, NULL) == 0) intid = ranges[0] + n - 1;
    if (!intid) return "no free MBI SPI";
    // Vector 0: the frame's MSI_SETSPI_NS (GICv2m, offset 0x40) with the INTID as data, unmasked; MSI-X on, its function mask off,
    // INTx off.
    uint64_t doorbell = frame + 0x40;
    wr32(entry, (uint32_t)doorbell);
    wr32(entry + 4, (uint32_t)(doorbell >> 32));
    wr32(entry + 8, intid);
    wr32(entry + 12, 0);
    wr16(p->config + at + 2, (uint16_t)((control | 0x8000) & ~0x4000));
    wr16(p->config + 4, rd16(p->config + 4) | 0x400);
    p->intid = intid;
    wr16(p->common + 0x10, 0);
    if (rd16(p->common + 0x10) == NO_VECTOR) return "the device took no configuration vector";
    return NULL;
}

void droidvm_vio_pci_quiet(struct droidvm_vio_pci *p) {
    if (!p->intid) return;
    uint8_t at = msix_capability(p);
    if (at) wr16(p->config + at + 2, (uint16_t)(rd16(p->config + at + 2) & ~0x8000));
    (void)armcpu_spi_set_enabled(p->intid, false);   // the line stays attached to its ArmCpu slot, masked
    p->intid = 0;
}

uint32_t droidvm_vio_pci_read(struct droidvm_vio_pci *p, unsigned offset, unsigned width) {
    if (offset >= 0x100) {
        unsigned at = offset - 0x100;
        if (!p->device || at + width > p->device_length) return 0;
        return width == 1 ? rd8(p->device + at) : width == 2 ? rd16(p->device + at) : rd32(p->device + at);
    }
    switch (offset) {
    case 0x000: return 0x74726976;   // "virt"
    case 0x004: return 2;
    case 0x008: return p->device_type;
    case 0x00c: return 0x1af4;
    case 0x010: return rd32(p->common + 0x04);
    case 0x034: return rd16(p->common + 0x18);
    case 0x044: return rd16(p->common + 0x1c);
    case 0x060: return rd8(p->isr);
    case 0x070: return rd8(p->common + 0x14);
    case 0x0fc: return rd8(p->common + 0x15);
    default: return 0;
    }
}

void droidvm_vio_pci_write(struct droidvm_vio_pci *p, unsigned offset, uint32_t value, unsigned width) {
    if (offset >= 0x100) {
        unsigned at = offset - 0x100;
        if (!p->device || at + width > p->device_length) return;
        if (width == 1) wr8(p->device + at, (uint8_t)value);
        else if (width == 2) wr16(p->device + at, (uint16_t)value);
        else wr32(p->device + at, value);
        return;
    }
    switch (offset) {
    case 0x014: wr32(p->common + 0x00, value); break;
    case 0x020: wr32(p->common + 0x0c, value); break;
    case 0x024: wr32(p->common + 0x08, value); break;
    case 0x030: p->queue = (uint16_t)value; wr16(p->common + 0x16, (uint16_t)value); break;
    case 0x038: wr16(p->common + 0x18, (uint16_t)value); break;
    case 0x044:
        if (value) {
            // the queue's interrupt: vector 0 (every interrupt of the device), or none without MSI-X
            wr16(p->common + 0x1a, p->intid ? 0 : NO_VECTOR);
            if (p->queue < DROIDVM_VIO_PCI_QUEUES) p->notify_off[p->queue] = rd16(p->common + 0x1e);
        }
        wr16(p->common + 0x1c, (uint16_t)value);
        break;
    case 0x050:
        wr16(p->notify + (uintptr_t)(value < DROIDVM_VIO_PCI_QUEUES ? p->notify_off[value] : 0) * p->notify_multiplier,
             (uint16_t)value);
        break;
    case 0x064: break;   // reading the ISR acknowledged it
    case 0x070:
        wr8(p->common + 0x14, (uint8_t)value);
        // a reset takes the device's vectors with it: the configuration's back on vector 0 at each step after it
        if (value && p->intid) wr16(p->common + 0x10, 0);
        break;
    case 0x080: wr32(p->common + 0x20, value); break;
    case 0x084: wr32(p->common + 0x24, value); break;
    case 0x090: wr32(p->common + 0x28, value); break;
    case 0x094: wr32(p->common + 0x2c, value); break;
    case 0x0a0: wr32(p->common + 0x30, value); break;
    case 0x0a4: wr32(p->common + 0x34, value); break;
    default: break;
    }
}
