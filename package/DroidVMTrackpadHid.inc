// SPDX-License-Identifier: BSD-2-Clause-Patent
// DroidVM: the VM's trackpad as a Magic Trackpad 2 connected over Bluetooth, so that iPadOS's own multitouch stack makes the pointer,
// the scrolling and the gestures of it.
//
// Appended by ipados27/scripts/build-droidvm-hid-input.py to its staged NativeHidInput.cpp, as DroidVMMotionHid.inc is; published by
// HidInputProbe.cpp when the VM has a trackpad (DroidVMVirtioInput.c, which sends this device each of the trackpad's frames).
//
// iPadOS's bluetoothd publishes a Magic Trackpad 2 (vendor 76 of the Bluetooth SIG, product 613) as IOHIDUserDevices of its own, one
// per function, each with a report descriptor built into bluetoothd (TrackpadInputDevice: device management, trackpad, actuator and a
// fourth). The trackpad's -- a mouse (report 2), a Digitizer/Touch Pad collection (report 0x3f) and a vendor one (0xff00/0x0c, report
// 0x44, the longest input report) -- is matched by AppleTopCaseDriverV2's personality "Magic Trackpad BT iOS/tvOS Event Driver -
// TopCase - 613" (vendor 76, product 613, usage pair Generic Desktop/Mouse) for AppleTopCaseHIDEventDriver's
// AppleMultitouchTrackpadHIDEventDriver, which makes the multitouch device (AppleMultitouchDriver) whose HSTouchHIDService property
// the HSTouchHIDService plug-in matches; board.toml adds those kexts to the kernel collection. This device has that descriptor and
// those identifiers. Its input reports are the Magic Trackpad 2's multitouch frames, report 2 at full length (a 12-byte header, then
// 9 bytes a finger, as Linux's hid-magicmouse reads them), and it answers the feature reports the driver asks a Magic Trackpad 2 for
// (family, sensor, surface) as acidanthera's VoodooInput does in its emulation of the same device, whose descriptors are bluetoothd's.
//
// It has no button: the guest's tap clicks (Settings, General, Trackpad: Tap to Click). It sets no HIDDefaultBehavior, so IOHIDFamily's
// generic event driver does not take the mouse collection for a mouse.
extern "C" {
// As DroidVMVirtioInput.c: a contact's tracking id, position in hundredths of a millimetre from the surface's top left corner, whether
// it still touches (0: lifted since the last frame), whether it is a palm, and its contact size (0..255; 0: the device has none).
struct droidvm_trackpad_contact { int32_t id, x, y; uint8_t down, palm, major, minor; };
void native_trackpad_set_sink(void (*sink)(void *, const struct droidvm_trackpad_contact *, unsigned), void *context);
void droidvm_trackpad_surface(uint32_t *width, uint32_t *height);
// For the host rig (peek.py): [0] reports sent, [1] feature reports asked for, [2] set reports, [3] contacts beyond the 15
// identifiers dropped; the last feature report asked for; and the reports that repeated still fingers.
volatile uint32_t droidvm_trackpad_hid[4], droidvm_trackpad_hid_last_get, droidvm_trackpad_hid_repeats;
}

#if defined(NATIVE_HID_SOURCE_API) && NATIVE_HID_SOURCE_API
#define DROIDVM_TRACKPAD_INPUT_REPORT kIOHIDReportTypeInput
#else
#define DROIDVM_TRACKPAD_INPUT_REPORT 0
#endif

