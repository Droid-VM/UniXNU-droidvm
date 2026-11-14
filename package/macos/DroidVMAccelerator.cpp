// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// macOS: the Metal accelerator DroidVMGpu.cpp publishes (DROIDVM_GPU_MACOS). On iPadOS it is Adreno740's AGXAccelerator, staged under
// the original IOGPUFamily's IOAcceleratorES (build-adreno740-accelerator.py); macOS's Metal discovery matches
// IOServiceMatching("IOAccelerator") instead -- IOGraphicsFamily's class, which the macOS collection names, so this one derives from
// it by name like any kext (ipados27/kexts/DroidVMGraphics.macos.json; fileset-add-kexts.py checks the vtable against the family's).
// IOGPU's +[IOGPUMetalDevice registerAcceleratorService:] then reads MetalPluginName and MetalPluginClassName and loads the plugin
// from /Library/GPUBundles. Opens go to NativeAdreno's user client, as on iPadOS. Named after Apple's accelerator, as there, so sandbox
// profiles that name the class accept the plugin's open. The shape follows macOS-DroidVM's DroidVMGpuKext.cpp and upstream UniXNU's
// macOS accelerator base (AdrenoMetal db8e7cd).
//
// Also the hooks NativeAdrenoPublish.cpp calls that only the iPadOS kernel provides (the display's GOP scanout hand-off, the boot
// log export).
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>

// IOGraphicsFamily's IOAccelerator: an IOService of the same size with no overrides beyond the destructors and getMetaClass
// (IOKit/graphics/IOAccelerator.h of the macOS SDK; its slots are checked against the collection's).
class IOAccelerator : public IOService {
    OSDeclareAbstractStructors(IOAccelerator)
};
static_assert(sizeof(IOAccelerator) == sizeof(IOService), "IOGraphicsFamily's IOAccelerator adds no fields");

extern "C" IOReturn native_adreno_new_user_client(IOService *, task_t, void *, UInt32, OSDictionary *, IOUserClient **, unsigned);
extern "C" void native_adreno_note_property_read(const char *);

class AGXAccelerator : public IOAccelerator {
    OSDeclareDefaultStructors(AGXAccelerator)
public:
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override;
    OSObject *copyProperty(const OSSymbol *key) const override;
    OSObject *copyProperty(const OSString *key) const override;
    OSObject *copyProperty(const char *key) const override;
};
OSDefineMetaClassAndStructors(AGXAccelerator, IOAccelerator)

IOReturn AGXAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                                       IOUserClient **handler)
{
    unsigned limit = 40;   // sheng-gpu-metal-opens: connections ever granted to the plugin (0 = unlimited)
    PE_parse_boot_argn("sheng-gpu-metal-opens", &limit, sizeof(limit));
    return native_adreno_new_user_client(this, owningTask, securityID, type, properties, handler, limit);
}

OSObject *AGXAccelerator::copyProperty(const OSSymbol *key) const
{
    native_adreno_note_property_read(key ? key->getCStringNoCopy() : nullptr);
    return IORegistryEntry::copyProperty(key);
}

OSObject *AGXAccelerator::copyProperty(const OSString *key) const
{
    native_adreno_note_property_read(key ? key->getCStringNoCopy() : nullptr);
    return IORegistryEntry::copyProperty(key);
}

OSObject *AGXAccelerator::copyProperty(const char *key) const
{
    native_adreno_note_property_read(key);
    return IORegistryEntry::copyProperty(key);
}

extern "C" {
IOService *native_adreno_accelerator_allocate(void) { return new AGXAccelerator; }
unsigned native_adreno_accelerator_instance_size(void) { return sizeof(AGXAccelerator); }
// The iPadOS display's GOP scanout hand-off (NativeAdrenoPublish.cpp's methods 0x7100 on): no such presenter on macOS, whose display
// is DroidVMDisplay.cpp's.
IOMemoryDescriptor *native_gop_handoff_memory(void) { return nullptr; }
IOReturn native_gop_handoff_call(uint32_t, task_t, const void *, size_t, void *, size_t) { return kIOReturnUnsupported; }
// The iPadOS rig's boot log export (DroidVMStubs.c there), which NativeAdrenoPublish.cpp feeds: nothing stores the pieces here.
int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) { return 0; }
}
