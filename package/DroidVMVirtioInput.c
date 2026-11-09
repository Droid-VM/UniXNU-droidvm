// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Touch from the phone's screen. The DroidVM app hands the touches on its VM screen to crosvm, which turns them into a
// virtio-input multi-touch device (protocol B: slots, tracking ids, positions, SYN_REPORT); --virtio-mmio-input puts
// the app's input devices on the virtio-mmio transport, and make-droidvm-adt.py (--input-mmio) publishes their windows
// as /arm-io/input-N, compatible "droidvm,virtio-input". A worker here polls the event queues of the ones it
// recognises -- a direct multi-touch screen, and an absolute pointer as one finger while its button is down -- and
// publishes the finger set where NativeHidInput's digitizer reads it every 8 ms (native_touch_*, a seqlock: odd
// while written; DroidVMStubs.c), on the 30480 x 20320 axes it expects. The other input devices stay unconfigured.
//
// A pen (crosvm --input stylus, or any evdev pen: INPUT_PROP_DIRECT with BTN_TOOL_PEN and ABS_X/Y) is read too, with what it has of
// ABS_PRESSURE, ABS_DISTANCE, ABS_TILT_X/Y and the barrel buttons. Each of its frames goes to NativeHidInput's pen sink (the
// digitizer's Stylus collection, report 4), with the tilt also as the polar angles iPadOS reads an Apple Pencil's by.
//
// A trackpad (crosvm's trackpad with contacts=N, or any evdev multi-touch pad: INPUT_PROP_POINTER, multi-touch positions and their
// resolution) is read too: each of its frames, every contact down and every one lifted since the last, goes to the trackpad sink,
// in hundredths of a millimetre on its surface, where DroidVMTrackpadHid.inc publishes it as a Magic Trackpad's report.
//
// Motion sensors (crosvm --input motion, or any evdev accelerometer: INPUT_PROP_ACCELEROMETER with ABS_X/Y/Z, the gravity vector
// in the panel's frame, Y up the panel) are read too. Their latest values, and the device orientation they say, are published in
// droidvm_motion for DroidVMMotion.c's sysctl, where the owner tool that tells backboardd the device orientation reads them.
//
// NativeHidInput tells a lifted finger by the list getting shorter: whatever was published past the new count is
// reported up. A finger that lifts while one published after it stays down is therefore first moved to the end of
// the list (still down, where it was) and the list is shortened only once the digitizer has reported that order.
//
// droidvm-input=0 leaves the devices alone.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#if DROIDVM_INPUT_DYNAMIC
#include <vm/vm_kern_xnu.h>
#endif

extern void IOSleep(unsigned milliseconds);
// ArmCpu (the GIC): a handler for one SPI, run in the primary interrupt context before the EOI; 0 when attached.
extern int armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);

#define DROIDVM_INPUT_DEVICES 8
#define DROIDVM_INPUT_BUFFERS 64       // crosvm's event queue size
#define DROIDVM_INPUT_SLOTS 10
#define DROIDVM_INPUT_POLL_MS 4
// With every device's interrupt attached the worker sleeps until one fires, looking anyway after this long (ms) in case one was
// lost; while fingers are down it still looks every DROIDVM_INPUT_POLL_MS (touch_still).
#define DROIDVM_INPUT_GUARD_MS 250
#define DROIDVM_TOUCH_FINGERS 10       // NativeHidInput's report
#define DROIDVM_TOUCH_AXIS_X 30480
#define DROIDVM_TOUCH_AXIS_Y 20320
// A finger that comes down within this share (percent) of the screen's bottom starts at the bottom edge. iPadOS takes a swipe
// as the Home gesture only from the edge band (NativeHidInput: 48 of 2032 rows, ~19 px of 800), and on the phone that band
// lies in Android's own navigation gesture area, which keeps those touches for itself. "Bottom" is the panel edge gravity
// points at when motion sensors say which (the device held sideways puts the Home gesture on a side of the panel), and the
// panel's own bottom when they do not.
#define DROIDVM_TOUCH_BOTTOM_SNAP 4
// A finger held still. Apple's digitizer reports every finger on the glass at 120 Hz, never twice at exactly one place (sensor
// noise), and iPadOS reads a held finger's pause from those reports: SpringBoard tells swipe-up-and-hold (the app switcher) from
// swipe-up (home) by the finger's speed falling while it is held. The phone's touchscreen filters a still finger's noise away and
// an unchanged position is no movement to iPadOS, so a held finger was reported as nothing at all, its last movement's speed stood
// until it lifted, and the switcher could hardly be reached (home every time a synthetic swipe held still; the switcher with a
// 2 px wobble). While fingers are down and no frame has come for this long (ms), the set is published again with every x one axis
// unit off (1/30480 of the panel, about 0.07 px) and back the next time, as a digitizer's noise.
#define DROIDVM_TOUCH_STILL_MS 8

enum { EV_SYN = 0, EV_KEY = 1, EV_ABS = 3 };
enum { SYN_REPORT = 0, ABS_X = 0, ABS_Y = 1, ABS_Z = 2, ABS_RZ = 5, ABS_PRESSURE = 0x18, ABS_DISTANCE = 0x19, ABS_TILT_X = 0x1a,
       ABS_TILT_Y = 0x1b, ABS_MT_SLOT = 0x2f, ABS_MT_TOUCH_MAJOR = 0x30, ABS_MT_TOUCH_MINOR = 0x31, ABS_MT_POSITION_X = 0x35,
       ABS_MT_POSITION_Y = 0x36, ABS_MT_TOOL_TYPE = 0x37, ABS_MT_TRACKING_ID = 0x39, MT_TOOL_PALM = 2,
       BTN_LEFT = 0x110, BTN_TOOL_PEN = 0x140, BTN_TOOL_RUBBER = 0x141, BTN_TOUCH = 0x14a, BTN_STYLUS = 0x14b, BTN_STYLUS2 = 0x14c,
       INPUT_PROP_POINTER = 0, INPUT_PROP_DIRECT = 1, INPUT_PROP_ACCELEROMETER = 6, KEY_ENTER = 28, KEY_A = 30 };
enum { CFG_ID_NAME = 1, CFG_PROP_BITS = 0x10, CFG_EV_BITS = 0x11, CFG_ABS_INFO = 0x12 };
enum { KIND_NONE, KIND_TOUCH, KIND_POINTER, KIND_KEYBOARD, KIND_MOTION, KIND_PEN, KIND_TRACKPAD };
// The pen's axes past ABS_X/Y, in its state and axis arrays.
enum { PEN_PRESSURE, PEN_DISTANCE, PEN_TILT_X, PEN_TILT_Y, PEN_AXES };
// What the pen sink gets (NativeHidInput's report 4 and its dispatch wrapper): flags (1 in range, 2 the tip touches, 4 the barrel
// button, 8 the eraser end), x and y 0..32767, pressure 0..4095, distance 0..255, tilt x and y in whole degrees, and the pen's
// altitude from the normal and azimuth (the cap end's direction from +x, counterclockwise with y up, as UIKit reads a display's:
// towards -y of the panel's y-down frame) in IOFixed radians.
enum { PEN_OUT_FLAGS, PEN_OUT_X, PEN_OUT_Y, PEN_OUT_PRESSURE, PEN_OUT_DISTANCE, PEN_OUT_TILT_X, PEN_OUT_TILT_Y, PEN_OUT_ALTITUDE,
       PEN_OUT_AZIMUTH, PEN_OUT_VALUES };
