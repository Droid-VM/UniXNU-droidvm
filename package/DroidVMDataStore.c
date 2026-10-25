// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The persistent store of the Data disk, over a writable virtio-mmio block device. NativeRamBlock keeps the pages a running
// system changes in a window of 4096-byte blocks (its PersistOverlay.h) through the native_ufs_store_* interface, which
// UniXNU-sheng's NativeUfs provides from the end of the phone's userdata partition. Here the window is a sparse file on the
// host phone: crosvm --virtio-mmio-block, which make-droidvm-adt.py (--data-store-mmio) publishes as /arm-io/data-store with
// compatible "droidvm,data-store", so the read-only, verified NativeVirtioBlock leaves it alone.
//
// Disk layout: block 0 is the label the host writes when it makes the file (tools/rig/make-data-store.sh): "DroidVMDataStore"
// and version 1 (u32, little endian). The window is blocks 1 .. 1+N and must fit the disk.
//
// droidvm-data-persist=<0|1|2>, like sheng-data-persist: 0 (or absent) keeps Data volatile; 1 opens the window read-only
// (NativeRamBlock copies the stored pages into RAM, and writes stay volatile); 2 reads and writes it, and NativeRamBlock serves
// the changed pages from the disk alone (its direct mode).
//
// One polled request at a time, under a mutex, through a bounce buffer in the kernel's static memory, which is physically
// contiguous. A write is complete when crosvm has written it to the file, so it survives a stop or crash of the VM: the host's
// page cache holds it. For a phone that loses power, a flusher thread asks crosvm to fsync the file DROIDVM_STORE_FLUSH_MS after
// a write at the latest, and sync() flushes at once.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

extern void IOSleep(unsigned milliseconds);

#define DROIDVM_STORE_BLOCK 4096u
#define DROIDVM_STORE_STAGE_BLOCKS 32u      // NativeRamBlock's kStagePages: its longest run is one request
#define DROIDVM_STORE_QUEUE 8u
#define DROIDVM_STORE_TIMEOUT_MS 30000u     // a phone's flash can stall a write for seconds; a timeout fails the store for good
#define DROIDVM_STORE_FLUSH_MS 5000u

// VirtIO 1.2 MMIO, split queue, as NativeVirtioBlock: the descriptors at 0, the driver ring at 128, the device ring at 256,
// the request header at 512 and its status byte at 528 of one page.
struct droidvm_store_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct droidvm_store_request { uint32_t type, reserved; uint64_t sector; };
struct droidvm_store_used { uint32_t id, length; };
_Static_assert(sizeof(struct droidvm_store_desc) == 16 && sizeof(struct droidvm_store_request) == 16, "VirtIO wire layout");
enum { VIRTIO_BLK_T_IN = 0, VIRTIO_BLK_T_OUT = 1, VIRTIO_BLK_T_FLUSH = 4 };

static uint8_t droidvm_store_queue[4096] __attribute__((aligned(4096)));
static uint8_t droidvm_store_stage[DROIDVM_STORE_STAGE_BLOCKS * DROIDVM_STORE_BLOCK] __attribute__((aligned(4096)));

// For the host rig (peek.py): state 0 closed, 1 open read-only, 2 open read-write, 3 failed (an I/O error or a timeout: every
// later request is refused); why: the reason of a refused open or of the failure.
volatile uint32_t droidvm_store_state;
const char *volatile droidvm_store_why = "";
volatile uint64_t droidvm_store_disk_blocks, droidvm_store_window_blocks;
volatile uint64_t droidvm_store_reads, droidvm_store_writes, droidvm_store_flushes, droidvm_store_errors;
volatile uint32_t droidvm_store_last_status;

static uintptr_t droidvm_store_registers;
static bool droidvm_store_device, droidvm_store_can_flush, droidvm_store_dirty;
static uint64_t droidvm_store_queue_physical, droidvm_store_stage_physical, droidvm_store_sectors;
static uint16_t droidvm_store_available, droidvm_store_used_index;
static lck_grp_t *droidvm_store_group;
static lck_mtx_t *droidvm_store_lock;

