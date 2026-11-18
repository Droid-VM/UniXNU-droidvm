// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// A virtio device on crosvm's PCI host (virtio-pci, modern) under the board's virtio drivers, which were written against the
// virtio-mmio register layout: droidvm_vio_pci_read/write take a virtio-mmio register offset and width and do what that access
// means on the PCI device's common, notify, ISR and device configuration structures. A driver keeps its offsets and asks
// here only when its device is a PCI one (DroidVMVirtioPci.c; the PCI function is found by DroidVMVirtioPciNub.cpp, macOS).
//
// What the virtio-mmio offsets become:
//   0x000 magic, 0x004 version, 0x008 device id   "virt", 2, the virtio device type (PCI device id - 0x1040)
//   0x010 / 0x014 device features / select        common 0x04 / 0x00
//   0x020 / 0x024 driver features / select        common 0x0c / 0x08
//   0x030 queue select                            common 0x16, and the queue whose notify offset 0x050 uses
//   0x034 queue size max, 0x038 queue size        common 0x18 (the size the device offers until the driver sets one)
//   0x044 queue ready                             common 0x1c; the queue's MSI-X vector (0) is set before it is enabled
//   0x050 queue notify (the queue's index)        notify + queue_notify_off * multiplier
//   0x060 interrupt status, 0x064 ack             the ISR (reading clears it), nothing (the read acknowledged)
//   0x070 status                                  common 0x14
//   0x080/0x090/0x0a0 (+4) descriptor/driver/device areas   common 0x20/0x28/0x30 (+4)
//   0x0fc configuration generation                common 0x15
//   0x100 +                                       the device configuration
#ifndef DROIDVM_VIRTIO_PCI_H
#define DROIDVM_VIRTIO_PCI_H
#include <stdint.h>
#include <stdbool.h>

#define DROIDVM_VIO_PCI_QUEUES 16

struct droidvm_vio_pci {
    uintptr_t config;                        // the function's 4 KiB of ECAM
    uintptr_t common, notify, isr, device;   // the virtio structures, mapped
    uint32_t notify_multiplier, device_length;
    uint32_t device_type;                    // virtio device id (18 input, 16 gpu, ...)
    uint16_t queue;                          // the selected queue
    uint16_t notify_off[DROIDVM_VIO_PCI_QUEUES];
    uint32_t intid;                          // the MBI SPI its MSI-X vector 0 raises, 0 without
    uint8_t bus, slot, function;
};

#ifdef __cplusplus
extern "C" {
#endif
// Maps the function (bus, slot, function) of the PCI host's ECAM window (/arm-io/pcie), its virtio structures and MSI-X table, turns
// on memory decoding and bus mastering. NULL, or why not.
const char *droidvm_vio_pci_open(struct droidvm_vio_pci *p, uint8_t bus, uint8_t slot, uint8_t function);
// Points the function's MSI-X vector 0 at a free MBI SPI (from the top of /arm-io/gic-mbi's range, the MSI controller hands out the
// bottom) attached to handler, and enables MSI-X (INTx off). Every interrupt of the device -- configuration and queues -- uses
// vector 0, each queue as the driver enables it: call this before the queues are set up. NULL, or why not.
const char *droidvm_vio_pci_interrupt(struct droidvm_vio_pci *p, void (*handler)(void *), void *context);
// The interrupt off again (a device the driver could not start): its SPI masked, MSI-X off.
void droidvm_vio_pci_quiet(struct droidvm_vio_pci *p);
// A host pool a virtio-mmio window's node carries (drm-host-pool, media-host-pool), which DarwinBoot puts on /arm-io/pcie for the
// PCI devices: its guest-physical base and size. false without one.
bool droidvm_vio_pci_pool(const char *name, uint64_t *base, uint64_t *size);

// DroidVMVirtioPciWatch.cpp (C++, in each driver's kext): the functions DroidVMVirtioPCI holds (DroidVMVirtioPciNub.cpp).
// The next one of virtio type `type` (and role, when given: "data-store" for the Data disk's virtio-blk) no driver has taken yet,
// lowest PCI address first, opened (droidvm_vio_pci_open) and marked taken; NULL when there is none. When an untaken PCI
// function of that type has no holder yet, waits up to wait_ms for it.
struct droidvm_vio_pci *droidvm_vio_pci_take(uint32_t type, const char *role, uint32_t wait_ms);
// Hands each function of `type` held now or later to add (NULL: it is the driver's now; otherwise why not).
void droidvm_vio_pci_watch(uint32_t type, const char *(*add)(struct droidvm_vio_pci *));
uint32_t droidvm_vio_pci_read(struct droidvm_vio_pci *p, unsigned offset, unsigned width);
void droidvm_vio_pci_write(struct droidvm_vio_pci *p, unsigned offset, uint32_t value, unsigned width);
#ifdef __cplusplus
}
#endif
#endif
