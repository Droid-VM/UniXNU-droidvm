// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The transport of crosvm's virtio-media devices (V4L2 over virtio: a camera, a stateful decoder, a stateful encoder) on the
// virtio-mmio transport. crosvm's --virtio-media devices (the DroidVM app's VPU peripheral: MediaCodec and Camera2 in helpers
// running as the app's uid) are moved onto the MMIO transport by the fork's --virtio-mmio-media, and make-droidvm-adt.py
// (--media-mmio) publishes their windows as /arm-io/virtio-media-N, compatible "droidvm,virtio-media", each with the
// media-host-pool property: the absolute GPA and size of the pre-shared media_host pool (crosvm --pre-alloc media-host-mb).
// The devices' MMAP buffers live there; MMAP answers with an offset into it.
//
// This file is the transport half: it maps each window, brings the device up (VIRTIO_F_VERSION_1, and ACCESS_PLATFORM where
// offered), keeps the event queue stocked, and runs one command at a time per device.
// Level SPIs acknowledge queue completions and wake command waiters and the event worker. The window's interrupt is its GIC
// INTID in the ADT: "droidvm,intid" (DarwinBoot's ADT built from the firmware's FDT) or "gic-spi" (macOS-DroidVM's). A window
// without either -- a static ADT names none -- or whose SPI cannot be attached is polled instead, as every device was before:
// a command waits a millisecond at a time, the event worker wakes every millisecond while events come and every 50 when quiet.
// The protocol (driver/protocol.h of the virtio-media repository):
//   commandq  OPEN -> session id; CLOSE; IOCTL(session, the _IO* number of a VIDIOC_*) with the ioctl's payload out (for _IOW
//             and _IOWR) and back (for _IOR and _IOWR), the response header in front of it; MMAP(session, offset) -> an offset
//             into the pool; MUNMAP. A response's status is 0 or a Linux errno. VIDIOC_QUERYCAP is the driver's to answer
//             (the device refuses it): the config space has its device_caps and card.
//   eventq    DQBUF (a struct v4l2_buffer and its planes), EVENT (a struct v4l2_event), ERROR (the session is gone).
// At start every device is checked once: OPEN, VIDIOC_ENUM_FMT on each of its queues, CLOSE; the result is in
// droidvm_media_status[N] for the host rig (peek.py).
//
// droidvm-media=0 leaves the devices alone.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#if DROIDVM_DEVICE_DYNAMIC
#include "DroidVMDmaRegion.h"
#endif

extern int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
extern void delay(int usec);

enum {
    MEDIA_MAX = 4,
    MEDIA_QUEUE = 64,                  // descriptors per queue
    MEDIA_COMMANDQ = 0, MEDIA_EVENTQ = 1,
    MEDIA_EVENTS = 16,                 // event buffers kept with the device
    MEDIA_EVENT_SIZE = 640,            // >= sizeof(struct virtio_media_event_dqbuf): 8 + 88 + 8 * 64
    MEDIA_PAYLOAD = 4096,              // the largest ioctl payload a command carries each way
    VIRTIO_ID_MEDIA = 48,
    VIRTQ_DESC_F_NEXT = 1, VIRTQ_DESC_F_WRITE = 2,
    // virtio-media commands and events
    CMD_OPEN = 1, CMD_CLOSE = 2, CMD_IOCTL = 3, CMD_MMAP = 4, CMD_MUNMAP = 5,
    EVT_ERROR = 0, EVT_DQBUF = 1, EVT_EVENT = 2,
    // V4L2 (videodev2.h): ioctl numbers, buffer types, capabilities
    VIDIOC_ENUM_FMT = 2,
    BUF_TYPE_VIDEO_CAPTURE = 1, BUF_TYPE_VIDEO_OUTPUT = 2, BUF_TYPE_VIDEO_CAPTURE_MPLANE = 9, BUF_TYPE_VIDEO_OUTPUT_MPLANE = 10,
    CAP_VIDEO_CAPTURE = 0x1, CAP_VIDEO_OUTPUT = 0x2, CAP_VIDEO_CAPTURE_MPLANE = 0x1000, CAP_VIDEO_OUTPUT_MPLANE = 0x2000,
    CAP_VIDEO_M2M_MPLANE = 0x4000, CAP_VIDEO_M2M = 0x8000,
};

