// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The guest's restart and shutdown. XNU's halt_all_cpus ends in PEHaltRestart(kPERestartCPU or kPEHaltCPU), which the platform
// expert answers through PE_halt_restart; this board's (ArmDTPlatform) sets none, so a reboot printed "MACH Reboot" and the
// calling vCPU spun in halt_all_cpus' loop for good, the others stopped, the VM never reset (a macOS guest after its first login
// swap, 2026-11-14). Here restart is PSCI SYSTEM_RESET and halt SYSTEM_OFF, through the conduit the device tree's /psci names
// (crosvm's: hvc); the VMM then ends or restarts the VM as for any guest. Every other request (the panic paths' kPEPanicRestartCPU,
// kPEPanicSync, ...) is left as it was, unanswered, so a panicked guest stays up for the host rig to read (panic.py).
// Installed by the late probe (DroidVMLateProbe.c), on iPadOS 27 and macOS alike.
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <ptrauth.h>
#include <stdint.h>
#include <string.h>

extern void proc_selfname(char *buf, int size);

extern int (*PE_halt_restart)(unsigned int type);
// IOKit/IOPlatformExpert.h's request types (its C++ header): the two answered here
enum { DroidVMHaltCPU = 0, DroidVMRestartCPU = 1 };

// For the host rig (peek.py): 0 not installed, 1 hvc, 2 smc; and the last request, type + 1.
volatile uint32_t droidvm_reset_conduit, droidvm_reset_request;

static uint64_t droidvm_psci(uint64_t function)
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = 0, x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
    if (droidvm_reset_conduit == 2)
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    else
        __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    return x0;
}

static int droidvm_halt_restart(unsigned int type)
{
    droidvm_reset_request = type + 1;
    // Who asked: the process and the return addresses of the frames above (symbolized with the kernel's map on the host).
    char name[32] = {};
    proc_selfname(name, sizeof(name));
    printf("droidvm-reset: type %u by %s, from", type, name);
    const uintptr_t *fp = (const uintptr_t *)__builtin_frame_address(0);
    for (int i = 0; i < 12 && fp && !((uintptr_t)fp & 7) && (uintptr_t)fp >> 48 == 0xffff; i++) {
        printf(" %lx", (unsigned long)(uintptr_t)ptrauth_strip((void *)fp[1], ptrauth_key_return_address));
        fp = (const uintptr_t *)fp[0];
    }
    printf("\n");
    // droidvm-reset-hold=1: the stopped guest stays as it was, its log readable (who asked, what came before), as without a handler
    uint32_t hold = 0;
    if (PE_parse_boot_argn("droidvm-reset-hold", &hold, sizeof(hold)) && hold)
        return -1;
    if (type == DroidVMRestartCPU)
        droidvm_psci(0x84000009);   // SYSTEM_RESET: no return
    else if (type == DroidVMHaltCPU)
        droidvm_psci(0x84000008);   // SYSTEM_OFF: no return
    return -1;                      // as without a handler
}

void droidvm_reset_install(void)
{
    if (PE_halt_restart)
        return;   // the platform has its own
    DTEntry psci = NULL;
    const void *method = NULL;
    unsigned int size = 0;
    droidvm_reset_conduit = 1;
    if (SecureDTLookupEntry(NULL, "/psci", &psci) == kSuccess &&
        SecureDTGetProperty(psci, "method", &method, &size) == kSuccess && size >= 3 && !strncmp(method, "smc", 3))
        droidvm_reset_conduit = 2;
    PE_halt_restart = droidvm_halt_restart;
}
