// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The guest's speaker and microphone as the built-in audio devices iOS expects: IOAudio2Devices, the class CoreAudio's HAL
// matches (HALS_IOA2Manager, in audiomxd: IOServiceMatching("IOAudio2Device") + IOObjectConformsTo), opened through an
// IOAudio2DeviceUserClient, the user client audiomxd is entitled to (and BuiltinAudioPlugin lists). Everything above is stock:
// the HAL's HALS_IOA2Device/Engine, VirtualAudio.plugin (routes, ports, volume), AVAudioSession.
//
// VirtualAudio's DeviceFactory_Aspen picks the physical-device class by the device UID string, with a single-transducer class
// per kind. We publish TWO devices: UID "Speaker" -> Device_Speaker_Aspen (a routable speaker output port, loads the J120
// speaker DSP chain) and UID "Mic" -> Device_DigitalMic_Aspen (a routable built-in-mic input port). The default VirtualAudio
// device aggregates the two as a duplex route. UID "Codec" (the whole built-in codec) was the wrong target: it needs a
// transducer/acoustic DB and produced no port.
//
// The original IOAudio2Family is in the prelinked kernel (a dependency of IOAVFamily), so its two class names are taken, and its
// subclass contract is closed. These classes have their own names and implement the whole user-client ABI themselves; at
// attach their metaclasses are linked under the family's (DroidVMAudioDevice -> IOAudio2Device, DroidVMAudioUserClient ->
// IOAudio2DeviceUserClient), which is what IOService matching and IOObjectConformsTo test. No family code runs on these objects.
//
// The ABI, read off 21H461's AudioServerDriverTransports_IOA2 (ASDT::IOA2UserClient) and CoreAudio (HALS_IOA2UCDevice):
//   registry   the description, as dictionary keys with spaces ("device UID", "output streams"/"input streams", ...); sample
//              rates are Fixed64 (32.32) SInt64 numbers
//   open       IOServiceOpen type 0; IOConnectSetNotificationPort type 0 (IOAudio2Notification)
//   memory     type 0: the IOAudio2EngineStatus (>= 0x28 bytes: +0 zero sample time, +8 its host time, +0x10 seed; read twice
//              until equal; +0x20 the HAL's write end). type 0x10000000 + stream ID: the stream's ring buffer
//   selectors  0 StartIO, 1 StopIO, 2 SetControlValue, 3 PerformConfigChange, 4 SetNominalSampleRate(Fixed64),
//              5 SetStreamCurrentFormat(id + 40-byte ASBD), 6 SetStreamActive, 10 StartIOWithFlags, 11 StopIOWithFlags,
//              12 SetClientDescription; the HAL also writes "exclusive access owner" (hog mode) through setProperties.
//   I/O        no trap: the HAL reads/writes the ring at the zero-timestamp position. The per-device engine moves one period at
//              a time between its ring and the board's PCM transport (DroidVMVirtioSnd.c): the speaker device plays, the mic
//              device captures.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/kmod.h>
#include <libkern/OSKextLib.h>

extern kmod_info_t g_kernel_kmod_info;

extern "C" {
// The PCM transport (DroidVMVirtioSnd.c). Without a virtio-snd device it takes every output period and gives no input, so the
// engines play into silence and record silence. play/capture move one period of interleaved S16LE frames and return how many
// frames the device took or gave; running(output,input) starts/stops each direction's stream (change-detected inside).
uint32_t droidvm_snd_play(const int16_t *frames, uint32_t count);
uint32_t droidvm_snd_capture(int16_t *frames, uint32_t count);
void droidvm_snd_running(bool output, bool input);
int droidvm_audio_attach(void);
// For the host rig (peek.py): how far attach got, the engines' state and their counters.
volatile uint32_t droidvm_audio_stage, droidvm_audio_out_running, droidvm_audio_in_running, droidvm_audio_clients;
volatile uint64_t droidvm_audio_periods, droidvm_audio_late, droidvm_audio_played, droidvm_audio_captured;
volatile uint64_t droidvm_audio_last_selector, droidvm_audio_selectors, droidvm_audio_bad_selectors, droidvm_audio_maps;
volatile int droidvm_audio_family_status = -1, droidvm_audio_hog_owner = -1;
}