struct media_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct v4l2_fmtdesc { uint32_t index, type, flags; char description[32]; uint32_t pixelformat, mbus_code, reserved[3]; };
_Static_assert(sizeof(struct media_desc) == 16 && sizeof(struct v4l2_fmtdesc) == 64,
               "virtio-media / V4L2 wire layout");

struct media_device {
    uintptr_t registers;
    uint64_t pool_base, pool_size;     // the media_host pool (absolute GPA)
    uint32_t device_caps, device_type;
    char card[33];
    uint16_t avail[2], used[2];        // our indices into each queue's available and used rings
    lck_mtx_t *lock;                   // one command at a time
    bool up;
    // The layer above takes copied events outside the transport lock on the event worker.
    void (*sink)(void *context, const uint8_t *event, uint32_t size);
    void *sink_context;
    uint32_t interrupt_id;             // 0: the ADT names none
    bool irq;                          // the SPI is attached; otherwise the device is polled
};

// Per device: one page per queue (descriptors at 0, the driver ring at 1024, the device ring at 2048), the event buffers,
// and the command's buffers (a command, its payload out, the response header, the payload back).
struct media_io_buffers {
    uint8_t command[64], response[64];
    uint8_t out[MEDIA_PAYLOAD], in[MEDIA_PAYLOAD];
};
#if DROIDVM_DEVICE_DYNAMIC
// DROIDVM_DEVICE_DYNAMIC (the iPadOS 27 board): the same buffers from one contiguous region allocated when the device starts
// (DroidVMDmaRegion.h), as pointers indexed as the arrays are.
static struct droidvm_dma media_dma;
static uint8_t (*media_rings)[2][4096];
static uint8_t (*media_events)[MEDIA_EVENTS][MEDIA_EVENT_SIZE];
static struct media_io_buffers *media_io;
static bool media_buffers(void) {
    const uint64_t rings = (uint64_t)MEDIA_MAX * 2 * 4096;
    const uint64_t events = ((uint64_t)MEDIA_MAX * MEDIA_EVENTS * MEDIA_EVENT_SIZE + 4095) & ~(uint64_t)4095;
    if (!droidvm_dma_alloc(&media_dma, rings + events + (uint64_t)MEDIA_MAX * sizeof(struct media_io_buffers))) return false;
    uint8_t *at = (uint8_t *)media_dma.base;
    media_rings = (uint8_t (*)[2][4096])at;
    media_events = (uint8_t (*)[MEDIA_EVENTS][MEDIA_EVENT_SIZE])(at + rings);
    media_io = (struct media_io_buffers *)(at + rings + events);
    return true;
}
#else
static uint8_t media_rings[MEDIA_MAX][2][4096] __attribute__((aligned(4096)));
static uint8_t media_events[MEDIA_MAX][MEDIA_EVENTS][MEDIA_EVENT_SIZE] __attribute__((aligned(4096)));
static struct media_io_buffers media_io[MEDIA_MAX] __attribute__((aligned(4096)));
static bool media_buffers(void) { return true; }
#endif
static struct media_device media[MEDIA_MAX];
static unsigned media_count;
static lck_grp_t *media_group;
static bool media_pending;

// For the host rig (peek.py).
volatile uint32_t droidvm_media_found, droidvm_media_up;
volatile uint32_t droidvm_media_stage[MEDIA_MAX];
volatile uint64_t droidvm_media_commands, droidvm_media_timeouts, droidvm_media_events[3];
char droidvm_media_status[MEDIA_MAX][192];

// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t media_read32(const struct media_device *d, unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(d->registers + offset) : "memory");
    return value;
}
static void media_write32(const struct media_device *d, unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(d->registers + offset) : "memory");
}
static void media_write64(const struct media_device *d, unsigned offset, uint64_t value) {
    media_write32(d, offset, (uint32_t)value);
    media_write32(d, offset + 4, (uint32_t)(value >> 32));
}
static void media_barrier(void) { __asm__ volatile("dmb osh" ::: "memory"); }
#if DROIDVM_DEVICE_DYNAMIC
static uint64_t pa(const void *p) { return droidvm_dma_pa(&media_dma, p); }
#else
static uint64_t pa(const void *p) { return ml_static_vtop((vm_offset_t)p); }
#endif