class DroidVMTrackpadHIDDevice : public IOHIDDevice {
    OSDeclareDefaultStructors(DroidVMTrackpadHIDDevice)
    // The Magic Trackpad 2's coordinates (VoodooInput's MT2_MAX_X/Y): x -4067..4067 left to right, y -2603..2603 bottom to top.
    enum { kSpanX = 8134, kSpanY = 5206 };
    // A finger's state (bits 29-31 of its first word): coming down, touching, lifting, gone.
    enum { kStateInactive = 0, kStateStart = 3, kStateActive = 4, kStateStop = 7 };
    // A finger's type (bits 26-28): which finger the device takes it for.
    enum { kFingerUndefined = 0, kFingerIndex = 2, kFingerPalm = 6 };
    enum { kIdentifiers = 15, kHeader = 12, kFingerBytes = 9 };
    // The device's report interval (its multitouch device's ReportInterval, 8000 us). Frames that come closer together are merged
    // into the next report: the multitouch stack takes a finger's velocity from the reports' times, and reports a millisecond or less
    // apart ("Frame interval is zero!") get none, while a flood of them overruns backboardd's queue ("Pointer event dropped").
    enum { kIntervalMS = 8 };
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    uint32_t width = 0, height = 0;   // the surface, in hundredths of a millimetre
    uint64_t started = 0, lastSent = 0;
    uint64_t lastStamp = 0;            // the last report's time stamp (ms since start): each one is later than the one before
    // The contacts not yet reported: the latest state of each since the last report.
    struct droidvm_trackpad_contact pending[kIdentifiers] = {};
    unsigned pendingCount = 0;
    // The fingers on the trackpad after the last report, reported again each interval while no frame comes: a Magic Trackpad reports
    // every 8 ms for as long as a finger touches it, still or not, and the multitouch stack takes a pause from those reports (three
    // fingers that stop after swiping up open the app switcher; without them it saw the swipe's last speed at the lift, and went
    // home). The VM's trackpad sends a still finger every 50 ms. Repeating stops when no frame came for kHeldMS (a lost trackpad).
    enum { kHeldMS = 250 };
    struct droidvm_trackpad_contact held[kIdentifiers] = {};
    unsigned heldCount = 0;
    uint64_t lastFrame = 0;
    // The Magic Trackpad's finger identifiers 1..15: the tracking id each stands for, the finger it is taken for, and whether its
    // contact was reported already (its next report is not its first). Taken round robin, so that a new contact does not get the
    // identifier of the one that just lifted.
    struct Finger { int32_t id; uint8_t type; bool used, reported; } fingers[kIdentifiers + 1] = {};
    unsigned nextIdentifier = 1;
    OSData *queried = nullptr;   // the answer to the feature report asked for by the last set report 1

