// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The guest's network, carried over crosvm's virtio-net on the virtio-mmio transport. crosvm's --net device (a tap bridged to
// the phone's br-wifi) is moved onto the MMIO transport by the fork's --virtio-mmio-net, and make-droidvm-adt.py (--net-mmio)
// publishes its window as /arm-io/net, compatible "droidvm,net". This file is the transport half: it maps that window, brings
// the virtio-net device up (VIRTIO_F_VERSION_1 + VIRTIO_NET_F_MAC, no offloads or mergeable buffers -- so every packet carries a
// 12-byte virtio_net_hdr in one descriptor), drives the receive and transmit split virtqueues, and runs a polling worker.
//
// Above it, the device is an IOKit ethernet controller (DroidVMVirtioEthernet.cpp) on Apple's IONetworkingFamily, which
// DroidVMVirtioNetIf.cpp brings up; both are C++ in the IOKit driver universe (its net/mbuf headers redefine core types this
// Mach-universe file also defines). The halves share only this plain-C API:
//   transport -> controller:  droidvm_net_if_attach(mac), droidvm_net_if_input(frame, length)
//   controller -> transport:  droidvm_net_tx_frame(frame, length)
//
// droidvm-net=0 leaves the device alone.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#if DROIDVM_DEVICE_DYNAMIC
#include "DroidVMDmaRegion.h"
#endif

extern void IOSleep(unsigned milliseconds);
extern void delay(int usec);
extern int thread_policy_set(thread_t, int flavor, int *policy_info, unsigned count);   // lower the worker below the monitor
// The IOKit half (DroidVMVirtioNetIf.cpp).
extern int droidvm_net_if_attach(const uint8_t mac[6]);
extern void droidvm_net_if_input(const void *frame, unsigned length);

enum {
    DROIDVM_NET_QUEUE = 64,          // descriptors per queue (<= crosvm's QueueNumMax)
    DROIDVM_NET_HDR = 12,            // sizeof(struct virtio_net_hdr_v1) once VIRTIO_F_VERSION_1 is negotiated
    DROIDVM_NET_FRAME = 1514,        // a full ethernet frame without the FCS
    DROIDVM_NET_BUFFER = 2048,       // DROIDVM_NET_HDR + DROIDVM_NET_FRAME, rounded up
    DROIDVM_NET_POLL_MS = 10,
};
enum { VIRTIO_NET_F_MAC = 5, VIRTQ_DESC_F_WRITE = 2, VIRTQ_AVAIL_F_NO_INTERRUPT = 1 };

struct droidvm_net_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
_Static_assert(sizeof(struct droidvm_net_desc) == 16, "VirtIO wire layout");

// One page per queue: descriptors at 0, the driver (available) ring at 1024, the device (used) ring at 2048.
#if DROIDVM_DEVICE_DYNAMIC
// DROIDVM_DEVICE_DYNAMIC (the iPadOS 27 board): the same buffers from one contiguous region allocated when the device starts
// (DroidVMDmaRegion.h), as pointers indexed as the arrays are.
static struct droidvm_dma droidvm_net_dma;
static uint8_t *droidvm_net_rx_ring, *droidvm_net_tx_ring;
static uint8_t (*droidvm_net_rx_data)[DROIDVM_NET_BUFFER], (*droidvm_net_tx_data)[DROIDVM_NET_BUFFER];
static bool net_buffers(void) {
    const uint64_t data = (uint64_t)DROIDVM_NET_QUEUE * DROIDVM_NET_BUFFER;
    if (!droidvm_dma_alloc(&droidvm_net_dma, 2 * 4096 + 2 * data)) return false;
    uint8_t *at = (uint8_t *)droidvm_net_dma.base;
    droidvm_net_rx_ring = at;
    droidvm_net_tx_ring = at + 4096;
    droidvm_net_rx_data = (uint8_t (*)[DROIDVM_NET_BUFFER])(at + 2 * 4096);
    droidvm_net_tx_data = (uint8_t (*)[DROIDVM_NET_BUFFER])(at + 2 * 4096 + data);
    return true;
}
static uint64_t net_pa(const void *p) { return droidvm_dma_pa(&droidvm_net_dma, p); }
#else
static uint8_t droidvm_net_rx_ring[4096] __attribute__((aligned(4096)));
static uint8_t droidvm_net_tx_ring[4096] __attribute__((aligned(4096)));
static uint8_t droidvm_net_rx_data[DROIDVM_NET_QUEUE][DROIDVM_NET_BUFFER] __attribute__((aligned(4096)));
static uint8_t droidvm_net_tx_data[DROIDVM_NET_QUEUE][DROIDVM_NET_BUFFER] __attribute__((aligned(4096)));
static bool net_buffers(void) { return true; }
static uint64_t net_pa(const void *p) { return ml_static_vtop((vm_offset_t)p); }
#endif

