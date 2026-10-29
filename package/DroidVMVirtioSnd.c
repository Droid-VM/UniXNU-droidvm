// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The guest's speaker and microphone, carried over crosvm's virtio-snd on the virtio-mmio transport. crosvm's --virtio-snd card
// (the DroidVM app's VIRTIO_SOUND peripheral: AAudio in a helper running as the app's uid) is moved onto the MMIO transport by the
// fork's --virtio-mmio-snd, and make-droidvm-adt.py (--snd-mmio) publishes its window as /arm-io/virtio-snd, compatible
// "droidvm,virtio-snd". This file is the transport half: it maps that window, brings the device up (VIRTIO_F_VERSION_1 only),
// picks the first output and the first input PCM stream, sets them to 48 kHz S16 and moves PCM periods on the transmit and
// receive queues, polling as every virtio driver of this guest does (the queues are NO_INTERRUPT).
//
// Above it the device is iOS's built-in audio device (DroidVMAudio.cpp, an IOAudio2Device). The engine there calls, from its
// one thread, in periods of 240 frames (5 ms):
//   droidvm_snd_running(output, input)     start or stop the streams (called with the engine thread not running)
//   droidvm_snd_play(frames, count)         queue one period of interleaved stereo S16 for the speaker
//   droidvm_snd_capture(frames, count)      take one period of mono S16 from the microphone, if the device has one ready
// Without the device the engine runs on its own clock and plays into silence.
//
// droidvm-snd=0 leaves the device alone.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#if DROIDVM_DEVICE_DYNAMIC
#include "DroidVMDmaRegion.h"
#endif

extern void IOSleep(unsigned milliseconds);
extern void delay(int usec);
extern int droidvm_audio_attach(void);

enum {
    SND_SLOTS = 16,                  // periods in flight per direction (80 ms)
    SND_QUEUE = 64,                  // descriptors per queue: three per slot (header, data, status)
    SND_PERIOD_FRAMES = 240,
    SND_OUT_CHANNELS = 2, SND_IN_CHANNELS = 1,
    SND_MAX_PERIOD = SND_PERIOD_FRAMES * 2 * 2,   // a stereo S16 period, the most either direction carries
    SND_CONTROL = 0, SND_EVENT = 1, SND_TX = 2, SND_RX = 3,
    VIRTQ_DESC_F_NEXT = 1, VIRTQ_DESC_F_WRITE = 2,
    // virtio-snd (VIRTIO 1.2, 5.14)
    SND_R_PCM_INFO = 0x0100, SND_R_PCM_SET_PARAMS = 0x0101, SND_R_PCM_PREPARE = 0x0102, SND_R_PCM_RELEASE = 0x0103,
    SND_R_PCM_START = 0x0104, SND_R_PCM_STOP = 0x0105, SND_S_OK = 0x8000,
    SND_D_OUTPUT = 0, SND_D_INPUT = 1, SND_PCM_FMT_S16 = 5, SND_PCM_RATE_48000 = 7,
};

struct snd_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct snd_pcm_info { uint32_t hda_fn_nid, features; uint64_t formats, rates; uint8_t direction, channels_min, channels_max, padding[5]; };
struct snd_set_params { uint32_t code, stream_id, buffer_bytes, period_bytes, features; uint8_t channels, format, rate, padding; };
struct snd_slot { uint32_t stream_id; uint32_t status[2]; uint8_t data[SND_MAX_PERIOD]; };
_Static_assert(sizeof(struct snd_desc) == 16 && sizeof(struct snd_pcm_info) == 32 && sizeof(struct snd_set_params) == 24,
               "virtio-snd wire layout");