// What the trackpad sink gets for each contact of a frame (DroidVMTrackpadHid.inc declares it as well): its tracking id, its
// position in hundredths of a millimetre from the surface's top left corner, whether it still touches (0: lifted since the last
// frame, where it was), whether the device calls it a palm, and its contact size, 0..255 of the device's range (0 when it has none).
struct droidvm_trackpad_contact { int32_t id, x, y; uint8_t down, palm, major, minor; };
// UIDeviceOrientation, as backboardd takes it: 0 not known (lying flat, or between two), 1 portrait, 2 upside down, 3 landscape
// left (the Home side on the right: the panel's left edge down), 4 landscape right (its right edge down).
enum { ORIENTATION_UNKNOWN, ORIENTATION_PORTRAIT, ORIENTATION_UPSIDE_DOWN, ORIENTATION_LANDSCAPE_LEFT,
       ORIENTATION_LANDSCAPE_RIGHT };

struct droidvm_input_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct droidvm_input_event { uint16_t type, code; int32_t value; };
_Static_assert(sizeof(struct droidvm_input_desc) == 16 && sizeof(struct droidvm_input_event) == 8, "VirtIO wire layout");

// One page per queue: descriptors at 0, the driver ring at 1024, the device ring at 2048 (64 entries fit).
#if DROIDVM_INPUT_DYNAMIC
// A board whose kernel data has no room for them (the iPadOS 27 fileset: the native __DATA must fit the original kernel's slot)
// takes each device's two queue pages and its event page from the kernel map when the device starts; each lies in one page, so
// each page's own physical address is all the device needs.
extern uint64_t kvtophys(vm_offset_t va);
static uint8_t *droidvm_input_pages[DROIDVM_INPUT_DEVICES];
static bool input_pages(unsigned index) {
    vm_offset_t address = 0;
    if (droidvm_input_pages[index]) return true;
    if (kmem_alloc(kernel_map, &address, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS)
        return false;
    droidvm_input_pages[index] = (uint8_t *)address;
    return true;
}
static uint64_t input_physical(const void *address) { return kvtophys((vm_offset_t)address); }
#define DROIDVM_INPUT_QUEUE(index, queue) (droidvm_input_pages[index] + (queue) * 4096)
#define DROIDVM_INPUT_BUFFER(index) ((struct droidvm_input_event *)(droidvm_input_pages[index] + 2 * 4096))
#else
static uint8_t droidvm_input_queues[DROIDVM_INPUT_DEVICES][2][4096] __attribute__((aligned(4096)));
static struct droidvm_input_event droidvm_input_buffers[DROIDVM_INPUT_DEVICES][DROIDVM_INPUT_BUFFERS] __attribute__((aligned(4096)));
static bool input_pages(unsigned index) { (void)index; return true; }
static uint64_t input_physical(const void *address) { return ml_static_vtop((vm_offset_t)address); }
#define DROIDVM_INPUT_QUEUE(index, queue) droidvm_input_queues[index][queue]
#define DROIDVM_INPUT_BUFFER(index) droidvm_input_buffers[index]
#endif
_Static_assert(DROIDVM_INPUT_BUFFERS * sizeof(struct droidvm_input_event) <= 4096, "one page of event buffers");

struct droidvm_input {
    uintptr_t registers;
    unsigned kind, index;
    uint16_t size, used;
    int32_t min_x, span_x, min_y, span_y;
    unsigned slot;
    struct { int32_t id, x, y, major, minor, tool; } slots[DROIDVM_INPUT_SLOTS];   // id -1: no finger
    int32_t pointer_x, pointer_y;
    bool pointer_down;
    uint8_t key_modifiers, keys[6];   // a keyboard's boot report state: modifier bits and up to six usages held
    bool keys_changed;
    int32_t motion[6];                 // motion sensors: ABS_X..ABS_RZ as last reported
    // A pen: the ends in range, contact and the barrel buttons, its position and its other axes as last reported, each axis's
    // range (span 0: the device has none) and the tilt's resolution (units per radian, 0 when the device states none).
    bool pen_tip, pen_eraser, pen_touch, pen_barrel, pen_was_in_range;
    int32_t pen_x, pen_y, pen[PEN_AXES], pen_min[PEN_AXES], pen_span[PEN_AXES], pen_tilt_res;
    bool interrupt;                    // its SPI (the ADT's droidvm,intid) wakes the worker
    // A trackpad: its resolution (units per millimetre), its contact size's range (span 0: it has none) and the contacts it sent
    // last, so that the next frame reports the ones gone from it as lifted.
    int32_t res_x, res_y, major_min, major_span;
    struct droidvm_trackpad_contact trackpad_last[DROIDVM_INPUT_SLOTS];
    unsigned trackpad_last_count;
};
static struct droidvm_input droidvm_inputs[DROIDVM_INPUT_DEVICES];
static unsigned droidvm_input_count;

// For the host rig (peek.py): devices found, recognised (kinds: 1 touch, 2 pointer, 3 keyboard, 4 motion, 5 pen, 6 trackpad, 0 left
// alone), events and frames seen, keyboard, pen and trackpad reports sent.
volatile uint32_t droidvm_input_found, droidvm_input_started, droidvm_input_kinds[DROIDVM_INPUT_DEVICES];
volatile uint64_t droidvm_input_events, droidvm_input_frames, droidvm_input_published, droidvm_input_key_reports,
    droidvm_input_still,   // publications of a held finger set (touch_still)
    droidvm_input_pen_reports, droidvm_input_trackpad_reports;
// The devices' interrupts: [0] taken (each wakes the worker), [1] the worker's sleeps, [2] of those ended by their guard or the
// held-finger step, [3] guard ends whose look found a frame (an interrupt that never came), [4] devices attached. The worker
// polled every 4 ms before: 250 wakes a second of a vCPU with nothing to read on a still screen. droidvm_input_irq 0: poll so
// again (read before each sleep).
volatile uint64_t droidvm_input_irq_counts[5];
volatile uint32_t droidvm_input_irq = 1;
char droidvm_input_names[DROIDVM_INPUT_DEVICES][48];

// The motion sensors' latest frame and the orientation it says (DroidVMMotion.c publishes it as kern.droidvm_motion). seq is
// odd while a frame is written, so a reader that sees the same even seq before and after its copy has a whole one.
struct droidvm_motion { volatile uint32_t seq; int32_t orientation, accel[3], gyro[3]; };
struct droidvm_motion droidvm_motion;
extern void droidvm_motion_register(void);

extern volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];