    static void put(uint8_t *out, uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) out[i] = (uint8_t)(value >> (8 * i));
    }
    unsigned identifierOf(int32_t id, bool create) {
        for (unsigned i = 1; i <= kIdentifiers; ++i)
            if (fingers[i].used && fingers[i].id == id) return i;
        if (!create) return 0;
        unsigned down = 0;
        for (unsigned i = 1; i <= kIdentifiers; ++i) down += fingers[i].used;
        for (unsigned k = 0; k < kIdentifiers; ++k) {
            unsigned i = (nextIdentifier + k - 1) % kIdentifiers + 1;
            if (fingers[i].used) continue;
            // The first finger down an index finger, the next ones the middle, ring and little fingers.
            fingers[i] = {id, (uint8_t)(kFingerIndex + down % 4), true, false};
            nextIdentifier = i % kIdentifiers + 1;
            return i;
        }
        return 0;
    }
    void stamp(uint8_t *report, bool touching, uint64_t milliseconds) {
        memset(report, 0, kHeader);
        report[0] = 2;
        report[7] = touching ? 3 : 2;
        report[8] = 0x31;
        report[9] = (uint8_t)(milliseconds << 3 | 4);
        report[10] = (uint8_t)(milliseconds >> 5);
        report[11] = (uint8_t)(milliseconds >> 13);
    }
    void send(const uint8_t *report, unsigned length) {
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(report, length, kIODirectionNone);
        if (buffer && handleReport(buffer, DROIDVM_TRACKPAD_INPUT_REPORT, 0) == kIOReturnSuccess)
            droidvm_trackpad_hid[0] = droidvm_trackpad_hid[0] + 1;
        OSSafeReleaseNULL(buffer);
    }
    uint64_t elapsedMS(uint64_t since) const {
        uint64_t nanoseconds = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - since, &nanoseconds);
        return nanoseconds / 1000000;
    }
    // A frame from the trackpad: its contacts merged into the pending ones (a contact's latest state wins; one lifted stays lifted), sent
    // at once when the last report is an interval old, else when the timer says it is.
    void merge(const struct droidvm_trackpad_contact *contacts, unsigned count) {
        lastFrame = mach_absolute_time();
        for (unsigned c = 0; c < count; ++c) {
            unsigned i = 0;
            while (i < pendingCount && pending[i].id != contacts[c].id) ++i;
            if (i == pendingCount) {
                if (pendingCount == kIdentifiers) {
                    droidvm_trackpad_hid[3] = droidvm_trackpad_hid[3] + 1;
                    continue;
                }
                ++pendingCount;
            }
            pending[i] = contacts[c];
        }
        const uint64_t since = lastSent ? elapsedMS(lastSent) : kIntervalMS;
        if (since >= kIntervalMS) flush();
        else if (timer) timer->setTimeoutMS((uint32_t)(kIntervalMS - since));
    }
    void flush() {
        if (!pendingCount) return;
        frame(pending, pendingCount);
        pendingCount = 0;
        lastSent = mach_absolute_time();
        if (heldCount && timer) timer->setTimeoutMS(kIntervalMS);
    }
    // The timer: the pending contacts when a frame came within the interval, else the fingers still down again.
    static void flushTimer(OSObject *owner, IOTimerEventSource *) {
        DroidVMTrackpadHIDDevice *self = OSDynamicCast(DroidVMTrackpadHIDDevice, owner);
        if (!self) return;
        if (!self->pendingCount && self->heldCount && self->elapsedMS(self->lastFrame) < kHeldMS) {
            memcpy(self->pending, self->held, sizeof(self->held[0]) * self->heldCount);
            self->pendingCount = self->heldCount;
            droidvm_trackpad_hid_repeats = droidvm_trackpad_hid_repeats + 1;
        }
        self->flush();
    }
    // One frame of the trackpad as a Magic Trackpad 2's report; after the last finger lifts, the reports the device ends a touch with
    // (as VoodooInput sends them): the lifted fingers again a report interval later, the first one gone, and the header alone.
    void frame(const struct droidvm_trackpad_contact *contacts, unsigned count) {
        uint8_t report[kHeader + kFingerBytes * kIdentifiers];
        unsigned n = 0;
        bool touching = false;
        uint64_t milliseconds = elapsedMS(started);
        if (milliseconds <= lastStamp) milliseconds = lastStamp + 1;
        for (unsigned c = 0; c < count; ++c) {
            const struct droidvm_trackpad_contact &contact = contacts[c];
            unsigned identifier = identifierOf(contact.id, contact.down);
            if (!identifier) {
                if (contact.down) droidvm_trackpad_hid[3] = droidvm_trackpad_hid[3] + 1;
                continue;
            }
            Finger &finger = fingers[identifier];
            touching |= contact.down;
            const uint8_t state = !contact.down ? kStateStop : finger.reported ? kStateActive : kStateStart;
            const uint8_t type = contact.palm ? kFingerPalm : finger.type;
            int32_t x = (int32_t)((int64_t)contact.x * kSpanX / width) - kSpanX / 2;
            int32_t y = kSpanY / 2 - (int32_t)((int64_t)contact.y * kSpanY / height);
            uint8_t *out = report + kHeader + kFingerBytes * n++;
            put(out, ((uint32_t)x & 0x1fff) | ((uint32_t)y & 0x1fff) << 13 | (uint32_t)type << 26 | (uint32_t)state << 29, 4);
            // Touch major and minor, size and pressure: VoodooInput's for a finger whose device gives no pressure; 0 once lifted.
            out[4] = contact.down ? 20 : 0;
            out[5] = contact.down ? 20 : 0;
            out[6] = contact.down ? 10 : 0;
            out[7] = contact.down ? 5 : 0;
            out[8] = (uint8_t)(identifier | 4 << 5);   // the angle: pi/2
            finger.reported = true;
            if (!contact.down) finger.used = false;
        }
        if (!n) return;
        stamp(report, touching, lastStamp = milliseconds);
        send(report, kHeader + kFingerBytes * n);
        heldCount = 0;
        for (unsigned c = 0; c < count; ++c)
            if (contacts[c].down) held[heldCount++] = contacts[c];
        if (touching) return;
        stamp(report, false, lastStamp += kIntervalMS);
        send(report, kHeader + kFingerBytes * n);
        report[kHeader + 3] &= 0x03;   // the first finger: undefined, inactive
        stamp(report, false, lastStamp += 1);
        send(report, kHeader + kFingerBytes * n);
        stamp(report, false, lastStamp += kIntervalMS);
        send(report, kHeader);
    }
    static void notify(void *context, const struct droidvm_trackpad_contact *contacts, unsigned count) {
        DroidVMTrackpadHIDDevice *self = static_cast<DroidVMTrackpadHIDDevice *>(context);
        if (self && self->loop && contacts)
            self->loop->runAction(&DroidVMTrackpadHIDDevice::frameAction, self, const_cast<struct droidvm_trackpad_contact *>(contacts),
                                  (void *)(uintptr_t)count);
    }
    static IOReturn frameAction(OSObject *owner, void *contacts, void *count, void *, void *) {
        DroidVMTrackpadHIDDevice *self = OSDynamicCast(DroidVMTrackpadHIDDevice, owner);
        if (self && self->width && self->height)
            self->merge(static_cast<const struct droidvm_trackpad_contact *>(contacts), (unsigned)(uintptr_t)count);
        return kIOReturnSuccess;
    }
    // A feature report the driver asks for, by its id: the family (0xd1, 0x81), the sensor's rows and columns (0xd3), its regions
    // (0xd0, 0xa1), the surface's size in hundredths of a millimetre (0xd9), and all of them at once (0xdb), as VoodooInput answers.
    OSData *answer(uint8_t id) const {
        static const uint8_t family[] = {0xd1, 0x81};
        static const uint8_t sensor[] = {0xd3, 0x01, 0x16, 0x1e, 0x03, 0x95, 0x00, 0x14, 0x1e, 0x62, 0x05, 0x00, 0x00};
        static const uint8_t regions[] = {0xd0, 0x02, 0x01, 0x00, 0x14, 0x01, 0x00, 0x1e, 0x00, 0x02, 0x14, 0x02, 0x01, 0x0e, 0x02, 0x00};
        static const uint8_t region[] = {0xa1, 0x00, 0x00, 0x05, 0x00, 0xfc, 0x01};
        static const uint8_t unknown[] = {0x7f, 0x00, 0x00, 0x00, 0x00};
        uint8_t surface[17] = {0xd9, 0, 0, 0, 0, 0, 0, 0, 0, 0x44, 0xe3, 0x52, 0xff, 0xbd, 0x1e, 0xe4, 0x26};
        put(surface + 1, width, 4);
        put(surface + 5, height, 4);
        OSData *data = OSData::withCapacity(96);
        if (!data) return nullptr;
        switch (id) {
        case 0x00: data->appendByte(0x00, 1); data->appendByte(0x01, 1); break;
        case 0x02: data->appendByte(0x02, 1); data->appendByte(0x01, 1); break;
        case 0xc8: data->appendByte(0xc8, 1); data->appendByte(0x08, 1); break;
        case 0xd1: data->appendBytes(family, sizeof(family)); break;
        case 0xd3: data->appendBytes(sensor, sizeof(sensor)); break;
        case 0xd0: data->appendBytes(regions, sizeof(regions)); break;
        case 0xa1: data->appendBytes(region, sizeof(region)); break;
        case 0xd9: data->appendBytes(surface, sizeof(surface)); break;
        case 0x7f: data->appendBytes(unknown, sizeof(unknown)); break;
        case 0xdb: {
            // each part after its length (2 bytes), the first after a header of its own
            static const uint8_t header[] = {0xdb, 0x01, 0x02, 0x00};
            data->appendBytes(header, sizeof(header));
            data->appendBytes(family, sizeof(family));
            static const uint8_t l1[] = {0x0d, 0x00}, l2[] = {0x10, 0x00}, l3[] = {0x07, 0x00}, l4[] = {0x11, 0x00};
            data->appendBytes(l1, 2); data->appendBytes(sensor, sizeof(sensor));
            data->appendBytes(l2, 2); data->appendBytes(regions, sizeof(regions));
            data->appendBytes(l3, 2); data->appendBytes(region, sizeof(region));
            data->appendBytes(l4, 2); data->appendBytes(surface, sizeof(surface));
            data->appendBytes(unknown, sizeof(unknown));
            break;
        }
        default: break;
        }
        return data;
    }

