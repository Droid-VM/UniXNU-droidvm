// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// DroidVMGicMsi: the interrupt controller of crosvm's GIC MBI SPIs (--mbi-spis, which the app gives every Gunyah VM), the
// interrupt parent of Apple's PCIe bridge AppleVirtualPlatformPCIE, so that the virtio-pci devices (and xHCI) crosvm puts on its
// PCI host get MSI/MSI-X. Taken from Droid-VM/macOS-DroidVM overlay/kext/DroidVMGicMsi.cpp (docs/k27-vmapple-pcie-msi-plan.md
// there), unchanged in what it does; here a kext of the board (ipados27/kexts/DroidVMGicMsi.json) whose nub DarwinBoot makes from
// the FDT (FdtAdt.c: /arm-io/gic-mbi beside /arm-io/pcie, only when the FDT has an MBI frame).
//
// Its nub is /arm-io/gic-mbi: reg is crosvm's MBI frame, a GICv2m MSI frame whose MSI_TYPER has bit 31 set, and mbi-ranges the
// first INTID and the number of SPIs. The bridge waits for the service whose AAPL,phandle is its device-interrupt-parent, then
// builds its MSI controller (AppleVirtualPlatformPCIEMSIController) on it: it looks this controller up by the service's
// InterruptControllerName, maps the service's memory msi-frame-index (the frame) and hands this controller each vector as
// (INTID, the MSI controller's IOInterruptVector) through initVector, enableVector and disableVectorHard. So the phandle is set
// only once all of that is in place, on this service (the nub was registered long before), and an interrupt calls the vector's
// handler directly (the MSI controller's handleInterrupt panics).
//
// The SPIs are ArmCpu's (edge, group 1, spread over the running CPUs; the handler runs before the EOI). A line is attached the
// first time its vector is initialized and stays attached; enable and disable mask it at the distributor. An MSI that comes while
// the line is masked stays pending in the distributor and is taken once it is unmasked.
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>

extern "C" IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback,
                                      uint64_t *router);
extern "C" IOReturn armcpu_spi_set_enabled(uint32_t intid, bool enabled);
extern "C" IOReturn armcpu_spi_distribute(uint32_t intid, uint32_t ordinal, uint32_t *cpu, uint64_t *router);

// /arm-io/pcie's device-interrupt-parent ('MBIF', DarwinBoot FdtAdt.c GIC_MBI_PHANDLE). The nub itself carries no AAPL,phandle.
static constexpr uint32_t kGicMbiPhandle = 0x4d424946;

class DroidVMGicMsi : public IOInterruptController {
    OSDeclareDefaultStructors(DroidVMGicMsi)

    struct Line {
        DroidVMGicMsi *owner;
        uint32_t intid;
        uint32_t cpuOrdinal;
        bool attached;
        IOInterruptVector *volatile vector;
    };
    uint32_t fBase = 0;
    uint32_t fCount = 0;
    uint32_t fNextCpu = 0;
    Line *fLines = nullptr;

    Line *line(IOInterruptVectorNumber number);
    static void dispatch(void *context);

public:
    bool start(IOService *provider) override;
    void free() override;
    IOReturn handleInterrupt(void *refCon, IOService *nub, int source) override;
    bool vectorCanBeShared(IOInterruptVectorNumber number, IOInterruptVector *vector) override;
    int getVectorType(IOInterruptVectorNumber number, IOInterruptVector *vector) override;
    void initVector(IOInterruptVectorNumber number, IOInterruptVector *vector) override;
    void disableVectorHard(IOInterruptVectorNumber number, IOInterruptVector *vector) override;
    void enableVector(IOInterruptVectorNumber number, IOInterruptVector *vector) override;
};
OSDefineMetaClassAndStructors(DroidVMGicMsi, IOInterruptController)

bool DroidVMGicMsi::start(IOService *provider)
{
    if (!IOInterruptController::start(provider)) {
        return false;
    }
    OSData *ranges = OSDynamicCast(OSData, provider->getProperty("mbi-ranges"));
    OSArray *memory = provider->getDeviceMemory();
    if (ranges == nullptr || ranges->getLength() != 8 || memory == nullptr || memory->getCount() == 0) {
        IOLog("DroidVMGicMsi: %s has no mbi-ranges or no frame\n", provider->getName());
        return false;
    }
    const uint32_t *range = static_cast<const uint32_t *>(ranges->getBytesNoCopy());
    fBase = range[0];
    fCount = range[1];
    if (fBase < 32 || fCount == 0 || fBase + fCount > 1020) {
        IOLog("DroidVMGicMsi: bad mbi-ranges %u+%u\n", fBase, fCount);
        return false;
    }
    fLines = IONewZero(Line, fCount);
    // The MSI controller hands registerInterrupt and enableInterrupt to this controller, whose IOInterruptController
    // code takes the vector by the nub's specifier: an INTID.
    vectors = IONewZero(IOInterruptVector, fBase + fCount);
    if (fLines == nullptr || vectors == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < fBase + fCount; i++) {
        vectors[i].interruptLock = IOLockAlloc();
        if (vectors[i].interruptLock == nullptr) {
            return false;
        }
    }
    for (uint32_t i = 0; i < fCount; i++) {
        fLines[i].owner = this;
        fLines[i].intid = fBase + i;
    }
    // The bridge maps memory msi-frame-index (0) of this service: the nub's frame.
    setDeviceMemory(memory);
    const OSSymbol *name = OSSymbol::withCString("DroidVMGicMsi");
    IOReturn result = getPlatform()->registerInterruptController(const_cast<OSSymbol *>(name), this);
    if (result != kIOReturnSuccess) {
        IOLog("DroidVMGicMsi: registerInterruptController: 0x%x\n", result);
        name->release();
        return false;
    }
    setProperty("InterruptControllerName", const_cast<OSSymbol *>(name));
    name->release();
    setProperty("AAPL,phandle", const_cast<uint32_t *>(&kGicMbiPhandle), sizeof(kGicMbiPhandle));
    IOLog("DroidVMGicMsi: INTID %u..%u\n", fBase, fBase + fCount - 1);
    registerService();
    return true;
}

