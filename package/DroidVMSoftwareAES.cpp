// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// UniXNU's SoftwareAES (drivers/SoftwareAES: NativeAES, AES-CBC in software through corecrypto) on iPadOS 27, the
// IOAESAccelerator a DroidVM guest has no hardware for. AppleImage4 waits for one as it starts on the device tree's defaults node
// (its personality: IONameMatch defaults): without it that start never returned, the IOKit configuration thread stayed blocked,
// the defaults nub busy and IOKit never quiet, so configd's InterfaceNamer waited out its 5-minute quiet timeout before naming any
// new interface, and every other wait for a quiet registry ran to its end.
//
// The iPadOS 17 board links SoftwareAES with the kernel (XNU_NATIVE_AES, a kernel personality on IOResources), which requires the
// prelinked crypto bootstrap; this board's is the kernel's own (source). So NativeAES rides in the display archive like the board's
// other drivers: its class registers here, in its own OSMetaClass module load, with its parent's metaclass, constructor and methods
// bound to the original family's (SoftwareAES-24A446.S), and it starts on IOResources. The bound parent is checked first against
// the live IOAESAccelerator (the metaclass by that name, 0x120 bytes): a fileset that moved it leaves NativeAES out.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

#if !DROIDVM_NATIVE_KEXT
extern kmod_info_t g_kernel_kmod_info;
extern "C" void droidvm_late_init_NativeAES(void) __asm__("__GLOBAL__sub_I_NativeAES.cpp");
#endif
extern "C" const char droidvm_aes_parent_meta[] __asm__("__ZN16IOAESAccelerator10gMetaClassE");
extern "C" volatile uint32_t native_aes_kat_status;

extern "C" {
// For the host rig (peek.py): 1 the parent checked, 2 NativeAES registered, 3 started; 0x80 | n refused at step n.
volatile uint32_t droidvm_software_aes_stage;
int droidvm_software_aes_attach(void);
}

int droidvm_software_aes_attach(void) {
    if (droidvm_software_aes_stage) return -1;
    uint32_t enabled = 1;
    PE_parse_boot_argn("droidvm-software-aes", &enabled, sizeof enabled);
    if (!enabled) return 0;
    const OSSymbol *parent = OSSymbol::withCString("IOAESAccelerator");
    const OSMetaClass *live = parent ? OSMetaClass::getMetaClassWithName(parent) : nullptr;
    OSSafeReleaseNULL(parent);
    if (!live || live != reinterpret_cast<const OSMetaClass *>(droidvm_aes_parent_meta) || live->getClassSize() != 0x120) {
        droidvm_software_aes_stage = 0x81;
        IOLog("DroidVMSoftwareAES: the live IOAESAccelerator (%p) is not the bound one (%p): NativeAES not started\n", live,
              droidvm_aes_parent_meta);
        return -1;
    }
    droidvm_software_aes_stage = 1;

#if !DROIDVM_NATIVE_KEXT
    void *module = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!module) {
        droidvm_software_aes_stage = 0x82;
        return -1;
    }
    droidvm_late_init_NativeAES();
    if (OSMetaClass::postModLoad(module)) {
        droidvm_software_aes_stage = 0x82;
        return -1;
    }
#endif   // a kext's NativeAES is registered as it loads, after IOCryptoAcceleratorFamily (its OSBundleLibraries)
    droidvm_software_aes_stage = 2;

    IOService *resources = IOService::getResourceService();
    const OSSymbol *name = OSSymbol::withCString("NativeAES");
    OSObject *object = name ? OSMetaClass::allocClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    IOService *aes = OSDynamicCast(IOService, object);
    if (!aes || !resources || !aes->init() || !aes->attach(resources)) {
        OSSafeReleaseNULL(object);
        droidvm_software_aes_stage = 0x83;
        return -1;
    }
    // NativeAES::start runs its known-answer test and registers the service, which wakes AppleImage4's wait.
    if (!aes->start(resources)) {
        aes->detach(resources);
        aes->release();
        droidvm_software_aes_stage = 0x84;
        IOLog("DroidVMSoftwareAES: NativeAES did not start (known-answer test %u)\n", native_aes_kat_status);
        return -1;
    }
    aes->release();
    droidvm_software_aes_stage = 3;
    IOLog("DroidVMSoftwareAES: NativeAES started, known-answer test %u\n", native_aes_kat_status);
    return 0;
}