// One page per queue: descriptors at 0, the driver (available) ring at 1024, the device (used) ring at 2048.
#if DROIDVM_DEVICE_DYNAMIC
// DROIDVM_DEVICE_DYNAMIC (the iPadOS 27 board): the same buffers from one contiguous region allocated when the device starts
// (DroidVMDmaRegion.h), as pointers indexed as the arrays are.
static struct droidvm_dma snd_dma;
static uint8_t (*snd_rings)[4096];
static struct snd_slot *snd_tx, *snd_rx;
static uint8_t *snd_control_request, *snd_control_reply;
static bool snd_buffers(void) {
    const uint64_t slots = ((uint64_t)SND_SLOTS * sizeof(struct snd_slot) + 4095) & ~(uint64_t)4095;
    if (!droidvm_dma_alloc(&snd_dma, 4 * 4096 + 2 * slots + 4096)) return false;
    uint8_t *at = (uint8_t *)snd_dma.base;
    snd_rings = (uint8_t (*)[4096])at;
    snd_tx = (struct snd_slot *)(at + 4 * 4096);
    snd_rx = (struct snd_slot *)(at + 4 * 4096 + slots);
    snd_control_request = at + 4 * 4096 + 2 * slots;
    snd_control_reply = snd_control_request + 1024;
    return true;
}
#else
static uint8_t snd_rings[4][4096] __attribute__((aligned(4096)));
static struct snd_slot snd_tx[SND_SLOTS] __attribute__((aligned(4096)));
static struct snd_slot snd_rx[SND_SLOTS] __attribute__((aligned(4096)));
static uint8_t snd_control_request[256] __attribute__((aligned(256))), snd_control_reply[1024] __attribute__((aligned(1024)));
static bool snd_buffers(void) { return true; }
#endif

static struct {
    uintptr_t registers;
    int32_t out_stream, in_stream;    // -1: none
    uint8_t in_channels;              // what the input stream takes (its minimum when that is above one)
    uint16_t avail[4], used[4];       // our indices into each queue's available and used rings
    uint16_t tx_free;                 // transmit slots not with the device
    uint8_t tx_next;                  // the next transmit slot to fill (they return in order)
    bool up, out_running, in_running;
} snd;

// For the host rig (peek.py).
volatile uint32_t droidvm_snd_found, droidvm_snd_stage, droidvm_snd_streams;
volatile uint64_t droidvm_snd_tx_periods, droidvm_snd_tx_drops, droidvm_snd_rx_periods, droidvm_snd_rx_empty, droidvm_snd_errors;
volatile uint32_t droidvm_snd_selftest_pushed, droidvm_snd_selftest_done;
char droidvm_snd_status[64];

// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t snd_read32(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(snd.registers + offset) : "memory");
    return value;
}
static void snd_write32(unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(snd.registers + offset) : "memory");
}
static void snd_write64(unsigned offset, uint64_t value) {
    snd_write32(offset, (uint32_t)value);
    snd_write32(offset + 4, (uint32_t)(value >> 32));
}
static void snd_barrier(void) { __asm__ volatile("dmb osh" ::: "memory"); }
#if DROIDVM_DEVICE_DYNAMIC
static uint64_t pa(const void *p) { return droidvm_dma_pa(&snd_dma, p); }
#else
static uint64_t pa(const void *p) { return ml_static_vtop((vm_offset_t)p); }
#endif

static struct snd_desc *descriptors(unsigned q) { return (struct snd_desc *)snd_rings[q]; }
static volatile uint16_t *available(unsigned q) { return (volatile uint16_t *)(snd_rings[q] + 1024); }
static volatile uint16_t *used(unsigned q) { return (volatile uint16_t *)(snd_rings[q] + 2048); }

// Puts the chain that starts at descriptor `head` on queue q and tells the device.
static void post(unsigned q, uint16_t head) {
    available(q)[2 + snd.avail[q] % SND_QUEUE] = head;
    snd_barrier();
    available(q)[1] = ++snd.avail[q];
    snd_barrier();
    snd_write32(0x50, q);
}

