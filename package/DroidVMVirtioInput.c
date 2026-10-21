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
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

extern void IOSleep(unsigned milliseconds);

#define DROIDVM_INPUT_DEVICES 8
#define DROIDVM_INPUT_BUFFERS 64       // crosvm's event queue size
#define DROIDVM_INPUT_SLOTS 10
#define DROIDVM_INPUT_POLL_MS 4
#define DROIDVM_TOUCH_FINGERS 10       // NativeHidInput's report
#define DROIDVM_TOUCH_AXIS_X 30480
#define DROIDVM_TOUCH_AXIS_Y 20320
// A finger that comes down within this share (percent) of the screen's bottom starts at the bottom edge. iPadOS takes a swipe
// as the Home gesture only from the edge band (NativeHidInput: 48 of 2032 rows, ~19 px of 800), and on the phone that band
// lies in Android's own navigation gesture area, which keeps those touches for itself.
#define DROIDVM_TOUCH_BOTTOM_SNAP 4

enum { EV_SYN = 0, EV_KEY = 1, EV_ABS = 3 };
enum { SYN_REPORT = 0, ABS_X = 0, ABS_Y = 1, ABS_MT_SLOT = 0x2f, ABS_MT_POSITION_X = 0x35, ABS_MT_POSITION_Y = 0x36,
       ABS_MT_TRACKING_ID = 0x39, BTN_LEFT = 0x110, BTN_TOUCH = 0x14a, INPUT_PROP_DIRECT = 1 };
enum { CFG_ID_NAME = 1, CFG_PROP_BITS = 0x10, CFG_EV_BITS = 0x11, CFG_ABS_INFO = 0x12 };
enum { KIND_NONE, KIND_TOUCH, KIND_POINTER };

struct droidvm_input_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct droidvm_input_event { uint16_t type, code; int32_t value; };
_Static_assert(sizeof(struct droidvm_input_desc) == 16 && sizeof(struct droidvm_input_event) == 8, "VirtIO wire layout");

// One page per queue: descriptors at 0, the driver ring at 1024, the device ring at 2048 (64 entries fit).
static uint8_t droidvm_input_queues[DROIDVM_INPUT_DEVICES][2][4096] __attribute__((aligned(4096)));
static struct droidvm_input_event droidvm_input_buffers[DROIDVM_INPUT_DEVICES][DROIDVM_INPUT_BUFFERS] __attribute__((aligned(4096)));

struct droidvm_input {
    uintptr_t registers;
    unsigned kind, index;
    uint16_t size, used;
    int32_t min_x, span_x, min_y, span_y;
    unsigned slot;
    struct { int32_t id, x, y; } slots[DROIDVM_INPUT_SLOTS];   // id -1: no finger
    int32_t pointer_x, pointer_y;
    bool pointer_down;
};
static struct droidvm_input droidvm_inputs[DROIDVM_INPUT_DEVICES];
static unsigned droidvm_input_count;

// For the host rig (peek.py): devices found, recognised (kinds: 1 touch, 2 pointer, 0 left alone), events and frames seen.
volatile uint32_t droidvm_input_found, droidvm_input_started, droidvm_input_kinds[DROIDVM_INPUT_DEVICES];
volatile uint64_t droidvm_input_events, droidvm_input_frames, droidvm_input_published;
char droidvm_input_names[DROIDVM_INPUT_DEVICES][48];

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
    *(volatile uint16_t *)(page + 1024) = 1;   // VIRTQ_AVAIL_F_NO_INTERRUPT: this driver polls
    uint64_t physical = ml_static_vtop((vm_offset_t)page);
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
    uint8_t *events = droidvm_input_queues[d->index][0], *status = droidvm_input_queues[d->index][1];
    d->size = DROIDVM_INPUT_BUFFERS;
    if (!input_queue(d, 0, d->size, events)) return "event queue";
    if (!input_queue(d, 1, 8, status)) return "status queue";   // crosvm wants both; nothing is ever sent on it
    struct droidvm_input_desc *descriptors = (struct droidvm_input_desc *)events;
    volatile uint16_t *available = (volatile uint16_t *)(events + 1024);
    uint64_t buffers = ml_static_vtop((vm_offset_t)droidvm_input_buffers[d->index]);
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
    if (!input_bit(abs, nabs, ABS_MT_POSITION_X) && input_bit(abs, nabs, ABS_X) && input_bit(abs, nabs, ABS_Y) &&
        (input_bit(keys, nkeys, BTN_LEFT) || input_bit(keys, nkeys, BTN_TOUCH)) &&
        input_axis(d, ABS_X, &d->min_x, &d->span_x) && input_axis(d, ABS_Y, &d->min_y, &d->span_y))
        return KIND_POINTER;
    return KIND_NONE;
}