namespace {
enum : uint32_t {
    Rate = 48000, RingFrames = 8192, PeriodFrames = 240,   // 5 ms
    OutputChannels = 2, InputChannels = 1, StreamID = 1,
    SafetyOffset = 2 * PeriodFrames, Latency = PeriodFrames,
    TransportBuiltIn = 0x626c746e,                        // 'bltn'
    FormatLinearPCM = 0x6c70636d,                         // 'lpcm'
    FlagsS16 = 0x4 | 0x8,                                 // signed integer, packed (little endian)
    TerminalSpeaker = 0x73706b72, TerminalMicrophone = 0x6d696372,   // 'spkr', 'micr'
    StatusType = 0, StreamMemory = 0x10000000,
};
struct EngineStatus { volatile uint64_t sampleTime, hostTime, seed, reserved, writeEnd; };
static_assert(sizeof(EngineStatus) >= 0x28, "the HAL refuses a smaller engine status");
constexpr int64_t fixed64(uint32_t hz) { return int64_t(hz) << 32; }
// Wired kernel memory a client maps: the engine status and the ring.
const IOOptionBits Shared = IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryKernelUserShared);

// The two devices share one PCM transport, which starts/stops each direction independently. Keep a combined view so either
// device starting or stopping tells the transport the right {output, input} pair.
IOLock *gTransportLock;
bool gOutRunning, gInRunning;
void transportSet(bool output, bool running) {
    IOLockLock(gTransportLock);
    if (output) gOutRunning = running; else gInRunning = running;
    droidvm_audio_out_running = gOutRunning; droidvm_audio_in_running = gInRunning;
    droidvm_snd_running(gOutRunning, gInRunning);
    IOLockUnlock(gTransportLock);
}
}

class DroidVMAudioDevice : public IOService {
    OSDeclareDefaultStructors(DroidVMAudioDevice)
    typedef IOService super;
public:
    bool isInput;
    const char *uid;
    IOBufferMemoryDescriptor *statusMemory, *ring;
    IOLock *lock;
    thread_t engine;
    bool running, stopping;
    uint64_t startTime;
    uint32_t seed;
    int32_t hogOwner = -1;

    bool initRole(bool input, const char *deviceUID);
    void free() override;
    bool start(IOService *provider) override;
    EngineStatus *status() { return static_cast<EngineStatus *>(statusMemory->getBytesNoCopy()); }
    uint32_t channels() const { return isInput ? InputChannels : OutputChannels; }
    IOMemoryDescriptor *memory(uint32_t type);
    IOReturn startIO();
    IOReturn stopIO();
    void publish(IORegistryEntry *entry);
    IOReturn setProperties(OSObject *properties) override;
    IOReturn apply(OSObject *properties, IORegistryEntry *client);
    static void engineMain(void *, wait_result_t);
    void run();
};

class DroidVMAudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(DroidVMAudioUserClient)
    typedef IOUserClient super;
    DroidVMAudioDevice *device;
    bool startedIO;
public:
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void stop(IOService *provider) override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
    IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) override;
    IOReturn setProperties(OSObject *properties) override;
};

OSDefineMetaClassAndStructors(DroidVMAudioDevice, IOService)
OSDefineMetaClassAndStructors(DroidVMAudioUserClient, IOUserClient)

