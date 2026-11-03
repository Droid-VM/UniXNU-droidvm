// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The iPadOS 27 board's built-in boot arguments (ipados27/board.toml), as DroidVMBootArgs.c is the iPadOS 17 board's: PE_parse_boot_argn
// and PE_parse_boot_arg_str look an argument up here when the command line (kernel_args, which DarwinBoot keeps to 1023 bytes and
// which already holds most of them) does not have it (the iPadOS 27 bring-up patch's weak PE_builtin_boot_args, which this
// definition replaces); the command line wins, so a switch set here is turned off for one boot with name=0 there.
char PE_builtin_boot_args[] =
    // jobs this guest cannot serve, disabled by the launch policy before launchd starts (DroidVMLaunchPolicy.c): watchdogd has no
    // watchdog backend, identityservicesd, CommCenter, fairplayd and accessoryupdaterd wait for hardware or services there are none of.
    // Not bluetoothd, though it finds no Bluetooth controller and exits ("Unable to match bluetooth service in device tree"), launchd
    // starting it again every 10 seconds: disabled, its Mach services are gone, and audiomxd's BTAudioHALPlugin reconnects from its
    // connection's error handler at once, looking com.apple.BTAudioHALPlugin.xpc up about 1900 times a second, and launchd, which
    // reads the password file for each failed lookup (no system-proxy persona), spent 90% of a CPU answering it.
    "droidvm-disable-jobs=watchdogd,identityservicesd,CommCenter,fairplayd.A2,accessoryupdaterd "
    // the deletable system apps of the OS volume's staged_system_apps in /Applications (the bring-up patch's union) and lsd's
    // first-boot semaphore that keeps them registered (DroidVMLaunchPolicy.c)
    "owner-staged-apps=1 "
    // Data protection classes on a Data volume without content protection (the bring-up patch): PosterBoard's wallpaper store
    "owner-dataprotect=1 "
    // the App cryptex's daemons and the owner binaries (ownervisor, the debug tools) run without the launch constraints of the
    // platform category their trust entries lack (fork patches/xnu-12377/zzyc-owner-skip-launch-constraints.patch: sheng-no-lc;
    // iPadOS 17's patch read owner-skip-launch-constraints): AMFI refused guidedbrowsingd and webinspectord from /private/preboot
    "sheng-no-lc=1 "
    // the App cryptex (Safari's daemons and settings) from the seed's /private/var/ngpu/preboot over /private/preboot, and the owner
    // service manager (ownervisor: ssh on port 2222 and the debug shell), both started by the launch policy (DroidVMLaunchPolicy.c)
    "owner-app-cryptex=1 "
    "owner-service-manager=1 "
    // the hypervisor hidden from the audio server (fork patches/xnu-12377/zzyd-owner-hide-vmm.patch: kern.hv_vmm_present 0 for the
    // processes named): MobileGestalt answers IsVirtualDevice from it, and in a virtual device iPadOS 27's VirtualAudio expects
    // Apple's research VM and publishes no audio device but the Null one. Not for everyone: SpringBoard's backlight state machine
    // then waits on panel hardware and its watchdog kills SpringBoard. And from Safari's GPU process: iPadOS 27's WebGPU.framework asks
    // kern.hv_vmm_present and, in a virtual machine, makes an adapter only when the WebKitAllowWebGPUOnVMs default is set, so no page
    // had WebGPU; this guest's GPU is the phone's own Adreno, not a paravirtual one.
    "owner-hide-vmm=audiomxd,com.apple.WebKit.GPU "
    // VideoToolbox's H.264 and HEVC on the phone's codecs: Apple's paravirtualization driver published at boot (DroidVMVTParavirt.cpp;
    // only when crosvm gives a virtio-media decoder or encoder), the guest kernel playing the host (DroidVMVTHost.cpp). VideoToolbox
    // asks whether it runs in a guest once per process, so a switch made later reaches only processes started after it (videocodecd
    // keeps the answer it had). In a guest it also drops the hardware codecs that are not paravirtualized (AVD, AVE: there is no such
    // hardware here) and relaxes MediaToolbox's screen-protection checks; CMPhoto asks for a paravirtualized JPEG codec, is refused
    // and keeps its software one
    "droidvm-vt-paravirt=1 "
    // the timebase userspace sees, Apple's 24 MHz over the host's 19.2 MHz counter (patches/xnu-12377/droidvm-timebase-24mhz.patch):
    // libswift_Concurrency converts Task.sleep and every ContinuousClock deadline to ticks as ns * 3 / 125, and on the host's rate
    // they waited a quarter of the host's uptime (Settings' debounced sidebar updates never arrived)
    "droidvm-timebase-hz=24000000 "
    // memory pressure (the warning that has Turnip's freed-BO cache, the Metal plugin's held imports and Apple's frameworks give
    // their caches back) below 1280 MiB of available memory rather than XNU's 848 at 6 GiB: the GPU's memory is wired guest
    // memory, and the guest ran out of free pages and paged its files in through the seed disk's polled reads with 1.1-1.3 GiB
    // still counted available, never reaching the warning
    "memorystatus_pressure_threshold_mb=1280 "
    // 40 times the time every other CPU has to enter and to leave the debugger (osfmk/arm/model_dep.c debug_ack_timeout, 240000
    // timebase ticks: 12.5 ms here): a stackshot takes the debugger, and a vCPU the host keeps off its core longer than that made it
    // a panic ("Resuming from debugger synchronization failed", osanalyticshelper's stackshot at boot, 2026-11-01)
    "ml-timeout-debug-ack-scale=40 "
    // the processes besides the render server that get the accelerator's user client and a native Metal device (the launch
    // policy writes the plugin's NativeAdrenoMetal.clients from it at every boot): "*", every process, as on an iPad, the sandbox
    // deciding each open as for Apple's GPU (DroidVMGpu.cpp grantedProcesses, droidvm-gpu-sandbox-first.patch). A named list
    // (with "@apps" for every app) was the rule while each client's cost and the plugin's gaps were unknown: iPadOS 17's 14 clients
    // took the host to swap, and Maps' VectorKit once wedged the render server. Measured 2026-11-03 with every process: 6 resident
    // clients after boot (SpringBoard, the Dock folders, a poster, the widget renderer, the icon service, the render server), the
    // guest's wired memory as with the list, and Maps draws its map. SpringBoard with a device offers iPadOS's windowed apps; in
    // that mode no home indicator shows (the 11-02 "home gesture" report), and the bottom-edge swipe goes home in either mode.
    // Here rather than on the command line, which it no longer fits (1023 bytes).
    "droidvm-gpu-clients=*";
