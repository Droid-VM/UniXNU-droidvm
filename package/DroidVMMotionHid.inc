// SPDX-License-Identifier: BSD-2-Clause-Patent
// DroidVM: the motion sensors as the stock iPad has them, so CoreMotion and backboardd read them the stock way.
//
// Appended by scripts/build-native-legacy-display.py to its staged NativeHidInput.cpp: the same IOHIDDevice ABI
// (OriginalHidDeviceABI.h), and the translation unit whose late static initializer HidInputProbe.cpp runs, so this class's
// metaclass is registered with NativeHidInput's.
//
// Two devices, as the iPad's sensor processor publishes them: an accelerometer and a gyroscope. locationd matches each by its
// primary usage pair (CLIoHidInterface::setMatchingForDevices: the Apple vendor page 0xff00 with usage 3, the accelerometer,
// or 9, the gyroscope), and the primary pair is the descriptor's first application collection. Inside it the data fields are
// the HID sensor page's, which IOHIDFamily's IOHIDEventDriver turns into accelerometer and gyro events (parseAccelElement,
// handleAccelReport and the gyro ones). The values come from the virtio motion device (DroidVMVirtioInput.c fills
// droidvm_motion): the gravity vector, 256 per g, and the angular velocity, 16 per degree per second, counter-clockwise
// positive, in the frame of the guest's panel -- the frame and the signs iOS uses -- so only the units change here.
extern "C" {
struct droidvm_motion { volatile uint32_t seq; int32_t orientation, accel[3], gyro[3]; };
extern struct droidvm_motion droidvm_motion;
}

class DroidVMMotionHid : public IOHIDDevice {
    OSDeclareDefaultStructors(DroidVMMotionHid)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    bool gyroscope = false;
    uint32_t lastSeq = 0;

    // Two application collections. The first is the primary usage pair locationd matches (vendor page, usage 3 or 9) and holds the
    // sensor's physical collection: three signed 32-bit fields, logical and physical range +-1000000, unit exponent -3, so
    // IOHIDEventDriver's exponent scaling gives the value in thousandths as the event's IOFixed: milli-g (accelerometer),
    // milliradians per second (gyroscope). The second, empty, is only a usage pair: IOHIDFamily attaches IOHIDEventDriver to an
    // interface by its pairs, and the one personality that takes a motion sensor ("HID Sensor") names {0xff0c, 3}.
#define DROIDVM_MOTION_DESCRIPTOR(usage, sensor, field)                                                                    \
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
        int32_t raw[3];
        for (unsigned i = 0; i < 3; ++i) raw[i] = self->gyroscope ? droidvm_motion.gyro[i] : droidvm_motion.accel[i];
        __asm__ volatile("dmb ish" ::: "memory");
        if (droidvm_motion.seq != before) return;  // torn read: the next tick has it
        self->lastSeq = before;
        int32_t report[3];
        for (unsigned i = 0; i < 3; ++i) {
            // milli-g: 256 per g. milliradians per second: 16 per degree per second, 17.4533 milliradians a degree.
            report[i] = clamp(self->gyroscope ? (int64_t)raw[i] * 174533 / 160000 : (int64_t)raw[i] * 1000 / 256);
        }
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
        if (buffer) self->handleReport(buffer, 0, 0);
        OSSafeReleaseNULL(buffer);
    }

public:
    void setGyroscope(bool value) { gyroscope = value; }
    IOWorkLoop *getWorkLoop() const override { return loop; }
    OSString *newTransportString() const override { return OSString::withCString("SPU"); }
    OSString *newProductString() const override {
        return OSString::withCString(gyroscope ? "DroidVM gyroscope" : "DroidVM accelerometer");
    }
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        static const unsigned char accelerometer[] = DROIDVM_MOTION_DESCRIPTOR(0x03, 0x73, 0x53);
        static const unsigned char gyro[] = DROIDVM_MOTION_DESCRIPTOR(0x09, 0x76, 0x57);
        if (!out) return kIOReturnBadArgument;
        *out = gyroscope ? IOBufferMemoryDescriptor::withBytes(gyro, sizeof(gyro), kIODirectionNone)
                         : IOBufferMemoryDescriptor::withBytes(accelerometer, sizeof(accelerometer), kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        loop = IOWorkLoop::workLoop();
        if (!loop) return false;
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
#undef DROIDVM_MOTION_DESCRIPTOR

extern "C" IOService *droidvm_motion_hid_allocate(bool gyroscope) {
    DroidVMMotionHid *device = new DroidVMMotionHid;
    if (device) device->setGyroscope(gyroscope);
    return device;
}
