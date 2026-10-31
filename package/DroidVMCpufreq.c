// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The performance the guest's own scheduler knows it needs, told to the host. crosvm's virtual cpufreq device
// (--virt-cpufreq-upstream, the interface of Linux's virtual-cpufreq driver) has one 4 KiB window per vCPU; DarwinBoot publishes
// the FDT's qemu,virtual-cpufreq node as /arm-io/cpufreq. A write of a frequency (kHz, one of the window's table) to SET_PERF
// traps to crosvm on the writing vCPU's own thread, which sets its utilization clamp to match: the host's governor then runs
// that core at the frequency asked, from the next scheduling event (0.2-7 ms measured on the phone, against 40-116 ms for the
// governor to climb from the bottom on its own).
//
// Without it the host sees each vCPU 30-47% busy through a page swipe and keeps the cores at 0.9-1.5 GHz, while the drops come
// from one guest thread running flat out on one vCPU for 12-35 ms: the governor measures the work done at the current frequency
// and climbs about 25% per 16 ms window, so the burst is over before the core is fast.
//
// The request follows XNU's performance-control classes (thread_get_perfcontrol_class), through the CLPC interface's context
// switch callout (sched_perfcontrol_csw, every switch, the idle thread's included): a vCPU asks for its top frequency when a
// thread of a class in droidvm_cpufreq_classes comes on (UI, above-UI and realtime: the compositor, the app's main thread,
// their frame workers) and for its lowest once threads of other classes have had it for droidvm-cpufreq-hold microseconds
// (default 2000) with none of those. A vCPU going idle keeps its request: the clamp only acts while its thread runs. A vCPU
// writes only on a change, a few times per frame at most.
//
// droidvm-cpufreq=0 leaves the device alone. droidvm_cpufreq_mode (poke.py; 1 on, 0 off) switches it at run time: off, each vCPU
// drops its request at its next switch.
//
// The same callout places threads on the host's big cores. With __ARM_AMP__ (droidvm-amp.patch) the ADT's cluster-types make an E
// and a P cluster (DarwinBoot: P where the FDT's capacity-dmips-mhz is the highest), and the Edge scheduler keeps each thread
// group on its preferred cluster, which without CLPC stays the boot CPU's: the E cluster. So each thread group, the first time
// one of its threads comes on, gets what CLPC would give it (sched_perfcontrol_thread_group_preferred_clusters_set): the E
// cluster, and the P cluster for the droidvm_cpufreq_classes threads. The first byte of its machine data (8 bytes, CLPC's, zeroed
// at creation and unused without it) marks it done. droidvm-cluster-prefer=0 leaves placement to the scheduler's default.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <kern/clock.h>
#include <kern/kern_types.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

#define CPUFREQ_WINDOW     0x1000
#define CPUFREQ_SET_PERF   0x04
#define CPUFREQ_TBL_LEN    0x08
#define CPUFREQ_TBL_SEL    0x0c
#define CPUFREQ_TBL_RD     0x10
#define CPUFREQ_MAX_CPUS   8

#define CPUFREQ_CLASSES ((1u << PERFCONTROL_CLASS_UI) | (1u << PERFCONTROL_CLASS_ABOVEUI) | (1u << PERFCONTROL_CLASS_REALTIME))

volatile uint32_t droidvm_cpufreq_stage;       // 1 window found, 2 mapped, 3 tables read, 4 callout registered
volatile uint32_t droidvm_cpufreq_mode = 1;
volatile uint32_t droidvm_cpufreq_classes = CPUFREQ_CLASSES;
volatile uint32_t droidvm_cpufreq_cpus;
volatile uint32_t droidvm_cpufreq_fmin[CPUFREQ_MAX_CPUS], droidvm_cpufreq_fmax[CPUFREQ_MAX_CPUS];
volatile uint64_t droidvm_cpufreq_raises[CPUFREQ_MAX_CPUS], droidvm_cpufreq_drops[CPUFREQ_MAX_CPUS];
volatile uint32_t droidvm_cpufreq_e_cluster = UINT32_MAX, droidvm_cpufreq_p_cluster = UINT32_MAX;
volatile uint64_t droidvm_cpufreq_groups;     // thread groups given their preferred clusters

