// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The guest scheduler's own record of what its threads do, read by the host from guest RAM (tools/rig/schedtrace.py), so
// nothing runs in the guest to watch it: spindump's sampler took a whole vCPU of six and was itself what the other threads
// queued behind. droidvm-sched-trace.patch calls the two hooks this installs:
//   - every context switch: the thread that came on (its time runnable before, thread->same_pri_latency) and the one that went
//     off (its state, and while it waits its wait event and block hint, still set at this point);
//   - every wakeup: the thread woken and the thread that woke it.
// Records go into a physically contiguous ring the host reads by physical address (droidvm_sched_trace_pa), in mach absolute
// time like the Metal plugin's frames.trace; each thread's process and thread names go into a table after the ring the first
// time the thread comes on.
//
// droidvm_sched_trace_on (poke.py) 1 records, 0 stops; droidvm_sched_trace_head counts the records written (the ring keeps the
// last DROIDVM_SCHED_TRACE_RECORDS; set it to 0 before a capture). droidvm-sched-trace=0 allocates nothing.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <arm/pmap/pmap.h>
#include <vm/vm_kern_xnu.h>
#include <pexpert/pexpert.h>

#define DROIDVM_SCHED_TRACE_RECORDS (1u << 18)
#define DROIDVM_SCHED_TRACE_NAMES   8192

struct droidvm_sched_record {      // 48 bytes
    uint64_t time;                  // mach_absolute_time
    uint8_t type;                   // 1 context switch, 2 wakeup
    uint8_t cpu;
    uint8_t old_state;              // switch: the thread switched from, its state (TH_WAIT 0x01, TH_SUSP 0x02, TH_RUN 0x04, ...)
    uint8_t priority;               // switch: the thread switched to, its scheduled priority; wakeup: the woken thread's
    uint32_t block_hint;            // switch: the thread switched from, its block hint (kern/block_hint.h) while it waits
    uint64_t thread;                // switch: the thread switched to; wakeup: the thread woken
    uint64_t other;                 // switch: the thread switched from; wakeup: the waker
    uint64_t latency;               // switch: the thread switched to, its time runnable before it came on (absolute time)
    uint64_t event;                 // switch: the thread switched from, its wait event while it waits
};

struct droidvm_sched_name {        // 72 bytes
    uint64_t thread;
    int32_t pid;
    uint32_t reserved;
    char process[16];
    char name[40];
};

extern void (*volatile droidvm_sched_trace_switch)(thread_t old, thread_t new, uint64_t timestamp);
extern void (*volatile droidvm_sched_trace_wake)(thread_t thread, thread_t waker);
extern char *proc_name_address(void *p);
extern void bsd_getthreadname(void *uth, char *buffer);

volatile uint32_t droidvm_sched_trace_on;
volatile uint64_t droidvm_sched_trace_head;
volatile uint64_t droidvm_sched_trace_pa, droidvm_sched_trace_bytes;   // the ring, then the name table
const uint32_t droidvm_sched_trace_records = DROIDVM_SCHED_TRACE_RECORDS, droidvm_sched_trace_names = DROIDVM_SCHED_TRACE_NAMES;

static struct droidvm_sched_record *trace_ring;
static struct droidvm_sched_name *trace_names;

static struct droidvm_sched_record *trace_next(void) {
    const uint64_t at = __atomic_fetch_add(&droidvm_sched_trace_head, 1, __ATOMIC_RELAXED);
    return &trace_ring[at & (DROIDVM_SCHED_TRACE_RECORDS - 1)];
}

// The thread's names, the first time it comes on: an open-addressed table keyed by thread id, never emptied (a full one names
// no more threads).
static void trace_name(thread_t thread) {
    const uint64_t id = thread->thread_id;
    const uint32_t hash = (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 51);
    for (uint32_t probe = 0; probe < 32; probe++) {
        struct droidvm_sched_name *slot = &trace_names[(hash + probe) & (DROIDVM_SCHED_TRACE_NAMES - 1)];
        uint64_t seen = __atomic_load_n(&slot->thread, __ATOMIC_RELAXED);
        if (seen == id) return;
        if (seen) continue;
        if (!__atomic_compare_exchange_n(&slot->thread, &seen, id, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            if (seen == id) return;
            continue;
        }
        task_t task = get_threadtask(thread);
        void *proc = task ? get_bsdtask_info(task) : NULL;
        slot->pid = task ? task_pid(task) : -1;
        const char *process = proc ? proc_name_address(proc) : "kernel_task";
        if (process) strlcpy(slot->process, process, sizeof slot->process);
        void *uthread = get_bsdthread_info(thread);
        if (uthread) {
            char name[64] = {0};
            bsd_getthreadname(uthread, name);
            strlcpy(slot->name, name, sizeof slot->name);
        }
        return;
    }
}

static void trace_switch(thread_t old, thread_t new, uint64_t timestamp) {
    if (!droidvm_sched_trace_on) return;
    struct droidvm_sched_record *record = trace_next();
    record->time = timestamp;
    record->type = 1;
    record->cpu = (uint8_t)cpu_number();
    record->old_state = (uint8_t)old->state;
    record->priority = (uint8_t)new->sched_pri;
    record->block_hint = (uint32_t)old->block_hint;
    record->thread = new->thread_id;
    record->other = old->thread_id;
    record->latency = new->same_pri_latency;
    record->event = (uint64_t)old->wait_event;
    trace_name(new);
}

static void trace_wake(thread_t thread, thread_t waker) {
    if (!droidvm_sched_trace_on) return;
    struct droidvm_sched_record *record = trace_next();
    record->time = mach_absolute_time();
    record->type = 2;
    record->cpu = (uint8_t)cpu_number();
    record->old_state = 0;
    record->priority = (uint8_t)thread->sched_pri;
    record->block_hint = 0;
    record->thread = thread->thread_id;
    record->other = waker ? waker->thread_id : 0;
    record->latency = 0;
    record->event = 0;
}

void droidvm_sched_trace_attach(void);

void droidvm_sched_trace_attach(void) {
    uint32_t enabled = 1;
    PE_parse_boot_argn("droidvm-sched-trace", &enabled, sizeof(enabled));
    if (!enabled || trace_ring) return;
    const vm_size_t ring = (vm_size_t)DROIDVM_SCHED_TRACE_RECORDS * sizeof(struct droidvm_sched_record);
    const vm_size_t bytes = round_page(ring + (vm_size_t)DROIDVM_SCHED_TRACE_NAMES * sizeof(struct droidvm_sched_name));
    vm_offset_t va = 0;
    if (kmem_alloc_contig(kernel_map, &va, bytes, (vm_offset_t)PAGE_MASK, 0, 0,
                          KMA_NOPAGEWAIT | KMA_KOBJECT | KMA_PERMANENT, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
        printf("DroidVMSchedTrace: no %lu contiguous bytes\n", (unsigned long)bytes);
        return;
    }
    memset((void *)va, 0, bytes);
    trace_ring = (struct droidvm_sched_record *)va;
    trace_names = (struct droidvm_sched_name *)(va + ring);
    droidvm_sched_trace_bytes = bytes;
    droidvm_sched_trace_pa = kvtophys(va);
    droidvm_sched_trace_wake = trace_wake;
    droidvm_sched_trace_switch = trace_switch;
    printf("DroidVMSchedTrace: %u records + %u names at PA 0x%llx (%lu bytes), off until droidvm_sched_trace_on\n",
           DROIDVM_SCHED_TRACE_RECORDS, DROIDVM_SCHED_TRACE_NAMES, (unsigned long long)droidvm_sched_trace_pa,
           (unsigned long)bytes);
}