static unsigned index_of(const struct media_device *d) { return (unsigned)(d - media); }
static struct media_desc *descriptors(const struct media_device *d, unsigned q) { return (struct media_desc *)media_rings[index_of(d)][q]; }
static volatile uint16_t *available(const struct media_device *d, unsigned q) { return (volatile uint16_t *)(media_rings[index_of(d)][q] + 1024); }
static volatile uint16_t *used(const struct media_device *d, unsigned q) { return (volatile uint16_t *)(media_rings[index_of(d)][q] + 2048); }

// Puts the chain that starts at descriptor `head` on queue q and tells the device.
static void post(struct media_device *d, unsigned q, uint16_t head) {
    available(d, q)[2 + d->avail[q] % MEDIA_QUEUE] = head;
    media_barrier();
    available(d, q)[1] = ++d->avail[q];
    media_barrier();
    media_write32(d, 0x50, q);
}

// ACK before the GIC EOI/resample. No mutex or session callback runs in interrupt context.
static void media_interrupt(void *context) {
    struct media_device *d = context;
    uint32_t status = media_read32(d, 0x60) & 3;
    if (status) media_write32(d, 0x64, status);
    thread_wakeup((event_t)&d->used[MEDIA_COMMANDQ]);
    __atomic_store_n(&media_pending, true, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&media_pending);
}

// When a wait for the device ends: `deadline` with its interrupt, a millisecond from now (at most `deadline`) when polled.
static uint64_t wake_by(const struct media_device *d, uint64_t deadline) {
    if (d->irq) return deadline;
    uint64_t soon;
    clock_interval_to_deadline(1, NSEC_PER_MSEC, &soon);
    return soon < deadline ? soon : deadline;
}

// Copy and replenish under the transport lock, then deliver without it: a session may hold its lock across a command.
// Returns whether there was any.
static bool reap_events(struct media_device *d) {
    for (bool any = false;; any = true) {
        uint8_t copy[MEDIA_EVENT_SIZE];
        void (*sink)(void *, const uint8_t *, uint32_t);
        void *context;
        lck_mtx_lock(d->lock);
        if (used(d, MEDIA_EVENTQ)[1] == d->used[MEDIA_EVENTQ]) { lck_mtx_unlock(d->lock); return any; }
        media_barrier();
        const uint32_t *ring = (const uint32_t *)(used(d, MEDIA_EVENTQ) + 2);
        const uint32_t id = ring[(d->used[MEDIA_EVENTQ] % MEDIA_QUEUE) * 2];
        ++d->used[MEDIA_EVENTQ];
        memcpy(copy, media_events[index_of(d)][id % MEDIA_EVENTS], sizeof copy);
        uint32_t event;
        memcpy(&event, copy, 4);
        if (event <= EVT_EVENT) droidvm_media_events[event]++;
        sink = d->sink; context = d->sink_context;
        post(d, MEDIA_EVENTQ, (uint16_t)(id % MEDIA_EVENTS));
        lck_mtx_unlock(d->lock);
        if (sink) sink(context, copy, sizeof copy);
    }
}

