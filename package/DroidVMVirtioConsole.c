// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The VM's virtio-console ports as BSD terminals: crosvm's --serial hardware=virtio-console devices, moved onto the virtio-mmio
// transport by the fork's --virtio-mmio-console and published by DarwinBoot (FdtAdt.c) or make-droidvm-adt.py --console-mmio as
// /arm-io/virtio-console-N, compatible "droidvm,virtio-console". Each port is a tty, /dev/cu.NAME and /dev/tty.NAME, NAME being the
// name the host gives it (crosvm --serial ...,name=NAME; VIRTIO_CONSOLE_PORT_NAME) or vportNpM for a port without one, as Linux's
// /dev/vportNpM. What the guest writes goes to the host's end of the port; what the host writes comes in through the line
// discipline, as on any serial port (gpsd reading NMEA, a getty).
//
// The transport (OASIS VirtIO 1.2, 5.3): VIRTIO_F_VERSION_1, and VIRTIO_CONSOLE_F_MULTIPORT when the device offers it (crosvm
// does for a named port): then the control queues (2, 3) carry the port's arrival (DEVICE_ADD, answered PORT_READY), its name,
// the host's side opening (PORT_OPEN from the device) and the guest's (PORT_OPEN from here, when the tty is first opened and
// last closed: crosvm passes a named port's input only while the guest has it open). Port 0 uses queues 0 and 1, port N > 0
// queues 2 + 2N and 3 + 2N. Without MULTIPORT the device is port 0 alone, with no name. No other feature (no F_SIZE resizing,
// no emergency write).
//
// One worker per device sleeps on the device's SPI (the ADT's droidvm,intid, through ArmCpu's armcpu_spi_attach) with a 250 ms
// guard, polls every 20 ms without one, and takes input, completed output and control messages. Output starts from the tty's
// t_oproc (any thread, the tty locked): it fills free transmit buffers from the output queue and notifies the device; when they
// are all in flight the tty stays TS_BUSY until the worker reclaims some and starts it again.
//
// droidvm-console=0 leaves the devices alone.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/tty.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#if DROIDVM_DEVICE_DYNAMIC
#include "DroidVMDmaRegion.h"
#endif

extern void IOSleep(unsigned milliseconds);
extern void delay(int usec);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_static_vtop(vm_offset_t vaddr);
// ArmCpu (the GIC): a handler for one SPI, run in the primary interrupt context before the EOI; 0 when attached.
extern int armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);

enum {
    DVC_DEVICES = 4,          // /arm-io/virtio-console-0..3
    DVC_PORTS = 4,            // ports per device (crosvm's named port is a device of one)
    DVC_QUEUE = 16,           // descriptors per queue (<= crosvm's QUEUE_SIZE)
    DVC_RX_BYTES = 256,       // a receive buffer
    DVC_TX_BYTES = 1024,      // a transmit buffer
    DVC_CONTROL_BYTES = 128,  // a control message: 8 bytes, a port name after them
    DVC_NAME = 64,
    DVC_POLL_MS = 20,
    DVC_GUARD_MS = 250,
};
enum { VIRTIO_CONSOLE_F_MULTIPORT = 1, VIRTQ_DESC_F_WRITE = 2 };
enum {
    DVC_DEVICE_READY = 0, DVC_DEVICE_ADD = 1, DVC_DEVICE_REMOVE = 2, DVC_PORT_READY = 3, DVC_CONSOLE_PORT = 4, DVC_RESIZE = 5,
    DVC_PORT_OPEN = 6, DVC_PORT_NAME = 7,
};

struct dvc_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct dvc_control { uint32_t id; uint16_t event, value; };
_Static_assert(sizeof(struct dvc_desc) == 16 && sizeof(struct dvc_control) == 8, "VirtIO wire layout");