// One control request and its reply, synchronously (the control queue has a single chain in flight). The request and reply
// buffers are static; the caller fills the request and reads the reply.
static bool control(uint32_t request_size, uint32_t reply_size) {
    struct snd_desc *d = descriptors(SND_CONTROL);
    d[0] = (struct snd_desc){pa(snd_control_request), request_size, VIRTQ_DESC_F_NEXT, 1};
    d[1] = (struct snd_desc){pa(snd_control_reply), reply_size, VIRTQ_DESC_F_WRITE, 0};
    memset(snd_control_reply, 0, reply_size);
    post(SND_CONTROL, 0);
    for (unsigned waited = 0; used(SND_CONTROL)[1] == snd.used[SND_CONTROL]; ++waited) {
        if (waited == 2000) { droidvm_snd_errors++; return false; }   // 2 s
        IOSleep(1);
    }
    snd_barrier();
    ++snd.used[SND_CONTROL];
    uint32_t status;
    memcpy(&status, snd_control_reply, 4);
    if (status != SND_S_OK) droidvm_snd_errors++;
    return status == SND_S_OK;
}

static bool pcm_command(uint32_t code, int32_t stream) {
    uint32_t request[2] = {code, (uint32_t)stream};
    memcpy(snd_control_request, request, sizeof(request));
    return control(sizeof(request), 4);
}

static bool set_params(int32_t stream, uint8_t channels) {
    const uint32_t period = SND_PERIOD_FRAMES * 2 * channels;
    struct snd_set_params p = {SND_R_PCM_SET_PARAMS, (uint32_t)stream, period * SND_SLOTS, period, 0, channels,
                               SND_PCM_FMT_S16, SND_PCM_RATE_48000, 0};
    memcpy(snd_control_request, &p, sizeof(p));
    return control(sizeof(p), 4) && pcm_command(SND_R_PCM_PREPARE, stream);
}

// The receive slots all go to the device at once; each comes back holding one period.
static void rx_post_all(void) {
    struct snd_desc *d = descriptors(SND_RX);
    const uint32_t period = SND_PERIOD_FRAMES * 2 * snd.in_channels;
    for (uint16_t s = 0; s < SND_SLOTS; ++s) {
        snd_rx[s].stream_id = (uint32_t)snd.in_stream;
        d[3 * s] = (struct snd_desc){pa(&snd_rx[s].stream_id), 4, VIRTQ_DESC_F_NEXT, (uint16_t)(3 * s + 1)};
        d[3 * s + 1] = (struct snd_desc){pa(snd_rx[s].data), period, VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE, (uint16_t)(3 * s + 2)};
        d[3 * s + 2] = (struct snd_desc){pa(snd_rx[s].status), 8, VIRTQ_DESC_F_WRITE, 0};
        post(SND_RX, (uint16_t)(3 * s));
    }
}

// Takes the transmit chains the device has played back off the used ring.
static void tx_reap(void) {
    while (used(SND_TX)[1] != snd.used[SND_TX]) {
        snd_barrier();
        const uint32_t *ring = (const uint32_t *)(used(SND_TX) + 2);
        const uint32_t id = ring[(snd.used[SND_TX] % SND_QUEUE) * 2];
        if (snd_tx[id / 3 % SND_SLOTS].status[0] != SND_S_OK) droidvm_snd_errors++;
        ++snd.used[SND_TX];
        ++snd.tx_free;
    }
}