namespace {
void setNumber(OSDictionary *d, const char *key, uint64_t value, unsigned bits = 32) {
    if (OSNumber *n = OSNumber::withNumber(value, bits)) { d->setObject(key, n); n->release(); }
}
void setNumber(IORegistryEntry *e, const char *key, uint64_t value, unsigned bits = 32) {
    if (OSNumber *n = OSNumber::withNumber(value, bits)) { e->setProperty(key, n); n->release(); }
}
OSDictionary *format(uint32_t channels) {
    OSDictionary *f = OSDictionary::withCapacity(8);
    if (!f) return nullptr;
    setNumber(f, "sample rate", uint64_t(fixed64(Rate)), 64);
    setNumber(f, "format ID", FormatLinearPCM);
    setNumber(f, "format flags", FlagsS16);
    setNumber(f, "bytes per packet", 2 * channels);
    setNumber(f, "frames per packet", 1);
    setNumber(f, "bytes per frame", 2 * channels);
    setNumber(f, "channels per frame", channels);
    setNumber(f, "bits per channel", 16);
    return f;
}
OSArray *streams(uint32_t id, uint32_t channels, uint32_t terminal) {
    OSDictionary *stream = OSDictionary::withCapacity(8), *current = format(channels), *range = format(channels);
    OSArray *available = OSArray::withCapacity(1), *list = OSArray::withCapacity(1);
    if (stream && current && range && available && list) {
        setNumber(range, "min sample rate", uint64_t(fixed64(Rate)), 64);
        setNumber(range, "max sample rate", uint64_t(fixed64(Rate)), 64);
        available->setObject(range);
        setNumber(stream, "stream ID", id);
        setNumber(stream, "starting channel", 1);
        setNumber(stream, "latency", 0);
        setNumber(stream, "terminal type", terminal);
        stream->setObject("current format", current);
        stream->setObject("available formats", available);
        list->setObject(stream);
    }
    OSSafeReleaseNULL(stream); OSSafeReleaseNULL(current); OSSafeReleaseNULL(range); OSSafeReleaseNULL(available);
    return list;
}
uint64_t absolute(uint64_t frames) { uint64_t t; nanoseconds_to_absolutetime(frames * 1000000000ULL / Rate, &t); return t; }
}

bool DroidVMAudioDevice::initRole(bool input, const char *deviceUID) {
    if (!super::init(nullptr)) return false;
    isInput = input;
    uid = deviceUID;
    lock = IOLockAlloc();
    statusMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, Shared, page_size, page_size);
    ring = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, Shared, RingFrames * 2 * channels(), page_size);
    if (!lock || !statusMemory || !ring) return false;
    bzero(statusMemory->getBytesNoCopy(), page_size);
    bzero(ring->getBytesNoCopy(), ring->getLength());
    return true;
}

void DroidVMAudioDevice::free() {
    OSSafeReleaseNULL(statusMemory); OSSafeReleaseNULL(ring);
    if (lock) IOLockFree(lock);
    lock = nullptr;
    super::free();
}

// The description the HAL caches (HALB_UCObject::CacheProperties) and VirtualAudio classifies the device by. Published on the
// device and copied onto each user client, since the HAL reads some keys through the service and some through the connection.
void DroidVMAudioDevice::publish(IORegistryEntry *e) {
    e->setProperty("device UID", uid);
    e->setProperty("device name", uid);
    e->setProperty("device manufacturer", "Apple Inc.");
    e->setProperty("model UID", uid);
    setNumber(e, "transport type", TransportBuiltIn);
    setNumber(e, "clock domain", 1);   // the HAL warns that a built-in device in clock domain 0 is unusual
    setNumber(e, "output latency", Latency);
    setNumber(e, "input latency", Latency);
    setNumber(e, "output safety offset", SafetyOffset);
    setNumber(e, "input safety offset", SafetyOffset);
    setNumber(e, "io buffer frame size", RingFrames);
    setNumber(e, "sample rate", uint64_t(fixed64(Rate)), 64);
    setNumber(e, "exclusive access owner", uint32_t(hogOwner));
    e->setProperty("is running", running ? kOSBooleanTrue : kOSBooleanFalse);
    e->setProperty("is private", kOSBooleanFalse);
    e->setProperty("supports prewarming", kOSBooleanFalse);
    e->setProperty("supports client description", kOSBooleanFalse);
    e->setProperty("wants output trap", kOSBooleanFalse);
    e->setProperty("wants input trap", kOSBooleanFalse);
    // One direction each: the speaker device has an output stream, the mic device an input stream.
    const char *key = isInput ? "input streams" : "output streams";
    if (OSArray *a = streams(StreamID, channels(), isInput ? TerminalMicrophone : TerminalSpeaker)) { e->setProperty(key, a); a->release(); }
    if (OSArray *a = OSArray::withCapacity(1)) { e->setProperty("controls", a); a->release(); }
}