// One page per queue: descriptors at 0, the driver (available) ring at 1024, the device (used) ring at 2048; then its buffers.
struct dvc_queue {
    uint8_t *ring, *data;
    uint32_t bytes;           // one buffer
    uint16_t index;           // the queue's number
    uint16_t head;            // the available ring's next index
    uint16_t used;            // our index into the used ring
    bool on;
};

struct dvc_device;
struct dvc_port {
    struct dvc_device *device;
    uint32_t id;
    struct dvc_queue rx, tx;
    struct tty *tty;
    char name[DVC_NAME];
    bool present, host_open, guest_open, console, published;
};

struct dvc_device {
    uintptr_t registers;
#if DROIDVM_DEVICE_DYNAMIC
    struct droidvm_dma dma;
#endif
    uint32_t unit, ports;
    bool multiport, interrupt;
    volatile int lock;        // the transmit and control-transmit rings: a tty's start routine and the worker both post
    struct dvc_queue control_rx, control_tx;
    struct dvc_port port[DVC_PORTS];
};

static struct dvc_device dvc_devices[DVC_DEVICES];
static struct tty *dvc_ttys[DVC_DEVICES * DVC_PORTS];   // the cdevsw's d_ttys, by minor
static int dvc_major = -1;

// For the host rig (peek.py).
volatile uint32_t droidvm_console_found, droidvm_console_started, droidvm_console_ports;
volatile uint64_t droidvm_console_rx_bytes, droidvm_console_tx_bytes, droidvm_console_rx_drops, droidvm_console_irqs[DVC_DEVICES];
char droidvm_console_status[DVC_DEVICES][64];

// A trapped MMIO access must be a single load/store with a plain base register (see DroidVMVirtioInput.c).
static uint32_t dvc_read32(struct dvc_device *d, unsigned offset) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(d->registers + offset) : "memory");
    return value;
}
static void dvc_write32(struct dvc_device *d, unsigned offset, uint32_t value) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(d->registers + offset) : "memory");
}
static void dvc_write64(struct dvc_device *d, unsigned offset, uint64_t value) {
    dvc_write32(d, offset, (uint32_t)value);
    dvc_write32(d, offset + 4, (uint32_t)(value >> 32));
}
static inline void dvc_barrier(void) { __asm__ volatile("dmb osh" ::: "memory"); }
static void dvc_acquire(struct dvc_device *d) { while (__atomic_test_and_set(&d->lock, __ATOMIC_ACQUIRE)) __asm__ volatile("yield"); }
static void dvc_release(struct dvc_device *d) { __atomic_clear(&d->lock, __ATOMIC_RELEASE); }

static uint64_t dvc_pa(struct dvc_device *d, const void *p) {
#if DROIDVM_DEVICE_DYNAMIC
    return droidvm_dma_pa(&d->dma, p);
#else
    (void)d;
    return ml_static_vtop((vm_offset_t)p);
#endif
}

static struct dvc_desc *dvc_descriptors(struct dvc_queue *q) { return (struct dvc_desc *)q->ring; }
static volatile uint16_t *dvc_available(struct dvc_queue *q) { return (volatile uint16_t *)(q->ring + 1024); }
static volatile uint16_t *dvc_used(struct dvc_queue *q) { return (volatile uint16_t *)(q->ring + 2048); }
static uint8_t *dvc_buffer(struct dvc_queue *q, unsigned slot) { return q->data + (size_t)slot * q->bytes; }

// Puts buffer `slot` (length bytes; the device writes it when `writable`) on the available ring and notifies the device.
static void dvc_post(struct dvc_device *d, struct dvc_queue *q, uint16_t slot, uint32_t length, bool writable) {
    dvc_descriptors(q)[slot] = (struct dvc_desc){dvc_pa(d, dvc_buffer(q, slot)), length, writable ? VIRTQ_DESC_F_WRITE : 0, 0};
    dvc_available(q)[2 + q->head % DVC_QUEUE] = slot;
    dvc_barrier();
    ++q->head;
    dvc_available(q)[1] = q->head;
    dvc_barrier();
    dvc_write32(d, 0x50, q->index);
}

