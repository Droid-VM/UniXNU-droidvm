// SPDX-License-Identifier: BSD-2-Clause-Patent
// DroidVM: the motion sensors as the stock iPad has them, so CoreMotion and backboardd read them the stock way.
//
// Appended by scripts/build-native-legacy-display.py to its staged NativeHidInput.cpp: the same IOHIDDevice ABI
// (OriginalHidDeviceABI.h), and the translation unit whose late static initializer HidInputProbe.cpp runs, so this class's
// metaclass is registered with NativeHidInput's.
//
// Three devices, as the iPad's sensor processor publishes them, each found by its primary usage pair (the descriptor's first
// application collection):
//   - the accelerometer {0xff00, 3} and the gyroscope {0xff00, 9}, which locationd matches (CLIoHidInterface::
//     setMatchingForDevices) and serves to CoreMotion; their data fields are the HID sensor page's acceleration and angular
//     velocity, which IOHIDFamily's IOHIDEventDriver turns into accelerometer and gyro events;
//   - the device orientation {0x20, 0x8a}, which backboardd's CoreMotion matches for the interface orientation: one bit per
//     orientation on the Apple motion page (usages 0x64 + UIDeviceOrientation, as the stock IOHIDEventDriver parses them), and
//     IOHIDEventDriver sends an orientation event for the bit that turns on. The orientation is the one DroidVMVirtioInput.c
//     derives from gravity, the decision the iPad's sensor processor makes itself. CoreMotion takes the sample's time from the
//     event's first child, a vendor event {0xff15, 2} with an 8-byte timestamp (the sensor processor's time sync), and skips a
//     sample whose time is not after the last one ("Skipping stale orientation (3 @ 0)"); IOHIDEventDriver attaches such a child
//     from a field on that page under an Apple vendor Message collection {0xff00, 0x23}, so each report carries the time.
// IOHIDFamily attaches IOHIDEventDriver to an interface by its usage pairs, and the one personality that takes a motion sensor
// ("HID Sensor") names {0xff0c, 3}: each descriptor ends with an empty application collection carrying that pair.
// The values come from the virtio motion device (droidvm_motion): the gravity vector, 256 per g, and the angular velocity, 16 per
// degree per second, counter-clockwise positive, in the frame of the guest's panel -- the frame and the signs iOS uses.
extern "C" {
struct droidvm_motion { volatile uint32_t seq; int32_t orientation, accel[3], gyro[3]; };
extern struct droidvm_motion droidvm_motion;
}

enum { DROIDVM_MOTION_ACCELEROMETER, DROIDVM_MOTION_GYROSCOPE, DROIDVM_MOTION_ORIENTATION };

class DroidVMMotionHid : public IOHIDDevice {
    OSDeclareDefaultStructors(DroidVMMotionHid)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    unsigned kind = DROIDVM_MOTION_ACCELEROMETER;
    uint32_t lastSeq = 0;
    int32_t lastOrientation = -1;
    uint64_t orientationSentAt = 0;

    // The vector sensors: three signed 32-bit fields, logical and physical range +-1000000, unit exponent -3, so
    // IOHIDEventDriver's exponent scaling gives the value in thousandths as the event's IOFixed: milli-g (accelerometer),
    // milliradians per second (gyroscope).
#define DROIDVM_MOTION_VECTOR(usage, sensor, field)                                                                        \
    {0x06, 0x00, 0xff, 0x09, usage, 0xa1, 0x01,                                                                           \
     0x05, 0x20, 0x09, sensor, 0xa1, 0x00,                                                                                \
     0x0a, field, 0x04, 0x0a, field + 1, 0x04, 0x0a, field + 2, 0x04,                                                     \
     0x17, 0xc0, 0xbd, 0xf0, 0xff, 0x27, 0x40, 0x42, 0x0f, 0x00,                                                          \
     0x37, 0xc0, 0xbd, 0xf0, 0xff, 0x47, 0x40, 0x42, 0x0f, 0x00,                                                          \
     0x55, 0x0d, 0x75, 0x20, 0x95, 0x03, 0x81, 0x02, 0xc0, 0xc0,                                                          \
     0x06, 0x0c, 0xff, 0x09, 0x03, 0xa1, 0x01, 0xc0}

    static int32_t clamp(int64_t value) {
        return value > 1000000 ? 1000000 : value < -1000000 ? -1000000 : (int32_t)value;
    }