static uint32_t store_read32(unsigned offset) {
    return *(volatile uint32_t *)(droidvm_store_registers + offset);
}
static void store_write32(unsigned offset, uint32_t value) {
    *(volatile uint32_t *)(droidvm_store_registers + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
static void store_write64(unsigned offset, uint64_t value) {
    store_write32(offset, (uint32_t)value);
    store_write32(offset + 4, (uint32_t)(value >> 32));
}
static inline void store_barrier(void) { __asm__ volatile("dmb osh" ::: "memory"); }

static bool store_reset(void) {
    store_write32(0x70, 0);
    uint64_t start = mach_absolute_time(), limit;
    nanoseconds_to_absolutetime(1000 * NSEC_PER_MSEC, &limit);
    while (store_read32(0x70)) {
        if (mach_absolute_time() - start >= limit) return false;
        delay(1);
    }
    return true;
}

// Every request after a failure is refused, and the device is stopped: a store that lost a write must not take another.
static void store_fail(const char *why) {
    droidvm_store_why = why;
    droidvm_store_state = 3;
    droidvm_store_errors = droidvm_store_errors + 1;
    if (!store_reset()) printf("droidvm data store: the device did not reset\n");
    printf("droidvm data store failed: %s (status %u)\n", why, droidvm_store_last_status);
}

// One request, polled: a short spin for the common fast completion, then 1 ms sleeps. Caller holds the lock (or is open).
static bool store_request(uint32_t type, uint64_t sector, uint32_t bytes) {
    struct droidvm_store_desc *descriptors = (struct droidvm_store_desc *)droidvm_store_queue;
    struct droidvm_store_request *request = (struct droidvm_store_request *)(droidvm_store_queue + 512);
    volatile uint8_t *status = droidvm_store_queue + 528;
    const uint64_t queue = droidvm_store_queue_physical;
    *request = (struct droidvm_store_request){type, 0, sector};
    *status = 0xff;
    if (bytes) {
        descriptors[0] = (struct droidvm_store_desc){queue + 512, sizeof(*request), 1, 1};
        // A read's buffer is written by the device (VIRTQ_DESC_F_WRITE).
        descriptors[1] = (struct droidvm_store_desc){droidvm_store_stage_physical, bytes,
                                                     (uint16_t)(type == VIRTIO_BLK_T_IN ? 3 : 1), 2};
    } else {
        descriptors[0] = (struct droidvm_store_desc){queue + 512, sizeof(*request), 1, 2};
    }
    descriptors[2] = (struct droidvm_store_desc){queue + 528, 1, 2, 0};
    volatile uint16_t *available = (volatile uint16_t *)(droidvm_store_queue + 128);
    available[2 + droidvm_store_available % DROIDVM_STORE_QUEUE] = 0;
    store_barrier();
    droidvm_store_available++;
    available[1] = droidvm_store_available;
    store_barrier();
    store_write32(0x50, 0);
    volatile uint16_t *used = (volatile uint16_t *)(droidvm_store_queue + 256);
    uint64_t start = mach_absolute_time(), spin, limit;
    nanoseconds_to_absolutetime(200 * NSEC_PER_USEC, &spin);   // then sleep, so the vCPU goes idle (DroidVMVirtioGpu.cpp pause())
    nanoseconds_to_absolutetime((uint64_t)DROIDVM_STORE_TIMEOUT_MS * NSEC_PER_MSEC, &limit);
    while (used[1] == droidvm_store_used_index) {
        uint64_t waited = mach_absolute_time() - start;
        if (waited >= limit) {
            store_fail("request timed out");
            return false;
        }
        if (waited < spin) delay(1);
        else IOSleep(1);
        store_barrier();
    }
    store_barrier();
    volatile struct droidvm_store_used *element =
        (volatile struct droidvm_store_used *)(droidvm_store_queue + 260) + droidvm_store_used_index % DROIDVM_STORE_QUEUE;
    bool valid = used[1] == (uint16_t)(droidvm_store_used_index + 1) && element->id == 0 && *status == 0 &&
                 (type != VIRTIO_BLK_T_IN || element->length >= bytes + 1);
    droidvm_store_used_index++;
    droidvm_store_last_status = *status;
    uint32_t interrupts = store_read32(0x60) & 3;
    if (interrupts) store_write32(0x64, interrupts);
    if (!valid) store_fail(type == VIRTIO_BLK_T_IN ? "read failed" : type == VIRTIO_BLK_T_OUT ? "write failed" : "flush failed");
    return valid;
}

// Finds /arm-io/data-store, maps it and brings the device up; returns the reason it cannot, or NULL.
static const char *store_attach(unsigned mode) {
    DTEntry armio, node;
    const void *value;
    unsigned int size;
    uint64_t ranges[3], reg[2];
    if (SecureDTLookupEntry(NULL, "/arm-io", &armio) != kSuccess ||
        SecureDTGetProperty(armio, "ranges", &value, &size) != kSuccess || size < sizeof(ranges)) return "no /arm-io ranges";
    memcpy(ranges, value, sizeof(ranges));
    if (SecureDTLookupEntry(NULL, "/arm-io/data-store", &node) != kSuccess) return "no /arm-io/data-store";
    if (SecureDTGetProperty(node, "reg", &value, &size) != kSuccess || size != sizeof(reg)) return "no data-store reg";
    memcpy(reg, value, sizeof(reg));
    if (reg[0] < ranges[0] || reg[1] < 0x108 || reg[1] > ranges[2] || reg[0] - ranges[0] > ranges[2] - reg[1])
        return "data-store reg outside /arm-io";
    droidvm_store_registers = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), 0x1000);
    if (!droidvm_store_registers) return "cannot map the data-store registers";
    if (store_read32(0x00) != 0x74726976 || store_read32(0x04) != 2 || store_read32(0x08) != 2)
        return "data-store is no modern virtio-mmio block device";
    droidvm_store_queue_physical = ml_static_vtop((vm_offset_t)droidvm_store_queue);
    droidvm_store_stage_physical = ml_static_vtop((vm_offset_t)droidvm_store_stage);
    if (ml_static_vtop((vm_offset_t)droidvm_store_stage + sizeof(droidvm_store_stage) - 4096) !=
        droidvm_store_stage_physical + sizeof(droidvm_store_stage) - 4096) return "bounce buffer not contiguous";
    droidvm_store_device = true;
    if (!store_reset()) return "device reset failed";
    store_write32(0x70, 1);
    store_write32(0x70, 3);
    store_write32(0x14, 0);
    uint32_t low = store_read32(0x10);
    store_write32(0x14, 1);
    uint32_t high = store_read32(0x10);
    if (!(high & 1)) return "no VIRTIO_F_VERSION_1";
    bool readOnly = (low & (1u << 5)) != 0;
    if (readOnly && mode == 2) return "the data-store disk is read-only";
    droidvm_store_can_flush = (low & (1u << 9)) != 0;
    store_write32(0x24, 0);
    store_write32(0x20, (readOnly ? 1u << 5 : 0) | (droidvm_store_can_flush ? 1u << 9 : 0));
    store_write32(0x24, 1);
    store_write32(0x20, 1);
    store_write32(0x70, 11);
    if (!(store_read32(0x70) & 8)) return "features refused";
    store_write32(0x30, 0);
    if (store_read32(0x44) || store_read32(0x34) < DROIDVM_STORE_QUEUE) return "no usable queue 0";
    memset(droidvm_store_queue, 0, sizeof(droidvm_store_queue));
    *(volatile uint16_t *)(droidvm_store_queue + 128) = 1;   // VIRTQ_AVAIL_F_NO_INTERRUPT: this driver polls
    droidvm_store_available = droidvm_store_used_index = 0;
    store_barrier();
    store_write32(0x38, DROIDVM_STORE_QUEUE);
    store_write64(0x80, droidvm_store_queue_physical);
    store_write64(0x90, droidvm_store_queue_physical + 128);
    store_write64(0xa0, droidvm_store_queue_physical + 256);
    store_write32(0x44, 1);
    bool stable = false;
    for (unsigned attempt = 0; attempt < 8 && !stable; ++attempt) {
        uint32_t generation = store_read32(0xfc);
        uint64_t sectors = store_read32(0x100) | (uint64_t)store_read32(0x104) << 32;
        if (generation == store_read32(0xfc)) {
            droidvm_store_sectors = sectors;
            stable = true;
        }
    }
    if (!stable) return "unstable capacity";
    store_write32(0x70, 15);
    if ((store_read32(0x70) & 0x4f) != 15) return "device not ready";
    return NULL;
}

