// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The macOS 27 guest's built-in boot arguments (ipados27/board.toml [os_overrides."macos-27.0.1-26A434"], build-droidvm-board-core.py
// takes this file instead of DroidVMBootArgs27.c for a macosx kernel). PE_parse_boot_argn and PE_parse_boot_arg_str look an argument up
// here when the command line does not have it; the command line wins, so name=0 there turns a switch off for one boot.
//
// Only what does not depend on the userspace: the iPadOS file's GPU client list, App cryptex, owner data protection, VideoToolbox host
// and staged apps are iPadOS's (macOS's equivalents come with the macOS bring-up, meta todo/macos-bringup.md).
char PE_builtin_boot_args[] =
    // the 24 MHz timebase user space expects on Apple silicon (the guest's counter runs at 19.2 MHz): DroidVMClock.c / timebase patch
    "droidvm-timebase-hz=24000000 "
    // a guest vCPU can be descheduled by the host for longer than a native core: scale the lock and debugger acknowledgement timeouts
    "ml-timeout-lock-panic-scale=40 "
    "ml-timeout-lock-scale=40 "
    "ml-timeout-debug-ack-scale=100 "
    // the console on the VM's UART and the serial keyboard (droidvm-serial-uarts.patch), as on iPadOS
    "serial=3 "
    // every process may open the GPU's accelerator, the sandbox deciding (DroidVMGpu.cpp grantedProcesses), as on iPadOS
    "droidvm-gpu-clients=* "
    // verbose boot: the console shows the kernel log until WindowServer takes the display
    "-v";
