// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// DroidVMVirtioPCI (macOS, in the DroidVMGicMsi kext, ipados27/kexts/DroidVMGicMsi.json): holds the virtio-pci functions the
// board's own drivers serve and publishes each one's place for its driver: DroidVMPCIAddress (bus << 16 | slot << 11 |
// function << 8, IOPCIFamily's IOPCIAddressSpace from the nub's reg) and DroidVMVirtioType (the virtio device id). The driver's
// kext, which starts later, finds the published services (DroidVMVirtioPciWatch.cpp) and reaches the function through the ECAM
// window (DroidVMVirtioPci.c). The GPU, input, sound and media functions are held as they come; a block device only when it is the
// Data store (DroidVMDataStore.c), whose block 0 carries the label, read here once (DroidVMRole "data-store") -- the other disks
// fail start and Apple's transport takes them, as before.
//
// Its personalities match by PCI id at probe score 20000: Apple's AppleVirtIOPCITransport matches every 0x1af4 function and its
// probe sets its score to 10000, so at 2000 these lost every function to it without a word (5566, 11-18). It sits beside the MSI
// controller, the PCIe bridge's other piece: a kext of its own did not fit the collection's __DATA (fileset-add-kexts.py: past
// the chain table).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include "DroidVMVirtioPci.h"

class DroidVMVirtioPCI : public IOService {
    OSDeclareDefaultStructors(DroidVMVirtioPCI)

public:
    bool start(IOService *provider) override;
};
OSDefineMetaClassAndStructors(DroidVMVirtioPCI, IOService)

static uint32_t number(IOService *provider, const char *key)
{
    OSData *data = OSDynamicCast(OSData, provider->getProperty(key));
    uint32_t value = 0;
    if (data != nullptr && data->getLength() >= sizeof(value)) {
        memcpy(&value, data->getBytesNoCopy(), sizeof(value));
    }
    return value;
}

// Whether a virtio-blk function is the Data store: its block 0 begins with "DroidVMDataStore" and version 1 (tools/rig
// make-data-store.sh). One polled read of sector 0 on a queue of its own, the device reset before and after.
static bool dataStore(struct droidvm_vio_pci *p)
{
    IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous), 8192, 0x0000fffffffff000ULL);
    if (buffer == nullptr) {
        return false;
    }
    uint8_t *page = static_cast<uint8_t *>(buffer->getBytesNoCopy());
    const uint64_t pa = buffer->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    bool label = false;
    auto rd = [p](unsigned offset) { return droidvm_vio_pci_read(p, offset, 4); };
    auto wr = [p](unsigned offset, uint32_t value) { droidvm_vio_pci_write(p, offset, value, 4); };
    memset(page, 0, 8192);
    wr(0x70, 0);
    for (unsigned i = 0; rd(0x70) && i < 10000; ++i) {
        IODelay(100);
    }
    wr(0x70, 1);
    wr(0x70, 3);
    wr(0x14, 1);
    const uint32_t high = rd(0x10) & 3;   // VERSION_1, and ACCESS_PLATFORM where a pseudo-unprotected crosvm offers it
    wr(0x24, 0); wr(0x20, 0); wr(0x24, 1); wr(0x20, high); wr(0x70, 11);
    if (pa != 0 && (high & 1) && (rd(0x70) & 8)) {
        wr(0x30, 0);
        if (!rd(0x44) && rd(0x34) >= 4) {
            // descriptors at 0, the driver ring at 128, the device ring at 256, the request at 512, its status at 528, data at 4096
            struct Desc { uint64_t address; uint32_t length; uint16_t flags, next; };
            Desc *desc = reinterpret_cast<Desc *>(page);
            desc[0] = {pa + 512, 16, 1, 1};
            desc[1] = {pa + 4096, 512, 1 | 2, 2};
            desc[2] = {pa + 528, 1, 2, 0};
            const uint32_t request[4] = {0, 0, 0, 0};   // VIRTIO_BLK_T_IN, sector 0
            memcpy(page + 512, request, sizeof(request));
            page[528] = 0xff;
            volatile uint16_t *avail = reinterpret_cast<volatile uint16_t *>(page + 128);
            volatile uint16_t *used = reinterpret_cast<volatile uint16_t *>(page + 256);
            avail[0] = 1;   // no interrupt: polled
            avail[2] = 0;
            wr(0x38, 4);
            wr(0x80, uint32_t(pa)); wr(0x84, uint32_t(pa >> 32));
            wr(0x90, uint32_t(pa + 128)); wr(0x94, uint32_t((pa + 128) >> 32));
            wr(0xa0, uint32_t(pa + 256)); wr(0xa4, uint32_t((pa + 256) >> 32));
            wr(0x44, 1);
            wr(0x70, 15);
            __asm__ volatile("dmb osh" ::: "memory");
            avail[1] = 1;
            __asm__ volatile("dmb osh" ::: "memory");
            wr(0x50, 0);
            for (unsigned i = 0; used[1] == 0 && i < 20000; ++i) {
                IODelay(100);   // up to 2 s
            }
            __asm__ volatile("dmb osh" ::: "memory");
            uint32_t version = 0;
            memcpy(&version, page + 4096 + 16, sizeof(version));
            label = used[1] != 0 && page[528] == 0 && memcmp(page + 4096, "DroidVMDataStore", 16) == 0 && version == 1;
        }
    }
    wr(0x70, 0);
    for (unsigned i = 0; rd(0x70) && i < 10000; ++i) {
        IODelay(100);
    }
    buffer->release();
    return label;
}

bool DroidVMVirtioPCI::start(IOService *provider)
{
    if (!IOService::start(provider)) {
        return false;
    }
    const uint32_t hi = number(provider, "reg"), vendor = number(provider, "vendor-id") & 0xffff,
                   device = number(provider, "device-id") & 0xffff;
    if (vendor != 0x1af4 || device < 0x1041 || device > 0x107f) {
        IOLog("DroidVMVirtioPCI: %s is not a modern virtio device\n", provider->getName());
        return false;
    }
    if (device == 0x1042) {
        // a disk: held only as the Data store
        struct droidvm_vio_pci pci;
        const char *why = droidvm_vio_pci_open(&pci, (hi >> 16) & 0xff, (hi >> 11) & 0x1f, (hi >> 8) & 7);
        const bool store = why == nullptr && dataStore(&pci);
        IOLog("DroidVMVirtioPCI: %02x:%02x.%x virtio-blk: %s\n", (hi >> 16) & 0xff, (hi >> 11) & 0x1f, (hi >> 8) & 7,
              why ? why : store ? "the Data store" : "not the Data store, Apple's");
        if (!store) {
            return false;
        }
        setProperty("DroidVMRole", "data-store");
    }
    setProperty("DroidVMPCIAddress", hi & 0xffff00, 32);
    setProperty("DroidVMVirtioType", device - 0x1040, 32);
    IOLog("DroidVMVirtioPCI: %02x:%02x.%x virtio type %u held for the board's driver\n", (hi >> 16) & 0xff, (hi >> 11) & 0x1f,
          (hi >> 8) & 7, device - 0x1040);
    registerService();
    return true;
}