static uintptr_t cpufreq_regs;
static uint64_t cpufreq_hold;
static struct {
    uint64_t hot;   // when a thread of a boosted class last came on
    bool high;
} __attribute__((aligned(64))) cpufreq_state[CPUFREQ_MAX_CPUS];

static inline volatile uint32_t *cpufreq_reg(uint32_t cpu, uint32_t offset) {
    return (volatile uint32_t *)(cpufreq_regs + (uintptr_t)cpu * CPUFREQ_WINDOW + offset);
}

static void cpufreq_set(uint32_t cpu, bool high) {
    cpufreq_state[cpu].high = high;
    *cpufreq_reg(cpu, CPUFREQ_SET_PERF) = high ? droidvm_cpufreq_fmax[cpu] : droidvm_cpufreq_fmin[cpu];
    if (high) droidvm_cpufreq_raises[cpu]++;
    else droidvm_cpufreq_drops[cpu]++;
}

static void cpufreq_prefer(void *group) {
    uint32_t overrides[PERFCONTROL_CLASS_MAX];
    for (uint32_t class = 0; class < PERFCONTROL_CLASS_MAX; class++) {
        overrides[class] = ((droidvm_cpufreq_classes >> class) & 1) ? droidvm_cpufreq_p_cluster
                                                                     : SCHED_PERFCONTROL_PREFERRED_CLUSTER_OVERRIDE_NONE;
    }
    sched_perfcontrol_thread_group_preferred_clusters_set(group, droidvm_cpufreq_e_cluster, overrides, 0);
    droidvm_cpufreq_groups++;
}

static void cpufreq_csw(perfcontrol_event event, uint32_t cpu, uint64_t timestamp, uint32_t flags,
                        struct perfcontrol_thread_data *offcore, struct perfcontrol_thread_data *oncore,
                        struct perfcontrol_cpu_counters *counters, uint64_t *timeout_ticks) {
    (void)event; (void)flags; (void)offcore; (void)counters; (void)timeout_ticks;
    const perfcontrol_class_t class = oncore->perfctl_class;
    if (class == PERFCONTROL_CLASS_IDLE) return;
    volatile uint8_t *group = oncore->thread_group_data;
    if (droidvm_cpufreq_p_cluster != UINT32_MAX && group && !*group) {
        *group = 1;
        cpufreq_prefer((void *)(uintptr_t)group);
    }
    if (cpu >= droidvm_cpufreq_cpus) return;
    if (droidvm_cpufreq_mode && ((droidvm_cpufreq_classes >> class) & 1)) {
        cpufreq_state[cpu].hot = timestamp;
        if (!cpufreq_state[cpu].high) cpufreq_set(cpu, true);
    } else if (cpufreq_state[cpu].high && (!droidvm_cpufreq_mode || timestamp - cpufreq_state[cpu].hot > cpufreq_hold)) {
        cpufreq_set(cpu, false);
    }
}

static struct sched_perfcontrol_callbacks cpufreq_callbacks = {
    // Version 5 (the context switch and state update callouts) and no later one: 6-10 hand thread group flags, work interval
    // control, blocking relations and shared resource hints to the controller, which change what the scheduler does.
    .version = SCHED_PERFCONTROL_CALLBACKS_VERSION_5,
    .csw = cpufreq_csw,
};