static void store_flusher(void *parameter, wait_result_t result) {
    (void)parameter; (void)result;
    for (;;) {
        IOSleep(DROIDVM_STORE_FLUSH_MS);
        lck_mtx_lock(droidvm_store_lock);
        if (droidvm_store_state == 2 && droidvm_store_dirty && store_request(VIRTIO_BLK_T_FLUSH, 0, 0)) {
            droidvm_store_dirty = false;
            droidvm_store_flushes = droidvm_store_flushes + 1;
        }
        lck_mtx_unlock(droidvm_store_lock);
    }
}

static unsigned store_mode(void) {
    static int cached = -1;
    if (cached < 0) {
        uint32_t value = 0;
        if (!PE_parse_boot_argn("droidvm-data-persist", &value, sizeof(value)) || value > 2) value = 0;
        cached = (int)value;
    }
    return (unsigned)cached;
}

unsigned native_ufs_store_mode(void) { return store_mode(); }

// Returns 1 and the first 4096-byte block of the window on the disk, 0 on refusal (the reason in *why, which may be null).
int native_ufs_store_open(uint64_t blocks, uint64_t *first, const char **why) {
    const char *reason = NULL;
    unsigned mode = store_mode();
    if (!mode) reason = "off";
    else if (droidvm_store_state || droidvm_store_lock) reason = "already open";
    else if (!(droidvm_store_group = lck_grp_alloc_init("droidvm-data-store", LCK_GRP_ATTR_NULL)) ||
             !(droidvm_store_lock = lck_mtx_alloc_init(droidvm_store_group, LCK_ATTR_NULL))) reason = "no lock";
    else reason = store_attach(mode);
    if (!reason) {
        droidvm_store_disk_blocks = droidvm_store_sectors / 8;
        if (!blocks || droidvm_store_disk_blocks < 1 || blocks > droidvm_store_disk_blocks - 1)
            reason = "the window does not fit the disk";
    }
    if (!reason) {
        droidvm_store_state = 1;   // for store_request's failure path; the label decides
        if (!store_request(VIRTIO_BLK_T_IN, 0, DROIDVM_STORE_BLOCK)) reason = "cannot read the label";
        else {
            uint32_t version;
            memcpy(&version, droidvm_store_stage + 16, sizeof(version));
            if (memcmp(droidvm_store_stage, "DroidVMDataStore", 16) || version != 1) reason = "no DroidVMDataStore label";
        }
    }
    if (reason) {
        if (droidvm_store_device && droidvm_store_state != 3) (void)store_reset();
        droidvm_store_state = droidvm_store_state == 3 ? 3 : 0;
        droidvm_store_why = reason;
        if (why) *why = reason;
        printf("droidvm data store refused: %s\n", reason);
        return 0;
    }
    droidvm_store_window_blocks = blocks;
    droidvm_store_state = mode == 2 ? 2 : 1;
    if (mode == 2 && droidvm_store_can_flush) {
        thread_t thread;
        if (kernel_thread_start(store_flusher, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    }
    if (first) *first = 1;
    if (why) *why = "";
    printf("droidvm data store: %llu-block window on a %llu-block disk, %s%s\n", (unsigned long long)blocks,
           (unsigned long long)droidvm_store_disk_blocks, mode == 2 ? "read-write" : "read-only",
           droidvm_store_can_flush ? ", flushed" : ", no flush command");
    return 1;
}

static bool store_open_for(uint64_t rel, uint32_t count, bool write) {
    uint32_t state = droidvm_store_state;
    return (state == 2 || (state == 1 && !write)) && count && rel < droidvm_store_window_blocks &&
           count <= droidvm_store_window_blocks - rel;
}

int native_ufs_store_read(uint64_t rel, uint32_t count, void *destination) {
    if (!destination || !store_open_for(rel, count, false)) return 0;
    uint8_t *to = destination;
    bool ok = true;
    lck_mtx_lock(droidvm_store_lock);
    while (count && (ok = store_open_for(rel, count, false))) {
        uint32_t n = count > DROIDVM_STORE_STAGE_BLOCKS ? DROIDVM_STORE_STAGE_BLOCKS : count;
        if (!(ok = store_request(VIRTIO_BLK_T_IN, (1 + rel) * 8, n * DROIDVM_STORE_BLOCK))) break;
        memcpy(to, droidvm_store_stage, (size_t)n * DROIDVM_STORE_BLOCK);
        droidvm_store_reads = droidvm_store_reads + n;
        to += (size_t)n * DROIDVM_STORE_BLOCK;
        rel += n;
        count -= n;
    }
    lck_mtx_unlock(droidvm_store_lock);
    return ok;
}

int native_ufs_store_write(uint64_t rel, uint32_t count, const void *source) {
    if (!source || !store_open_for(rel, count, true)) return 0;
    const uint8_t *from = source;
    bool ok = true;
    lck_mtx_lock(droidvm_store_lock);
    while (count && (ok = store_open_for(rel, count, true))) {
        uint32_t n = count > DROIDVM_STORE_STAGE_BLOCKS ? DROIDVM_STORE_STAGE_BLOCKS : count;
        memcpy(droidvm_store_stage, from, (size_t)n * DROIDVM_STORE_BLOCK);
        if (!(ok = store_request(VIRTIO_BLK_T_OUT, (1 + rel) * 8, n * DROIDVM_STORE_BLOCK))) break;
        droidvm_store_dirty = true;
        droidvm_store_writes = droidvm_store_writes + n;
        from += (size_t)n * DROIDVM_STORE_BLOCK;
        rel += n;
        count -= n;
    }
    lck_mtx_unlock(droidvm_store_lock);
    return ok;
}

int native_ufs_store_sync(void) {
    if (droidvm_store_state != 2) return 0;
    if (!droidvm_store_can_flush) return 1;
    lck_mtx_lock(droidvm_store_lock);
    bool ok = droidvm_store_state == 2 && store_request(VIRTIO_BLK_T_FLUSH, 0, 0);
    if (ok) {
        droidvm_store_dirty = false;
        droidvm_store_flushes = droidvm_store_flushes + 1;
    }
    lck_mtx_unlock(droidvm_store_lock);
    return ok;
}

// NativeUfs re-reads its GPT to prove the store did not touch the partition table; this disk holds nothing else.
unsigned native_ufs_store_gpt_check(void) { return 0; }