bool DroidVMAudioDevice::start(IOService *provider) {
    if (!super::start(provider)) return false;
    publish(this);
    setProperty("IOUserClientClass", "DroidVMAudioUserClient");
    registerService();
    return true;
}

// The one property the HAL writes (HALB_UCObject::SetProperty, through the connection): "exclusive access owner", the pid holding
// hog mode, or -1. VirtualAudio takes hog mode on the device before it builds its ports, and gives up ("Unable to take hog
// mode") if it is refused. Other keys are accepted and ignored.
IOReturn DroidVMAudioDevice::apply(OSObject *properties, IORegistryEntry *client) {
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    if (!d) return kIOReturnBadArgument;
    if (OSNumber *owner = OSDynamicCast(OSNumber, d->getObject("exclusive access owner"))) {
        IOLockLock(lock);
        hogOwner = int32_t(owner->unsigned32BitValue());
        IOLockUnlock(lock);
        setNumber(this, "exclusive access owner", uint32_t(hogOwner));
        if (client) setNumber(client, "exclusive access owner", uint32_t(hogOwner));
        droidvm_audio_hog_owner = hogOwner;
    }
    return kIOReturnSuccess;
}

IOReturn DroidVMAudioDevice::setProperties(OSObject *properties) { return apply(properties, nullptr); }

IOMemoryDescriptor *DroidVMAudioDevice::memory(uint32_t type) {
    if (type == StatusType) return statusMemory;
    if (type == StreamMemory + StreamID) return ring;
    return nullptr;
}

IOReturn DroidVMAudioDevice::startIO() {
    IOLockLock(lock);
    if (!running) {
        while (stopping) IOLockSleep(lock, &stopping, THREAD_UNINT);
        bzero(ring->getBytesNoCopy(), ring->getLength());
        EngineStatus *s = status();
        startTime = mach_absolute_time();
        s->sampleTime = 0; s->hostTime = startTime; s->seed = ++seed; s->writeEnd = 0;
        __asm__ volatile("dmb ish" ::: "memory");
        running = true;
        setProperty("is running", kOSBooleanTrue);
        transportSet(!isInput, true);
        if (kernel_thread_start(engineMain, this, &engine) == KERN_SUCCESS) thread_deallocate(engine);
        else running = false;
    }
    IOLockUnlock(lock);
    return running ? kIOReturnSuccess : kIOReturnNoResources;
}

IOReturn DroidVMAudioDevice::stopIO() {
    IOLockLock(lock);
    if (running) {
        running = false; stopping = true;
        while (stopping) IOLockSleep(lock, &stopping, THREAD_UNINT);   // the engine thread clears it as it leaves
        setProperty("is running", kOSBooleanFalse);
        transportSet(!isInput, false);
    }
    IOLockUnlock(lock);
    return kIOReturnSuccess;
}

void DroidVMAudioDevice::engineMain(void *self, wait_result_t) { static_cast<DroidVMAudioDevice *>(self)->run(); }