void DroidVMGicMsi::free()
{
    // Attached lines keep pointers to fLines, so a controller that attached one is never freed (it is never terminated).
    if (fLines != nullptr) {
        IODelete(fLines, Line, fCount);
        fLines = nullptr;
    }
    if (vectors != nullptr) {
        for (uint32_t i = 0; i < fBase + fCount; i++) {
            if (vectors[i].interruptLock != nullptr) {
                IOLockFree(vectors[i].interruptLock);
            }
        }
        IODelete(vectors, IOInterruptVector, fBase + fCount);
        vectors = nullptr;
    }
    IOInterruptController::free();
}

DroidVMGicMsi::Line *DroidVMGicMsi::line(IOInterruptVectorNumber number)
{
    if (number < 0 || static_cast<uint32_t>(number) < fBase || static_cast<uint32_t>(number) - fBase >= fCount) {
        return nullptr;
    }
    return &fLines[static_cast<uint32_t>(number) - fBase];
}

// IOInterruptController::handleInterrupt's handling of one vector, for the vector the MSI controller registered.
void DroidVMGicMsi::dispatch(void *context)
{
    Line *line = static_cast<Line *>(context);
    IOInterruptVector *vector = line->vector;
    if (vector == nullptr) {
        armcpu_spi_set_enabled(line->intid, false);
        return;
    }
    vector->interruptActive = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!vector->interruptDisabledSoft) {
        vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
    } else {
        // Soft disabled: mask the line until enableInterrupt, which sees interruptDisabledHard and calls enableVector.
        vector->interruptDisabledHard = 1;
        armcpu_spi_set_enabled(line->intid, false);
    }
    vector->interruptActive = 0;
}

IOReturn DroidVMGicMsi::handleInterrupt(void *, IOService *, int)
{
    return kIOReturnSuccess;   // ArmCpu dispatches each line itself
}

bool DroidVMGicMsi::vectorCanBeShared(IOInterruptVectorNumber, IOInterruptVector *)
{
    return false;
}

int DroidVMGicMsi::getVectorType(IOInterruptVectorNumber, IOInterruptVector *)
{
    return kIOInterruptTypeEdge;
}

void DroidVMGicMsi::initVector(IOInterruptVectorNumber number, IOInterruptVector *vector)
{
    Line *entry = line(number);
    if (entry == nullptr) {
        IOLog("DroidVMGicMsi: vector %d is not an MBI SPI\n", static_cast<int>(number));
        return;
    }
    entry->vector = vector;
    if (!entry->attached) {
        IOReturn result = armcpu_spi_attach(entry->intid, true, &DroidVMGicMsi::dispatch, entry, nullptr, nullptr);
        if (result != kIOReturnSuccess) {
            IOLog("DroidVMGicMsi: attach INTID %u: 0x%x\n", entry->intid, result);
            return;
        }
        entry->attached = true;
        entry->cpuOrdinal = __atomic_fetch_add(&fNextCpu, 1U, __ATOMIC_RELAXED);
    }
    // armcpu_spi_attach enables the line; a vector starts hard disabled until enableVector.
    armcpu_spi_set_enabled(entry->intid, false);
    uint32_t cpu;
    uint64_t router;
    IOReturn result = armcpu_spi_distribute(entry->intid, entry->cpuOrdinal, &cpu, &router);
    if (result != kIOReturnSuccess) {
        IOLog("DroidVMGicMsi: route INTID %u: 0x%x\n", entry->intid, result);
        return;
    }
    IOLog("DroidVMGicMsi: INTID %u -> CPU %u IROUTER 0x%llx\n", entry->intid, cpu, router);
}

void DroidVMGicMsi::disableVectorHard(IOInterruptVectorNumber number, IOInterruptVector *)
{
    Line *entry = line(number);
    if (entry != nullptr && entry->attached) {
        armcpu_spi_set_enabled(entry->intid, false);
    }
}

void DroidVMGicMsi::enableVector(IOInterruptVectorNumber number, IOInterruptVector *vector)
{
    Line *entry = line(number);
    if (entry != nullptr && entry->attached) {
        entry->vector = vector;
        armcpu_spi_set_enabled(entry->intid, true);
    }
}