// One command, synchronously: `command` (with `out` bytes of payload behind it) goes to the device, which writes a response
// of `response` bytes (its header first) and up to `in` bytes of payload back into media_io[].in. Returns the response's
// status (0 or a Linux errno), or -1 when the device did not answer.
// `extra`: a second device-readable buffer after the payload, at guest-physical `extra_pa` (a USERPTR buffer's scatter list, which
// the device reads on from the payload: it walks the chain's readable descriptors as one stream).
static int command(struct media_device *d, uint32_t command_size, uint32_t out, uint64_t extra_pa, uint32_t extra, uint32_t response,
                   uint32_t in) {
    struct media_desc *desc = descriptors(d, MEDIA_COMMANDQ);
    uint16_t n = 0;
    desc[n] = (struct media_desc){pa(media_io[index_of(d)].command), command_size, 0, 0};
    if (out) { desc[n].flags = VIRTQ_DESC_F_NEXT; desc[n].next = (uint16_t)(n + 1); ++n;
               desc[n] = (struct media_desc){pa(media_io[index_of(d)].out), out, 0, 0}; }
    if (extra) { desc[n].flags = VIRTQ_DESC_F_NEXT; desc[n].next = (uint16_t)(n + 1); ++n;
                 desc[n] = (struct media_desc){extra_pa, extra, 0, 0}; }
    if (response) { desc[n].flags |= VIRTQ_DESC_F_NEXT; desc[n].next = (uint16_t)(n + 1); ++n;
                    desc[n] = (struct media_desc){pa(media_io[index_of(d)].response), response, VIRTQ_DESC_F_WRITE, 0}; }
    if (in) { desc[n].flags |= VIRTQ_DESC_F_NEXT; desc[n].next = (uint16_t)(n + 1); ++n;
              desc[n] = (struct media_desc){pa(media_io[index_of(d)].in), in, VIRTQ_DESC_F_WRITE, 0}; }
    memset(media_io[index_of(d)].response, 0, sizeof(media_io[0].response));
    droidvm_media_commands++;
    post(d, MEDIA_COMMANDQ, 0);
    uint64_t deadline;
    clock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);
    for (;;) {
        // Register before checking the used index so a concurrent IRQ cannot be lost.
        assert_wait_deadline((event_t)&d->used[MEDIA_COMMANDQ], THREAD_UNINT, wake_by(d, deadline));
        if (used(d, MEDIA_COMMANDQ)[1] != d->used[MEDIA_COMMANDQ]) {
            clear_wait(current_thread(), THREAD_AWAKENED);
            break;
        }
        if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT && mach_absolute_time() >= deadline) {
            droidvm_media_timeouts++;
            return -1;
        }
    }
    media_barrier();
    ++d->used[MEDIA_COMMANDQ];
    if (!response) return 0;
    uint32_t status;
    memcpy(&status, media_io[index_of(d)].response, 4);
    return (int)status;
}

// The commands, for the layers above (the device's lock held around each).
int droidvm_media_open(unsigned device, uint32_t *session) {
    if (device >= media_count || !media[device].up) return -1;
    struct media_device *d = &media[device];
    lck_mtx_lock(d->lock);
    const uint32_t cmd[2] = {CMD_OPEN, 0};
    memcpy(media_io[device].command, cmd, sizeof(cmd));
    int status = command(d, sizeof(cmd), 0, 0, 0, 16, 0);
    if (!status) memcpy(session, media_io[device].response + 8, 4);
    lck_mtx_unlock(d->lock);
    return status;
}

int droidvm_media_close(unsigned device, uint32_t session) {
    if (device >= media_count || !media[device].up) return -1;
    struct media_device *d = &media[device];
    lck_mtx_lock(d->lock);
    const uint32_t cmd[4] = {CMD_CLOSE, 0, session, 0};
    memcpy(media_io[device].command, cmd, sizeof(cmd));
    int status = command(d, sizeof(cmd), 0, 0, 0, 0, 0);
    lck_mtx_unlock(d->lock);
    return status;
}

// An ioctl: `out` bytes of `payload` go to the device (an _IOW or _IOWR ioctl), and `in` bytes come back into it (_IOR or
// _IOWR; the device writes the whole structure back, so `in` is the structure's size).
int droidvm_media_ioctl_sg(unsigned device, uint32_t session, uint32_t code, void *payload, uint32_t out, uint32_t in, uint64_t sg,
                           uint32_t sg_bytes);
int droidvm_media_ioctl(unsigned device, uint32_t session, uint32_t code, void *payload, uint32_t out, uint32_t in) {
    return droidvm_media_ioctl_sg(device, session, code, payload, out, in, 0, 0);
}