// Register accesses are single loads and stores with a plain base register: the hypervisor emulates a trapped MMIO access
// from the syndrome, and a post-indexed or paired access (what a byte loop compiles to) has none -- the guest gets a
// synchronous external abort instead.
static uint32_t input_read32(const struct droidvm_input *d, unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(d->registers + offset) : "memory");
    return value;
}
static uint8_t input_read8(const struct droidvm_input *d, unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldrb %w0, [%1]" : "=r"(value) : "r"(d->registers + offset) : "memory");
    return (uint8_t)value;
}
static void input_write32(const struct droidvm_input *d, unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(d->registers + offset) : "memory");
}
static void input_write8(const struct droidvm_input *d, unsigned offset, uint8_t value) {
    __asm__ volatile("strb %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)value), "r"(d->registers + offset) : "memory");
}
static void input_write64(const struct droidvm_input *d, unsigned offset, uint64_t value) {
    input_write32(d, offset, (uint32_t)value);
    input_write32(d, offset + 4, (uint32_t)(value >> 32));
}
static inline void input_barrier(void) { __asm__ volatile("dmb osh" ::: "memory"); }

// The virtio-input configuration: select and subsel at 0x100/0x101, the answer's size at 0x102, its bytes from 0x108.
static unsigned input_config(const struct droidvm_input *d, uint8_t select, uint8_t subsel, uint8_t *out, unsigned max) {
    input_write8(d, 0x100, select);
    input_write8(d, 0x101, subsel);
    unsigned size = input_read8(d, 0x102);
    if (size > max) size = max;
    for (unsigned i = 0; i < size; ++i) out[i] = input_read8(d, 0x108 + i);
    return size;
}
static bool input_bit(const uint8_t *bits, unsigned size, unsigned bit) {
    return bit / 8 < size && (bits[bit / 8] >> (bit % 8) & 1);
}
static bool input_axis(const struct droidvm_input *d, uint8_t axis, int32_t *min, int32_t *span) {
    uint8_t info[20];
    if (input_config(d, CFG_ABS_INFO, axis, info, sizeof(info)) < 8) return false;
    int32_t lo, hi;
    memcpy(&lo, info, 4);
    memcpy(&hi, info + 4, 4);
    if (hi <= lo) return false;
    *min = lo;
    *span = hi - lo + 1;
    return true;
}

// A multi-touch position axis with its resolution (units per millimetre; 0 when the device states none).
static bool input_axis_res(const struct droidvm_input *d, uint8_t axis, int32_t *min, int32_t *span, int32_t *res) {
    uint8_t info[20] = {0};
    if (input_config(d, CFG_ABS_INFO, axis, info, sizeof(info)) < 8) return false;
    int32_t lo, hi;
    memcpy(&lo, info, 4);
    memcpy(&hi, info + 4, 4);
    memcpy(res, info + 16, 4);
    if (hi <= lo) return false;
    *min = lo;
    *span = hi - lo + 1;
    return true;
}

// A pen axis the device may lack: its range and (tilt) resolution, or a zero span.
static void pen_axis(struct droidvm_input *d, unsigned axis, uint8_t code) {
    uint8_t info[20] = {0};
    int32_t lo, hi;
    d->pen_span[axis] = 0;
    if (input_config(d, CFG_ABS_INFO, code, info, sizeof(info)) < 8) return;
    memcpy(&lo, info, 4);
    memcpy(&hi, info + 4, 4);
    if (hi <= lo) return;
    d->pen_min[axis] = lo;
    d->pen_span[axis] = hi - lo + 1;
    if (axis == PEN_TILT_X) memcpy(&d->pen_tilt_res, info + 16, 4);
}

static bool input_reset(const struct droidvm_input *d) {
    input_write32(d, 0x70, 0);
    for (unsigned waited = 0; input_read32(d, 0x70); ++waited) {
        if (waited > 1000) return false;
        delay(1);
    }
    return true;
}

static bool input_queue(struct droidvm_input *d, unsigned queue, uint16_t size, uint8_t *page) {
    input_write32(d, 0x30, queue);
    if (input_read32(d, 0x44) || input_read32(d, 0x34) < size) return false;
    memset(page, 0, 4096);
    *(volatile uint16_t *)(page + 1024) = 1;   // VIRTQ_AVAIL_F_NO_INTERRUPT until the worker sleeps on the interrupt
    uint64_t physical = input_physical(page);
    input_barrier();
    input_write32(d, 0x38, size);
    input_write64(d, 0x80, physical);
    input_write64(d, 0x90, physical + 1024);
    input_write64(d, 0xa0, physical + 2048);
    input_write32(d, 0x44, 1);
    return true;
}

// Brings a recognised device up with every event buffer posted; NULL, or why not.
static const char *input_start(struct droidvm_input *d) {
    if (!input_reset(d)) return "reset";
    input_write32(d, 0x70, 1);
    input_write32(d, 0x70, 3);
    input_write32(d, 0x14, 1);
    if (!(input_read32(d, 0x10) & 1)) return "no VIRTIO_F_VERSION_1";
    input_write32(d, 0x24, 0);
    input_write32(d, 0x20, 0);
    input_write32(d, 0x24, 1);
    input_write32(d, 0x20, 1);
    input_write32(d, 0x70, 11);
    if (!(input_read32(d, 0x70) & 8)) return "features refused";
    if (!input_pages(d->index)) return "no queue pages";
    uint8_t *events = DROIDVM_INPUT_QUEUE(d->index, 0), *status = DROIDVM_INPUT_QUEUE(d->index, 1);
    d->size = DROIDVM_INPUT_BUFFERS;
    if (!input_queue(d, 0, d->size, events)) return "event queue";
    if (!input_queue(d, 1, 8, status)) return "status queue";   // crosvm wants both; nothing is ever sent on it
    struct droidvm_input_desc *descriptors = (struct droidvm_input_desc *)events;
    volatile uint16_t *available = (volatile uint16_t *)(events + 1024);
    uint64_t buffers = input_physical(DROIDVM_INPUT_BUFFER(d->index));
    for (uint16_t i = 0; i < d->size; ++i) {
        descriptors[i] = (struct droidvm_input_desc){buffers + i * sizeof(struct droidvm_input_event),
                                                     sizeof(struct droidvm_input_event), 2, 0};
        available[2 + i] = i;
    }
    input_barrier();
    available[1] = d->size;
    d->used = 0;
    input_write32(d, 0x70, 15);
    if ((input_read32(d, 0x70) & 0x4f) != 15) return "device not ready";
    input_barrier();
    input_write32(d, 0x50, 0);
    return NULL;
}

// What the device is: a direct multi-touch screen, an absolute pointer with a button, or neither.
static unsigned input_identify(struct droidvm_input *d) {
    uint8_t name[48] = {0}, props[16] = {0}, abs[16] = {0}, keys[64] = {0};
    input_config(d, CFG_ID_NAME, 0, name, sizeof(name) - 1);
    memcpy(droidvm_input_names[d->index], name, sizeof(name));
    unsigned nprops = input_config(d, CFG_PROP_BITS, 0, props, sizeof(props));
    unsigned nabs = input_config(d, CFG_EV_BITS, EV_ABS, abs, sizeof(abs));
    unsigned nkeys = input_config(d, CFG_EV_BITS, EV_KEY, keys, sizeof(keys));
    if (input_bit(abs, nabs, ABS_MT_POSITION_X) && input_bit(abs, nabs, ABS_MT_POSITION_Y) &&
        input_bit(props, nprops, INPUT_PROP_DIRECT) &&
        input_axis(d, ABS_MT_POSITION_X, &d->min_x, &d->span_x) && input_axis(d, ABS_MT_POSITION_Y, &d->min_y, &d->span_y))
        return KIND_TOUCH;
    // A trackpad: multi-touch positions on a surface that is not the screen.
    if (input_bit(abs, nabs, ABS_MT_POSITION_X) && input_bit(abs, nabs, ABS_MT_POSITION_Y) &&
        input_bit(props, nprops, INPUT_PROP_POINTER) && !input_bit(props, nprops, INPUT_PROP_DIRECT) &&
        input_axis_res(d, ABS_MT_POSITION_X, &d->min_x, &d->span_x, &d->res_x) &&
        input_axis_res(d, ABS_MT_POSITION_Y, &d->min_y, &d->span_y, &d->res_y)) {
        if (!input_bit(abs, nabs, ABS_MT_TOUCH_MAJOR) || !input_axis(d, ABS_MT_TOUCH_MAJOR, &d->major_min, &d->major_span))
            d->major_span = 0;
        return KIND_TRACKPAD;
    }
    // A pen before the pointer below, which it would also pass for (ABS_X/Y and BTN_TOUCH).
    if (input_bit(keys, nkeys, BTN_TOOL_PEN) && input_bit(props, nprops, INPUT_PROP_DIRECT) && input_bit(abs, nabs, ABS_X) &&
        input_bit(abs, nabs, ABS_Y) && input_axis(d, ABS_X, &d->min_x, &d->span_x) && input_axis(d, ABS_Y, &d->min_y, &d->span_y)) {
        static const uint8_t codes[PEN_AXES] = {ABS_PRESSURE, ABS_DISTANCE, ABS_TILT_X, ABS_TILT_Y};
        for (unsigned axis = 0; axis < PEN_AXES; ++axis)
            if (input_bit(abs, nabs, codes[axis])) pen_axis(d, axis, codes[axis]);
        return KIND_PEN;
    }
    if (!input_bit(abs, nabs, ABS_MT_POSITION_X) && input_bit(abs, nabs, ABS_X) && input_bit(abs, nabs, ABS_Y) &&
        (input_bit(keys, nkeys, BTN_LEFT) || input_bit(keys, nkeys, BTN_TOUCH)) &&
        input_axis(d, ABS_X, &d->min_x, &d->span_x) && input_axis(d, ABS_Y, &d->min_y, &d->span_y))
        return KIND_POINTER;
    if (input_bit(keys, nkeys, KEY_A) && input_bit(keys, nkeys, KEY_ENTER)) return KIND_KEYBOARD;
    if (input_bit(props, nprops, INPUT_PROP_ACCELEROMETER) && input_bit(abs, nabs, ABS_X) && input_bit(abs, nabs, ABS_Y) &&
        input_bit(abs, nabs, ABS_Z))
        return KIND_MOTION;
    return KIND_NONE;
}

// The device orientation gravity says, as iio-sensor-proxy decides it: an axis wins when gravity leans more than 35 degrees
// along it out of the plane of the other two (tan^2 35 = 0.49, in integers: no floating point in the kernel), and nothing
// changes while both lean that far, or neither does (flat on a table). Only directions matter, so the device's units do not.
static int32_t motion_orientation(int32_t previous, const int32_t accel[3]) {
    int64_t x = accel[0], y = accel[1], z = accel[2];
    bool along_x = 100 * x * x > 49 * (y * y + z * z), along_y = 100 * y * y > 49 * (x * x + z * z);
    if (along_x == along_y) return previous;
    if (along_x) return x > 0 ? ORIENTATION_LANDSCAPE_RIGHT : ORIENTATION_LANDSCAPE_LEFT;
    return y > 0 ? ORIENTATION_UPSIDE_DOWN : ORIENTATION_PORTRAIT;
}

static void motion_publish(const struct droidvm_input *d) {
    droidvm_motion.seq = droidvm_motion.seq + 1;
    input_barrier();
    for (unsigned i = 0; i < 3; ++i) {
        droidvm_motion.accel[i] = d->motion[i];
        droidvm_motion.gyro[i] = d->motion[3 + i];
    }
    droidvm_motion.orientation = motion_orientation(droidvm_motion.orientation, d->motion);
    input_barrier();
    droidvm_motion.seq = droidvm_motion.seq + 1;
}

// The fingers in the order they were published: device, slot (DROIDVM_INPUT_SLOTS for a pointer) and HID identifier.
struct droidvm_finger { uint8_t device, slot, id; bool fresh; int32_t x, y; };
static struct droidvm_finger droidvm_order[DROIDVM_TOUCH_FINGERS];
static unsigned droidvm_order_count;

static int32_t input_scale(int32_t value, int32_t min, int32_t span, int32_t axis) {
    int64_t v = (int64_t)(value - min) * axis / span;
    return v < 0 ? 0 : v >= axis ? axis - 1 : (int32_t)v;
}

// The digitizer's report count when the finger set was last published, and when (mach_absolute_time).
static uint32_t droidvm_published_at;
static uint64_t droidvm_published_time;

// NativeHidInput reads a published finger set when it is told to (it has no timer for touch): the sink it installs, as with sheng's
// touch thread (kits/sheng/TouchProbe.cpp). It runs the report on its work loop, from this input thread.
static void (*droidvm_touch_sink)(void *);
static void *droidvm_touch_sink_context;
void native_touch_set_sink(void (*sink)(void *), void *context) {
    droidvm_touch_sink_context = context;
    __atomic_store_n(&droidvm_touch_sink, sink, __ATOMIC_RELEASE);
}

static void touch_publish(const struct droidvm_finger *fingers, unsigned count) {
    droidvm_published_at = native_touch_hid_reports;
    droidvm_published_time = mach_absolute_time();
    uint32_t seq = native_touch_seq & ~1u;
    native_touch_seq = seq + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    for (unsigned i = 0; i < count; ++i) {
        native_touch_ids[i] = fingers[i].id;
        native_touch_xs[i] = fingers[i].x;
        native_touch_ys[i] = fingers[i].y;
    }
    native_touch_count = count;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = seq + 2;
    droidvm_input_published = droidvm_input_published + 1;
    void (*sink)(void *) = __atomic_load_n(&droidvm_touch_sink, __ATOMIC_ACQUIRE);
    if (sink) sink(droidvm_touch_sink_context);
}

// Waits (bounded) until the digitizer has turned the last publication into a report.
static void touch_wait_report(uint32_t before) {
    for (unsigned waited = 0; native_touch_hid_reports == before && waited < 40; waited += 2) IOSleep(2);
}

static bool finger_down(const struct droidvm_finger *f) {
    const struct droidvm_input *d = &droidvm_inputs[f->device];
    return f->slot < DROIDVM_INPUT_SLOTS ? d->slots[f->slot].id >= 0 : d->pointer_down;
}

// After a frame: drop the lifted fingers (each moved to the end first), update the others, append the new ones.
// A finger coming down near the bottom edge -- the one gravity points at -- starts on it (DROIDVM_TOUCH_BOTTOM_SNAP).
static void touch_snap(struct droidvm_finger *f) {
    const int32_t snap_x = DROIDVM_TOUCH_AXIS_X * DROIDVM_TOUCH_BOTTOM_SNAP / 100;
    const int32_t snap_y = DROIDVM_TOUCH_AXIS_Y * DROIDVM_TOUCH_BOTTOM_SNAP / 100;
    switch (droidvm_motion.orientation) {
    case ORIENTATION_UPSIDE_DOWN:
        if (f->y < snap_y) f->y = 0;
        break;
    case ORIENTATION_LANDSCAPE_LEFT:
        if (f->x < snap_x) f->x = 0;
        break;
    case ORIENTATION_LANDSCAPE_RIGHT:
        if (f->x >= DROIDVM_TOUCH_AXIS_X - snap_x) f->x = DROIDVM_TOUCH_AXIS_X - 1;
        break;
    default:
        if (f->y >= DROIDVM_TOUCH_AXIS_Y - snap_y) f->y = DROIDVM_TOUCH_AXIS_Y - 1;
        break;
    }
}

static void touch_frame(void) {
    struct droidvm_finger next[DROIDVM_TOUCH_FINGERS];
    unsigned n = 0;
    // The digitizer reports only the newest set: a finger that lifts before the set it went down in was reported would
    // never have touched. A tap's down and up frames often arrive within one poll.
    bool lifting = false;
    for (unsigned i = 0; i < droidvm_order_count && !lifting; ++i) lifting = !finger_down(&droidvm_order[i]);
    if (lifting && native_touch_hid_reports == droidvm_published_at) touch_wait_report(droidvm_published_at);
    for (;;) {
        unsigned i = 0;
        while (i < droidvm_order_count && finger_down(&droidvm_order[i])) ++i;
        if (i == droidvm_order_count) break;
        uint32_t before = native_touch_hid_reports;
        if (i + 1 < droidvm_order_count) {
            struct droidvm_finger lifted = droidvm_order[i];
            memmove(&droidvm_order[i], &droidvm_order[i + 1], (droidvm_order_count - i - 1) * sizeof(lifted));
            droidvm_order[droidvm_order_count - 1] = lifted;
            touch_publish(droidvm_order, droidvm_order_count);
            touch_wait_report(before);
            before = native_touch_hid_reports;
        }
        // Shortened on its own, so that a finger appended in this frame cannot take the lifted one's place unseen.
        touch_publish(droidvm_order, --droidvm_order_count);
        touch_wait_report(before);
    }
    for (unsigned i = 0; i < droidvm_order_count; ++i) next[n++] = droidvm_order[i];
    for (unsigned k = 0; k < droidvm_input_count; ++k) {
        const struct droidvm_input *d = &droidvm_inputs[k];
        for (unsigned s = 0; s <= DROIDVM_INPUT_SLOTS && n < DROIDVM_TOUCH_FINGERS; ++s) {
            bool down = s < DROIDVM_INPUT_SLOTS ? d->kind == KIND_TOUCH && d->slots[s].id >= 0 : d->kind == KIND_POINTER && d->pointer_down;
            if (!down) continue;
            bool known = false;
            for (unsigned i = 0; i < n && !known; ++i) known = next[i].device == k && next[i].slot == s;
            if (!known) {
                // Tracking ids grow with every new contact: successive fingers of one slot get different identifiers.
                uint8_t id = s < DROIDVM_INPUT_SLOTS ? (uint8_t)(1 + (uint32_t)d->slots[s].id % 120) : 127;
                next[n++] = (struct droidvm_finger){(uint8_t)k, (uint8_t)s, id, true, 0, 0};
            }
        }
    }
    for (unsigned i = 0; i < n; ++i) {
        const struct droidvm_input *d = &droidvm_inputs[next[i].device];
        int32_t x = next[i].slot < DROIDVM_INPUT_SLOTS ? d->slots[next[i].slot].x : d->pointer_x;
        int32_t y = next[i].slot < DROIDVM_INPUT_SLOTS ? d->slots[next[i].slot].y : d->pointer_y;
        next[i].x = input_scale(x, d->min_x, d->span_x, DROIDVM_TOUCH_AXIS_X);
        next[i].y = input_scale(y, d->min_y, d->span_y, DROIDVM_TOUCH_AXIS_Y);
        if (next[i].fresh) touch_snap(&next[i]);
        next[i].fresh = false;
    }
    memcpy(droidvm_order, next, n * sizeof(next[0]));
    droidvm_order_count = n;
    touch_publish(droidvm_order, droidvm_order_count);
}

// The keyboard: Linux key codes (what the app's virtio keyboard sends) as USB HID keyboard usages, and the eight modifier keys as
// the boot report's modifier bits. NativeHidInput sends each state as its report 1 when told (native_keyboard_set_sink, from this
// input thread), as the touch frames.
static const uint8_t droidvm_key_usages[128] = {
    [1] = 0x29, [2] = 0x1e, [3] = 0x1f, [4] = 0x20, [5] = 0x21, [6] = 0x22, [7] = 0x23, [8] = 0x24, [9] = 0x25, [10] = 0x26,
    [11] = 0x27, [12] = 0x2d, [13] = 0x2e, [14] = 0x2a, [15] = 0x2b, [16] = 0x14, [17] = 0x1a, [18] = 0x08, [19] = 0x15,
    [20] = 0x17, [21] = 0x1c, [22] = 0x18, [23] = 0x0c, [24] = 0x12, [25] = 0x13, [26] = 0x2f, [27] = 0x30, [28] = 0x28,
    [30] = 0x04, [31] = 0x16, [32] = 0x07, [33] = 0x09, [34] = 0x0a, [35] = 0x0b, [36] = 0x0d, [37] = 0x0e, [38] = 0x0f,
    [39] = 0x33, [40] = 0x34, [41] = 0x35, [43] = 0x31, [44] = 0x1d, [45] = 0x1b, [46] = 0x06, [47] = 0x19, [48] = 0x05,
    [49] = 0x11, [50] = 0x10, [51] = 0x36, [52] = 0x37, [53] = 0x38, [55] = 0x55, [57] = 0x2c, [58] = 0x39, [59] = 0x3a,
    [60] = 0x3b, [61] = 0x3c, [62] = 0x3d, [63] = 0x3e, [64] = 0x3f, [65] = 0x40, [66] = 0x41, [67] = 0x42, [68] = 0x43,
    [69] = 0x53, [70] = 0x47, [71] = 0x5f, [72] = 0x60, [73] = 0x61, [74] = 0x56, [75] = 0x5c, [76] = 0x5d, [77] = 0x5e,
    [78] = 0x57, [79] = 0x59, [80] = 0x5a, [81] = 0x5b, [82] = 0x62, [83] = 0x63, [86] = 0x64, [87] = 0x44, [88] = 0x45,
    [96] = 0x58, [98] = 0x54, [99] = 0x46, [102] = 0x4a, [103] = 0x52, [104] = 0x4b, [105] = 0x50, [106] = 0x4f, [107] = 0x4d,
    [108] = 0x51, [109] = 0x4e, [110] = 0x49, [111] = 0x4c, [127] = 0x65,
};
static uint8_t key_modifier(unsigned code) {
    switch (code) {
    case 29: return 0x01; case 42: return 0x02; case 56: return 0x04; case 125: return 0x08;   // left Ctrl, Shift, Alt, Meta
    case 97: return 0x10; case 54: return 0x20; case 100: return 0x40; case 126: return 0x80;  // right Ctrl, Shift, Alt, Meta
    default: return 0;
    }
}
static void (*droidvm_keyboard_sink)(void *, const uint8_t *);
static void *droidvm_keyboard_sink_context;
void native_keyboard_set_sink(void (*sink)(void *, const uint8_t *), void *context) {
    droidvm_keyboard_sink_context = context;
    __atomic_store_n(&droidvm_keyboard_sink, sink, __ATOMIC_RELEASE);
}
// A key down or up (auto-repeat is the guest's own, so the device's repeats are not passed on). A seventh key held is dropped.
static void keyboard_key(struct droidvm_input *d, unsigned code, bool down) {
    uint8_t bit = key_modifier(code);
    if (bit) {
        d->key_modifiers = down ? (uint8_t)(d->key_modifiers | bit) : (uint8_t)(d->key_modifiers & ~bit);
        d->keys_changed = true;
        return;
    }
    uint8_t usage = code < sizeof(droidvm_key_usages) ? droidvm_key_usages[code] : 0;
    if (!usage) return;
    unsigned i = 0;
    while (i < 6 && d->keys[i] != usage) ++i;
    if (down && i == 6) {
        for (i = 0; i < 6 && d->keys[i]; ++i) {}
        if (i == 6) return;
        d->keys[i] = usage;
    } else if (!down && i < 6) {
        memmove(&d->keys[i], &d->keys[i + 1], 5 - i);
        d->keys[5] = 0;
    } else {
        return;
    }
    d->keys_changed = true;
}
static void keyboard_publish(struct droidvm_input *d) {
    void (*sink)(void *, const uint8_t *) = __atomic_load_n(&droidvm_keyboard_sink, __ATOMIC_ACQUIRE);
    if (!d->keys_changed || !sink) return;
    uint8_t report[8] = {d->key_modifiers, 0};
    memcpy(report + 2, d->keys, sizeof(d->keys));
    d->keys_changed = false;
    sink(droidvm_keyboard_sink_context, report);
    droidvm_input_key_reports = droidvm_input_key_reports + 1;
}

// The pen's tilt in whole degrees and its polar angles, in integers (no floating point in the kernel): tan of whole degrees in
// 16.16, interpolated between them, and atan as the inverse of that table.
static const uint32_t pen_tan[90] = {
    0, 1144, 2289, 3435, 4583, 5734, 6888, 8047, 9210, 10380,
    11556, 12739, 13930, 15130, 16340, 17560, 18792, 20036, 21294, 22566,
    23853, 25157, 26478, 27818, 29179, 30560, 31964, 33392, 34846, 36327,
    37837, 39378, 40951, 42560, 44205, 45889, 47615, 49385, 51202, 53070,
    54991, 56970, 59009, 61113, 63287, 65536, 67865, 70279, 72785, 75391,
    78103, 80930, 83882, 86969, 90203, 93595, 97161, 100917, 104880, 109070,
    113512, 118230, 123255, 128622, 134369, 140542, 147196, 154393, 162207, 170727,
    180059, 190330, 201699, 214359, 228551, 244584, 262851, 283868, 308323, 337153,
    371673, 413778, 466313, 533748, 623533, 749080, 937208, 1250501, 1876705, 3754555,
};
#define PEN_DEGREES_PER_RADIAN_16 3754936u   // 180 / pi in 16.16
// tan of 0 <= degrees (16.16) < 89 degrees.
static uint32_t pen_tan_of(uint32_t degrees) {
    uint32_t n = degrees >> 16, f = degrees & 0xffff;
    if (n >= 89) return pen_tan[89];
    return pen_tan[n] + (uint32_t)(((uint64_t)(pen_tan[n + 1] - pen_tan[n]) * f) >> 16);
}
// atan of 0 <= t (16.16), in degrees (16.16).
static uint32_t pen_atan_of(uint32_t t) {
    if (t >= pen_tan[89]) return (90u << 16) - (uint32_t)(((uint64_t)PEN_DEGREES_PER_RADIAN_16 << 16) / t);
    unsigned lo = 0, hi = 89;
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (pen_tan[mid] <= t) lo = mid; else hi = mid;
    }
    return (lo << 16) + (uint32_t)(((uint64_t)(t - pen_tan[lo]) << 16) / (pen_tan[lo + 1] - pen_tan[lo]));
}
static uint32_t pen_isqrt(uint64_t v) {
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}
// Degrees (16.16) as IOFixed radians.
static int32_t pen_radians(int64_t degrees) {
    return (int32_t)(degrees * 65536 / PEN_DEGREES_PER_RADIAN_16);
}
// A tilt axis's value in degrees (16.16, clamped below 89): by its resolution in units per radian (evdev's for tilt), or as
// degrees when the device states none.
static int32_t pen_tilt_degrees(const struct droidvm_input *d, unsigned axis) {
    if (!d->pen_span[axis]) return 0;
    int64_t v = d->pen[axis];
    int64_t degrees = d->pen_tilt_res > 0 ? v * PEN_DEGREES_PER_RADIAN_16 / d->pen_tilt_res : v << 16;
    const int64_t bound = 89ll << 16;
    return (int32_t)(degrees > bound ? bound : degrees < -bound ? -bound : degrees);
}
// A pen axis scaled to 0..max-1 (0 when the device has none).
static int32_t pen_scaled(const struct droidvm_input *d, unsigned axis, int32_t max) {
    return d->pen_span[axis] ? input_scale(d->pen[axis], d->pen_min[axis], d->pen_span[axis], max) : 0;
}

