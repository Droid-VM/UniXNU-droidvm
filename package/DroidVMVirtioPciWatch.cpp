// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The board drivers' side of DroidVMVirtioPCI (DroidVMVirtioPciNub.cpp, in the DroidVMGicMsi kext; macOS), compiled into each
// driver's kext: a virtio-pci function that service holds is opened through the ECAM window (DroidVMVirtioPci.c) and handed to
// the driver of its virtio type -- by droidvm_vio_pci_take for a driver that looks for its devices once as it starts (sound,
// media, GPU, the Data store), by droidvm_vio_pci_watch for one that takes them as they come (input). The service's DroidVMDriver
// property says a function is taken (or why it could not be opened), so no two drivers open one.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include "DroidVMVirtioPci.h"

namespace {

IOLock *takeLock()
{
    static IOLock *volatile lock;
    if (lock == nullptr) {
        IOLock *made = IOLockAlloc(), *none = nullptr;
        if (made != nullptr && !__atomic_compare_exchange_n(&lock, &none, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            IOLockFree(made);
        }
    }
    return lock;
}

uint32_t number(IORegistryEntry *entry, const char *key)
{
    OSNumber *value = OSDynamicCast(OSNumber, entry->getProperty(key));
    return value != nullptr ? value->unsigned32BitValue() : ~0u;
}

bool roleIs(IORegistryEntry *entry, const char *role)
{
    OSString *value = OSDynamicCast(OSString, entry->getProperty("DroidVMRole"));
    return role == nullptr ? value == nullptr : value != nullptr && value->isEqualTo(role);
}

// Opens a held function for its driver and marks it; NULL (and the reason on the service) when it cannot be opened.
struct droidvm_vio_pci *openHeld(IOService *service)
{
    const uint32_t hi = number(service, "DroidVMPCIAddress");
    struct droidvm_vio_pci *pci = IONewZero(struct droidvm_vio_pci, 1);
    if (pci == nullptr) {
        return nullptr;
    }
    const char *why = droidvm_vio_pci_open(pci, (hi >> 16) & 0xff, (hi >> 11) & 0x1f, (hi >> 8) & 7);
    if (why == nullptr && pci->device_type != number(service, "DroidVMVirtioType")) {
        why = "the ECAM function is not the published one";
    }
    service->setProperty("DroidVMDriver", why ? why : "opened");
    if (why != nullptr) {
        IOLog("DroidVMVirtioPCI: %02x:%02x.%x: %s\n", (hi >> 16) & 0xff, (hi >> 11) & 0x1f, (hi >> 8) & 7, why);
        IODelete(pci, struct droidvm_vio_pci, 1);   // its mappings stay (a few pages); nothing points at it
        return nullptr;
    }
    return pci;   // the driver keeps it for good: the function is never let go
}

// A PCI function of virtio type `type` whose matching is not done: no client yet, or DroidVMVirtioPCI still in its start (its
// properties come last, and a disk's start reads block 0 before them: a driver looking in between would find nothing held).
bool unmatchedFunction(uint32_t type)
{
    OSDictionary *matching = IOService::serviceMatching("IOPCIDevice");
    OSIterator *it = matching != nullptr ? IOService::getMatchingServices(matching) : nullptr;
    OSSafeReleaseNULL(matching);
    bool found = false;
    while (it != nullptr && !found) {
        IOService *nub = OSDynamicCast(IOService, it->getNextObject());
        if (nub == nullptr) {
            break;
        }
        OSData *vendor = OSDynamicCast(OSData, nub->getProperty("vendor-id"));
        OSData *device = OSDynamicCast(OSData, nub->getProperty("device-id"));
        if (vendor == nullptr || device == nullptr || vendor->getLength() < 2 || device->getLength() < 2 ||
            *static_cast<const uint16_t *>(vendor->getBytesNoCopy()) != 0x1af4 ||
            *static_cast<const uint16_t *>(device->getBytesNoCopy()) != 0x1040 + type) {
            continue;
        }
        OSIterator *children = nub->getChildIterator(gIOServicePlane);
        IORegistryEntry *child = children != nullptr ? OSDynamicCast(IORegistryEntry, children->getNextObject()) : nullptr;
        found = children != nullptr &&
                (child == nullptr || (child->metaCast("DroidVMVirtioPCI") != nullptr && child->getProperty("DroidVMVirtioType") == nullptr));
        OSSafeReleaseNULL(children);
    }
    OSSafeReleaseNULL(it);
    return found;
}

// Whether the PCI host has published any function yet; and whether the device tree has a PCI host at all (DarwinBoot's
// /arm-io/pcie, made only with crosvm's MBI frame).
bool anyFunction()
{
    OSDictionary *matching = IOService::serviceMatching("IOPCIDevice");
    OSIterator *it = matching != nullptr ? IOService::getMatchingServices(matching) : nullptr;
    OSSafeReleaseNULL(matching);
    const bool any = it != nullptr && it->getNextObject() != nullptr;
    OSSafeReleaseNULL(it);
    return any;
}

bool pciHost()
{
    IORegistryEntry *host = IORegistryEntry::fromPath("/arm-io/pcie", gIODTPlane);
    const bool found = host != nullptr;
    OSSafeReleaseNULL(host);
    return found;
}

struct Watch {
    uint32_t type;
    const char *(*add)(struct droidvm_vio_pci *);
};

bool published(void *target, void *, IOService *service, IONotifier *)
{
    const Watch *watch = static_cast<const Watch *>(target);
    if (number(service, "DroidVMVirtioType") != watch->type || !roleIs(service, nullptr)) {
        return true;
    }
    IOLockLock(takeLock());
    struct droidvm_vio_pci *pci = service->getProperty("DroidVMDriver") == nullptr ? openHeld(service) : nullptr;
    IOLockUnlock(takeLock());
    if (pci == nullptr) {
        return true;
    }
    const char *why = watch->add(pci);
    IOLog("DroidVMVirtioPCI: %02x:%02x.%x (virtio type %u) to its driver: %s (MBI INTID %u)\n", pci->bus, pci->slot, pci->function,
          watch->type, why ? why : "taken", pci->intid);
    service->setProperty("DroidVMDriver", why ? why : "taken");
    return true;
}

} // namespace

// A driver that starts at IOBSD finds the functions held long before; the display starts before the PCI host has enumerated, and
// waits for it: while the host has published nothing (with /arm-io/pcie in the device tree), then while a function of the type has
// no driver yet, and for a second after the host's first function, while it publishes the rest.
extern "C" struct droidvm_vio_pci *droidvm_vio_pci_take(uint32_t type, const char *role, uint32_t wait_ms)
{
    bool hostWasEmpty = false;
    uint32_t settled = 0;
    for (uint32_t waited = 0;; waited += 100, IOSleep(100)) {
        OSDictionary *matching = IOService::serviceMatching("DroidVMVirtioPCI");
        OSIterator *it = matching != nullptr ? IOService::getMatchingServices(matching) : nullptr;
        OSSafeReleaseNULL(matching);
        IOService *best = nullptr;
        uint32_t bestAddress = ~0u;
        IOLockLock(takeLock());
        while (it != nullptr) {
            IOService *service = OSDynamicCast(IOService, it->getNextObject());
            if (service == nullptr) {
                break;
            }
            const uint32_t address = number(service, "DroidVMPCIAddress");
            if (number(service, "DroidVMVirtioType") == type && roleIs(service, role) &&
                service->getProperty("DroidVMDriver") == nullptr && address < bestAddress) {
                best = service, bestAddress = address;
            }
        }
        struct droidvm_vio_pci *pci = best != nullptr ? openHeld(best) : nullptr;
        IOLockUnlock(takeLock());
        OSSafeReleaseNULL(it);
        if (pci != nullptr) {
            IOLog("DroidVMVirtioPCI: %02x:%02x.%x (virtio type %u%s%s) to its driver\n", pci->bus, pci->slot, pci->function, type,
                  role ? ", " : "", role ? role : "");
        }
        if (pci != nullptr || best != nullptr || waited >= wait_ms) {
            return pci;
        }
        if (!anyFunction()) {
            if (!pciHost()) {
                return nullptr;
            }
            hostWasEmpty = true;
            continue;
        }
        if (unmatchedFunction(type)) {
            continue;
        }
        if (!hostWasEmpty || (settled += 100) > 1000) {
            if (hostWasEmpty) {
                IOLog("DroidVMVirtioPCI: no virtio type %u on the PCI host (waited %u ms)\n", type, waited);
            }
            return nullptr;
        }
    }
}

// The notifier stays for the boot (one per driver that asks).
extern "C" void droidvm_vio_pci_watch(uint32_t type, const char *(*add)(struct droidvm_vio_pci *))
{
    Watch *watch = IONewZero(Watch, 1);
    OSDictionary *matching = IOService::serviceMatching("DroidVMVirtioPCI");
    if (watch == nullptr || matching == nullptr) {
        OSSafeReleaseNULL(matching);
        return;
    }
    watch->type = type;
    watch->add = add;
    IONotifier *notifier = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, published, watch, nullptr, 0);
    matching->release();
    if (notifier == nullptr) {
        IOLog("DroidVMVirtioPCI: no notification for the held functions of virtio type %u\n", type);
    }
}