uint32_t droidvm_snd_play(const int16_t *frames, uint32_t count) {
    if (!snd.out_running || count != SND_PERIOD_FRAMES) return count;
    tx_reap();
    if (!snd.tx_free) { droidvm_snd_tx_drops++; return 0; }   // the host is behind: this period is dropped
    const uint8_t s = snd.tx_next;
    snd.tx_next = (uint8_t)((s + 1) % SND_SLOTS);
    --snd.tx_free;
    const uint32_t bytes = count * 2 * SND_OUT_CHANNELS;
    memcpy(snd_tx[s].data, frames, bytes);
    snd_tx[s].stream_id = (uint32_t)snd.out_stream;
    snd_tx[s].status[0] = 0;
    struct snd_desc *d = descriptors(SND_TX);
    d[3 * s] = (struct snd_desc){pa(&snd_tx[s].stream_id), 4, VIRTQ_DESC_F_NEXT, (uint16_t)(3 * s + 1)};
    d[3 * s + 1] = (struct snd_desc){pa(snd_tx[s].data), bytes, VIRTQ_DESC_F_NEXT, (uint16_t)(3 * s + 2)};
    d[3 * s + 2] = (struct snd_desc){pa(snd_tx[s].status), 8, VIRTQ_DESC_F_WRITE, 0};
    post(SND_TX, (uint16_t)(3 * s));
    droidvm_snd_tx_periods++;
    return count;
}

uint32_t droidvm_snd_capture(int16_t *frames, uint32_t count) {
    if (!snd.in_running || count != SND_PERIOD_FRAMES) return 0;
    if (used(SND_RX)[1] == snd.used[SND_RX]) { droidvm_snd_rx_empty++; return 0; }
    snd_barrier();
    const uint32_t *ring = (const uint32_t *)(used(SND_RX) + 2);
    const uint32_t id = ring[(snd.used[SND_RX] % SND_QUEUE) * 2];
    ++snd.used[SND_RX];
    const uint16_t s = (uint16_t)(id / 3 % SND_SLOTS);
    const int16_t *data = (const int16_t *)snd_rx[s].data;
    if (snd.in_channels == 1) memcpy(frames, data, count * 2);
    else for (uint32_t i = 0; i < count; ++i) frames[i] = data[i * snd.in_channels];   // the first channel of a wider stream
    post(SND_RX, (uint16_t)(3 * s));
    droidvm_snd_rx_periods++;
    return count;
}

// START/STOP of both streams. Called by the engine while its thread is not running, so the queues have one user at a time.
// A stopped stream's chains come back to us (the device returns what it held), so the indices are taken up to date before
// the next start.
void droidvm_snd_running(bool output, bool input) {
    if (!snd.up) return;
    if (snd.out_stream >= 0 && output != snd.out_running) {
        if (output) {
            snd.tx_free = SND_SLOTS; snd.tx_next = 0;
            snd.out_running = pcm_command(SND_R_PCM_START, snd.out_stream);
        } else {
            pcm_command(SND_R_PCM_STOP, snd.out_stream);
            snd.out_running = false;
            for (unsigned waited = 0; waited < 200 && snd.tx_free < SND_SLOTS; ++waited) { tx_reap(); IOSleep(1); }
        }
    }
    if (snd.in_stream >= 0 && input != snd.in_running) {
        if (input) {
            snd.used[SND_RX] = used(SND_RX)[1];
            rx_post_all();
            snd.in_running = pcm_command(SND_R_PCM_START, snd.in_stream);
        } else {
            pcm_command(SND_R_PCM_STOP, snd.in_stream);
            snd.in_running = false;
            IOSleep(20);
            snd.used[SND_RX] = used(SND_RX)[1];
        }
    }
}