// One period per pass, on the device's own clock: period n ends at startTime + n periods. The speaker device hands the period
// the hardware position just passed to the transport, then erases the ring so a client that stops writing plays silence. The
// mic device fills the period the HAL is about to read (safety-offset behind) with the transport's frames, or silence. The zero
// time stamp moves at each wrap of the ring, as IOAudioFamily's engines do.
void DroidVMAudioDevice::run() {
    retain();
    int16_t *buffer = static_cast<int16_t *>(ring->getBytesNoCopy());
    const uint32_t ch = channels();
    uint64_t done = 0;
    for (;;) {
        IOLockLock(lock);
        const bool live = running;
        IOLockUnlock(lock);
        if (!live) break;
        const uint64_t deadline = startTime + absolute(done + PeriodFrames);
        if (mach_absolute_time() > deadline + absolute(PeriodFrames)) droidvm_audio_late = droidvm_audio_late + 1;
        else clock_delay_until(deadline);
        int16_t *at = buffer + uint32_t(done % RingFrames) * ch;
        if (isInput) {
            const uint32_t captured = droidvm_snd_capture(at, PeriodFrames);
            if (captured < PeriodFrames) bzero(at + captured * ch, (PeriodFrames - captured) * 2 * ch);
            droidvm_audio_captured = droidvm_audio_captured + captured;
        } else {
            const uint32_t played = droidvm_snd_play(at, PeriodFrames);
            bzero(at, PeriodFrames * 2 * ch);
            droidvm_audio_played = droidvm_audio_played + played;
        }
        done += PeriodFrames;
        if (done % RingFrames == 0) {
            EngineStatus *s = status();
            s->hostTime = startTime + absolute(done);
            __asm__ volatile("dmb ish" ::: "memory");
            s->sampleTime = done;
            __asm__ volatile("dmb ish" ::: "memory");
        }
        droidvm_audio_periods = droidvm_audio_periods + 1;
    }
    IOLockLock(lock);
    stopping = false;
    IOLockWakeup(lock, &stopping, false);
    IOLockUnlock(lock);
    release();
}

bool DroidVMAudioUserClient::start(IOService *provider) {
    device = OSDynamicCast(DroidVMAudioDevice, provider);
    if (!device || !super::start(provider)) return false;
    device->publish(this);
    droidvm_audio_clients = droidvm_audio_clients + 1;
    return true;
}

IOReturn DroidVMAudioUserClient::clientClose() {
    if (startedIO && device) device->stopIO();
    startedIO = false;
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

void DroidVMAudioUserClient::stop(IOService *provider) {
    droidvm_audio_clients = droidvm_audio_clients - 1;
    super::stop(provider);
}

IOReturn DroidVMAudioUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a, IOExternalMethodDispatch *,
                                                  OSObject *, void *) {
    droidvm_audio_last_selector = selector;
    droidvm_audio_selectors = droidvm_audio_selectors + 1;
    switch (selector) {
    case 0: case 10: {                                     // StartIO, StartIOWithFlags
        IOReturn r = device->startIO();
        if (!r) startedIO = true;
        if (selector == 10 && a->scalarOutputCount >= 1) { a->scalarOutput[0] = 0; a->scalarOutputCount = 1; }
        return r;
    }
    case 1: case 11:                                       // StopIO, StopIOWithFlags
        startedIO = false;
        return device->stopIO();
    case 2:                                                // SetControlValue: no controls are published
        return kIOReturnBadArgument;
    case 3:                                                // PerformConfigChange: none is ever requested
        return kIOReturnSuccess;
    case 4: {                                              // SetNominalSampleRate: the one rate there is
        int64_t rate = 0;
        if (a->structureInputSize == sizeof rate) memcpy(&rate, a->structureInput, sizeof rate);
        return rate == fixed64(Rate) ? kIOReturnSuccess : kIOReturnUnsupported;
    }
    case 5: {                                              // SetStreamCurrentFormat: only the published format
        struct { int64_t rate; uint32_t id, flags, bytesPerPacket, framesPerPacket, bytesPerFrame, channels, bits, reserved; } f;
        if (a->scalarInputCount < 1 || a->structureInputSize != sizeof f) return kIOReturnBadArgument;
        memcpy(&f, a->structureInput, sizeof f);
        return f.rate == fixed64(Rate) && f.id == FormatLinearPCM && f.channels == device->channels() && f.bits == 16
            ? kIOReturnSuccess : kIOReturnUnsupported;
    }
    case 6: case 12:                                       // SetStreamActive, SetClientDescription
        return kIOReturnSuccess;
    default:
        droidvm_audio_bad_selectors = droidvm_audio_bad_selectors + 1;
        return kIOReturnUnsupported;
    }
}

IOReturn DroidVMAudioUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    IOMemoryDescriptor *m = device ? device->memory(type) : nullptr;
    if (!m) return kIOReturnBadArgument;
    m->retain();
    *memory = m;
    *options = 0;
    droidvm_audio_maps = droidvm_audio_maps + 1;
    return kIOReturnSuccess;
}