// A transmit-side queue's buffers the device has given back (the device completes them in order, as crosvm does). d->lock held.
static void dvc_reclaim(struct dvc_queue *q) {
    dvc_barrier();
    q->used = dvc_used(q)[1];
}
static bool dvc_full(struct dvc_queue *q) { return (uint16_t)(q->head - q->used) >= DVC_QUEUE; }

// One control message to the device.
static void dvc_control(struct dvc_device *d, uint32_t id, uint16_t event, uint16_t value) {
    if (!d->multiport) return;
    dvc_acquire(d);
    dvc_reclaim(&d->control_tx);
    if (!dvc_full(&d->control_tx)) {
        uint16_t slot = d->control_tx.head % DVC_QUEUE;
        *(struct dvc_control *)dvc_buffer(&d->control_tx, slot) = (struct dvc_control){id, event, value};
        dvc_post(d, &d->control_tx, slot, sizeof(struct dvc_control), false);
    }
    dvc_release(d);
}

static struct dvc_port *dvc_port_of(dev_t dev) {
    unsigned unit = (unsigned)minor(dev) / DVC_PORTS, id = (unsigned)minor(dev) % DVC_PORTS;
    if (unit >= DVC_DEVICES || id >= dvc_devices[unit].ports || !dvc_devices[unit].port[id].tty) return NULL;
    return &dvc_devices[unit].port[id];
}

// The tty's t_oproc, the tty locked: as much of the output queue as there are free transmit buffers for.
static void dvc_start(struct tty *tp) {
    struct dvc_port *port = dvc_port_of(tp->t_dev);
    if (port && !(tp->t_state & (TS_TIMEOUT | TS_BUSY | TS_TTSTOP))) {
        struct dvc_device *d = port->device;
        while (tp->t_outq.c_cc > 0) {
            dvc_acquire(d);
            dvc_reclaim(&port->tx);
            if (dvc_full(&port->tx)) {
                dvc_release(d);
                tp->t_state |= TS_BUSY;   // the worker starts it again as buffers come back
                break;
            }
            uint16_t slot = port->tx.head % DVC_QUEUE;
            int count = q_to_b(&tp->t_outq, dvc_buffer(&port->tx, slot), MIN(tp->t_outq.c_cc, DVC_TX_BYTES));
            if (count > 0) {
                dvc_post(d, &port->tx, slot, (uint32_t)count, false);
                droidvm_console_tx_bytes += (uint64_t)count;
            }
            dvc_release(d);
            if (count <= 0) break;
        }
    }
    (*linesw[tp->t_line].l_start)(tp);   // ttwwakeup: writers waiting for room
}

static int dvc_param(struct tty *tp, struct termios *t) { (void)tp; (void)t; return 0; }   // no line to program

static int dvc_open(dev_t dev, int flag, int devtype, proc_t p) {
    (void)flag; (void)devtype;
    struct dvc_port *port = dvc_port_of(dev);
    if (!port || !port->present) return ENXIO;
    struct tty *tp = port->tty;
    tty_lock(tp);
    tp->t_oproc = dvc_start;
    tp->t_param = dvc_param;
    tp->t_dev = dev;
    if (!(tp->t_state & TS_ISOPEN)) {
        tp->t_iflag = TTYDEF_IFLAG;
        tp->t_oflag = TTYDEF_OFLAG;
        tp->t_cflag = (CREAD | CS8 | CLOCAL);
        tp->t_lflag = TTYDEF_LFLAG;
        tp->t_ispeed = tp->t_ospeed = TTYDEF_SPEED;
        termioschars(&tp->t_termios);
        ttsetwater(tp);
        tp->t_winsize.ws_col = 80;
        tp->t_winsize.ws_row = 24;
    } else if ((tp->t_state & TS_XCLUDE) && proc_suser(p)) {
        tty_unlock(tp);
        return EBUSY;
    }
    tp->t_state |= TS_CARR_ON;   // no modem lines: always connected
    int error = (*linesw[tp->t_line].l_open)(dev, tp);
    bool first = !error && !port->guest_open;
    if (first) port->guest_open = true;
    tty_unlock(tp);
    if (first) dvc_control(port->device, port->id, DVC_PORT_OPEN, 1);
    return error;
}

