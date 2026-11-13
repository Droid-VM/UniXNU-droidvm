// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The motion sensors' command channel, as the iPad's sensor processor (SPU) offers it. CoreMotion's orientation notifier, in
// backboardd and in SpringBoard, opens the device under the device-orientation service (the orientation service's grandparent:
// DroidVMMotionHid.inc's orientation device, an AppleSPUHIDDevice as the iPad's is) through AppleSPULib
// (AppleSPU.kext/PlugIns/AppleSPULib.plugin, plug-in type 7ACF5332-1A35-4893-87CB-BA64E1887FAE): IOServiceOpen with type 'ashd',
// then IOConnectCallMethod selectors 0-6 (Reset, SetRawSPIMode, SetLVBoost, SetGrapeCS, Open, Close, SendCommand), and sends the
// sensor processor its configuration (form factor, cover handling). The guest's sensors are the virtio motion device the kernel
// reads itself (DroidVMVirtioInput.c), with nothing behind them to configure: every command is taken and has no effect, and
// SendCommand answers nothing.
//
// Without the channel the plug-in could not be made ("Unable to create plugin interface ... unsupported function"), the notifier
// kept no interface, and the configuration it sends again whenever locationd starts over dereferenced it: backboardd and
// SpringBoard crashed together (CLOrientationNotifier::sendConfiguration) each time locationd crashed or jetsam ended it.
//
// The class has the name of the iPad's own (AppleSPU.kext's, which the guest's kernelcache does not have): the platform sandbox
// policy lets a process open only the user clients it names ("System Policy: ... deny(1) iokit-open-user-client" for any other).
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>

#if !DROIDVM_NATIVE_KEXT
extern kmod_info_t g_kernel_kmod_info;
#endif

extern "C" {
volatile uint32_t droidvm_spu_opens, droidvm_spu_commands;
volatile int droidvm_spu_register_status = -1;
}

class AppleSPUHIDDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AppleSPUHIDDeviceUserClient)

public:
    bool start(IOService *provider) override {
        if (!IOUserClient::start(provider)) return false;
        droidvm_spu_opens = droidvm_spu_opens + 1;
        return true;
    }
    IOReturn clientClose() override {
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *, OSObject *,
                            void *) override {
        if (selector > 6 || !arguments) return kIOReturnUnsupported;
        droidvm_spu_commands = droidvm_spu_commands + 1;
        arguments->scalarOutputCount = 0;
        arguments->structureOutputSize = 0;
        arguments->structureOutputDescriptorSize = 0;
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(AppleSPUHIDDeviceUserClient, IOUserClient)

#if !DROIDVM_NATIVE_KEXT
#define DROIDVM_LATE_INIT(file) extern "C" void droidvm_late_init_##file(void) __asm__("__GLOBAL__sub_I_" #file ".cpp");
DROIDVM_LATE_INIT(DroidVMSensorProcessor)
#endif

// Registers the class in its own OSMetaClass module load (its static initializer is staged for this call, as the other board
// classes are), before HidInputProbe.cpp publishes the motion devices that name it.
extern "C" int droidvm_sensor_processor_register(void) {
    if (droidvm_spu_register_status >= 0) return droidvm_spu_register_status;
#if !DROIDVM_NATIVE_KEXT
    void *module = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    int status = 1;
    if (module) {
        droidvm_late_init_DroidVMSensorProcessor();
        status = OSMetaClass::postModLoad(module) ? 2 : 0;
    }
#else
    int status = 0;   // registered as the input kext loaded
#endif
    droidvm_spu_register_status = status;
    return status;
}
