// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// DroidVMVirtioPCI (macOS, in the DroidVMGicMsi kext, ipados27/kexts/DroidVMGicMsi.json): holds the virtio-pci functions the
// board's own drivers serve and publishes each one's place for its driver: DroidVMPCIAddress (bus << 16 | slot << 11 |
// function << 8, IOPCIFamily's IOPCIAddressSpace from the nub's reg) and DroidVMVirtioType (the virtio device id). It does nothing
// to the function. The driver's kext, which starts later, finds the published services (DroidVMVirtioPciWatch.cpp) and reaches
// the function through the ECAM window (DroidVMVirtioPci.c).
//
// Its personalities match by PCI id at probe score 20000: Apple's AppleVirtIOPCITransport matches every 0x1af4 function and its
// probe sets its score to 10000, so at 2000 these lost every function to it without a word (5566, 11-18). It sits beside the MSI
// controller, the PCIe bridge's other piece: a kext of its own did not fit the collection's __DATA (fileset-add-kexts.py: past
// the chain table).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>

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
    setProperty("DroidVMPCIAddress", hi & 0xffff00, 32);
    setProperty("DroidVMVirtioType", device - 0x1040, 32);
    IOLog("DroidVMVirtioPCI: %02x:%02x.%x virtio type %u held for the board's driver\n", (hi >> 16) & 0xff, (hi >> 11) & 0x1f,
          (hi >> 8) & 7, device - 0x1040);
    registerService();
    return true;
}