// The same with a scatter list after the payload (`sg_bytes` of it at guest-physical `sg`): VIDIOC_QBUF of a USERPTR buffer, whose
// v4l2_buffer is followed by the {start, length} entries of each plane's guest pages.
int droidvm_media_ioctl_sg(unsigned device, uint32_t session, uint32_t code, void *payload, uint32_t out, uint32_t in, uint64_t sg,
                           uint32_t sg_bytes) {
    if (device >= media_count || !media[device].up || out > MEDIA_PAYLOAD || in > MEDIA_PAYLOAD || (sg_bytes && !out)) return -1;
    struct media_device *d = &media[device];
    lck_mtx_lock(d->lock);
    const uint32_t cmd[4] = {CMD_IOCTL, 0, session, code};
    memcpy(media_io[device].command, cmd, sizeof(cmd));
    if (out) memcpy(media_io[device].out, payload, out);
    int status = command(d, sizeof(cmd), out, sg, sg_bytes, 8, in);
    if (!status && in) memcpy(payload, media_io[device].in, in);
    lck_mtx_unlock(d->lock);
    return status;
}

// MMAP: where the buffer at `offset` (from VIDIOC_QUERYBUF) lies, as an absolute GPA in the media_host pool.
int droidvm_media_mmap(unsigned device, uint32_t session, uint32_t offset, bool writable, uint64_t *gpa, uint64_t *length) {
    if (device >= media_count || !media[device].up) return -1;
    struct media_device *d = &media[device];
    lck_mtx_lock(d->lock);
    const uint32_t cmd[5] = {CMD_MMAP, 0, session, writable ? 1u : 0u, offset};
    memcpy(media_io[device].command, cmd, sizeof(cmd));
    int status = command(d, sizeof(cmd), 0, 0, 0, 24, 0);
    if (!status) {
        uint64_t at, size;
        memcpy(&at, media_io[device].response + 8, 8);
        memcpy(&size, media_io[device].response + 16, 8);
        if (at > d->pool_size || size > d->pool_size - at) status = -1;
        else { *gpa = d->pool_base + at; *length = size; }
    }
    lck_mtx_unlock(d->lock);
    return status;
}

int droidvm_media_munmap(unsigned device, uint64_t gpa) {
    if (device >= media_count || !media[device].up || gpa < media[device].pool_base) return -1;
    struct media_device *d = &media[device];
    lck_mtx_lock(d->lock);
    const uint64_t at = gpa - d->pool_base;
    uint32_t cmd[4] = {CMD_MUNMAP, 0, (uint32_t)at, (uint32_t)(at >> 32)};
    memcpy(media_io[device].command, cmd, sizeof(cmd));
    int status = command(d, sizeof(cmd), 0, 0, 0, 8, 0);
    lck_mtx_unlock(d->lock);
    return status;
}

unsigned droidvm_media_devices(void) { return media_count; }
uint32_t droidvm_media_caps(unsigned device) { return device < media_count ? media[device].device_caps : 0; }
const char *droidvm_media_card(unsigned device) { return device < media_count ? media[device].card : ""; }
bool droidvm_media_is_up(unsigned device) { return device < media_count && media[device].up; }

// The layer above's event sink, set once before its first session; its code/context stay alive with the transport.
void droidvm_media_set_sink(unsigned device, void (*sink)(void *, const uint8_t *, uint32_t), void *context) {
    if (device >= media_count) return;
    lck_mtx_lock(media[device].lock);
    media[device].sink_context = context;
    media[device].sink = sink;
    lck_mtx_unlock(media[device].lock);
}

