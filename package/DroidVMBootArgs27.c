// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The iPadOS 27 board's built-in boot arguments (ipados27/board.toml), as DroidVMBootArgs.c is the iPadOS 17 board's: PE_parse_boot_argn
// and PE_parse_boot_arg_str look an argument up here when the command line (kernel_args, which DarwinBoot keeps to 1023 bytes and
// which already holds most of them) does not have it (the iPadOS 27 bring-up patch's weak PE_builtin_boot_args, which this
// definition replaces); the command line wins, so a switch set here is turned off for one boot with name=0 there.
char PE_builtin_boot_args[] =
    // jobs this guest cannot serve, disabled by the launch policy before launchd starts (DroidVMLaunchPolicy.c): watchdogd has no
    // watchdog backend, identityservicesd, CommCenter, fairplayd and accessoryupdaterd wait for hardware or services there are none of
    "droidvm-disable-jobs=watchdogd,identityservicesd,CommCenter,fairplayd.A2,accessoryupdaterd "
    // the deletable system apps of the OS volume's staged_system_apps in /Applications (the bring-up patch's union) and lsd's
    // first-boot semaphore that keeps them registered (DroidVMLaunchPolicy.c)
    "owner-staged-apps=1 "
    // Data protection classes on a Data volume without content protection (the bring-up patch): PosterBoard's wallpaper store
    "owner-dataprotect=1 "
    // owner binaries (ownervisor, the debug tools) run without the launch constraints of the trust cache category they lack
    "owner-skip-launch-constraints=1 "
    // the processes besides the render server that get the accelerator's user client and a native Metal device, as on iPadOS 17
    // (DroidVMBootArgs.c): the launch policy writes the plugin's NativeAdrenoMetal.clients from it at every boot
    "droidvm-gpu-clients=WidgetRenderer_Default,*Poster*,iconservicesagent,com.apple.WebKit.*,Preferences,MetalCheck,app_ios,"
    "gfxprobe";