static void (*droidvm_pen_sink)(void *, const int32_t *);
static void *droidvm_pen_sink_context;
void native_pen_set_sink(void (*sink)(void *, const int32_t *), void *context) {
    droidvm_pen_sink_context = context;
    __atomic_store_n(&droidvm_pen_sink, sink, __ATOMIC_RELEASE);
}

// One pen frame to the sink (PEN_OUT_*). The polar angles from the tilt: the cap end points along (tan tx, tan ty, 1), so the
// altitude from the normal is atan |(tan tx, tan ty)| and the azimuth atan2(tan ty, tan tx), negated for UIKit's y-up sense (a
// pen leaning towards +x and +y reached Safari's Pointer Events leaning towards -x unnegated); an upright pen has azimuth 0.
static void pen_publish(struct droidvm_input *d) {
    void (*sink)(void *, const int32_t *) = __atomic_load_n(&droidvm_pen_sink, __ATOMIC_ACQUIRE);
    bool in_range = d->pen_tip || d->pen_eraser;
    if (!sink || (!in_range && !d->pen_was_in_range)) return;
    d->pen_was_in_range = in_range;
    bool touch = in_range && d->pen_touch;
    int32_t tx = pen_tilt_degrees(d, PEN_TILT_X), ty = pen_tilt_degrees(d, PEN_TILT_Y);
    uint32_t a = pen_tan_of((uint32_t)(tx < 0 ? -tx : tx)), b = pen_tan_of((uint32_t)(ty < 0 ? -ty : ty));
    uint32_t r = pen_isqrt((uint64_t)a * a + (uint64_t)b * b);
    int64_t azimuth = 0;
    if (a || b) {
        // atan2 by quadrant from the angle of |y| / |x| in the first one
        int64_t first = a ? pen_atan_of((uint32_t)(((uint64_t)b << 16) / a)) : 90ll << 16;
        if (tx >= 0) azimuth = ty >= 0 ? first : -first;
        else azimuth = ty >= 0 ? (180ll << 16) - first : first - (180ll << 16);
    }
    int32_t pen[PEN_OUT_VALUES] = {
        [PEN_OUT_FLAGS] = (in_range ? 1 : 0) | (touch ? 2 : 0) | (in_range && d->pen_barrel ? 4 : 0) | (in_range && d->pen_eraser ? 8 : 0),
        [PEN_OUT_X] = input_scale(d->pen_x, d->min_x, d->span_x, 32768),
        [PEN_OUT_Y] = input_scale(d->pen_y, d->min_y, d->span_y, 32768),
        [PEN_OUT_PRESSURE] = touch ? pen_scaled(d, PEN_PRESSURE, 4096) : 0,
        [PEN_OUT_DISTANCE] = in_range && !touch ? pen_scaled(d, PEN_DISTANCE, 256) : 0,
        [PEN_OUT_TILT_X] = tx / 65536,
        [PEN_OUT_TILT_Y] = ty / 65536,
        [PEN_OUT_ALTITUDE] = pen_radians(pen_atan_of(r)),
        [PEN_OUT_AZIMUTH] = pen_radians(-azimuth),
    };
    sink(droidvm_pen_sink_context, pen);
    droidvm_input_pen_reports = droidvm_input_pen_reports + 1;
}