// The fingers in the order they were published: device, slot (DROIDVM_INPUT_SLOTS for a pointer) and HID identifier.
struct droidvm_finger { uint8_t device, slot, id; bool fresh; int32_t x, y; };
static struct droidvm_finger droidvm_order[DROIDVM_TOUCH_FINGERS];
static unsigned droidvm_order_count;

static int32_t input_scale(int32_t value, int32_t min, int32_t span, int32_t axis) {
    int64_t v = (int64_t)(value - min) * axis / span;
    return v < 0 ? 0 : v >= axis ? axis - 1 : (int32_t)v;
}

// The digitizer's report count when the finger set was last published.
static uint32_t droidvm_published_at;

static void touch_publish(const struct droidvm_finger *fingers, unsigned count) {
    droidvm_published_at = native_touch_hid_reports;
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
static void touch_frame(void) {
    struct droidvm_finger next[DROIDVM_TOUCH_FINGERS];
    unsigned n = 0;
    // The digitizer samples every 8 ms and only sees the newest set: a finger that lifts before the set it went down in
    // was reported would never have touched. A tap's down and up frames often arrive within one poll.
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
        if (next[i].fresh && next[i].y >= DROIDVM_TOUCH_AXIS_Y * (100 - DROIDVM_TOUCH_BOTTOM_SNAP) / 100)
            next[i].y = DROIDVM_TOUCH_AXIS_Y - 1;
        next[i].fresh = false;
    }
    memcpy(droidvm_order, next, n * sizeof(next[0]));
    droidvm_order_count = n;
    touch_publish(droidvm_order, droidvm_order_count);
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
    } else if (d->kind == KIND_POINTER) {
        if (e->type == EV_ABS && e->code == ABS_X) d->pointer_x = e->value;
        else if (e->type == EV_ABS && e->code == ABS_Y) d->pointer_y = e->value;
        else if (e->type == EV_KEY && (e->code == BTN_LEFT || e->code == BTN_TOUCH)) d->pointer_down = e->value != 0;
    }
    return false;
}

// Takes what the device wrote, frame by frame, and gives the buffers back.
static void input_poll(struct droidvm_input *d) {
    uint8_t *page = droidvm_input_queues[d->index][0];
    volatile uint16_t *available = (volatile uint16_t *)(page + 1024);
    volatile uint16_t *used = (volatile uint16_t *)(page + 2048);
    const volatile uint32_t *ring = (const volatile uint32_t *)(page + 2052);
    unsigned returned = 0;
    uint16_t head = available[1];
    while (used[1] != d->used) {
        input_barrier();
        uint32_t id = ring[(d->used % d->size) * 2];
        if (id < d->size) {
            if (input_event(d, &droidvm_input_buffers[d->index][id])) {
                droidvm_input_frames = droidvm_input_frames + 1;
                touch_frame();
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

static void droidvm_input_worker(void *parameter, wait_result_t result) {
    (void)parameter; (void)result;
    for (;;) {
        for (unsigned k = 0; k < droidvm_input_count; ++k) input_poll(&droidvm_inputs[k]);
        IOSleep(DROIDVM_INPUT_POLL_MS);
    }
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
        printf("droidvm input %s: %s, %s\n", path, droidvm_input_names[d->index], d->kind == KIND_TOUCH ? "touch" : "pointer");
        droidvm_input_count++;
    }
    droidvm_input_started = droidvm_input_count;
    thread_t thread;
    if (droidvm_input_count && kernel_thread_start(droidvm_input_worker, NULL, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}