static const char *snd_bring_up(void) {
    snd_write32(0x70, 0);
    for (unsigned i = 0; snd_read32(0x70); ++i) { if (i == 1000) return "reset timeout"; delay(1000); }
    snd_write32(0x70, 1);          // ACKNOWLEDGE
    snd_write32(0x70, 3);          // DRIVER
    snd_write32(0x14, 1); uint32_t high = snd_read32(0x10);
    if (!(high & 1)) return "no VERSION_1";
    // VERSION_1 (bit 32 -> high bit 0) and, on a pseudo-unprotected VM, ACCESS_PLATFORM (bit 33 -> high bit 1): crosvm offers
    // it and the vhost-user backend maps the vrings through it, so the driver must accept it or the device needs-reset at
    // DRIVER_OK (as the virtio-gpu transport does, DroidVMVirtioGpu.cpp). virtio-snd has no low feature bit we need.
    high &= 3;
    snd_write32(0x24, 0); snd_write32(0x20, 0);
    snd_write32(0x24, 1); snd_write32(0x20, high);
    snd_write32(0x70, 11);         // FEATURES_OK
    if (!(snd_read32(0x70) & 8)) return "features refused";
    for (unsigned q = 0; q < 4; ++q) {
        memset(snd_rings[q], 0, sizeof(snd_rings[q]));
        *(volatile uint16_t *)(snd_rings[q] + 1024) = 1;   // VIRTQ_AVAIL_F_NO_INTERRUPT: we poll
        snd_write32(0x30, q);
        if (snd_read32(0x44) || snd_read32(0x34) < SND_QUEUE) return "queue unavailable or too small";
        const uint64_t base = pa(snd_rings[q]);
        snd_write32(0x38, SND_QUEUE);
        snd_write64(0x80, base); snd_write64(0x90, base + 1024); snd_write64(0xa0, base + 2048);
        snd_write32(0x44, 1);
    }
    snd_barrier();
    snd_write32(0x70, 15);         // DRIVER_OK
    if ((snd_read32(0x70) & 0x4f) != 15) return "device not ready";
    // The streams: the first output that takes 48 kHz S16 stereo, and the first input that takes 48 kHz S16.
    const uint32_t streams = snd_read32(0x104);
    droidvm_snd_streams = streams;
    snd.out_stream = snd.in_stream = -1;
    for (uint32_t first = 0; first < streams; first += 16) {
        const uint32_t count = streams - first < 16 ? streams - first : 16;
        uint32_t query[4] = {SND_R_PCM_INFO, first, count, sizeof(struct snd_pcm_info)};
        memcpy(snd_control_request, query, sizeof(query));
        if (!control(sizeof(query), 4 + count * sizeof(struct snd_pcm_info))) return "PCM_INFO failed";
        for (uint32_t i = 0; i < count; ++i) {
            struct snd_pcm_info info;
            memcpy(&info, snd_control_reply + 4 + i * sizeof(info), sizeof(info));
            if (!(info.formats & (1ULL << SND_PCM_FMT_S16)) || !(info.rates & (1ULL << SND_PCM_RATE_48000))) continue;
            if (info.direction == SND_D_OUTPUT && snd.out_stream < 0 &&
                info.channels_min <= SND_OUT_CHANNELS && info.channels_max >= SND_OUT_CHANNELS)
                snd.out_stream = (int32_t)(first + i);
            if (info.direction == SND_D_INPUT && snd.in_stream < 0 && info.channels_min <= 2) {
                snd.in_stream = (int32_t)(first + i);
                snd.in_channels = info.channels_min > SND_IN_CHANNELS ? info.channels_min : SND_IN_CHANNELS;
            }
        }
    }
    if (snd.out_stream >= 0 && !set_params(snd.out_stream, SND_OUT_CHANNELS)) return "output stream refused 48 kHz S16 stereo";
    if (snd.in_stream >= 0 && !set_params(snd.in_stream, snd.in_channels)) snd.in_stream = -1;
    return NULL;
}