// The trackpad: its surface's size in hundredths of a millimetre, by its axes' resolution; a device that states none is taken as
// 150 mm wide with square units.
static void trackpad_extent(const struct droidvm_input *d, uint32_t *width, uint32_t *height) {
    *width = d->res_x > 0 ? (uint32_t)((int64_t)d->span_x * 100 / d->res_x) : 15000;
    *height = d->res_y > 0 ? (uint32_t)((int64_t)d->span_y * 100 / d->res_y) : (uint32_t)((int64_t)*width * d->span_y / d->span_x);
}

static void (*droidvm_trackpad_sink)(void *, const struct droidvm_trackpad_contact *, unsigned);
static void *droidvm_trackpad_sink_context;
void native_trackpad_set_sink(void (*sink)(void *, const struct droidvm_trackpad_contact *, unsigned), void *context) {
    droidvm_trackpad_sink_context = context;
    __atomic_store_n(&droidvm_trackpad_sink, sink, __ATOMIC_RELEASE);
}

// One trackpad frame to the sink: the contacts down, then those of the last frame gone from it, lifted where they were.
static void trackpad_publish(struct droidvm_input *d) {
    struct droidvm_trackpad_contact contacts[2 * DROIDVM_INPUT_SLOTS];
    uint32_t width, height;
    trackpad_extent(d, &width, &height);
    unsigned n = 0;
    for (unsigned s = 0; s < DROIDVM_INPUT_SLOTS; ++s) {
        if (d->slots[s].id < 0) continue;
        uint8_t major = d->major_span ? (uint8_t)input_scale(d->slots[s].major, d->major_min, d->major_span, 256) : 0;
        uint8_t minor = d->major_span ? (uint8_t)input_scale(d->slots[s].minor, d->major_min, d->major_span, 256) : 0;
        contacts[n++] = (struct droidvm_trackpad_contact){d->slots[s].id,
                                                          input_scale(d->slots[s].x, d->min_x, d->span_x, (int32_t)width),
                                                          input_scale(d->slots[s].y, d->min_y, d->span_y, (int32_t)height),
                                                          1, d->slots[s].tool == MT_TOOL_PALM, major, minor};
    }
    const unsigned down = n;
    for (unsigned i = 0; i < d->trackpad_last_count; ++i) {
        bool kept = false;
        for (unsigned k = 0; k < down && !kept; ++k) kept = contacts[k].id == d->trackpad_last[i].id;
        if (kept) continue;
        contacts[n] = d->trackpad_last[i];
        contacts[n++].down = 0;
    }
    memcpy(d->trackpad_last, contacts, down * sizeof(contacts[0]));
    d->trackpad_last_count = down;
    void (*sink)(void *, const struct droidvm_trackpad_contact *, unsigned) =
        __atomic_load_n(&droidvm_trackpad_sink, __ATOMIC_ACQUIRE);
    if (!sink || !n) return;
    sink(droidvm_trackpad_sink_context, contacts, n);
    droidvm_input_trackpad_reports = droidvm_input_trackpad_reports + 1;
}

