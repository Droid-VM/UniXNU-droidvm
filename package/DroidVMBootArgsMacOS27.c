// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The macOS 27 guest's built-in boot arguments (ipados27/board.toml [os_overrides."macos-27.0.1-26A434"], build-droidvm-board-core.py
// takes this file instead of DroidVMBootArgs27.c for a macosx kernel). PE_parse_boot_argn and PE_parse_boot_arg_str look an argument up
// here when the command line does not have it; the command line wins, so name=0 there turns a switch off for one boot.
//
// Only what does not depend on the userspace, and the VideoToolbox host: the iPadOS file's App cryptex, owner data protection and
// staged apps are iPadOS's (macOS's equivalents come with the macOS bring-up, meta todo/macos-bringup.md).
char PE_builtin_boot_args[] =
    // the 24 MHz timebase user space expects on Apple silicon (the guest's counter runs at 19.2 MHz): DroidVMClock.c / timebase patch
    "droidvm-timebase-hz=24000000 "
    // a guest vCPU can be descheduled by the host for longer than a native core: scale the lock and debugger acknowledgement timeouts
    "ml-timeout-lock-panic-scale=40 "
    "ml-timeout-lock-scale=40 "
    "ml-timeout-debug-ack-scale=100 "
    // no panic for a slow MMIO access: a development kernel times every ml_io_read/ml_io_write (Apple's PCI drivers' register
    // accesses) against the interrupt-masked timeout, 500 us, and panics past it; in a VM each access traps to crosvm, which took
    // 557 us for the xHCI driver's host controller reset once (5566, 11-18: "Write to IO vaddr ... paddr 0x2000020 (value: 0x2)
    // timed out"). 0 here keeps the reports off instead of inheriting that timeout.
    "ml-timeout-report-phy-read-delay=0 "
    "ml-timeout-report-phy-write-delay=0 "
    // the console on the VM's UART and the serial keyboard (droidvm-serial-uarts.patch), as on iPadOS
    "serial=3 "
    // every process may open the GPU's accelerator, the sandbox deciding (DroidVMGpu.cpp grantedProcesses), as on iPadOS
    "droidvm-gpu-clients=* "
    // VideoToolbox's H.264 and HEVC on the phone's codecs, as on iPadOS: Apple's paravirtualization driver published at boot
    // (DroidVMVTParavirt.cpp; only when crosvm gives a virtio-media decoder or encoder), the guest kernel playing the host
    // (DroidVMVTHost.cpp). Without it macOS's VideoToolbox never saw a host and decoded everything in software: the lock screen's
    // Aerial (4096x2160 HEVC Main10, WallpaperAerials) opened no session at all after the merge. With it, 5566 11-15: the Aerial's
    // sessions reach msm_vidc (crosvm backend=auto) in decode order and decode into the player's pool surfaces in place.
    "droidvm-vt-paravirt=1 "
    // the hypervisor hidden from Weather (the shared arch series' zzyd-owner-hide-vmm.patch: kern.hv_vmm_present 0 for the processes
    // named): under a VMM its VFX makes no argument encoder, so the materials bind no sampler or texture and its sky draws no cloud
    // (Droid-VM/macOS-DroidVM f73fe08, whose own patch did the same for boot-arg vmm-hidden=, Weather by default). Every other
    // process, WindowServer among them, still reads 1.
    "owner-hide-vmm=Weather "
    // verbose boot: the console shows the kernel log until WindowServer takes the display
    "-v";