static int dvc_close(dev_t dev, int flag, int mode, proc_t p) {
    (void)mode; (void)p;
    struct dvc_port *port = dvc_port_of(dev);
    if (!port) return ENXIO;
    struct tty *tp = port->tty;
    tty_lock(tp);
    int error = (*linesw[tp->t_line].l_close)(tp, flag);
    ttyclose(tp);
    bool was = port->guest_open;
    port->guest_open = false;
    tty_unlock(tp);
    if (was) dvc_control(port->device, port->id, DVC_PORT_OPEN, 0);
    return error;
}

static int dvc_read(dev_t dev, struct uio *uio, int ioflag) {
    struct dvc_port *port = dvc_port_of(dev);
    if (!port) return ENXIO;
    tty_lock(port->tty);
    int error = (*linesw[port->tty->t_line].l_read)(port->tty, uio, ioflag);
    tty_unlock(port->tty);
    return error;
}

static int dvc_write(dev_t dev, struct uio *uio, int ioflag) {
    struct dvc_port *port = dvc_port_of(dev);
    if (!port) return ENXIO;
    tty_lock(port->tty);
    int error = (*linesw[port->tty->t_line].l_write)(port->tty, uio, ioflag);
    tty_unlock(port->tty);
    return error;
}

static int dvc_ioctl(dev_t dev, u_long cmd, caddr_t data, int flag, proc_t p) {
    struct dvc_port *port = dvc_port_of(dev);
    if (!port) return ENXIO;
    struct tty *tp = port->tty;
    tty_lock(tp);
    int error = (*linesw[tp->t_line].l_ioctl)(tp, cmd, data, flag, p);
    if (error == ENOTTY) error = ttioctl_locked(tp, cmd, data, flag, p);
    tty_unlock(tp);
    return error;
}

static int dvc_stop(struct tty *tp, int rw) { (void)tp; (void)rw; return 0; }   // output already handed to the device stays there
static int dvc_reset(int unit) { (void)unit; return 0; }

static struct cdevsw dvc_cdevsw = {
    .d_open = dvc_open,
    .d_close = dvc_close,
    .d_read = dvc_read,
    .d_write = dvc_write,
    .d_ioctl = dvc_ioctl,
    .d_stop = dvc_stop,
    .d_reset = dvc_reset,
    .d_ttys = dvc_ttys,
    .d_select = ttselect,
    .d_mmap = eno_mmap,
    .d_strategy = eno_strat,
    .d_reserved_1 = eno_getc,
    .d_reserved_2 = eno_putc,
    .d_type = D_TTY,
};

// The port's device nodes, once the host has said what it is: on the worker's thread (devfs may block).
static void dvc_publish(struct dvc_port *port) {
    struct dvc_device *d = port->device;
    if (port->published || dvc_major < 0) return;
    if (!port->name[0]) snprintf(port->name, sizeof(port->name), "vport%up%u", d->unit, port->id);
    if (!port->tty) {
        struct tty *tp = ttymalloc();
        if (!tp) return;
        unsigned minor = d->unit * DVC_PORTS + port->id;
        tp->t_dev = makedev(dvc_major, minor);
        dvc_ttys[minor] = tp;
        port->tty = tp;
    }
    port->published = true;
    (void)devfs_make_node(port->tty->t_dev, DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "cu.%s", port->name);
    (void)devfs_make_node(port->tty->t_dev, DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "tty.%s", port->name);
    droidvm_console_ports = droidvm_console_ports + 1;
}

