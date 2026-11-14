// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The kext form of a board driver: its module start and stop, and an IOService that starts the driver once IOResources matches it.
// Built-in, the board's late probe calls each driver's attach function (build-droidvm-legacy-display.py, native_touch_probe); as a
// kext of the kernel collection (ipados27/kexts/NAME.json, scripts/build-droidvm-kexts.py, scripts/fileset-add-kexts.py) the
// driver's own sources are compiled with DROIDVM_NATIVE_KEXT=1 -- their classes are registered by OSKext as the kext loads, not by
// a staged module load -- and this starter, compiled with
//   DROIDVM_KEXT_STARTER   the starter's class name (unique per kext: the IOKitPersonalities' IOClass)
//   DROIDVM_KEXT_ATTACH    the driver's attach function, int (void) or void (void)
// calls the attach function on a thread of its own as IOResources matches it (a start that waited would hold IOKit's matching).
// The same shape as macOS-DroidVM's overlay/kext starters (DroidVMAudioStarter) and NativeKextGlue.cpp.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <mach/kmod.h>

// DROIDVM_KEXT_NO_STARTER: the module start and stop alone, for a driver whose own personalities start it (a spec without attach).
#if !DROIDVM_KEXT_NO_STARTER && (!defined(DROIDVM_KEXT_STARTER) || !defined(DROIDVM_KEXT_ATTACH))
#error DROIDVM_KEXT_STARTER and DROIDVM_KEXT_ATTACH name the starter class and the driver's attach function
#endif

extern "C" {
// OSKext::start runs the kext's C++ constructors (OSRuntimeInitializeCPP) around these; the driver itself starts through IOKit.
kern_return_t droidvm_kext_start(kmod_info_t *, void *) { return KERN_SUCCESS; }
kern_return_t droidvm_kext_stop(kmod_info_t *, void *) { return KERN_SUCCESS; }
}

#if !DROIDVM_KEXT_NO_STARTER
extern "C" {
// The driver's attach function; its return value, if it has one, is not looked at here (the driver reports its own stage).
void DROIDVM_KEXT_ATTACH(void);
}

namespace {
void attachMain(void *, wait_result_t) { DROIDVM_KEXT_ATTACH(); }
}

class DROIDVM_KEXT_STARTER : public IOService {
    OSDeclareDefaultStructors(DROIDVM_KEXT_STARTER)
public:
    bool start(IOService *provider) override {
        static bool started;
        if (started || !IOService::start(provider)) return false;
        started = true;
        thread_t thread;
        if (kernel_thread_start(attachMain, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
        return true;
    }
};
OSDefineMetaClassAndStructors(DROIDVM_KEXT_STARTER, IOService)
#endif
