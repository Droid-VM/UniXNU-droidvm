// SPDX-License-Identifier: BSD-2-Clause-Patent
// DroidVM: the pointer of a macOS guest. macOS makes its pointer of mice and absolute pointers (as QEMU's USB tablet is), not of a
// touch screen's digitizer, so where the kernel is built with DROIDVM_INPUT_MOUSE (the macOS input kext) the VM's devices become:
//
//   the touch screen (and the app's tablet mode, an absolute pointer DroidVMVirtioInput.c publishes as one finger): an absolute
//     pointer, QEMU's tablet report (X/Y 0..32767, three buttons, a wheel) and a horizontal wheel (Consumer AC Pan). One finger
//     moves the pointer there and presses the left button once it has been down kPressMS or moved (a tap clicks, a drag drags):
//     the wait keeps a two-finger tap's first finger from clicking; two or more scroll by the movement of their centre, with no
//     button; two fingers lifted soon and nearly where they came down click the right button there.
//   the mouse (the app's mouse mode, DroidVMVirtioInput.c's mouse sink): a mouse, its motion as 16-bit relative X/Y, five buttons,
//     the wheel and the horizontal wheel.
//
// Each device is an IOHIDDevice with HIDDefaultBehavior, so IOHIDFamily's generic IOHIDEventDriver serves it as it serves any USB
// mouse. Appended by ipados27/scripts/build-droidvm-hid-input.py to its staged NativeHidInput.cpp for macOS (whose touch seqlock,
// native_touch_*, it reads as NativeHidInput's digitizer does); published by HidInputProbe.cpp in place of the touch digitizer, and
// when the VM has a mouse.
#if DROIDVM_INPUT_MOUSE
extern "C" {
void native_mouse_set_sink(void (*sink)(void *, const int32_t *), void *context);
bool droidvm_mouse_present(void);
// For the host rig (peek.py): [0] mouse reports sent, [1] pointer reports sent, [2] reports refused (handleReport), [3] right clicks
// of a two-finger tap, [4] scroll reports of two or more fingers.
volatile uint32_t droidvm_pointer_hid[5];
// The fingers' travel, in the panel's pixels (of its 2732), per wheel step of the touch screen's scroll (poke.py; read as each
// scroll report goes): its wheel is not accelerated (HIDSupportsScrollAcceleration false), and at 80 Safari's page moved as far as
// the fingers did (400 pixels of travel, 400 of page; 40 moved it twice as far).
volatile uint32_t droidvm_pointer_scroll_px = 80;
}

class DroidVMPointerHIDDevice : public IOHIDDevice {
    OSDeclareDefaultStructors(DroidVMPointerHIDDevice)
    // The touch seqlock's axes (DroidVMVirtioInput.c publishes the fingers on NativeHidInput's 30480 x 20320 panel axes).
    enum { kAxisX = 30480, kAxisY = 20320, kRange = 32767 };
    // The two-finger tap: at most this long (ms) and this far (pointer units, about 1.5% of the panel) from where it began.
    enum { kTapMS = 300, kTapSlop = kRange * 3 / 200, kPressMS = 80 };
    // A wheel step per this many pointer units of the fingers' centre (droidvm_pointer_scroll_px of the panel's 2732 pixels).
    static int32_t scrollStep() {
        const uint32_t px = droidvm_pointer_scroll_px;
        return (int32_t)((int64_t)kRange * (px ? px : 1) / 2732) ?: 1;
    }
    IOWorkLoop *loop = nullptr;
    bool absolute = false;
    // The pointer: where it was last sent and the left button; the touch's gesture so far.
    uint32_t lastSeq = 0;
    int32_t x = 0, y = 0;
    bool left = false;
    bool pending = false;              // one finger down, its press waiting (kPressMS or a move), from where it came down
    uint64_t downAt = 0;
    int32_t downX = 0, downY = 0;
    bool scrolling = false;            // two or more fingers came down since the touch began: no button until all lift
    unsigned most = 0;                 // the most fingers down at once in this touch
    uint64_t began = 0;                // when the touch's second finger came down (mach_absolute_time)
    int32_t centreX = 0, centreY = 0, startX = 0, startY = 0, carryX = 0, carryY = 0;
    bool moved = false;                // the fingers' centre went past kTapSlop