// The control messages the device sent: a port's arrival, name, console role and host side.
static void dvc_control_rx(struct dvc_device *d) {
    struct dvc_queue *q = &d->control_rx;
    const volatile uint32_t *ring = (const volatile uint32_t *)(q->ring + 2052);
    while (dvc_used(q)[1] != q->used) {
        dvc_barrier();
        uint32_t slot = ring[(q->used % DVC_QUEUE) * 2], length = ring[(q->used % DVC_QUEUE) * 2 + 1];
        ++q->used;
        if (slot >= DVC_QUEUE) continue;
        const uint8_t *buffer = dvc_buffer(q, slot);
        if (length >= sizeof(struct dvc_control) && length <= DVC_CONTROL_BYTES) {
            struct dvc_control message;
            memcpy(&message, buffer, sizeof(message));
            struct dvc_port *port = message.id < d->ports ? &d->port[message.id] : NULL;
            switch (message.event) {
            case DVC_DEVICE_ADD:
                if (port) port->present = true;
                dvc_control(d, message.id, DVC_PORT_READY, port ? 1 : 0);
                break;
            case DVC_DEVICE_REMOVE:
                if (port) port->present = port->host_open = false;
                break;
            case DVC_CONSOLE_PORT:
                if (port) port->console = true;
                break;
            case DVC_PORT_NAME:
                if (port && !port->published) {
                    // as a device node's name: letters, digits, '.', '-' and '_'; anything else '_'
                    uint32_t n = MIN(length - (uint32_t)sizeof(message), (uint32_t)DVC_NAME - 1);
                    for (uint32_t i = 0; i < n; ++i) {
                        char c = (char)buffer[sizeof(message) + i];
                        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                     c == '.' || c == '-' || c == '_';
                        port->name[i] = plain ? c : '_';
                    }
                    port->name[n] = 0;
                }
                break;
            case DVC_PORT_OPEN:
                // crosvm opens the host side when the port is ready, after its name: the port's nodes then.
                if (port) {
                    port->host_open = message.value != 0;
                    if (port->present) dvc_publish(port);
                }
                break;
            default:   // DVC_RESIZE: not negotiated; nothing else comes from the device
                break;
            }
        }
        dvc_post(d, q, (uint16_t)slot, DVC_CONTROL_BYTES, true);
    }
}

// What the host wrote to a port: into its line discipline when the tty is open, else dropped.
static void dvc_port_rx(struct dvc_device *d, struct dvc_port *port) {
    struct dvc_queue *q = &port->rx;
    const volatile uint32_t *ring = (const volatile uint32_t *)(q->ring + 2052);
    while (dvc_used(q)[1] != q->used) {
        dvc_barrier();
        uint32_t slot = ring[(q->used % DVC_QUEUE) * 2], length = ring[(q->used % DVC_QUEUE) * 2 + 1];
        ++q->used;
        if (slot >= DVC_QUEUE) continue;
        length = MIN(length, (uint32_t)DVC_RX_BYTES);
        struct tty *tp = port->tty;
        if (tp && length) {
            const uint8_t *buffer = dvc_buffer(q, slot);
            tty_lock(tp);
            if (tp->t_state & TS_ISOPEN) {
                for (uint32_t i = 0; i < length; ++i) (*linesw[tp->t_line].l_rint)(buffer[i], tp);
                droidvm_console_rx_bytes += length;
            } else {
                droidvm_console_rx_drops += length;
            }
            tty_unlock(tp);
        } else if (length) {
            droidvm_console_rx_drops += length;
        }
        dvc_post(d, q, (uint16_t)slot, DVC_RX_BYTES, true);
    }
}