static bool input_event(struct droidvm_input *d, const struct droidvm_input_event *e) {
    droidvm_input_events = droidvm_input_events + 1;
    if (e->type == EV_SYN) return e->code == SYN_REPORT;
    if (d->kind == KIND_TOUCH && e->type == EV_ABS) {
        if (e->code == ABS_MT_SLOT) d->slot = e->value >= 0 && e->value < DROIDVM_INPUT_SLOTS ? (unsigned)e->value : DROIDVM_INPUT_SLOTS;
        else if (d->slot < DROIDVM_INPUT_SLOTS) {
            if (e->code == ABS_MT_TRACKING_ID) d->slots[d->slot].id = e->value < 0 ? -1 : e->value;
            else if (e->code == ABS_MT_POSITION_X) d->slots[d->slot].x = e->value;
            else if (e->code == ABS_MT_POSITION_Y) d->slots[d->slot].y = e->value;
        }
    } else if (d->kind == KIND_TRACKPAD && e->type == EV_ABS) {
        if (e->code == ABS_MT_SLOT) d->slot = e->value >= 0 && e->value < DROIDVM_INPUT_SLOTS ? (unsigned)e->value : DROIDVM_INPUT_SLOTS;
        else if (d->slot < DROIDVM_INPUT_SLOTS) {
            switch (e->code) {
            case ABS_MT_TRACKING_ID:
                d->slots[d->slot].id = e->value < 0 ? -1 : e->value;
                if (e->value >= 0) d->slots[d->slot].tool = 0;
                break;
            case ABS_MT_POSITION_X: d->slots[d->slot].x = e->value; break;
            case ABS_MT_POSITION_Y: d->slots[d->slot].y = e->value; break;
            case ABS_MT_TOUCH_MAJOR: d->slots[d->slot].major = e->value; break;
            case ABS_MT_TOUCH_MINOR: d->slots[d->slot].minor = e->value; break;
            case ABS_MT_TOOL_TYPE: d->slots[d->slot].tool = e->value; break;
            default: break;
            }
        }
    } else if (d->kind == KIND_POINTER) {
        if (e->type == EV_ABS && e->code == ABS_X) d->pointer_x = e->value;
        else if (e->type == EV_ABS && e->code == ABS_Y) d->pointer_y = e->value;
        else if (e->type == EV_KEY && (e->code == BTN_LEFT || e->code == BTN_TOUCH)) d->pointer_down = e->value != 0;
    } else if (d->kind == KIND_KEYBOARD && e->type == EV_KEY && e->value != 2) {
        keyboard_key(d, e->code, e->value != 0);
    } else if (d->kind == KIND_MOTION && e->type == EV_ABS && e->code <= ABS_RZ) {
        d->motion[e->code] = e->value;
    } else if (d->kind == KIND_PEN && e->type == EV_ABS) {
        switch (e->code) {
        case ABS_X: d->pen_x = e->value; break;
        case ABS_Y: d->pen_y = e->value; break;
        case ABS_PRESSURE: d->pen[PEN_PRESSURE] = e->value; break;
        case ABS_DISTANCE: d->pen[PEN_DISTANCE] = e->value; break;
        case ABS_TILT_X: d->pen[PEN_TILT_X] = e->value; break;
        case ABS_TILT_Y: d->pen[PEN_TILT_Y] = e->value; break;
        default: break;
        }
    } else if (d->kind == KIND_PEN && e->type == EV_KEY) {
        bool down = e->value != 0;
        switch (e->code) {
        case BTN_TOOL_PEN: d->pen_tip = down; break;
        case BTN_TOOL_RUBBER: d->pen_eraser = down; break;
        case BTN_TOUCH: d->pen_touch = down; break;
        case BTN_STYLUS: case BTN_STYLUS2: d->pen_barrel = down; break;
        default: break;
        }
    }
    return false;
}