// The device's vCPU windows, mapped, with each one's frequency table (the host core's): how many, 0 without the device.
static uint32_t cpufreq_map(void) {
    DTEntry armio, node; const void *value; unsigned size; uint64_t ranges[3], reg[2];
    if (SecureDTLookupEntry(NULL, "/arm-io", &armio) != kSuccess ||
        SecureDTGetProperty(armio, "ranges", &value, &size) != kSuccess || size < sizeof ranges) {
        return 0;
    }
    memcpy(ranges, value, sizeof ranges);
    if (SecureDTLookupEntry(NULL, "/arm-io/cpufreq", &node) != kSuccess ||
        SecureDTGetProperty(node, "reg", &value, &size) != kSuccess || size != sizeof reg) {
        return 0;
    }
    memcpy(reg, value, sizeof reg);
    droidvm_cpufreq_stage = 1;
    if (reg[0] < ranges[0] || reg[1] < CPUFREQ_WINDOW || reg[1] > ranges[2] || reg[0] - ranges[0] > ranges[2] - reg[1]) return 0;
    uint32_t cpus = (uint32_t)(reg[1] / CPUFREQ_WINDOW);
    if (cpus > CPUFREQ_MAX_CPUS) cpus = CPUFREQ_MAX_CPUS;
    if (cpus > (uint32_t)ml_get_cpu_count()) cpus = (uint32_t)ml_get_cpu_count();
    cpufreq_regs = ml_io_map((vm_offset_t)(reg[0] - ranges[0] + ranges[1]), (vm_size_t)cpus * CPUFREQ_WINDOW);
    if (!cpufreq_regs) return 0;
    droidvm_cpufreq_stage = 2;
    for (uint32_t cpu = 0; cpu < cpus; cpu++) {
        const uint32_t entries = *cpufreq_reg(cpu, CPUFREQ_TBL_LEN);
        uint32_t lo = UINT32_MAX, hi = 0;
        for (uint32_t i = 0; i < entries && i < 64; i++) {
            *cpufreq_reg(cpu, CPUFREQ_TBL_SEL) = i;
            const uint32_t khz = *cpufreq_reg(cpu, CPUFREQ_TBL_RD);
            if (khz && khz < lo) lo = khz;
            if (khz > hi) hi = khz;
        }
        if (!hi) return 0;
        droidvm_cpufreq_fmin[cpu] = lo;
        droidvm_cpufreq_fmax[cpu] = hi;
    }
    droidvm_cpufreq_stage = 3;
    return cpus;
}

void droidvm_cpufreq_attach(void);

void droidvm_cpufreq_attach(void) {
    uint32_t enabled = 1, prefer = 1, hold_us = 2000;
    PE_parse_boot_argn("droidvm-cpufreq", &enabled, sizeof(enabled));
    PE_parse_boot_argn("droidvm-cluster-prefer", &prefer, sizeof(prefer));
    PE_parse_boot_argn("droidvm-cpufreq-hold", &hold_us, sizeof(hold_us));
    if (droidvm_cpufreq_stage) return;
    const uint32_t cpus = enabled ? cpufreq_map() : 0;
    // The E and P clusters of an AMP topology (ml_parse_cpu_topology, from the ADT's cluster-types); none on an SMP kernel.
    const ml_topology_info_t *topology = ml_get_topology_info();
    for (uint32_t i = 0; prefer && i < topology->num_clusters; i++) {
        if (topology->clusters[i].cluster_type == CLUSTER_TYPE_E) droidvm_cpufreq_e_cluster = topology->clusters[i].cluster_id;
        if (topology->clusters[i].cluster_type == CLUSTER_TYPE_P) droidvm_cpufreq_p_cluster = topology->clusters[i].cluster_id;
    }
    if (droidvm_cpufreq_e_cluster == UINT32_MAX) droidvm_cpufreq_p_cluster = UINT32_MAX;
    if (!cpus && droidvm_cpufreq_p_cluster == UINT32_MAX) return;
    nanoseconds_to_absolutetime((uint64_t)hold_us * 1000, &cpufreq_hold);
    droidvm_cpufreq_cpus = cpus;
    sched_perfcontrol_register_callbacks(&cpufreq_callbacks, 0);
    droidvm_cpufreq_stage = 4;
    if (cpus) {
        printf("DroidVMCpufreq: %u vCPU windows, %u-%u kHz (cpu0) to %u-%u kHz (cpu%u), hold %u us\n", cpus,
               droidvm_cpufreq_fmin[0], droidvm_cpufreq_fmax[0], droidvm_cpufreq_fmin[cpus - 1], droidvm_cpufreq_fmax[cpus - 1],
               cpus - 1, hold_us);
    }
    if (droidvm_cpufreq_p_cluster != UINT32_MAX) {
        printf("DroidVMCpufreq: thread groups prefer cluster %u (E), classes 0x%x cluster %u (P)\n", droidvm_cpufreq_e_cluster,
               droidvm_cpufreq_classes, droidvm_cpufreq_p_cluster);
    }
}
