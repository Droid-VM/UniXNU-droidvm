// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The board drivers' side of DroidVMVirtioPCI (DroidVMVirtioPciNub.cpp, in the DroidVMGicMsi kext; macOS): each virtio-pci
// function that service holds, those there when the driver starts and those published later, is opened through the ECAM window
// (DroidVMVirtioPci.c) and handed to the driver of its virtio type. One notification for the kext; today the input driver's
// functions (type 18).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSNumber.h>
#include "DroidVMVirtioPci.h"

extern "C" const char *droidvm_input_add_pci(struct droidvm_vio_pci *pci);

static bool published(void *, void *, IOService *service, IONotifier *)
{
    OSNumber *address = OSDynamicCast(OSNumber, service->getProperty("DroidVMPCIAddress"));
    OSNumber *type = OSDynamicCast(OSNumber, service->getProperty("DroidVMVirtioType"));
    if (address == nullptr || type == nullptr || service->getProperty("DroidVMDriver") != nullptr) {
        return true;
    }
    const uint32_t hi = address->unsigned32BitValue();
    if (type->unsigned32BitValue() != 18) {
        return true;   // no driver here for it
    }
    struct droidvm_vio_pci *pci = IONewZero(struct droidvm_vio_pci, 1);
    if (pci == nullptr) {
        return true;
    }
    const char *why = droidvm_vio_pci_open(pci, (hi >> 16) & 0xff, (hi >> 11) & 0x1f, (hi >> 8) & 7);
    if (why == nullptr && pci->device_type != 18) {
        why = "the ECAM function is not the published one";
    }
    if (why == nullptr) {
        why = droidvm_input_add_pci(pci);
    }
    IOLog("DroidVMVirtioPCI: %02x:%02x.%x to the input driver: %s (MBI INTID %u)\n", (hi >> 16) & 0xff, (hi >> 11) & 0x1f,
          (hi >> 8) & 7, why ? why : "taken", pci->intid);
    service->setProperty("DroidVMDriver", why ? why : "input");
    if (why != nullptr) {
        IODelete(pci, struct droidvm_vio_pci, 1);   // its mappings stay (a few pages); nothing points at it
    }
    // Otherwise the driver keeps the structure for good: the function is never let go.
    return true;
}

// Called once by the input driver's start (DroidVMVirtioInput.c droidvm_input_start); the notifier stays for the boot.
extern "C" void droidvm_vio_pci_watch(void)
{
    static bool watching;
    if (watching) {
        return;
    }
    watching = true;
    OSDictionary *matching = IOService::serviceMatching("DroidVMVirtioPCI");
    if (matching == nullptr) {
        return;
    }
    IONotifier *notifier = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, published, nullptr, nullptr, 0);
    matching->release();
    if (notifier == nullptr) {
        IOLog("DroidVMVirtioPCI: no notification for the held functions\n");
    }
}
