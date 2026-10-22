// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Brings up the guest's IOKit network stack and puts the virtio-net controller on it. The transport half (DroidVMVirtioNet.c,
// the virtio-mmio virtqueues) calls in with plain C:
//   droidvm_net_if_attach(mac)            once the device is up
//   droidvm_net_if_input(frame, length)   for each received frame
// Apple's IONetworkingFamily-177 is compiled into the kernel (scripts/build-native-legacy-display.py) with its static
// initializers staged for a late call, as is DroidVMVirtioEthernet.cpp's; attach runs them in one OSMetaClass module load, then
// starts IONetworkStack the way the family's Info.plist personality does (on IOResources, once IOBSD is published; the
// transport starts after bsd_autoconf). The family's other personality, IOKDP (kernel debugging over ethernet), is not started.
//
// The controller sits on a nub of its own carrying "built-in", which IONetworkInterface turns into IOBuiltin, so configd's
// InterfaceNamer names the interface en0 and IPConfiguration configures it from the Data seed's network service.
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>

extern kmod_info_t g_kernel_kmod_info;

#define DROIDVM_LATE_INIT(file) extern "C" void droidvm_late_init_##file(void) __asm__("__GLOBAL__sub_I_" #file ".cpp");
DROIDVM_LATE_INIT(IOEthernetController)
DROIDVM_LATE_INIT(IOEthernetInterface)
DROIDVM_LATE_INIT(IOKernelDebugger)
DROIDVM_LATE_INIT(IOMbufMemoryCursor)
DROIDVM_LATE_INIT(IONetworkController)
DROIDVM_LATE_INIT(IONetworkData)
DROIDVM_LATE_INIT(IONetworkInterface)
DROIDVM_LATE_INIT(IONetworkMedium)
DROIDVM_LATE_INIT(IONetworkStack)
DROIDVM_LATE_INIT(IONetworkUserClient)
DROIDVM_LATE_INIT(IOOutputQueue)
DROIDVM_LATE_INIT(IOPacketQueue)
DROIDVM_LATE_INIT(DroidVMVirtioEthernet)

static void (*const droidvm_net_initializers[])(void) = {
    droidvm_late_init_IOEthernetController, droidvm_late_init_IOEthernetInterface, droidvm_late_init_IOKernelDebugger,
    droidvm_late_init_IOMbufMemoryCursor, droidvm_late_init_IONetworkController, droidvm_late_init_IONetworkData,
    droidvm_late_init_IONetworkInterface, droidvm_late_init_IONetworkMedium, droidvm_late_init_IONetworkStack,
    droidvm_late_init_IONetworkUserClient, droidvm_late_init_IOOutputQueue, droidvm_late_init_IOPacketQueue,
    droidvm_late_init_DroidVMVirtioEthernet,
};

extern "C" {
// For the host rig (peek.py): how far attach got, the module load's result, and the controller.
volatile uint32_t droidvm_net_if_stage;
volatile int droidvm_net_family_status = -1;
volatile uint64_t droidvm_net_controller;
IOService *droidvm_ethernet_create(const uint8_t mac[6], IOService *provider);
void droidvm_ethernet_input(IOService *controller, const void *frame, unsigned length);
int droidvm_net_if_attach(const uint8_t mac[6]);
void droidvm_net_if_input(const void *frame, unsigned length);
}

static IOService *droidvm_net_nic;

static IOService *droidvm_net_start_service(const char *class_name, IOService *provider) {
    const OSSymbol *name = OSSymbol::withCString(class_name);
    OSObject *object = name ? OSMetaClass::allocClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    IOService *service = OSDynamicCast(IOService, object);
    if (!service) {
        OSSafeReleaseNULL(object);
        return nullptr;
    }
    if (!service->init() || !service->attach(provider)) {
        service->release();
        return nullptr;
    }
    if (!service->start(provider)) {
        service->detach(provider);
        service->release();
        return nullptr;
    }
    return service;
}

int droidvm_net_if_attach(const uint8_t mac[6]) {
    IOService *resources = IOService::getResourceService();
    if (!resources) return -1;
    droidvm_net_if_stage = 1;
    void *module = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!module) return -1;
    for (auto initializer : droidvm_net_initializers) initializer();
    droidvm_net_family_status = OSMetaClass::postModLoad(module);
    if (droidvm_net_family_status) return -1;
    droidvm_net_if_stage = 2;
    if (!droidvm_net_start_service("IONetworkStack", resources)) return -1;
    droidvm_net_if_stage = 3;
    IOService *nub = OSTypeAlloc(IOService);
    if (!nub || !nub->init()) {
        OSSafeReleaseNULL(nub);
        return -1;
    }
    nub->setName("droidvm-net");
    nub->setProperty("built-in", kOSBooleanTrue);
    if (!nub->attach(resources)) {
        nub->release();
        return -1;
    }
    droidvm_net_if_stage = 4;
    droidvm_net_nic = droidvm_ethernet_create(mac, nub);
    if (!droidvm_net_nic) return -1;
    droidvm_net_controller = (uintptr_t)droidvm_net_nic;
    droidvm_net_if_stage = 5;
    return 0;
}

void droidvm_net_if_input(const void *frame, unsigned length) {
    if (droidvm_net_nic) droidvm_ethernet_input(droidvm_net_nic, frame, length);
}