    static int32_t scale(int value, int axis) {
        if (value < 0) value = 0;
        if (value >= axis) value = axis - 1;
        return (int32_t)((int64_t)value * kRange / (axis - 1));
    }
    static int8_t clamp8(int32_t v) { return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v); }
    uint64_t elapsedMS(uint64_t since) const {
        uint64_t ns;
        absolutetime_to_nanoseconds(mach_absolute_time() - since, &ns);
        return ns / 1000000;
    }
    void send(const uint8_t *report, unsigned length, unsigned counter) {
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(report, length, kIODirectionNone);
        IOReturn result = buffer ? handleReport(buffer, kIOHIDReportTypeInput, 0) : kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        if (result == kIOReturnSuccess) droidvm_pointer_hid[counter] = droidvm_pointer_hid[counter] + 1;
        else droidvm_pointer_hid[2] = droidvm_pointer_hid[2] + 1;
    }
    // The absolute pointer's report: buttons, X, Y, wheel, horizontal wheel.
    void point(uint8_t buttons, int8_t wheel = 0, int8_t pan = 0) {
        const uint8_t report[] = {buttons, (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8), (uint8_t)wheel, (uint8_t)pan};
        send(report, sizeof(report), 1);
    }

    // The touch screen's finger set (the seqlock NativeHidInput's digitizer reads), as the pointer.
    static void touchNotify(void *context) {
        DroidVMPointerHIDDevice *self = static_cast<DroidVMPointerHIDDevice *>(context);
        if (self && self->loop) self->loop->runAction(&DroidVMPointerHIDDevice::touchAction, self);
    }
    static IOReturn touchAction(OSObject *owner, void *, void *, void *, void *) {
        DroidVMPointerHIDDevice *self = OSDynamicCast(DroidVMPointerHIDDevice, owner);
        if (self) self->touch();
        return kIOReturnSuccess;
    }
    void touch() {
        const uint32_t before = native_touch_seq;
        if ((before & 1) || before == lastSeq) return;
        __asm__ volatile("dmb ish" ::: "memory");
        unsigned count = native_touch_count;
        if (count > 12) count = 12;
        int64_t sumX = 0, sumY = 0;
        int32_t firstX = 0, firstY = 0;
        for (unsigned i = 0; i < count; ++i) {
            const int32_t fx = scale(native_touch_xs[i], kAxisX), fy = scale(native_touch_ys[i], kAxisY);
            if (!i) { firstX = fx; firstY = fy; }
            sumX += fx;
            sumY += fy;
        }
        __asm__ volatile("dmb ish" ::: "memory");
        if (native_touch_seq != before) return;   // torn: the next publication tells again
        lastSeq = before;
        if (!count) {                              // all lifted: the button up, a tap's click, or the two-finger tap's right click
            if (left) { left = false; point(0); }
            if (pending) { pending = false; point(1); point(0); }
            if (scrolling && most >= 2 && !moved && elapsedMS(began) <= kTapMS) {
                x = startX;
                y = startY;
                point(2);
                point(0);
                droidvm_pointer_hid[3] = droidvm_pointer_hid[3] + 1;
            }
            scrolling = false;
            most = 0;
            return;
        }
        if (count > most) most = count;
        if (count >= 2 && !scrolling) {           // a second finger: let go of the button, scroll from the fingers' centre
            if (left) { left = false; point(0); }
            pending = false;
            scrolling = true;
            moved = false;
            began = mach_absolute_time();
            startX = centreX = (int32_t)(sumX / count);
            startY = centreY = (int32_t)(sumY / count);
            carryX = carryY = 0;
            return;
        }
        if (scrolling) {
            if (count < 2) return;                // one of them lifted: wait for the rest
            const int32_t cx = (int32_t)(sumX / count), cy = (int32_t)(sumY / count);
            // The content follows the fingers, as on a trackpad with natural scrolling (macOS's default). The system's scroll
            // direction applies to a wheel as well, and natural scrolling turns it round, so the fingers go as the wheel turn
            // that, turned round, moves the content with them: fingers moving up send the wheel towards the user (negative),
            // which scrolls the content up, fingers moving right the horizontal wheel to the right. With natural scrolling
            // off both go the other way, as a trackpad's do.
            carryY += cy - centreY;
            carryX += cx - centreX;
            centreX = cx;
            centreY = cy;
            if (cx - startX > kTapSlop || startX - cx > kTapSlop || cy - startY > kTapSlop || startY - cy > kTapSlop) moved = true;
            const int32_t step = scrollStep(), wheel = -(carryY / step), pan = carryX / step;
            if (!wheel && !pan) return;
            carryY += wheel * step;
            carryX -= pan * step;
            point(0, clamp8(wheel), clamp8(pan));
            droidvm_pointer_hid[4] = droidvm_pointer_hid[4] + 1;
            return;
        }
        x = firstX;
        y = firstY;
        if (!left && !pending) {                   // the finger came down: the pointer there, the press to come
            pending = true;
            downAt = mach_absolute_time();
            downX = x;
            downY = y;
            point(0);
            return;
        }
        if (pending && (elapsedMS(downAt) >= kPressMS || x - downX > kTapSlop || downX - x > kTapSlop || y - downY > kTapSlop ||
                        downY - y > kTapSlop)) {   // held or moved: press where it came down, then follow it
            pending = false;
            left = true;
            const int32_t nowX = x, nowY = y;
            x = downX;
            y = downY;
            point(1);
            x = nowX;
            y = nowY;
        }
        point(left ? 1 : 0);
    }

    // The mouse's frame: {buttons, x, y, wheel, horizontal wheel} (DroidVMVirtioInput.c).
    static void mouseNotify(void *context, const int32_t *frame) {
        DroidVMPointerHIDDevice *self = static_cast<DroidVMPointerHIDDevice *>(context);
        if (self && self->loop && frame) self->loop->runAction(&DroidVMPointerHIDDevice::mouseAction, self, const_cast<int32_t *>(frame));
    }
    static IOReturn mouseAction(OSObject *owner, void *frame, void *, void *, void *) {
        DroidVMPointerHIDDevice *self = OSDynamicCast(DroidVMPointerHIDDevice, owner);
        if (self) self->mouse(static_cast<const int32_t *>(frame));
        return kIOReturnSuccess;
    }
    void mouse(const int32_t *frame) {
        int32_t dx = frame[1], dy = frame[2], wheel = frame[3], pan = frame[4];
        do {   // a move past the report's 16 bits goes as several reports
            const int32_t sx = dx > 32767 ? 32767 : dx < -32767 ? -32767 : dx, sy = dy > 32767 ? 32767 : dy < -32767 ? -32767 : dy;
            const int8_t w = clamp8(wheel), p = clamp8(pan);
            const uint8_t report[] = {(uint8_t)(frame[0] & 0x1f), (uint8_t)sx, (uint8_t)(sx >> 8), (uint8_t)sy, (uint8_t)(sy >> 8),
                                      (uint8_t)w, (uint8_t)p};
            send(report, sizeof(report), 0);
            dx -= sx; dy -= sy; wheel -= w; pan -= p;
        } while (dx || dy || wheel || pan);
    }