// Takes what the device wrote, frame by frame, and gives the buffers back.
static void input_poll(struct droidvm_input *d) {
    uint8_t *page = DROIDVM_INPUT_QUEUE(d->index, 0);
    volatile uint16_t *available = (volatile uint16_t *)(page + 1024);
    volatile uint16_t *used = (volatile uint16_t *)(page + 2048);
    const volatile uint32_t *ring = (const volatile uint32_t *)(page + 2052);
    unsigned returned = 0;
    uint16_t head = available[1];
    while (used[1] != d->used) {
        input_barrier();
        uint32_t id = ring[(d->used % d->size) * 2];
        if (id < d->size) {
            if (input_event(d, &DROIDVM_INPUT_BUFFER(d->index)[id])) {
                droidvm_input_frames = droidvm_input_frames + 1;
                if (d->kind == KIND_KEYBOARD) keyboard_publish(d);
                else if (d->kind == KIND_MOTION) motion_publish(d);
                else if (d->kind == KIND_PEN) pen_publish(d);
                else if (d->kind == KIND_TRACKPAD) trackpad_publish(d);
                else touch_frame();
            }
            available[2 + (head + returned) % d->size] = (uint16_t)id;
            ++returned;
        }
        d->used++;
    }
    if (returned) {
        input_barrier();
        available[1] = (uint16_t)(head + returned);
        input_barrier();
        input_write32(d, 0x50, 0);
    }
    uint32_t interrupts = input_read32(d, 0x60) & 3;
    if (interrupts) input_write32(d, 0x64, interrupts);
}

// The fingers down, published again a hair off where they are when no frame has come for DROIDVM_TOUCH_STILL_MS (see there).
// droidvm_order keeps the true positions; only the publication moves.
static void touch_still(void) {
    static uint64_t interval;
    static bool off;
    if (!interval) nanoseconds_to_absolutetime((uint64_t)DROIDVM_TOUCH_STILL_MS * NSEC_PER_MSEC, &interval);
    if (!droidvm_order_count || mach_absolute_time() - droidvm_published_time < interval) return;
    struct droidvm_finger noisy[DROIDVM_TOUCH_FINGERS];
    off = !off;
    for (unsigned i = 0; i < droidvm_order_count; ++i) {
        noisy[i] = droidvm_order[i];
        if (off) noisy[i].x += noisy[i].x < DROIDVM_TOUCH_AXIS_X - 1 ? 1 : -1;
    }
    touch_publish(noisy, droidvm_order_count);
    droidvm_input_still = droidvm_input_still + 1;
}

// A device's interrupt, in the primary interrupt context: the line is level, so the status is acknowledged here; the worker reads
// the queue.
static void input_interrupt(void *context) {
    const struct droidvm_input *d = context;
    uint32_t status = input_read32(d, 0x60) & 3;
    if (status) input_write32(d, 0x64, status);
    __atomic_fetch_add(&droidvm_input_irq_counts[0], 1, __ATOMIC_RELAXED);
    thread_wakeup_prim((event_t)&droidvm_input_irq_counts, FALSE, THREAD_AWAKENED);
}

