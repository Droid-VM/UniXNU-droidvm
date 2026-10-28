// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The guest's battery: crosvm's goldfish battery (a generic virtual device, --battery type=goldfish) published as an
// IOPMPowerSource, the class powerd matches (IOServiceMatching("IOPMPowerSource")). IOPMPowerSource is open XNU IOKit, so this
// subclasses it directly -- there is no closed family to stand in for, unlike the audio device, and powerd's match finds a
// subclass of IOPMPowerSource wherever it sits. The host fills the goldfish registers from /sys/class/power_supply (crosvm's
// sysfs power backend); this driver reads them and republishes through IOPMPowerSource, which UIDevice, BatteryCenter and
// Settings > Battery read via powerd.
//
// make-droidvm-adt.py emits /arm-io/battery (compatible "google,goldfish-battery") at the MMIO window crosvm allocated for
// --battery. The register map is crosvm devices/src/bat.rs; the guest must write a nonzero INT_ENABLE before the host services
// the device (bat.rs gates its monitor on that), after which the registers reflect live /sys data.
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSMetaClass.h>
#include <machine/machine_routines.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <mach/kmod.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern "C" void IOSleep(unsigned milliseconds);
extern kmod_info_t g_kernel_kmod_info;

extern "C" {
int droidvm_battery_attach(void);
// For the host rig (peek.py): how far attach got and the last sampled values.
volatile uint32_t droidvm_battery_stage, droidvm_battery_updates;
volatile uint32_t droidvm_battery_percent, droidvm_battery_status, droidvm_battery_ac, droidvm_battery_present;
}

namespace {
// crosvm devices/src/bat.rs register offsets and status values.
enum : uint32_t {
    INT_ENABLE = 0x4, AC_ONLINE = 0x8, STATUS = 0xC, PRESENT = 0x14, CAPACITY = 0x18,
    VOLTAGE = 0x1C, CURRENT_NOW = 0x30,
    INT_MASK = 0x3,                       // BATTERY_STATUS_CHANGED | AC_STATUS_CHANGED
    STATUS_CHARGING = 1,
    PollMs = 2000,
};
const OSSymbol *gFullyCharged, *gRawExternal;
}

class DroidVMBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(DroidVMBattery)
    typedef IOPMPowerSource super;
    uintptr_t regs;
    thread_t poller;
    bool stop;
public:
    bool startAt(uintptr_t registers);
    void free() override;
    uint32_t rd(uint32_t off) { uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(regs + off) : "memory"); return v; }
    void wr(uint32_t off, uint32_t v) { __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(v), "r"(regs + off) : "memory"); }
    void sample();
    static void pollMain(void *, wait_result_t);
    void run();
};

OSDefineMetaClassAndStructors(DroidVMBattery, IOPMPowerSource)

bool DroidVMBattery::startAt(uintptr_t registers) {
    // init() is called by the caller before attach(); here the device is already a live registry
    // entry, so just bring the hardware up and take the first sample.
    regs = registers;
    // Give the host a nonzero interrupt-enable so it starts servicing updates (crosvm gates its
    // monitor and control commands on the guest enabling the device, else the VM control loop
    // blocks waiting for a reply).
    wr(INT_ENABLE, INT_MASK);
    sample();
    return true;
}

void DroidVMBattery::free() {
    OSSafeReleaseNULL(gFullyCharged);   // harmless if another path freed them; they are per-driver
    OSSafeReleaseNULL(gRawExternal);
    super::free();
}

