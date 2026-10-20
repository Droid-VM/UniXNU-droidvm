// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Definitions the shared display/HID/key-store probes expect from sheng's board objects, which a DroidVM
// guest does not link: there is no SPI touch panel, cpufreq or Adreno power domain to probe.
//
// The touch digitizer of NativeHidInput (sheng-touch-hid=1) reads its finger set from these globals with a
// seqlock (odd while being written). On sheng the touch probe fills them; in a DroidVM guest the host rig
// writes them through guest RAM (tools/rig), the same way UniXNU's QEMU live-hid.py does through gdb.
#include <stdint.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <pexpert/pexpert.h>

extern void IOSleep(unsigned milliseconds);
extern void boot_process_capture(void);
extern void boot_process_snapshot(void);
extern void droidvm_export_poll(void);
extern unsigned boot_ui_trace_role_rows(unsigned role, char rows[][120], unsigned max, unsigned detail);
extern unsigned boot_ui_trace_hot_rows(char rows[][120], unsigned max);

#define NATIVE_TOUCH_SLOTS 12

volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile uint32_t native_touch_hid_reports;
volatile int native_touch_xs[NATIVE_TOUCH_SLOTS], native_touch_ys[NATIVE_TOUCH_SLOTS], native_touch_ids[NATIVE_TOUCH_SLOTS];

// The diagnostic page's extension rows (XNU_BOOT_PROGRESS); written by the key-store probe, read by nobody here.
char boot_progress_ext[8][48];

// droidvm-monitor=<ms>: without sheng's diagnostic page nothing runs the process probe, so a worker samples it
// every <ms> into boot_process_rows, which the host rig reads from guest RAM (tools/rig/procs.py), and answers the
// host's file requests every 100 ms (DroidVMFileExport.c, tools/rig/export.py). Called from the late display probe,
// after bsd_autoconf, in place of sheng's touch bring-up.
volatile uint32_t droidvm_monitor_passes;
static unsigned droidvm_monitor_interval;
// The traced roles' threads after each pass (BootUiTraceProbe.c: main thread registers and frames, every thread group
// with the pid of the task its Mach message goes to, the hottest threads), for tools/rig/trace.py.
#define DROIDVM_TRACE_ROWS 96
char droidvm_trace_rows[DROIDVM_TRACE_ROWS][120];
volatile uint32_t droidvm_trace_count;

static void droidvm_trace_publish(void) {
    unsigned n = 0;
    for (unsigned role = 0; role < 4 && n + 6 <= DROIDVM_TRACE_ROWS; ++role)
        n += boot_ui_trace_role_rows(role, droidvm_trace_rows + n, DROIDVM_TRACE_ROWS - n, 1);
    if (n < DROIDVM_TRACE_ROWS)
        n += boot_ui_trace_hot_rows(droidvm_trace_rows + n, DROIDVM_TRACE_ROWS - n);
    droidvm_trace_count = n;
}

static void droidvm_monitor(void *parameter, wait_result_t result) {
    (void)parameter; (void)result;
    for (unsigned waited = droidvm_monitor_interval;; waited += 100) {
        if (waited >= droidvm_monitor_interval) {
            boot_process_capture();
            boot_process_snapshot();
            droidvm_trace_publish();
            droidvm_monitor_passes = droidvm_monitor_passes + 1;
            waited = 0;
        }
        droidvm_export_poll();
        IOSleep(100);
    }
}

void native_touch_probe(void) {
    thread_t thread;
    if (!PE_parse_boot_argn("droidvm-monitor", &droidvm_monitor_interval, sizeof(droidvm_monitor_interval)) ||
        !droidvm_monitor_interval)
        return;
    if (kernel_thread_start(droidvm_monitor, NULL, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}
void sheng_cpufreq_probe(void) {}
void sheng_gpu_power_probe(void) {}

// The shared probes and NativeRamBlock declare sheng's diagnostic page (BootProgress.c) and UFS store window
// (BootLog.c) weak and test for them, but a static kernel link still needs a definition. These keep the
// behaviour of a board without them: no diagnostic writer to yield to, no anchor, and the volatile Data disk.
int boot_progress_prepare_scanout(void) { return 1; }
void boot_progress_cancel_scanout(void) {}
int boot_progress_pulse_anchor(uintptr_t base, uint64_t bytes, unsigned width, unsigned height, unsigned row) {
    (void)base; (void)bytes; (void)width; (void)height; (void)row;
    return 0;
}
// The one message the display probe would put on the diagnostic page goes to the kernel log instead.
void boot_progress_stage(const char *message) { printf("boot-progress: %s\n", message); }

unsigned native_ufs_store_mode(void) { return 0; }
int native_ufs_store_open(uint64_t blocks, uint64_t *first, const char **why) {
    (void)blocks; (void)first;
    *why = "no store on this board";
    return 0;
}
int native_ufs_store_read(uint64_t block, uint32_t count, void *data) { (void)block; (void)count; (void)data; return 0; }
int native_ufs_store_write(uint64_t block, uint32_t count, const void *data) { (void)block; (void)count; (void)data; return 0; }
int native_ufs_store_sync(void) { return 0; }
unsigned native_ufs_store_gpt_check(void) { return 0; }
// NativeVirtioBlock asks NativeUfs whether a provider is a UFS window; -1 says it is not.
int native_ufs_block_authenticates(void *provider, const char *uuid, uint8_t *digest) {
    (void)provider; (void)uuid; (void)digest;
    return -1;
}