static struct {
    uintptr_t registers;
    uint8_t mac[6];
    uint16_t rx_used;         // our index into the receive used ring
    uint16_t tx_head;         // next available-ring slot to fill
    uint16_t tx_used;         // our index into the transmit used ring
    volatile int tx_lock;     // a brief spin lock: the output callback and the reclaim both touch the transmit ring
    bool up;
} droidvm_net;

// For the host rig (peek.py).
volatile uint32_t droidvm_net_found, droidvm_net_started, droidvm_net_stage;
volatile uint64_t droidvm_net_rx_packets, droidvm_net_tx_packets, droidvm_net_rx_drops, droidvm_net_tx_drops;
char droidvm_net_status[64];

// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t net_read32(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(droidvm_net.registers + offset) : "memory");
    return value;
}
static uint8_t net_read8(unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldrb %w0, [%1]" : "=r"(value) : "r"(droidvm_net.registers + offset) : "memory");
    return (uint8_t)value;
}
static void net_write32(unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(droidvm_net.registers + offset) : "memory");
}
static void net_write64(unsigned offset, uint64_t value) {
    net_write32(offset, (uint32_t)value);
    net_write32(offset + 4, (uint32_t)(value >> 32));
}
static inline void net_barrier(void) { __asm__ volatile("dmb osh" ::: "memory"); }
static void tx_acquire(void) { while (__atomic_test_and_set(&droidvm_net.tx_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("yield"); }
static void tx_release(void) { __atomic_clear(&droidvm_net.tx_lock, __ATOMIC_RELEASE); }

static bool net_reset(void) {
    net_write32(0x70, 0);
    for (unsigned waited = 0; net_read32(0x70); ++waited) {
        if (waited > 1000) return false;
        delay(1);
    }
    return true;
}

static bool net_queue(unsigned queue, uint8_t *page) {
    net_write32(0x30, queue);
    if (net_read32(0x44) || net_read32(0x34) < DROIDVM_NET_QUEUE) return false;
    memset(page, 0, 4096);
    *(volatile uint16_t *)(page + 1024) = VIRTQ_AVAIL_F_NO_INTERRUPT;   // this driver polls
    uint64_t physical = net_pa(page);
    net_barrier();
    net_write32(0x38, DROIDVM_NET_QUEUE);
    net_write64(0x80, physical);
    net_write64(0x90, physical + 1024);
    net_write64(0xa0, physical + 2048);
    net_write32(0x44, 1);
    return true;
}

// Brings the device up: negotiate, set up the receive and transmit queues, post every receive buffer. NULL, or why not.
static const char *net_bring_up(void) {
    if (!net_reset()) return "reset";
    net_write32(0x70, 1);
    net_write32(0x70, 3);
    net_write32(0x14, 0);
    uint32_t low = net_read32(0x10);
    net_write32(0x14, 1);
    uint32_t high = net_read32(0x10);
    if (!(high & 1)) return "no VIRTIO_F_VERSION_1";
    bool mac = (low >> VIRTIO_NET_F_MAC) & 1;
    net_write32(0x24, 0);
    net_write32(0x20, mac ? (1u << VIRTIO_NET_F_MAC) : 0);   // ack only MAC in the low word
    net_write32(0x24, 1);
    net_write32(0x20, 1);                                    // VIRTIO_F_VERSION_1 in the high word
    net_write32(0x70, 11);
    if (!(net_read32(0x70) & 8)) return "features refused";
    if (mac) for (unsigned i = 0; i < 6; ++i) droidvm_net.mac[i] = net_read8(0x100 + i);
    else { droidvm_net.mac[0] = 0x02; droidvm_net.mac[5] = 0x01; }   // a locally-administered fallback

    if (!net_queue(0, droidvm_net_rx_ring)) return "receive queue";
    if (!net_queue(1, droidvm_net_tx_ring)) return "transmit queue";

    // Post every receive buffer into the available ring before DRIVER_OK, as the input driver does: crosvm takes the ring as
    // the driver leaves it at DRIVER_OK, so filling it afterwards can be missed.
    struct droidvm_net_desc *descriptors = (struct droidvm_net_desc *)droidvm_net_rx_ring;
    volatile uint16_t *available = (volatile uint16_t *)(droidvm_net_rx_ring + 1024);
    for (uint16_t i = 0; i < DROIDVM_NET_QUEUE; ++i) {
        descriptors[i] = (struct droidvm_net_desc){net_pa(droidvm_net_rx_data[i]), DROIDVM_NET_BUFFER,
                                                   VIRTQ_DESC_F_WRITE, 0};
        available[2 + i] = i;
    }
    net_barrier();
    available[1] = DROIDVM_NET_QUEUE;
    droidvm_net.rx_used = 0;
    droidvm_net.tx_head = 0;
    droidvm_net.tx_used = 0;

    net_write32(0x70, 15);
    if ((net_read32(0x70) & 0x4f) != 15) return "device not ready";
    net_barrier();
    net_write32(0x50, 0);   // notify the receive queue
    return NULL;
}

// Put one outgoing ethernet frame (no virtio header) on the transmit queue. Called by the interface's output callback.
int droidvm_net_tx_frame(const void *frame, unsigned length) {
    if (!droidvm_net.up || length == 0 || length > DROIDVM_NET_FRAME) { droidvm_net_tx_drops++; return -1; }
    tx_acquire();
    volatile uint16_t *tx_used = (volatile uint16_t *)(droidvm_net_tx_ring + 2048);
    net_barrier();
    droidvm_net.tx_used = tx_used[1];
    if ((uint16_t)(droidvm_net.tx_head - droidvm_net.tx_used) >= DROIDVM_NET_QUEUE) {   // the ring is full
        tx_release();
        droidvm_net_tx_drops++;
        return -1;
    }
    uint16_t slot = droidvm_net.tx_head % DROIDVM_NET_QUEUE;
    uint8_t *buffer = droidvm_net_tx_data[slot];
    memset(buffer, 0, DROIDVM_NET_HDR);
    memcpy(buffer + DROIDVM_NET_HDR, frame, length);
    struct droidvm_net_desc *descriptors = (struct droidvm_net_desc *)droidvm_net_tx_ring;
    volatile uint16_t *available = (volatile uint16_t *)(droidvm_net_tx_ring + 1024);
    descriptors[slot] = (struct droidvm_net_desc){net_pa(buffer), DROIDVM_NET_HDR + length, 0, 0};
    available[2 + droidvm_net.tx_head % DROIDVM_NET_QUEUE] = slot;
    net_barrier();
    ++droidvm_net.tx_head;
    available[1] = droidvm_net.tx_head;
    net_barrier();
    net_write32(0x50, 1);   // notify the transmit queue
    tx_release();
    droidvm_net_tx_packets++;
    return 0;
}

// Take the frames the device wrote, hand them to the interface, and give each buffer back to the receive queue.
static void net_rx_poll(void) {
    struct droidvm_net_desc *descriptors = (struct droidvm_net_desc *)droidvm_net_rx_ring;
    volatile uint16_t *available = (volatile uint16_t *)(droidvm_net_rx_ring + 1024);
    volatile uint16_t *used = (volatile uint16_t *)(droidvm_net_rx_ring + 2048);
    const volatile uint32_t *ring = (const volatile uint32_t *)(droidvm_net_rx_ring + 2052);
    uint16_t head = available[1];
    unsigned returned = 0;
    while (used[1] != droidvm_net.rx_used) {
        net_barrier();
        uint32_t id = ring[(droidvm_net.rx_used % DROIDVM_NET_QUEUE) * 2];
        uint32_t written = ring[(droidvm_net.rx_used % DROIDVM_NET_QUEUE) * 2 + 1];
        if (id < DROIDVM_NET_QUEUE && written > DROIDVM_NET_HDR && written <= DROIDVM_NET_BUFFER) {
            // Hardware-style destination filter. crosvm's tap hands us every frame the bridge carries, which on a busy wifi
            // network is mostly multicast (mDNS, SSDP, IPv6 ND, ...) this guest has not joined. A real NIC filters by its
            // multicast table; this device has no control queue, so only broadcast and frames addressed to our own MAC pass.
            // (The controller's setMulticastList is where the joined groups would come from, to let IPv6 ND and mDNS through.)
            const uint8_t *dst = droidvm_net_rx_data[id] + DROIDVM_NET_HDR;
            bool broadcast = (dst[0] & dst[1] & dst[2] & dst[3] & dst[4] & dst[5]) == 0xff;
            bool ours = !(dst[0] & 1) && !memcmp(dst, droidvm_net.mac, 6);
            if (broadcast || ours) {
                droidvm_net_if_input(droidvm_net_rx_data[id] + DROIDVM_NET_HDR, written - DROIDVM_NET_HDR);
                droidvm_net_rx_packets++;
            } else {
                droidvm_net_rx_drops++;
            }
            descriptors[id] = (struct droidvm_net_desc){net_pa(droidvm_net_rx_data[id]),
                                                        DROIDVM_NET_BUFFER, VIRTQ_DESC_F_WRITE, 0};
            available[2 + (head + returned) % DROIDVM_NET_QUEUE] = (uint16_t)id;
            ++returned;
        }
        ++droidvm_net.rx_used;
    }
    if (returned) {
        net_barrier();
        available[1] = (uint16_t)(head + returned);
        net_barrier();
        net_write32(0x50, 0);
        // Acknowledge the device interrupt only when there was activity. An MMIO read every idle poll is a hypervisor trap
        // (VM exit) that, at a 1 ms cadence, starves other kernel work; the queues are set NO_INTERRUPT so there is nothing
        // to ack when idle anyway. The used-ring index above lives in guest RAM, so the idle poll touches no MMIO.
        uint32_t interrupts = net_read32(0x60) & 3;
        if (interrupts) net_write32(0x64, interrupts);
    }
}

static void droidvm_net_worker(void *parameter, wait_result_t result) {
    (void)parameter; (void)result;
    // Attach the BSD interface here, not in droidvm_net_start: that runs on the board-probe thread, where the networking
    // stack is not yet up and ifnet_attach would block it. On this own thread the attach completes once networking is ready.
    droidvm_net_stage = 4;
    if (droidvm_net_if_attach(droidvm_net.mac) != 0) {
        strlcpy(droidvm_net_status, "interface attach failed", sizeof(droidvm_net_status));
        return;
    }
    droidvm_net_stage = 5;
    droidvm_net_started = 1;
    snprintf(droidvm_net_status, sizeof(droidvm_net_status), "en up %02x:%02x:%02x:%02x:%02x:%02x",
             droidvm_net.mac[0], droidvm_net.mac[1], droidvm_net.mac[2], droidvm_net.mac[3], droidvm_net.mac[4], droidvm_net.mac[5]);
    for (;;) {
        net_rx_poll();
        IOSleep(DROIDVM_NET_POLL_MS);
    }
}

// Called once from the late board probe (DroidVMStubs.c native_touch_probe), after the input devices.
void droidvm_net_start(void) {
    uint32_t enabled = 1;
    PE_parse_boot_argn("droidvm-net", &enabled, sizeof(enabled));
    DTEntry armio, node;
    const void *value;
    unsigned int size;
    uint64_t ranges[3], reg[2];
    if (!enabled || SecureDTLookupEntry(NULL, "/arm-io", &armio) != kSuccess ||
        SecureDTGetProperty(armio, "ranges", &value, &size) != kSuccess || size < sizeof(ranges)) return;
    memcpy(ranges, value, sizeof(ranges));
    if (SecureDTLookupEntry(NULL, "/arm-io/net", &node) != kSuccess) return;
    droidvm_net_found = 1;
    if (SecureDTGetProperty(node, "reg", &value, &size) != kSuccess || size != sizeof(reg)) return;
    memcpy(reg, value, sizeof(reg));
    if (reg[0] < ranges[0] || reg[1] < 0x108 || reg[1] > ranges[2] || reg[0] - ranges[0] > ranges[2] - reg[1]) return;
    droidvm_net_stage = 1;
    droidvm_net.registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
    if (!droidvm_net.registers || net_read32(0x00) != 0x74726976 || net_read32(0x04) != 2 || net_read32(0x08) != 1) {
        strlcpy(droidvm_net_status, "no virtio-net at /arm-io/net", sizeof(droidvm_net_status));
        return;
    }
    droidvm_net_stage = 2;
    if (!net_buffers()) {
        strlcpy(droidvm_net_status, "no memory for the queues", sizeof(droidvm_net_status));
        return;
    }
    const char *why = net_bring_up();
    if (why) {
        strlcpy(droidvm_net_status, why, sizeof(droidvm_net_status));
        (void)net_reset();
        return;
    }
    droidvm_net_stage = 3;
    droidvm_net.up = true;   // the transmit path is ready before the interface can call it
    // The worker attaches the interface (off this board-probe thread) and then polls receive, so this returns at once. It is
    // dropped well below default priority: on a busy bridged network its receive processing otherwise starves the monitor
    // thread that services the host mailbox (and everything else) on this small VM.
    thread_t thread;
    if (kernel_thread_start(droidvm_net_worker, NULL, &thread) == KERN_SUCCESS) {
        struct { int importance; } precedence = {.importance = -40};
        thread_policy_set(thread, 3 /* THREAD_PRECEDENCE_POLICY */, (int *)&precedence, 1 /* THREAD_PRECEDENCE_POLICY_COUNT */);
        thread_deallocate(thread);
    }
}