// Read the goldfish registers and republish. The goldfish gives percent directly, microvolts and
// (signed) microamps, and a status of 0 unknown / 1 charging / 2 discharging / 3 not-charging; the
// host maps a full battery to not-charging, so "fully charged" is derived from being on external
// power at 100% and not charging.
void DroidVMBattery::sample() {
    const uint32_t present = rd(PRESENT), ac = rd(AC_ONLINE), st = rd(STATUS), cap = rd(CAPACITY);
    const uint32_t volt = rd(VOLTAGE), cur = rd(CURRENT_NOW);
    const bool charging = st == STATUS_CHARGING;
    const bool full = ac && !charging && cap >= 100;
    const uint32_t percent = cap > 100 ? 100 : cap;
    setBatteryInstalled(present != 0);
    setExternalConnected(ac != 0);
    setExternalChargeCapable(ac != 0);
    setIsCharging(charging);
    setMaxCapacity(100);
    setCurrentCapacity(percent);
    setVoltage(volt / 1000);                                    // microvolts -> millivolts
    setAmperage((int)((int32_t)cur / 1000));                    // microamps -> milliamps (signed)
    setAtWarnLevel(!ac && percent <= 10);
    setAtCriticalLevel(!ac && percent <= 2);
    if (gFullyCharged) setPSProperty(gFullyCharged, full ? kOSBooleanTrue : kOSBooleanFalse);
    if (gRawExternal) setPSProperty(gRawExternal, ac ? kOSBooleanTrue : kOSBooleanFalse);
    updateStatus();
    droidvm_battery_present = present; droidvm_battery_ac = ac;
    droidvm_battery_status = st; droidvm_battery_percent = percent;
    droidvm_battery_updates = droidvm_battery_updates + 1;
}

void DroidVMBattery::pollMain(void *self, wait_result_t) { static_cast<DroidVMBattery *>(self)->run(); }

void DroidVMBattery::run() {
    retain();
    while (!stop) {
        IOSleep(PollMs);
        if (stop) break;
        sample();
    }
    release();
}

#define DROIDVM_LATE_INIT(file) extern "C" void droidvm_late_init_##file(void) __asm__("__GLOBAL__sub_I_" #file ".cpp");
DROIDVM_LATE_INIT(DroidVMBattery)

// Registers the class in its own OSMetaClass module load (its static initializer is staged for this call, as the other board
// classes are), finds /arm-io/battery, maps the goldfish MMIO and publishes the IOPMPowerSource on a built-in nub.
int droidvm_battery_attach(void) {
    if (droidvm_battery_stage) return -1;
    uint32_t enabled = 1;
    PE_parse_boot_argn("droidvm-battery", &enabled, sizeof(enabled));
    if (!enabled) return 0;
    droidvm_battery_stage = 1;
    DTEntry armio, node; const void *value; unsigned size; uint64_t ranges[3], reg[2];
    if (SecureDTLookupEntry(nullptr, "/arm-io", &armio) != kSuccess ||
        SecureDTGetProperty(armio, "ranges", &value, &size) != kSuccess || size < sizeof ranges ||
        SecureDTLookupEntry(nullptr, "/arm-io/battery", &node) != kSuccess) return -1;
    memcpy(ranges, value, sizeof ranges);
    droidvm_battery_stage = 2;
    if (SecureDTGetProperty(node, "reg", &value, &size) != kSuccess || size != sizeof reg) return -1;
    memcpy(reg, value, sizeof reg);
    if (reg[0] < ranges[0] || reg[1] < 0x44 || reg[1] > ranges[2] || reg[0] - ranges[0] > ranges[2] - reg[1]) return -1;
    uintptr_t regs = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
    if (!regs) return -1;

    void *module = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!module) return -1;
    droidvm_late_init_DroidVMBattery();
    if (OSMetaClass::postModLoad(module)) return -1;
    droidvm_battery_stage = 3;

    gFullyCharged = OSSymbol::withCString(kIOPMFullyChargedKey);
    gRawExternal = OSSymbol::withCString("AppleRawExternalConnected");

    IOService *resources = IOService::getResourceService();
    IOService *nub = resources ? OSTypeAlloc(IOService) : nullptr;
    if (!nub || !nub->init()) { OSSafeReleaseNULL(nub); return -1; }
    nub->setName("droidvm-battery");
    nub->setProperty("built-in", kOSBooleanTrue);
    if (!nub->attach(resources)) { nub->release(); return -1; }
    nub->registerService();

    DroidVMBattery *battery = OSTypeAlloc(DroidVMBattery);
    if (!battery || !battery->init()) { OSSafeReleaseNULL(battery); return -1; }
    if (!battery->attach(nub)) { battery->release(); return -1; }
    if (!battery->startAt(regs)) { battery->detach(nub); battery->release(); return -1; }
    battery->registerService();

    thread_t thread;
    if (kernel_thread_start(&DroidVMBattery::pollMain, battery, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
    droidvm_battery_stage = 4;
    IOLog("DroidVMBattery: IOPMPowerSource published (percent %u, ac %u)\n", droidvm_battery_percent, droidvm_battery_ac);
    return 0;
}