// The event queues' VIRTQ_AVAIL_F_NO_INTERRUPT: off while the worker sleeps on the interrupts, on while it polls.
static void input_interrupts_on(bool on) {
    for (unsigned k = 0; k < droidvm_input_count; ++k)
        *(volatile uint16_t *)(DROIDVM_INPUT_QUEUE(droidvm_inputs[k].index, 0) + 1024) = on ? 0 : 1;
    input_barrier();
}

static void droidvm_input_worker(void *parameter, wait_result_t result) {
    (void)parameter; (void)result;
    bool interrupts = droidvm_input_count > 0, guarded = false, on = false;
    for (unsigned k = 0; k < droidvm_input_count; ++k) interrupts = interrupts && droidvm_inputs[k].interrupt;
    for (;;) {
        // Before the look: a frame the device returned before the flag changed raised no interrupt, and the look takes it.
        const bool want = interrupts && droidvm_input_irq;
        if (want != on) input_interrupts_on(on = want);
        // The interrupt count before the look: one taken during it, or after it but before the sleep is armed, makes the sleep
        // end at once.
        const uint64_t seen = __atomic_load_n(&droidvm_input_irq_counts[0], __ATOMIC_RELAXED), frames = droidvm_input_frames;
        for (unsigned k = 0; k < droidvm_input_count; ++k) input_poll(&droidvm_inputs[k]);
        if (guarded && droidvm_input_frames != frames) droidvm_input_irq_counts[3] = droidvm_input_irq_counts[3] + 1;
        touch_still();
        if (!on) {
            guarded = false;
            IOSleep(DROIDVM_INPUT_POLL_MS);
            continue;
        }
        uint64_t deadline;
        clock_interval_to_deadline(droidvm_order_count ? DROIDVM_INPUT_POLL_MS : DROIDVM_INPUT_GUARD_MS, NSEC_PER_MSEC, &deadline);
        assert_wait_deadline((event_t)&droidvm_input_irq_counts, THREAD_UNINT, deadline);
        droidvm_input_irq_counts[1] = droidvm_input_irq_counts[1] + 1;
        guarded = false;
        if (__atomic_load_n(&droidvm_input_irq_counts[0], __ATOMIC_RELAXED) != seen) {
            clear_wait(current_thread(), THREAD_AWAKENED);
        } else if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT) {
            droidvm_input_irq_counts[2] = droidvm_input_irq_counts[2] + 1;
            guarded = !droidvm_order_count;
        }
    }
}

static bool droidvm_motion_found, droidvm_pen_found;
static const struct droidvm_input *droidvm_trackpad;

// Whether the VM has a pen: HidInputProbe.cpp publishes the pen digitizer then (after droidvm_input_start, as for the motion
// sensors).
bool droidvm_pen_present(void) {
    return droidvm_pen_found;
}

// Whether the VM has a trackpad, and its surface's size in hundredths of a millimetre: HidInputProbe.cpp publishes a Magic Trackpad
// for it (DroidVMTrackpadHid.inc).
bool droidvm_trackpad_present(void) {
    return droidvm_trackpad != NULL;
}
void droidvm_trackpad_surface(uint32_t *width, uint32_t *height) {
    *width = *height = 0;
    if (droidvm_trackpad) trackpad_extent(droidvm_trackpad, width, height);
}

// Whether the VM has motion sensors. HidInputProbe.cpp publishes the accelerometer and gyroscope HID services (DroidVMMotionHid.inc)
// only then; it runs after droidvm_input_start, which finds the devices before it returns.
bool droidvm_motion_present(void) {
    return droidvm_motion_found;
}

// Called once from the late board probe (DroidVMStubs.c native_touch_probe).
void droidvm_input_start(void) {
    uint32_t enabled = 1;
    PE_parse_boot_argn("droidvm-input", &enabled, sizeof(enabled));
    DTEntry armio;
    const void *value;
    unsigned int size;
    uint64_t ranges[3];
    if (!enabled || SecureDTLookupEntry(NULL, "/arm-io", &armio) != kSuccess ||
        SecureDTGetProperty(armio, "ranges", &value, &size) != kSuccess || size < sizeof(ranges)) return;
    memcpy(ranges, value, sizeof(ranges));
    for (unsigned n = 0; n < DROIDVM_INPUT_DEVICES; ++n) {
        char path[32];
        snprintf(path, sizeof(path), "/arm-io/input-%u", n);
        DTEntry node;
        uint64_t reg[2];
        if (SecureDTLookupEntry(NULL, path, &node) != kSuccess) break;
        droidvm_input_found = droidvm_input_found + 1;
        if (SecureDTGetProperty(node, "reg", &value, &size) != kSuccess || size != sizeof(reg)) continue;
        memcpy(reg, value, sizeof(reg));
        if (reg[0] < ranges[0] || reg[1] < 0x108 || reg[1] > ranges[2] || reg[0] - ranges[0] > ranges[2] - reg[1]) continue;
        struct droidvm_input *d = &droidvm_inputs[droidvm_input_count];
        *d = (struct droidvm_input){.index = droidvm_input_count, .slot = 0};
        for (unsigned s = 0; s < DROIDVM_INPUT_SLOTS; ++s) d->slots[s].id = -1;
        d->registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
        if (!d->registers || input_read32(d, 0x00) != 0x74726976 || input_read32(d, 0x04) != 2 || input_read32(d, 0x08) != 18)
            continue;
        d->kind = input_identify(d);
        droidvm_input_kinds[droidvm_input_count] = d->kind;
        if (d->kind == KIND_NONE) {
            // Not used, but not left as the firmware left it either: the UEFI virtio drivers bind every input device,
            // and a device the guest never touches keeps whatever interrupt state they ended with.
            (void)input_reset(d);
            uint32_t interrupts = input_read32(d, 0x60) & 3;
            if (interrupts) input_write32(d, 0x64, interrupts);
            continue;
        }
        const char *why = input_start(d);
        if (why) {
            printf("droidvm input %s (%s): %s\n", path, droidvm_input_names[d->index], why);
            (void)input_reset(d);
            droidvm_input_kinds[droidvm_input_count] = KIND_NONE;
            continue;
        }
        printf("droidvm input %s: %s, %s\n", path, droidvm_input_names[d->index], d->kind == KIND_TOUCH ? "touch" :
               d->kind == KIND_POINTER ? "pointer" : d->kind == KIND_MOTION ? "motion sensors" : d->kind == KIND_PEN ? "pen" :
               d->kind == KIND_TRACKPAD ? "trackpad" : "keyboard");
        if (d->kind == KIND_MOTION) {
            droidvm_motion_register();
            droidvm_motion_found = true;
        }
        if (d->kind == KIND_PEN) droidvm_pen_found = true;
        if (d->kind == KIND_TRACKPAD && !droidvm_trackpad) droidvm_trackpad = d;
        // The window's interrupt (DarwinBoot copies it from the FDT); without one the worker keeps polling.
        if (SecureDTGetProperty(node, "droidvm,intid", &value, &size) == kSuccess && size == sizeof(uint32_t)) {
            uint32_t intid;
            memcpy(&intid, value, sizeof(intid));
            if (armcpu_spi_attach(intid, false, input_interrupt, d, NULL, NULL) == 0) {
                d->interrupt = true;
                droidvm_input_irq_counts[4] = droidvm_input_irq_counts[4] + 1;
            }
        }
        droidvm_input_count++;
    }
    droidvm_input_started = droidvm_input_count;
    thread_t thread;
    if (droidvm_input_count && kernel_thread_start(droidvm_input_worker, NULL, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}