    static void tick(OSObject *owner, IOTimerEventSource *source) {
        DroidVMMotionHid *self = OSDynamicCast(DroidVMMotionHid, owner);
        if (!self) return;
        // The device sends 50 frames a second; reading at twice that sends each one once.
        source->setTimeoutMS(10);
        uint32_t before = droidvm_motion.seq;
        if ((before & 1) || before == self->lastSeq) return;
        __asm__ volatile("dmb ish" ::: "memory");
        int32_t raw[3], orientation = droidvm_motion.orientation;
        for (unsigned i = 0; i < 3; ++i)
            raw[i] = self->kind == DROIDVM_MOTION_GYROSCOPE ? droidvm_motion.gyro[i] : droidvm_motion.accel[i];
        __asm__ volatile("dmb ish" ::: "memory");
        if (droidvm_motion.seq != before) return;  // torn read: the next tick has it
        self->lastSeq = before;
        IOBufferMemoryDescriptor *buffer = nullptr;
        if (self->kind == DROIDVM_MOTION_ORIENTATION) {
            // The event goes out when a bit turns on: on a change, and every 2 s again (all bits off, then the bit), because a
            // client that opens the service later -- backboardd, well after this device's first report -- hears only events.
            uint64_t now = mach_absolute_time(), period = 0;
            nanoseconds_to_absolutetime(2000000000ULL, &period);
            if (orientation < 0 || orientation > 6) return;
            if (orientation == self->lastOrientation && now - self->orientationSentAt < period) return;
            if (orientation == self->lastOrientation) {
                struct __attribute__((packed)) { uint8_t bits; uint64_t time; } none = {0, now};
                IOBufferMemoryDescriptor *clear = IOBufferMemoryDescriptor::withBytes(&none, sizeof(none), kIODirectionNone);
                if (clear) self->handleReport(clear, 0, 0);
                OSSafeReleaseNULL(clear);
            }
            self->lastOrientation = orientation;
            self->orientationSentAt = now;
            struct __attribute__((packed)) { uint8_t bits; uint64_t time; } report = {(uint8_t)(1u << orientation), now + 1};
            buffer = IOBufferMemoryDescriptor::withBytes(&report, sizeof(report), kIODirectionNone);
        } else {
            int32_t report[3];
            for (unsigned i = 0; i < 3; ++i) {
                // milli-g: 256 per g. milliradians per second: 16 per degree per second, 17.4533 milliradians a degree.
                report[i] = clamp(self->kind == DROIDVM_MOTION_GYROSCOPE ? (int64_t)raw[i] * 174533 / 160000
                                                                         : (int64_t)raw[i] * 1000 / 256);
            }
            buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
        }
        if (buffer) self->handleReport(buffer, 0, 0);
        OSSafeReleaseNULL(buffer);
    }

public:
    void setKind(unsigned value) { kind = value; }
    IOWorkLoop *getWorkLoop() const override { return loop; }
    OSString *newTransportString() const override { return OSString::withCString("SPU"); }
    OSString *newProductString() const override {
        return OSString::withCString(kind == DROIDVM_MOTION_GYROSCOPE     ? "DroidVM gyroscope"
                                     : kind == DROIDVM_MOTION_ORIENTATION ? "DroidVM device orientation"
                                                                          : "DroidVM accelerometer");
    }
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        static const unsigned char accelerometer[] = DROIDVM_MOTION_VECTOR(0x03, 0x73, 0x53);
        static const unsigned char gyro[] = DROIDVM_MOTION_VECTOR(0x09, 0x76, 0x57);
        // Sensor page, device orientation: seven one-bit fields on the Apple motion page (usages 0x64-0x6a), a padding bit, and
        // in a vendor Message collection the time stamp, eight bytes on page 0xff15 (usage 2).
        static const unsigned char orientation[] = {0x05, 0x20, 0x09, 0x8a, 0xa1, 0x01,
                                                    0x06, 0x0c, 0xff, 0x19, 0x64, 0x29, 0x6a, 0x15, 0x00, 0x25, 0x01,
                                                    0x75, 0x01, 0x95, 0x07, 0x81, 0x02, 0x95, 0x01, 0x81, 0x03,
                                                    0x06, 0x00, 0xff, 0x09, 0x23, 0xa1, 0x00,
                                                    0x06, 0x15, 0xff, 0x09, 0x02, 0x15, 0x00, 0x26, 0xff, 0x00,
                                                    0x75, 0x08, 0x95, 0x08, 0x81, 0x02, 0xc0, 0xc0,
                                                    0x06, 0x0c, 0xff, 0x09, 0x03, 0xa1, 0x01, 0xc0};
        if (!out) return kIOReturnBadArgument;
        *out = kind == DROIDVM_MOTION_GYROSCOPE ? IOBufferMemoryDescriptor::withBytes(gyro, sizeof(gyro), kIODirectionNone)
             : kind == DROIDVM_MOTION_ORIENTATION
                 ? IOBufferMemoryDescriptor::withBytes(orientation, sizeof(orientation), kIODirectionNone)
                 : IOBufferMemoryDescriptor::withBytes(accelerometer, sizeof(accelerometer), kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        loop = IOWorkLoop::workLoop();
        if (!loop) return false;
        // IOHIDEventDriver's personalities carry HIDDefaultBehavior, and IOHIDFamily's matching wants the device to have it too.
        if (!setProperty("HIDDefaultBehavior", true)) return false;
        setProperty("Built-In", true);
        if (!IOHIDDevice::start(provider)) return false;
        timer = IOTimerEventSource::timerEventSource(this, tick);
        if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) {
            OSSafeReleaseNULL(timer);
            IOHIDDevice::stop(provider);
            return false;
        }
        timer->setTimeoutMS(10);
        return true;
    }
    void stop(IOService *provider) override {
        if (timer) {
            timer->cancelTimeout();
            loop->removeEventSource(timer);
            OSSafeReleaseNULL(timer);
        }
        IOHIDDevice::stop(provider);
    }
    void free() override {
        OSSafeReleaseNULL(loop);
        IOHIDDevice::free();
    }
};
OSDefineMetaClassAndStructors(DroidVMMotionHid, IOHIDDevice)
#undef DROIDVM_MOTION_VECTOR

extern "C" IOService *droidvm_motion_hid_allocate(unsigned kind) {
    DroidVMMotionHid *device = new DroidVMMotionHid;
    if (device) device->setKind(kind);
    return device;
}