// Output the device finished: a tty waiting for buffers starts again.
static void dvc_port_tx(struct dvc_device *d, struct dvc_port *port) {
    struct tty *tp = port->tty;
    if (!tp) return;
    dvc_acquire(d);
    uint16_t before = port->tx.used;
    dvc_reclaim(&port->tx);
    bool freed = port->tx.used != before;
    dvc_release(d);
    if (!freed) return;
    tty_lock(tp);
    if (tp->t_state & TS_BUSY) {
        tp->t_state &= ~TS_BUSY;
        dvc_start(tp);
    }
    tty_unlock(tp);
}

static void dvc_service(struct dvc_device *d) {
    if (d->multiport) {
        dvc_control_rx(d);
        dvc_acquire(d);
        dvc_reclaim(&d->control_tx);
        dvc_release(d);
    }
    for (uint32_t i = 0; i < d->ports; ++i) {
        struct dvc_port *port = &d->port[i];
        if (!port->rx.on) continue;
        dvc_port_rx(d, port);
        dvc_port_tx(d, port);
    }
}

// The device's interrupt, in the primary interrupt context: the line is level, so the status is acknowledged here.
static void dvc_interrupt(void *context) {
    struct dvc_device *d = context;
    uint32_t status = dvc_read32(d, 0x60) & 3;
    if (status) dvc_write32(d, 0x64, status);
    __atomic_fetch_add(&droidvm_console_irqs[d->unit], 1, __ATOMIC_RELAXED);
    thread_wakeup_prim((event_t)&droidvm_console_irqs[d->unit], FALSE, THREAD_AWAKENED);
}

static void dvc_worker(void *parameter, wait_result_t result) {
    (void)result;
    struct dvc_device *d = parameter;
    if (!d->multiport) {   // port 0 alone, with no name and no control queue to announce it
        d->port[0].present = d->port[0].host_open = true;
        dvc_publish(&d->port[0]);
    }
    for (;;) {
        // The interrupt count before the look: one taken during it, or before the sleep is armed, ends the sleep at once.
        const uint64_t seen = __atomic_load_n(&droidvm_console_irqs[d->unit], __ATOMIC_RELAXED);
        dvc_service(d);
        if (!d->interrupt) {
            IOSleep(DVC_POLL_MS);
            continue;
        }
        uint64_t deadline;
        clock_interval_to_deadline(DVC_GUARD_MS, NSEC_PER_MSEC, &deadline);
        assert_wait_deadline((event_t)&droidvm_console_irqs[d->unit], THREAD_UNINT, deadline);
        if (__atomic_load_n(&droidvm_console_irqs[d->unit], __ATOMIC_RELAXED) != seen) clear_wait(current_thread(), THREAD_AWAKENED);
        else (void)thread_block(THREAD_CONTINUE_NULL);
    }
}

static bool dvc_reset_device(struct dvc_device *d) {
    dvc_write32(d, 0x70, 0);
    for (unsigned waited = 0; dvc_read32(d, 0x70); ++waited) {
        if (waited > 1000) return false;
        delay(1);
    }
    return true;
}

// Queue `index` over its page and buffers: false when the device has no such queue or a smaller one.
static bool dvc_queue(struct dvc_device *d, struct dvc_queue *q, uint16_t index, uint8_t *page, uint8_t *data, uint32_t bytes) {
    q->ring = page;
    q->data = data;
    q->bytes = bytes;
    q->index = index;
    q->head = q->used = 0;
    dvc_write32(d, 0x30, index);
    if (dvc_read32(d, 0x44) || dvc_read32(d, 0x34) < DVC_QUEUE) return false;
    memset(page, 0, 4096);
    uint64_t physical = dvc_pa(d, page);
    dvc_barrier();
    dvc_write32(d, 0x38, DVC_QUEUE);
    dvc_write64(d, 0x80, physical);
    dvc_write64(d, 0x90, physical + 1024);
    dvc_write64(d, 0xa0, physical + 2048);
    dvc_write32(d, 0x44, 1);
    q->on = true;
    return true;
}

