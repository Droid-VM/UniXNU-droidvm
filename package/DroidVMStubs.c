// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Definitions the shared display/HID/key-store probes expect from sheng's board objects, which a DroidVM
// guest does not link: there is no SPI touch panel, cpufreq or Adreno power domain to probe (the GPU hook,
// sheng_gpu_power_probe, is DroidVMGpu.cpp's: NativeAdreno and the Metal accelerator published for the virtio GPU).
//
// The touch digitizer of NativeHidInput (sheng-touch-hid=1) reads its finger set from these globals with a
// seqlock (odd while being written). On sheng the touch probe fills them; in a DroidVM guest the app's touch
// screen does through virtio-input (DroidVMVirtioInput.c), and the host rig can through guest RAM
// (tools/rig/touch.py), the way UniXNU's QEMU live-hid.py does through gdb.
#include <stdint.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <pexpert/pexpert.h>

extern void IOSleep(unsigned milliseconds);
extern void boot_process_capture(void);
extern void boot_process_snapshot(void);
extern void droidvm_export_poll(void);
extern void droidvm_fix_ownervisor_bootstrap(void);
extern void droidvm_input_start(void);
extern void droidvm_net_start(void);
extern void droidvm_snd_start(void);
extern int droidvm_battery_attach(void);
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
// after bsd_autoconf, in place of sheng's touch bring-up, which is where the virtio-input devices start too.
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
        // Hand the owner service manager launchd's bootstrap port once launchd is up (self-gating: a no-op after it
        // takes once, and when owner-service-manager=1 never spawned ownervisor). See DroidVMLaunchPolicy.c.
        droidvm_fix_ownervisor_bootstrap();
        IOSleep(100);
    }
}

void native_touch_probe(void) {
    thread_t thread;
    droidvm_input_start();
    droidvm_net_start();
    droidvm_snd_start();
    droidvm_battery_attach();
    if (!PE_parse_boot_argn("droidvm-monitor", &droidvm_monitor_interval, sizeof(droidvm_monitor_interval)) ||
        !droidvm_monitor_interval)
        return;
    if (kernel_thread_start(droidvm_monitor, NULL, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}
void sheng_cpufreq_probe(void) {}
// NativeHidInput hands its button sink to sheng's ButtonProbe; this board has no buttons to report.
void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {(void)sink;(void)context;}

// The shared probes declare sheng's diagnostic page (BootProgress.c) weak and test for it, but a static kernel link
// still needs a definition. These keep the behaviour of a board without it: no diagnostic writer to yield to and no
// anchor. (NativeRamBlock's store window is DroidVMDataStore.c.)
int boot_progress_prepare_scanout(void) { return 1; }
void boot_progress_cancel_scanout(void) {}
int boot_progress_pulse_anchor(uintptr_t base, uint64_t bytes, unsigned width, unsigned height, unsigned row) {
    (void)base; (void)bytes; (void)width; (void)height; (void)row;
    return 0;
}
// The one message the display probe would put on the diagnostic page goes to the kernel log instead.
void boot_progress_stage(const char *message) { printf("boot-progress: %s\n", message); }

// NativeAdrenoPublish hands the render server's pipeline caches to sheng's UFS log (BootLog.c) piece by piece; this board has no
// export area, so every piece is refused (the render server keeps its caches in memory).
int boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset, const void *data, uint32_t size) {
    (void)generation; (void)total; (void)offset; (void)data; (void)size;
    return -1;
}

// NativeVirtioBlock asks NativeUfs whether a provider is a UFS window; -1 says it is not.
int native_ufs_block_authenticates(void *provider, const char *uuid, uint8_t *digest) {
    (void)provider; (void)uuid; (void)digest;
    return -1;
}

// GopFramebuffer's direct scanout (sheng-dpu-scanout=2: the display hardware fetches the compositor's surface through sheng's UFS
// display window) imports these weakly from sheng's UFS driver, which this board does not have; the kernel's static link still
// needs a definition. The state's ready bit (31) stays clear, so the display never takes that path and keeps its copy.
volatile uint32_t native_ufs_dpu_state = 0;
uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count) {(void)slot; (void)pages; (void)count; return 0;}