public:
    void setAbsolute() { absolute = true; }
    IOWorkLoop *getWorkLoop() const override { return loop; }
    OSString *newTransportString() const override { return OSString::withCString("Virtual"); }
    OSString *newManufacturerString() const override { return OSString::withCString("DroidVM"); }
    OSString *newProductString() const override { return OSString::withCString(absolute ? "DroidVM Touch Screen" : "DroidVM Mouse"); }
    OSNumber *newVendorIDNumber() const override { return OSNumber::withNumber(0x1af4, 32); }   // virtio's
    OSNumber *newProductIDNumber() const override { return OSNumber::withNumber(absolute ? 0x1052 : 0x1053, 32); }
    OSNumber *newLocationIDNumber() const override { return OSNumber::withNumber(absolute ? 0x44565450 : 0x4456544d, 32); }
    OSNumber *newPrimaryUsagePageNumber() const override { return OSNumber::withNumber(1, 32); }   // Generic Desktop
    OSNumber *newPrimaryUsageNumber() const override { return OSNumber::withNumber(2, 32); }       // Mouse
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        // QEMU's USB tablet (hw/usb/dev-hid.c), with the horizontal wheel after its wheel.
        static const unsigned char tablet[] = {
            0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
            0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
            0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
            0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02,
            0x81, 0x02,
            0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
            0x05, 0x0c, 0x0a, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
            0xc0, 0xc0};
        // A five-button mouse with 16-bit motion, the wheel and the horizontal wheel.
        static const unsigned char mouse[] = {
            0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
            0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
            0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
            0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
            0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
            0x05, 0x0c, 0x0a, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
            0xc0, 0xc0};
        if (!out) return kIOReturnBadArgument;
        *out = absolute ? IOBufferMemoryDescriptor::withBytes(tablet, sizeof(tablet), kIODirectionNone)
                        : IOBufferMemoryDescriptor::withBytes(mouse, sizeof(mouse), kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        loop = IOWorkLoop::workLoop();
        if (!loop) return false;
        setProperty("HIDDefaultBehavior", true);
        // The touch screen's wheel is the fingers' travel (scrollStep), sent as precisely as they moved: the HID event system
        // (IOHIDPointerScrollFilter, from the event service's ancestors) must not accelerate it, or a short swipe of 17 steps
        // went through a whole page. The mouse's wheel is a real one and keeps the mouse's acceleration.
        if (absolute) setProperty("HIDSupportsScrollAcceleration", false);
        if (!IOHIDDevice::start(provider)) return false;
        if (absolute) native_touch_set_sink(&DroidVMPointerHIDDevice::touchNotify, this);
        else native_mouse_set_sink(&DroidVMPointerHIDDevice::mouseNotify, this);
        return true;
    }
    void stop(IOService *provider) override {
        if (absolute) native_touch_set_sink(nullptr, nullptr);
        else native_mouse_set_sink(nullptr, nullptr);
        IOHIDDevice::stop(provider);
    }
    void free() override {
        OSSafeReleaseNULL(loop);
        IOHIDDevice::free();
    }
};
OSDefineMetaClassAndStructors(DroidVMPointerHIDDevice, IOHIDDevice)

// HidInputProbe.cpp: the touch screen's absolute pointer (absolute) or the mouse.
extern "C" IOService *droidvm_pointer_hid_allocate(bool absolute) {
    DroidVMPointerHIDDevice *device = OSTypeAlloc(DroidVMPointerHIDDevice);
    if (device && absolute) device->setAbsolute();
    return device;
}
#endif