// Before DRIVER_OK: every receive buffer on its queue (crosvm takes the ring as the driver leaves it at DRIVER_OK).
static void dvc_fill(struct dvc_device *d, struct dvc_queue *q) {
    for (uint16_t slot = 0; slot < DVC_QUEUE; ++slot) {
        dvc_descriptors(q)[slot] = (struct dvc_desc){dvc_pa(d, dvc_buffer(q, slot)), q->bytes, VIRTQ_DESC_F_WRITE, 0};
        dvc_available(q)[2 + slot] = slot;
    }
    dvc_barrier();
    q->head = DVC_QUEUE;
    dvc_available(q)[1] = q->head;
}

static const char *dvc_bring_up(struct dvc_device *d) {
    if (!dvc_reset_device(d)) return "reset";
    dvc_write32(d, 0x70, 1);
    dvc_write32(d, 0x70, 3);
    dvc_write32(d, 0x14, 0);
    uint32_t low = dvc_read32(d, 0x10);
    dvc_write32(d, 0x14, 1);
    uint32_t high = dvc_read32(d, 0x10);
    if (!(high & 1)) return "no VIRTIO_F_VERSION_1";
    d->multiport = (low >> VIRTIO_CONSOLE_F_MULTIPORT) & 1;
    dvc_write32(d, 0x24, 0);
    dvc_write32(d, 0x20, d->multiport ? (1u << VIRTIO_CONSOLE_F_MULTIPORT) : 0);
    dvc_write32(d, 0x24, 1);
    dvc_write32(d, 0x20, 1);   // VIRTIO_F_VERSION_1
    dvc_write32(d, 0x70, 11);
    if (!(dvc_read32(d, 0x70) & 8)) return "features refused";
    uint32_t ports = d->multiport ? dvc_read32(d, 0x100 + 4) : 1;   // virtio_console_config.max_nr_ports
    d->ports = ports == 0 ? 1 : ports > DVC_PORTS ? DVC_PORTS : ports;

    // The region: two pages and the buffers per port, two pages and the control buffers.
    const uint64_t port_bytes = 2 * 4096 + (uint64_t)DVC_QUEUE * (DVC_RX_BYTES + DVC_TX_BYTES);
    const uint64_t control_bytes = 2 * 4096 + 2 * (uint64_t)DVC_QUEUE * DVC_CONTROL_BYTES;
    uint8_t *at;
#if DROIDVM_DEVICE_DYNAMIC
    if (!droidvm_dma_alloc(&d->dma, d->ports * port_bytes + control_bytes)) return "no memory for the queues";
    at = (uint8_t *)d->dma.base;
#else
    return "a static board has no region for this driver";
#endif
    for (uint32_t i = 0; i < d->ports; ++i) {
        struct dvc_port *port = &d->port[i];
        port->device = d;
        port->id = i;
        uint16_t rx = i == 0 ? 0 : (uint16_t)(2 + 2 * i), tx = (uint16_t)(rx + 1);
        uint8_t *data = at + 2 * 4096;
        if (!dvc_queue(d, &port->rx, rx, at, data, DVC_RX_BYTES) ||
            !dvc_queue(d, &port->tx, tx, at + 4096, data + DVC_QUEUE * DVC_RX_BYTES, DVC_TX_BYTES)) {
            if (i == 0) return "port 0 queues";
            port->rx.on = port->tx.on = false;
            d->ports = i;
            break;
        }
        dvc_fill(d, &port->rx);
        at += port_bytes;
    }
    if (d->multiport) {
        uint8_t *data = at + 2 * 4096;
        if (!dvc_queue(d, &d->control_rx, 2, at, data, DVC_CONTROL_BYTES) ||
            !dvc_queue(d, &d->control_tx, 3, at + 4096, data + DVC_QUEUE * DVC_CONTROL_BYTES, DVC_CONTROL_BYTES))
            return "control queues";
        dvc_fill(d, &d->control_rx);
    }
    dvc_write32(d, 0x70, 15);
    if ((dvc_read32(d, 0x70) & 0x4f) != 15) return "device not ready";
    dvc_barrier();
    for (uint32_t i = 0; i < d->ports; ++i) dvc_write32(d, 0x50, d->port[i].rx.index);
    if (d->multiport) {
        dvc_write32(d, 0x50, 2);
        dvc_control(d, 0, DVC_DEVICE_READY, 1);   // the device answers with DEVICE_ADD and PORT_NAME for each port
    }
    return NULL;
}