public:
    IOWorkLoop *getWorkLoop() const override { return loop; }
    OSString *newTransportString() const override { return OSString::withCString("Bluetooth"); }
    OSString *newManufacturerString() const override { return OSString::withCString("Apple Inc."); }
    OSString *newProductString() const override { return OSString::withCString("Magic Trackpad"); }
    OSString *newSerialNumberString() const override { return OSString::withCString("DroidVM"); }
    OSNumber *newVendorIDNumber() const override { return OSNumber::withNumber(76, 32); }
    OSNumber *newVendorIDSourceNumber() const override { return OSNumber::withNumber(1, 32); }   // Bluetooth SIG
    OSNumber *newProductIDNumber() const override { return OSNumber::withNumber(613, 32); }
    OSNumber *newVersionNumber() const override { return OSNumber::withNumber(0x100, 32); }
    OSNumber *newLocationIDNumber() const override { return OSNumber::withNumber(0x44564d54, 32); }
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        // bluetoothd's trackpad descriptor (TrackpadInputDevice, report id 0xf1 of the device).
        static const unsigned char trackpadDescriptor[] = {
            0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
            0x85, 0x02, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
            0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0x95, 0x04, 0x75, 0x08, 0x81, 0x01, 0xc0, 0xc0, 0x05, 0x0d,
            0x09, 0x05, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x09, 0x0c, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x10, 0x85, 0x3f,
            0x81, 0x22, 0xc0, 0x06, 0x00, 0xff, 0x09, 0x0c, 0xa1, 0x01, 0x06, 0x00, 0xff, 0x09, 0x0c, 0x15, 0x00, 0x26, 0xff, 0x00,
            0x85, 0x44, 0x75, 0x08, 0x96, 0x6b, 0x05, 0x81, 0x00, 0xc0};
        if (!out) return kIOReturnBadArgument;
        *out = IOBufferMemoryDescriptor::withBytes(trackpadDescriptor, sizeof(trackpadDescriptor), kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    IOReturn getReport(IOMemoryDescriptor *report, IOHIDReportType type, IOOptionBits options) override {
        (void)type;
        const uint8_t id = (uint8_t)(options & 0xff);
        droidvm_trackpad_hid[1] = droidvm_trackpad_hid[1] + 1;
        droidvm_trackpad_hid_last_get = id;
        if (!report) return kIOReturnBadArgument;
        OSData *data = id == 0x01 ? (queried ? (queried->retain(), queried) : nullptr) : answer(id);
        if (!data) return kIOReturnNoResources;
        report->writeBytes(0, data->getBytesNoCopy(), data->getLength());
        data->release();
        return kIOReturnSuccess;
    }
    // Report 1 asks for a feature report by its id (its second byte); getReport 1 then answers with it, as {1, id, 0, length, 0} for
    // the short ones and the surface itself for 0xd9.
    IOReturn setReport(IOMemoryDescriptor *report, IOHIDReportType type, IOOptionBits options) override {
        (void)type;
        droidvm_trackpad_hid[2] = droidvm_trackpad_hid[2] + 1;
        if (!report || (options & 0xff) != 0x01 || report->getLength() < 2) return kIOReturnSuccess;
        uint8_t asked[2] = {0, 0};
        report->readBytes(0, asked, sizeof(asked));
        static const uint8_t lengths[][2] = {{0xdb, 0x49}, {0xd1, 0x01}, {0xd3, 0x0c}, {0xd0, 0x0f}, {0xa1, 0x06}, {0x7f, 0x04}, {0xc8, 0x01}};
        OSData *data = nullptr;
        if (asked[1] == 0xd9) {
            data = answer(0xd9);
        } else {
            for (const auto &known : lengths) {
                if (known[0] != asked[1]) continue;
                const uint8_t answer[] = {0x01, known[0], 0x00, known[1], 0x00};
                data = OSData::withBytes(answer, sizeof(answer));
            }
        }
        if (data) {
            OSSafeReleaseNULL(queried);
            queried = data;
        }
        return kIOReturnSuccess;
    }
    bool start(IOService *provider) override {
        droidvm_trackpad_surface(&width, &height);
        if (!width || !height) return false;
        started = mach_absolute_time();
        loop = IOWorkLoop::workLoop();
        if (!loop) return false;
        // As bluetoothd's trackpad device has it (an extended data report of the device).
        if (OSNumber *extended = OSNumber::withNumber(60358656, 32)) {
            if (OSArray *list = OSArray::withObjects((const OSObject **)&extended, 1)) {
                setProperty("ExtendedData", list);
                list->release();
            }
            extended->release();
        }
        if (!IOHIDDevice::start(provider)) return false;
        timer = IOTimerEventSource::timerEventSource(this, &DroidVMTrackpadHIDDevice::flushTimer);
        if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) {
            OSSafeReleaseNULL(timer);
            IOHIDDevice::stop(provider);
            return false;
        }
        native_trackpad_set_sink(&DroidVMTrackpadHIDDevice::notify, this);
        return true;
    }
    void stop(IOService *provider) override {
        native_trackpad_set_sink(nullptr, nullptr);
        if (timer) {
            timer->cancelTimeout();
            loop->removeEventSource(timer);
            OSSafeReleaseNULL(timer);
        }
        IOHIDDevice::stop(provider);
    }
    void free() override {
        OSSafeReleaseNULL(queried);
        OSSafeReleaseNULL(loop);
        IOHIDDevice::free();
    }
};
OSDefineMetaClassAndStructors(DroidVMTrackpadHIDDevice, IOHIDDevice)
#undef DROIDVM_TRACKPAD_INPUT_REPORT

extern "C" IOService *droidvm_trackpad_hid_allocate(void) {
    return new DroidVMTrackpadHIDDevice;
}