// droidvm-snd-selftest=N: push N periods of a known ramp straight into the output stream, bypassing iOS's audio stack, so a
// `backend=file` card captures an exact pattern the host can verify byte for byte -- a self-contained proof of the
// guest -> virtio-snd -> vhost-user backend PCM path when no app is playing (e.g. the phone display is unavailable to drive a
// UI trigger). Sample value = (running frame index) & 0x7fff, identical on both channels, so the capture is a sawtooth.
static void snd_selftest_thread(void *arg, wait_result_t result) {
    (void)result;
    const uint32_t periods = (uint32_t)(uintptr_t)arg;
    static int16_t buf[SND_PERIOD_FRAMES * SND_OUT_CHANNELS];
    IOSleep(3000);                              // let the device and its backend settle after boot
    droidvm_snd_running(true, false);
    uint32_t frame = 0;
    for (uint32_t p = 0; p < periods; ++p) {
        for (uint32_t i = 0; i < SND_PERIOD_FRAMES; ++i) {
            const int16_t v = (int16_t)(frame++ & 0x7fff);
            buf[i * SND_OUT_CHANNELS] = v;
            buf[i * SND_OUT_CHANNELS + 1] = v;
        }
        for (unsigned retry = 0; droidvm_snd_play(buf, SND_PERIOD_FRAMES) == 0 && retry < 1000; ++retry) delay(1000);
        droidvm_snd_selftest_pushed = p + 1;
        delay(3000);                            // ~ one period of pacing so the transmit slots do not overflow
    }
    IOSleep(100);
    droidvm_snd_running(false, false);
    droidvm_snd_selftest_done = 1;
}

// Called once from the late board probe (DroidVMStubs.c native_touch_probe). The audio device is published either way: with
// no virtio-snd window it runs on its own clock and plays into silence, so iOS's audio stack still has its built-in device.
void droidvm_snd_start(void) {
    uint32_t enabled = 1;
    PE_parse_boot_argn("droidvm-snd", &enabled, sizeof(enabled));
    DTEntry armio, node;
    const void *value;
    unsigned int size;
    uint64_t ranges[3], reg[2];
    if (enabled && SecureDTLookupEntry(NULL, "/arm-io", &armio) == kSuccess &&
        SecureDTGetProperty(armio, "ranges", &value, &size) == kSuccess && size >= sizeof(ranges) &&
        SecureDTLookupEntry(NULL, "/arm-io/virtio-snd", &node) == kSuccess && snd_buffers()) {
        memcpy(ranges, value, sizeof(ranges));
        droidvm_snd_found = 1;
        if (SecureDTGetProperty(node, "reg", &value, &size) == kSuccess && size == sizeof(reg)) {
            memcpy(reg, value, sizeof(reg));
            if (reg[0] >= ranges[0] && reg[1] >= 0x110 && reg[1] <= ranges[2] && reg[0] - ranges[0] <= ranges[2] - reg[1]) {
                droidvm_snd_stage = 1;
                snd.registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
                if (!snd.registers || snd_read32(0x00) != 0x74726976 || snd_read32(0x04) != 2 || snd_read32(0x08) != 25) {
                    strlcpy(droidvm_snd_status, "no virtio-snd at /arm-io/virtio-snd", sizeof(droidvm_snd_status));
                } else {
                    droidvm_snd_stage = 2;
                    const char *why = snd_bring_up();
                    if (why) {
                        strlcpy(droidvm_snd_status, why, sizeof(droidvm_snd_status));
                        snd_write32(0x70, 0);
                    } else {
                        snd.up = true;
                        droidvm_snd_stage = 3;
                        snprintf(droidvm_snd_status, sizeof(droidvm_snd_status), "up: output stream %d, input stream %d x%u",
                                 snd.out_stream, snd.in_stream, snd.in_channels);
                    }
                }
            }
        }
    }
    uint32_t audio = 1;
    PE_parse_boot_argn("droidvm-audio", &audio, sizeof(audio));
    if (audio && droidvm_audio_attach() != 0) strlcpy(droidvm_snd_status, "audio device attach failed", sizeof(droidvm_snd_status));
    uint32_t selftest = 0;
    if (snd.up && PE_parse_boot_argn("droidvm-snd-selftest", &selftest, sizeof(selftest)) && selftest) {
        thread_t thread;
        if (kernel_thread_start(snd_selftest_thread, (void *)(uintptr_t)selftest, &thread) == KERN_SUCCESS)
            thread_deallocate(thread);
    }
}