// Called once from the late board probe (native_touch_probe), after the network and sound devices.
void droidvm_console_start(void) {
    uint32_t enabled = 1;
    PE_parse_boot_argn("droidvm-console", &enabled, sizeof(enabled));
    DTEntry armio, node;
    const void *value;
    unsigned int size;
    uint64_t ranges[3], reg[2];
    if (!enabled || SecureDTLookupEntry(NULL, "/arm-io", &armio) != kSuccess ||
        SecureDTGetProperty(armio, "ranges", &value, &size) != kSuccess || size < sizeof(ranges)) return;
    memcpy(ranges, value, sizeof(ranges));
    for (uint32_t unit = 0; unit < DVC_DEVICES; ++unit) {
        char path[32];
        snprintf(path, sizeof(path), "/arm-io/virtio-console-%u", unit);
        if (SecureDTLookupEntry(NULL, path, &node) != kSuccess) break;
        droidvm_console_found = droidvm_console_found + 1;
        struct dvc_device *d = &dvc_devices[unit];
        d->unit = unit;
        if (SecureDTGetProperty(node, "reg", &value, &size) != kSuccess || size != sizeof(reg)) continue;
        memcpy(reg, value, sizeof(reg));
        if (reg[0] < ranges[0] || reg[1] < 0x108 || reg[1] > ranges[2] || reg[0] - ranges[0] > ranges[2] - reg[1]) continue;
        d->registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
        if (!d->registers || dvc_read32(d, 0x00) != 0x74726976 || dvc_read32(d, 0x04) != 2 || dvc_read32(d, 0x08) != 3) {
            strlcpy(droidvm_console_status[unit], "no virtio-console there", sizeof(droidvm_console_status[unit]));
            continue;
        }
        if (dvc_major < 0) {
            dvc_major = cdevsw_add(-1, &dvc_cdevsw);
            if (dvc_major < 0) {
                strlcpy(droidvm_console_status[unit], "no character device major", sizeof(droidvm_console_status[unit]));
                return;
            }
            // kevent on these ttys through the tty filter, as select already does
            (void)cdevsw_setkqueueok(dvc_major, &dvc_cdevsw, 0);
        }
        const char *why = dvc_bring_up(d);
        if (why) {
            strlcpy(droidvm_console_status[unit], why, sizeof(droidvm_console_status[unit]));
            (void)dvc_reset_device(d);
            continue;
        }
        if (SecureDTGetProperty(node, "droidvm,intid", &value, &size) == kSuccess && size == sizeof(uint32_t)) {
            uint32_t intid;
            memcpy(&intid, value, sizeof(intid));
            d->interrupt = armcpu_spi_attach(intid, false, dvc_interrupt, d, NULL, NULL) == 0;
        }
        snprintf(droidvm_console_status[unit], sizeof(droidvm_console_status[unit]), "up: %u port(s)%s%s", d->ports,
                 d->multiport ? ", multiport" : "", d->interrupt ? ", interrupt" : ", polled");
        thread_t thread;
        if (kernel_thread_start(dvc_worker, d, &thread) == KERN_SUCCESS) {
            thread_deallocate(thread);
            droidvm_console_started = droidvm_console_started + 1;
        }
    }
}