static const char *bring_up(struct media_device *d) {
    media_write32(d, 0x70, 0);
    for (unsigned i = 0; media_read32(d, 0x70); ++i) { if (i == 1000) return "reset timeout"; delay(1000); }
    media_write32(d, 0x70, 1);          // ACKNOWLEDGE
    media_write32(d, 0x70, 3);          // DRIVER
    media_write32(d, 0x14, 1); uint32_t high = media_read32(d, 0x10);
    if (!(high & 1)) return "no VERSION_1";
    // VERSION_1 and, on a pseudo-unprotected VM, ACCESS_PLATFORM (see DroidVMVirtioSnd.c); virtio-media has no low bit we need.
    high &= 3;
    media_write32(d, 0x24, 0); media_write32(d, 0x20, 0);
    media_write32(d, 0x24, 1); media_write32(d, 0x20, high);
    media_write32(d, 0x70, 11);         // FEATURES_OK
    if (!(media_read32(d, 0x70) & 8)) return "features refused";
    for (unsigned q = 0; q < 2; ++q) {
        memset(media_rings[index_of(d)][q], 0, 4096);
        *(volatile uint16_t *)(media_rings[index_of(d)][q] + 1024) = 0;   // used-buffer interrupts
        media_write32(d, 0x30, q);
        if (media_read32(d, 0x44) || media_read32(d, 0x34) < MEDIA_QUEUE) return "queue unavailable or too small";
        const uint64_t base = pa(media_rings[index_of(d)][q]);
        media_write32(d, 0x38, MEDIA_QUEUE);
        media_write64(d, 0x80, base); media_write64(d, 0x90, base + 1024); media_write64(d, 0xa0, base + 2048);
        media_write32(d, 0x44, 1);
    }
    d->irq = d->interrupt_id && !armcpu_spi_attach(d->interrupt_id, false, media_interrupt, d, NULL, NULL);
    media_barrier();
    media_write32(d, 0x70, 15);         // DRIVER_OK
    if ((media_read32(d, 0x70) & 0x4f) != 15) return "device not ready";
    d->device_caps = media_read32(d, 0x100);
    d->device_type = media_read32(d, 0x104);
    for (unsigned i = 0; i < 32; i += 4) {
        const uint32_t word = media_read32(d, 0x108 + i);
        memcpy(d->card + i, &word, 4);
    }
    d->card[32] = 0;
    struct media_desc *desc = descriptors(d, MEDIA_EVENTQ);
    for (uint16_t e = 0; e < MEDIA_EVENTS; ++e) {
        desc[e] = (struct media_desc){pa(media_events[index_of(d)][e]), MEDIA_EVENT_SIZE, VIRTQ_DESC_F_WRITE, 0};
        post(d, MEDIA_EVENTQ, e);
    }
    return NULL;
}

// The formats of one queue as FourCCs, appended to `line`.
static void list_formats(unsigned device, uint32_t session, uint32_t type, const char *label, char *line, size_t size) {
    size_t at = strlen(line);
    snprintf(line + at, size - at, " %s:", label);
    for (uint32_t index = 0; index < 16; ++index) {
        struct v4l2_fmtdesc f = {.index = index, .type = type};
        if (droidvm_media_ioctl(device, session, VIDIOC_ENUM_FMT, &f, sizeof(f), sizeof(f))) break;
        at = strlen(line);
        snprintf(line + at, size - at, " %.4s", (const char *)&f.pixelformat);
    }
}

// The start-up check of one device: a session and the formats of its queues.
static void check(unsigned device) {
    char *line = droidvm_media_status[device];
    const size_t size = sizeof(droidvm_media_status[0]);
    uint32_t session;
    int status = droidvm_media_open(device, &session);
    if (status) { snprintf(line, size, "%s: OPEN failed (%d)", media[device].card, status); return; }
    droidvm_media_stage[device] = 4;
    const uint32_t caps = media[device].device_caps;
    snprintf(line, size, "%s caps %#x", media[device].card, caps);
    const bool mplane = caps & (CAP_VIDEO_M2M_MPLANE | CAP_VIDEO_CAPTURE_MPLANE | CAP_VIDEO_OUTPUT_MPLANE);
    if (caps & (CAP_VIDEO_M2M | CAP_VIDEO_M2M_MPLANE | CAP_VIDEO_OUTPUT | CAP_VIDEO_OUTPUT_MPLANE))
        list_formats(device, session, mplane ? BUF_TYPE_VIDEO_OUTPUT_MPLANE : BUF_TYPE_VIDEO_OUTPUT, "out", line, size);
    if (caps & (CAP_VIDEO_M2M | CAP_VIDEO_M2M_MPLANE | CAP_VIDEO_CAPTURE | CAP_VIDEO_CAPTURE_MPLANE))
        list_formats(device, session, mplane ? BUF_TYPE_VIDEO_CAPTURE_MPLANE : BUF_TYPE_VIDEO_CAPTURE, "cap", line, size);
    droidvm_media_stage[device] = 5;
    droidvm_media_close(device, session);
}

