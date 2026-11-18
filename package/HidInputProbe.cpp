// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off original HID observation; a separate explicit gate starts an
// owned offline input source. No entitlement or original provider is replaced.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#if DROIDVM_INPUT_MOUSE
#include <kern/thread.h>
#endif
#if !DROIDVM_NATIVE_KEXT
extern kmod_info_t g_kernel_kmod_info;
#endif
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" IOService *droidvm_motion_hid_allocate(unsigned kind);
extern "C" bool droidvm_motion_present(void);
extern "C" IOService *native_hid_pen_allocate(void);
extern "C" bool droidvm_pen_present(void);
// The trackpad's Magic Trackpad (DroidVMTrackpadHid.inc, in the iPadOS 27 board's staged NativeHidInput.cpp only).
extern "C" IOService *droidvm_trackpad_hid_allocate(void) __attribute__((weak_import));
extern "C" bool droidvm_trackpad_present(void);
#if DROIDVM_INPUT_MOUSE
extern "C" IOService *droidvm_pointer_hid_allocate(bool absolute);   // DroidVMPointerHid.inc (macOS)
extern "C" bool droidvm_mouse_present(void);
#endif
extern "C" int droidvm_sensor_processor_register(void);
extern "C" {
extern kern_return_t OSKextLoadKextWithIdentifier(const char *);
volatile uint32_t native_hid_stage,native_hid_resource_found;
volatile int native_hid_load_status=-1;
volatile int native_hid_matching_load_status=-1;
// The Magic Trackpad's kexts loaded with their personalities: AppleTopCaseHIDEventDriver, AppleTopCaseDriverV2.
volatile int native_hid_trackpad_load_status[2]={-1,-1};
volatile uint32_t native_hid_class_sizes[5];
volatile int native_hid_input_meta_status=-1;
volatile uint64_t native_hid_input_object;
volatile uint32_t native_hid_input_events[8];
volatile int native_hid_input_results[8]={-1,-1,-1,-1,-1,-1,-1,-1};
volatile uint32_t native_hid_input_command;
char native_hid_input_registry[2048];
char native_hid_input_props[1024];
#if DROIDVM_NO_LEGACY_DISPLAY   // macOS: no iPadOS display whose frames gate the unattended Home presses below; none are sent
static const volatile uint32_t native_legacy_transfer_calls = 0;
#else
extern volatile uint32_t native_legacy_transfer_calls;
#endif
// Optional unattended Home presses for hardware, which has no external harness writing the mailbox:
// offline-hid-auto-home=<seconds after the second presented frame>, offline-hid-auto-home-count (default 1)
// and offline-hid-auto-home-interval (default 20 s). Disabled unless the first argument is set.
static unsigned autoConfigured,autoFirst,autoCount,autoInterval,autoSent;
static uint64_t autoNext,autoFrequency;
static uint64_t counterNow(void){uint64_t v;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(v));return v;}
bool native_hid_input_requested(void) {
    if(native_legacy_transfer_calls<2)return false;
    if(!autoConfigured) {
        autoConfigured=1;autoFirst=0;autoCount=1;autoInterval=20;
        PE_parse_boot_argn("offline-hid-auto-home",&autoFirst,sizeof(autoFirst));
        PE_parse_boot_argn("offline-hid-auto-home-count",&autoCount,sizeof(autoCount));
        PE_parse_boot_argn("offline-hid-auto-home-interval",&autoInterval,sizeof(autoInterval));
        if(autoCount>8)autoCount=8;if(autoInterval<5)autoInterval=5;
        __asm__ volatile("mrs %0, cntfrq_el0":"=r"(autoFrequency));
        if(autoFirst && autoFrequency)autoNext=counterNow()+(uint64_t)autoFirst*autoFrequency;
    }
    if(autoFirst && autoFrequency && autoSent<autoCount && counterNow()>=autoNext) {
        ++autoSent;autoNext+=(uint64_t)autoInterval*autoFrequency;return true;
    }
    if(native_hid_input_command!=1)return false;
    native_hid_input_command=0;return true;
}
static void describeInput(IORegistryEntry *entry,unsigned depth,size_t &used) {
    if(depth>4 || used>=sizeof(native_hid_input_registry)-128)return;
    int count=snprintf(native_hid_input_registry+used,sizeof(native_hid_input_registry)-used,
        "%u %s [%s]\n",depth,entry->getName(),entry->getMetaClass()->getClassName());
    if(count<=0 || (size_t)count>=sizeof(native_hid_input_registry)-used)return;
    used+=count;OSIterator *it=entry->getChildIterator(gIOServicePlane);
    if(!it)return;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *next=OSDynamicCast(IORegistryEntry,child);
        if(next)describeInput(next,depth+1,used);
    }
    it->release();
}
// Compact one property of the first IOHIDEventDriver below entry: XML tags dropped, key end -> '=', value end -> ' '.
static IORegistryEntry *findEventDriver(IORegistryEntry *parent,unsigned depth) {
    if(!depth)return nullptr;
    OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
    IORegistryEntry *found=nullptr;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
        if(!entry)continue;
        if(entry->metaCast("IOHIDEventDriver")){found=entry;break;}
        if((found=findEventDriver(entry,depth-1)))break;
    }
    it->release();return found;
}
static void appendProperty(IORegistryEntry *driver,const char *key,const char *label,size_t &used) {
    OSObject *value=driver->copyProperty(key);
    int n=snprintf(native_hid_input_props+used,sizeof(native_hid_input_props)-used,"%s:",label);
    if(n>0)used+=n;
    if(value) {
        OSSerialize *text=OSSerialize::withCapacity(4096);
        if(text && value->serialize(text)) {
            const char *in=text->text();bool tag=false;char name[16];unsigned nameLen=0;
            for(unsigned i=0;in && in[i] && used<sizeof(native_hid_input_props)-2;++i) {
                char c=in[i];
                if(c=='<'){tag=true;nameLen=0;continue;}
                if(tag) {
                    if(c=='>') {
                        tag=false;name[nameLen]=0;
                        if(!strcmp(name,"/key"))native_hid_input_props[used++]='=';
                        else if(name[0]=='/')native_hid_input_props[used++]=' ';
                        else if(!strcmp(name,"true/")||!strcmp(name,"false/")){
                            used+=snprintf(native_hid_input_props+used,sizeof(native_hid_input_props)-used,"%s ",name[0]=='t'?"T":"F");
                        }
                    } else if(nameLen<sizeof(name)-1)name[nameLen++]=c;
                    continue;
                }
                if(c=='\n'||c=='\t')continue;
                native_hid_input_props[used++]=c;
            }
        }
        OSSafeReleaseNULL(text);OSSafeReleaseNULL(value);
    } else if(used<sizeof(native_hid_input_props)-6) {memcpy(native_hid_input_props+used,"none ",5);used+=5;}
    native_hid_input_props[used]=0;
}
__attribute__((noinline)) void native_hid_live_checkpoint(IOService *device) {
    size_t used=0;native_hid_input_registry[0]=0;describeInput(device,0,used);
    if(IORegistryEntry *driver=findEventDriver(device,4)) {
        size_t props=0;native_hid_input_props[0]=0;
        // backboardd's digitizer service classification reads Built-In/DisplayIntegrated from the event service it
        // opened, not from the HID device below it; publish them on the service itself (idempotent, once per second).
        // Set once only, per service: every setProperty on a service with attached user clients is a registry property-change
        // notification, and repeating it each second keeps backboardd/hidd busy re-evaluating the service. (One record for
        // every Built-In device's service: with a single one the devices took turns and each re-published every second.)
        static IORegistryEntry *publishedFor[4];
        bool published=false;
        for(IORegistryEntry *entry:publishedFor)published|=entry==driver;
        if(device->getProperty("Built-In") && !published) {
            for(IORegistryEntry *&entry:publishedFor)if(!entry){entry=driver;break;}
            // The pen digitizer keeps its usage, Digitizer/Stylus: backboardd's pencil digitizer, whose hover range and
            // maximum force it reads from the service as well (MaxHoverHeight, AccurateMaxDigitizerPressureValue).
            OSNumber *own=OSDynamicCast(OSNumber,device->getProperty("PrimaryUsage"));
            const bool pen=own && own->unsigned32BitValue()==0x20 && device->getProperty("MaxHoverHeight");
            const unsigned long long usage=pen?0x20:4;
            driver->setProperty("Built-In",kOSBooleanTrue);driver->setProperty("DisplayIntegrated",kOSBooleanTrue);
            if(pen) {
                driver->setProperty("MaxHoverHeight",device->getProperty("MaxHoverHeight"));
                driver->setProperty("AccurateMaxDigitizerPressureValue",device->getProperty("AccurateMaxDigitizerPressureValue"));
            }
            // backboardd (-[BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:], decoded from the target) drops a
            // digitizer event unless the sending service has PrimaryUsagePage 0x0D and PrimaryUsage 1, 4 or 0x20
            // ("ignoring digitizer event ... from unsupported service"). The combined device lists its keyboard collection
            // first, so the services would publish (1,6); publish Digitizer/Touch Screen on the services after they have
            // matched (setting it on the device would change driver matching, which is observed to depend on the layout).
            driver->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);driver->setProperty("PrimaryUsage",usage,32);
            // backboardd's digitizer filter is {DeviceUsagePage, DeviceUsage, Built-In}; make the flag visible on the
            // interface (the event service's provider) as well, not only on the device two levels down.
            if(IORegistryEntry *interface=driver->getParentEntry(gIOServicePlane)) {
                interface->setProperty("Built-In",kOSBooleanTrue);interface->setProperty("DisplayIntegrated",kOSBooleanTrue);
                interface->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);interface->setProperty("PrimaryUsage",usage,32);
            }
        }
        // The Digitizer/DebugState dumps (appendProperty) are not read on hardware runs without a photograph and walk
        // driver-internal state from a foreign thread; only the cheap Built-In readback stays.
        appendProperty(driver,"Built-In","BI",props);
        appendProperty(driver,"PrimaryUsagePage","PP",props);appendProperty(driver,"PrimaryUsage","PU",props);
    }
    __asm__ volatile("" ::: "memory");
}
void native_hid_input_observed(unsigned event,int result) {
    if(event<8){native_hid_input_events[event]=native_hid_input_events[event]+1;native_hid_input_results[event]=result;}
}
__attribute__((noinline)) void native_hid_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
#if DROIDVM_INPUT_MOUSE
// macOS: the HID event drivers are DriverKit's (AppleUserHIDDrivers), and they can be replaced in place. kernelmanagerd, relaunched
// after jetsam killed it as idle, activates the platform dexts again; IOKit then matches a new AppleUserHIDEventDriver to an
// interface whose old one still holds it, the new one's open fails (an IOHIDInterface takes one client), the old one closes after,
// and the interface is left open by nobody: IOHIDInterface::handleReport drops every report from then on. 11-15 on 5566: keyboard
// and pointer dead from 22:12:22 on, while the motion sensors, replaced in the other order, kept working; restarting the dext process
// brought them back. Every 2 s this looks at each device published here: an interface with a live driver that does not hold it, three
// looks in a row, has that driver terminated and is matched afresh, and the new driver opens it.
// droidvm_hid_watch: [0] looks, [1] interfaces found unheld, [2] drivers terminated, [3] devices watched; droidvm_hid_watch_test (poke)
// = index + 1 closes that device's interface for its driver once, as the race leaves it, to test the repair.
extern "C" {volatile uint32_t droidvm_hid_watch[4], droidvm_hid_watch_test;}
static IOService *hidWatched[16];
static uint8_t hidStrikes[16];
static void hidWatchAdd(IOService *device) {
    if(!device || droidvm_hid_watch[3]>=16)return;
    device->retain();
    hidWatched[droidvm_hid_watch[3]]=device;
    droidvm_hid_watch[3]=droidvm_hid_watch[3]+1;
}
// The first live child of a service (of the class, when one is given), retained.
static IOService *hidLiveChild(IORegistryEntry *parent,const char *className) {
    OSIterator *it=parent->getChildIterator(gIOServicePlane);
    if(!it)return nullptr;
    IOService *found=nullptr;
    while(OSObject *object=it->getNextObject()) {
        IOService *child=OSDynamicCast(IOService,object);
        if(child && !child->isInactive() && (!className || child->metaCast(className))){found=child;found->retain();break;}
    }
    it->release();
    return found;
}
static void hidWatchMain(void *,wait_result_t) {
    for(;;) {
        IOSleep(2000);
        droidvm_hid_watch[0]=droidvm_hid_watch[0]+1;
        for(unsigned i=0;i<droidvm_hid_watch[3];++i) {
            IOService *device=hidWatched[i];
            IOService *interface=device->isInactive()?nullptr:hidLiveChild(device,"IOHIDInterface");
            IOService *driver=interface?hidLiveChild(interface,nullptr):nullptr;
            if(driver && droidvm_hid_watch_test==i+1) {
                droidvm_hid_watch_test=0;
                interface->close(driver);
            }
            const bool unheld=driver && !interface->isOpen(driver);
            hidStrikes[i]=unheld?(uint8_t)(hidStrikes[i]+1):0;
            if(hidStrikes[i]==1)droidvm_hid_watch[1]=droidvm_hid_watch[1]+1;
            if(hidStrikes[i]>=3) {
                hidStrikes[i]=0;
                IOLog("DroidVMInput: %s's interface is held by no driver; rematching it\n",device->getName());
                driver->terminate();
                interface->registerService();
                droidvm_hid_watch[2]=droidvm_hid_watch[2]+1;
            }
            OSSafeReleaseNULL(driver);OSSafeReleaseNULL(interface);
        }
    }
}
static void hidWatchStart(void) {
    thread_t thread;
    if(droidvm_hid_watch[3] && kernel_thread_start(&hidWatchMain,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
}
#define HID_WATCH(device) hidWatchAdd(device)
#else
#define HID_WATCH(device) ((void)(device))
#endif
// The HID services of the input devices a VM may or may not have, each published once when its device is there: the pen digitizer
// (Digitizer/Stylus, the staged NativeHidInput's third mode), the mouse (macOS, DroidVMPointerHid.inc; the app's mouse mode), the
// trackpad as a Magic Trackpad 2 (DroidVMTrackpadHid.inc; its event driver is AppleTopCaseHIDEventDriver's, matched by
// AppleTopCaseDriverV2's personalities, the board's prelink roots: both loaded with their personalities first, as IOHIDEventDriver
// below), and the accelerometer, the gyroscope and the device orientation as the services locationd and backboardd look for
// (DroidVMMotionHid.inc). Run by native_hid_input_probe once the keyboard's device started, and again by the virtio input driver
// for each device it is handed after the start (a virtio-pci function, DroidVMVirtioInput.c); nothing before the first.
static IOService *volatile native_hid_extras_resource;
static volatile uint32_t native_hid_extras_busy;
static bool native_hid_pen_done, native_hid_trackpad_done, native_hid_motion_done;
#if DROIDVM_INPUT_MOUSE
static bool native_hid_mouse_done;
#endif
static IOService *native_hid_extra_start(IOService *device,IOService *resource) {
    bool started=device && device->metaCast("IOHIDDevice") && device->init() && device->attach(resource) && device->start(resource);
    if(!started && device){device->detach(resource);OSSafeReleaseNULL(device);}
    return device;
}
extern "C" void native_hid_input_extras(void) {
    IOService *resource=__atomic_load_n(&native_hid_extras_resource,__ATOMIC_ACQUIRE);
    if(!resource)return;
    while(__atomic_exchange_n(&native_hid_extras_busy,1U,__ATOMIC_ACQUIRE))IOSleep(1);
    if(!native_hid_pen_done && droidvm_pen_present()) {
        native_hid_pen_done=true;
        if(IOService *pen=native_hid_extra_start(native_hid_pen_allocate(),resource))HID_WATCH(pen);
    }
#if DROIDVM_INPUT_MOUSE
    if(!native_hid_mouse_done && droidvm_mouse_present()) {
        native_hid_mouse_done=true;
        if(IOService *mouse=native_hid_extra_start(droidvm_pointer_hid_allocate(false),resource))HID_WATCH(mouse);
    }
#endif
    if(!native_hid_trackpad_done && &droidvm_trackpad_hid_allocate && droidvm_trackpad_present()) {
        native_hid_trackpad_done=true;
        native_hid_trackpad_load_status[0]=OSKext::loadKextWithIdentifier("com.apple.driver.AppleTopCaseHIDEventDriver",
            true,false,kOSKextExcludeNone,kOSKextExcludeNone,nullptr);
        native_hid_trackpad_load_status[1]=OSKext::loadKextWithIdentifier("com.apple.driver.AppleTopCaseDriverV2",
            true,false,kOSKextExcludeNone,kOSKextExcludeNone,nullptr);
        if(IOService *trackpad=native_hid_extra_start(droidvm_trackpad_hid_allocate(),resource))HID_WATCH(trackpad);
    }
    if(!native_hid_motion_done && droidvm_motion_present()) {
        native_hid_motion_done=true;
        droidvm_sensor_processor_register();
        for(unsigned kind=0;kind<3;++kind)
            if(IOService *motion=native_hid_extra_start(droidvm_motion_hid_allocate(kind),resource))HID_WATCH(motion);
    }
    __atomic_store_n(&native_hid_extras_busy,0U,__ATOMIC_RELEASE);
}

void native_hid_input_probe(void) {
    unsigned enabled=0;
    if(!PE_parse_boot_argn("offline-hid-input",&enabled,sizeof(enabled)) || enabled!=1)return;
    if(native_hid_stage)return;
    native_hid_stage=1;
    native_hid_load_status=OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if(!native_hid_load_status)
        native_hid_matching_load_status=OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",
            true,false,kOSKextExcludeNone,kOSKextExcludeNone,nullptr);
    native_hid_stage=2;
    const char *names[]={"IOHIDUserDevice","IOHIDEventDriver","IOHIDEventService","IOHIDResourceDeviceUserClient","IOHIDDevice"};
    for(unsigned i=0;i<5;++i) {
        const OSSymbol *name=OSSymbol::withCString(names[i]);
        const OSMetaClass *meta=name?OSMetaClass::getMetaClassWithName(name):nullptr;
        if(meta)native_hid_class_sizes[i]=meta->getClassSize();
        OSSafeReleaseNULL(name);
    }
    if(native_hid_load_status==0) {
        OSDictionary *match=IOService::serviceMatching("IOHIDResource");
        IOService *resource=match?IOService::waitForMatchingService(match,5000000000ULL):nullptr;
        native_hid_resource_found=resource!=nullptr;
        unsigned sendHome=0;PE_parse_boot_argn("offline-hid-home",&sendHome,sizeof(sendHome));
        if(resource && sendHome==1 && native_hid_class_sizes[4]==208) {
#if !DROIDVM_NATIVE_KEXT
            void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
            if(handle) {
                native_hid_static_init();native_hid_input_meta_status=OSMetaClass::postModLoad(handle);
#else       // the input kext (ipados27/kexts/DroidVMInput.json): NativeHidInput was registered as the kext loaded, after IOHIDFamily
            {
                native_hid_input_meta_status=0;
#endif
                if(!native_hid_input_meta_status) {
                    IOService *device=native_hid_input_allocate();
                    native_hid_input_object=(uintptr_t)device;
                    bool valid=device && device->metaCast("IOHIDDevice") && device->init();
                    native_hid_input_observed(0,valid?0:kIOReturnError);
                    bool attached=valid && device->attach(resource);
                    native_hid_input_observed(1,attached?0:kIOReturnError);
                    bool started=attached && device->start(resource);
                    if(!started) {
                        native_hid_input_observed(7,kIOReturnError);
                        if(attached)device->detach(resource);
                        OSSafeReleaseNULL(device);native_hid_input_object=0;
                    } else HID_WATCH(device);
#if DROIDVM_INPUT_MOUSE
                    // DroidVM (macOS): no digitizer. macOS makes no pointer of a touch screen, so the touch screen is an absolute pointer of
                    // its own (DroidVMPointerHid.inc), and the device above stays a plain keyboard (no sheng-touch-hid: with the touch
                    // collection it is Built-In and its service gets the Digitizer/Touch Screen usage below, for backboardd, and macOS
                    // then took no key from it).
                    if(started) {
                        IOService *touch=droidvm_pointer_hid_allocate(true);
                        bool touchStarted=touch && touch->metaCast("IOHIDDevice") && touch->init() && touch->attach(resource) && touch->start(resource);
                        if(!touchStarted && touch){touch->detach(resource);OSSafeReleaseNULL(touch);}
                        if(touchStarted)HID_WATCH(touch);
                    }
#else
                    unsigned touchHid=0;
                    unsigned split=0;
                    PE_parse_boot_argn("sheng-touch-split",&split,sizeof(split));
                    if(started && split==1 && PE_parse_boot_argn("sheng-touch-hid",&touchHid,sizeof(touchHid)) && touchHid==1) {
                        // Separate digitizer service (primary usage Digitizer/Touch Screen, Built-In).
                        IOService *touch=native_hid_touch_allocate();
                        bool touchStarted=touch && touch->metaCast("IOHIDDevice") && touch->init() && touch->attach(resource) && touch->start(resource);
                        if(!touchStarted && touch){touch->detach(resource);OSSafeReleaseNULL(touch);}
                    }
#endif
                    // DroidVM: the services of the input devices a VM may have -- pen, mouse, trackpad, motion sensors --
                    // for those found by now; native_hid_input_extras publishes the ones found later (a virtio-pci device).
                    if(started) {
                        resource->retain();
                        __atomic_store_n(&native_hid_extras_resource,resource,__ATOMIC_RELEASE);
                        native_hid_input_extras();
                    }
                    // The offline devices live for this bounded boot. Its
                    // service attachment and timer remain genuine live objects.
                }
            }
        }
        OSSafeReleaseNULL(resource);OSSafeReleaseNULL(match);
    }
#if DROIDVM_INPUT_MOUSE
    hidWatchStart();
#endif
    native_hid_stage=3;native_hid_checkpoint();
}
}