IOReturn DroidVMAudioUserClient::setProperties(OSObject *properties) {
    return device ? device->apply(properties, this) : kIOReturnNotReady;
}

// The HAL registers for IOAudio2Notification config-change requests; this device never asks for one.
IOReturn DroidVMAudioUserClient::registerNotificationPort(mach_port_t, UInt32, io_user_reference_t) {
    return kIOReturnSuccess;
}

#define DROIDVM_LATE_INIT(file) extern "C" void droidvm_late_init_##file(void) __asm__("__GLOBAL__sub_I_" #file ".cpp");
DROIDVM_LATE_INIT(DroidVMAudio)

// OSMetaClass keeps its superclass after the vtable pointer and the reserved expansion pointer (libkern/c++/OSMetaClass.h:
// reserved, superClassLink, className). Checked against the superclass the class was defined with before it is changed, and
// the new parent must itself derive from that class, so every cast the C++ class allows stays true.
static bool reparent(const OSMetaClass *meta, const OSMetaClass *defined, const char *family) {
    const OSSymbol *name = OSSymbol::withCString(family);
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    const OSMetaClass **link = reinterpret_cast<const OSMetaClass **>(const_cast<OSMetaClass *>(meta)) + 2;
    bool conforms = false;
    for (const OSMetaClass *c = parent; c && !conforms; c = c->getSuperClass()) conforms = c == defined;
    if (!parent || *link != defined || !conforms) return false;
    *link = parent;
    return true;
}

// Publishes one device of a role (input/output) with its UID on a built-in nub of IOResources.
static bool publishDevice(IOService *resources, bool input, const char *uid, const char *nubName) {
    IOService *nub = OSTypeAlloc(IOService);
    if (!nub || !nub->init()) { OSSafeReleaseNULL(nub); return false; }
    nub->setName(nubName);
    nub->setProperty("built-in", kOSBooleanTrue);
    if (!nub->attach(resources)) { nub->release(); return false; }
    nub->registerService();
    DroidVMAudioDevice *device = OSTypeAlloc(DroidVMAudioDevice);
    bool ok = device && device->initRole(input, uid) && device->attach(nub) && device->start(nub);
    if (!ok && device) { if (device->getProvider()) device->detach(nub); device->release(); }
    return ok;
}

// Registers the two classes in their own OSMetaClass module load (their static initializer is staged for this call, as the
// network family's are), links them under the original family's classes, and publishes a speaker and a microphone device.
int droidvm_audio_attach(void) {
    IOService *resources = IOService::getResourceService();
    if (!resources || droidvm_audio_stage) return -1;
    gTransportLock = IOLockAlloc();
    if (!gTransportLock) return -1;
    droidvm_audio_stage = 1;
    void *module = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!module) return -1;
    droidvm_late_init_DroidVMAudio();
    droidvm_audio_family_status = OSMetaClass::postModLoad(module);
    if (droidvm_audio_family_status) return -1;
    droidvm_audio_stage = 2;
    // Normally loaded already, for IOAVFamily; a no-op then.
    droidvm_audio_family_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOAudio2Family");
    if (!reparent(DroidVMAudioDevice::metaClass, IOService::metaClass, "IOAudio2Device") ||
        !reparent(DroidVMAudioUserClient::metaClass, IOUserClient::metaClass, "IOAudio2DeviceUserClient")) {
        IOLog("DroidVMAudio: IOAudio2Family's classes not found (load %x)\n", droidvm_audio_family_status);
        return -1;
    }
    droidvm_audio_stage = 3;
    bool speaker = publishDevice(resources, false, "Speaker", "droidvm-audio-out");
    bool mic = publishDevice(resources, true, "Mic", "droidvm-audio-in");
    if (!speaker && !mic) return -1;
    droidvm_audio_stage = 4;
    IOLog("DroidVMAudio: published Speaker=%d Mic=%d as IOAudio2Devices\n", speaker, mic);
    return 0;
}