static void media_thread(void *arg, wait_result_t result) {
    (void)arg; (void)result;
    for (unsigned device = 0; device < media_count; ++device)
        if (media[device].up) check(device);
    // Sleep until an IRQ; an interrupt arriving before assert_wait is retained in media_pending. With a polled device among
    // them, wake on a timer as well: every millisecond while events come, every 50 once 100 polls in a row found none.
    bool polled = false;
    for (unsigned device = 0; device < media_count; ++device)
        polled |= media[device].up && !media[device].irq;
    for (unsigned quiet = 0;;) {
        if (polled)
            assert_wait_timeout((event_t)&media_pending, THREAD_UNINT, quiet < 100 ? 1 : 50, NSEC_PER_MSEC);
        else
            assert_wait((event_t)&media_pending, THREAD_UNINT);
        if (__atomic_exchange_n(&media_pending, false, __ATOMIC_ACQ_REL))
            clear_wait(current_thread(), THREAD_AWAKENED);
        else
            thread_block(THREAD_CONTINUE_NULL);
        bool any = false;
        for (unsigned device = 0; device < media_count; ++device)
            if (media[device].up) any |= reap_events(&media[device]);
        quiet = any ? 0 : quiet + 1;
    }
}

// Called once from the late board probe (DroidVMStubs.c native_touch_probe).
void droidvm_media_start(void) {
    uint32_t enabled = 1;
    PE_parse_boot_argn("droidvm-media", &enabled, sizeof(enabled));
    DTEntry armio, node;
    const void *value;
    unsigned int size;
    uint64_t ranges[3], reg[2];
    if (!enabled || SecureDTLookupEntry(NULL, "/arm-io", &armio) != kSuccess ||
        SecureDTGetProperty(armio, "ranges", &value, &size) != kSuccess || size < sizeof(ranges)) return;
    memcpy(ranges, value, sizeof(ranges));
    if (!(media_group = lck_grp_alloc_init("droidvm-media", LCK_GRP_ATTR_NULL)) || !media_buffers()) return;
    for (unsigned n = 0; n < MEDIA_MAX; ++n) {
        char path[32];
        snprintf(path, sizeof(path), "/arm-io/virtio-media-%u", n);
        if (SecureDTLookupEntry(NULL, path, &node) != kSuccess) break;
        struct media_device *d = &media[media_count];
        char *line = droidvm_media_status[media_count];
        const size_t line_size = sizeof(droidvm_media_status[0]);
        ++media_count;
        droidvm_media_found = media_count;
        if (SecureDTGetProperty(node, "media-host-pool", &value, &size) == kSuccess && size == 16) {
            memcpy(&d->pool_base, value, 8);
            memcpy(&d->pool_size, (const uint8_t *)value + 8, 8);
        }
        if ((SecureDTGetProperty(node, "droidvm,intid", &value, &size) == kSuccess ||
             SecureDTGetProperty(node, "gic-spi", &value, &size) == kSuccess) && size == sizeof d->interrupt_id)
            memcpy(&d->interrupt_id, value, size);
        if (SecureDTGetProperty(node, "reg", &value, &size) != kSuccess || size != sizeof(reg)) continue;
        memcpy(reg, value, sizeof(reg));
        if (reg[0] < ranges[0] || reg[1] < 0x110 || reg[1] > ranges[2] || reg[0] - ranges[0] > ranges[2] - reg[1]) continue;
        droidvm_media_stage[n] = 1;
        d->registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
        if (!d->registers || media_read32(d, 0x00) != 0x74726976 || media_read32(d, 0x04) != 2 ||
            media_read32(d, 0x08) != VIRTIO_ID_MEDIA) {
            snprintf(line, line_size, "no virtio-media at %s", path);
            continue;
        }
        droidvm_media_stage[n] = 2;
        if (!(d->lock = lck_mtx_alloc_init(media_group, LCK_ATTR_NULL))) { snprintf(line, line_size, "no lock"); continue; }
        const char *why = bring_up(d);
        if (why) {
            snprintf(line, line_size, "%s", why);
            media_write32(d, 0x70, 0);
            continue;
        }
        d->up = true;
        droidvm_media_up++;
        droidvm_media_stage[n] = 3;
        snprintf(line, line_size, "%s: up, caps %#x type %u, pool %#llx+%#llx, %s", d->card, d->device_caps, d->device_type,
                 d->pool_base, d->pool_size, d->irq ? "interrupt" : "polled");
    }
    if (droidvm_media_up) {
        thread_t thread;
        if (kernel_thread_start(media_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    }
}
