// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The DroidVM board's built-in boot arguments: the switches this board always sets. PE_parse_boot_argn and PE_parse_boot_arg_str
// look an argument up here when the boot loader's command line (board.toml's kernel_args, which DarwinBoot keeps to 1023 bytes)
// does not have it (UniXNU patches/xnu/native-builtin-boot-args.patch); the command line wins, so a switch set here is turned off
// for one boot with name=0 there. Everything listed is read in the kernel, by the kernel's patches or its drivers.
char PE_builtin_boot_args[] =
    // the original components the offline boot loads (UniXNU patches/xnu/offline-component-load-probes.patch)
    "offline-apfs-load=1 offline-mobile-graphics-load=1 offline-mobile-graphics-client=1 "
    // NativeLegacyDisplay, the IOMobileFramebuffer in GOP mode, and its probe (LegacyDisplayProbe.cpp). It defaults to
    // sheng-early-retire=0; the virtio display retires each swap when it takes the frame (native_legacy_gpu_retires_on_offer).
    "offline-legacy-register=1 offline-legacy-gop=1 offline-legacy-phase=4 offline-legacy-unit-scale=1 offline-legacy-pm=1 "
    "offline-legacy-validate=1 offline-legacy-transfer=1 offline-legacy-cpu-present=1 "
    // HID input (HidInputProbe.cpp) and the key store (KeyStoreProbe.cpp)
    "offline-hid-input=1 offline-hid-home=1 sheng-touch-hid=1 offline-key-events=1 sheng-aks=1 "
    // the keybag of a guest without a passcode: MobileKeyBag "Disabled", so SpringBoard neither revokes authentication at
    // boot nor waits for a passcode that does not exist (default 0 reads as a passcode set and unlocked)
    "sheng-aks-sls=3 "
    "sheng-smp=1 "
    // inter-processor interrupts. XNU signals a core (cpu_signal) only once that core has taken its first IPI, and skips it
    // silently until then: without the self-IPI ArmCpu sends from initCPU no core ever got one, so a panic or a stackshot never
    // stopped the other cores (a stackshot raced them and the DEVELOPMENT kernel panicked: "thread wait info count mismatch"),
    // and a core was woken for new work only by its timer. Gunyah drops the SGIs a guest writes to ICC_SGI1R_EL1, so the IPIs
    // are software-pended SPIs, as on sheng
    "armcpu-self-ipi=1 armcpu-spi-ipi=1 "
    // the Data disk keeps what the system writes in the store window of the third virtio-mmio disk (DroidVMDataStore.c)
    "droidvm-data-persist=2 "
    // the deletable system apps (Weather, Calendar, Notes, Maps, ...): the OS volume's /private/var/staged_system_apps as a
    // read-only union view over /Applications, since the Data seed carries no copy for the first boot's migration to install
    // (UniXNU patches/xnu/zzy-union-staged-apps.patch)
    "owner-staged-apps=1 "
    // the App cryptex (Safari's daemons, SafariSupport.bundle, Safari's settings pane) from the seed's /private/var/ngpu/preboot
    // shown read-only over /private/preboot, where /System/Cryptexes/App leads (DroidVMLaunchPolicy.c,
    // scripts/add-app-cryptex-data.py)
    "owner-app-cryptex=1 "
    // /private/var/ngpu/bin shown read-only over /usr/libexec, so launchd's cached job for a daemon there (it ignores the plist
    // file's ProgramArguments) execs our binary of the same basename instead. droidvm-diag rides in as /usr/libexec/remindd
    // (scripts/build-droidvm-diag.py).
    "owner-tools-overlay=1 "
    // Our owner-signed binaries are in the trust cache but not platform binaries, so AMFI's launch constraints (which gate a
    // system-service launch on a platform binary) kill them with "Launch Constraint Violation". The original RELEASE AMFI
    // ignores its own amfi_* boot-args, so XNU clears the error itself (patches/xnu/zzy-union-staged-apps.patch). This
    // VM is not a security boundary.
    "owner-skip-launch-constraints=1 "
    // the data protection class a caller sets is kept on the unencrypted Data volume (and an unset one reads as class D,
    // "None"): PosterBoard checks its data store's class back and, finding none, had no wallpaper store at all (UniXNU
    // patches/xnu/zzu-dataprotect-without-cprotect.patch)
    "owner-dataprotect=1 "
    // start the owner service manager once at boot. launchd takes its jobs from the signed compiled cache and ignores an owner
    // overlay plist's ProgramArguments, and a new label is never enumerated, so no launchd overlay launches an owner binary
    // here; the kernel launch-policy hook posix_spawns /private/var/ngpu/bin/ownervisor itself (DroidVMLaunchPolicy.c,
    // scripts/build-droidvm-ownervisor.py, scripts/add-ownervisor-data.py). It brings up the owner debug services and the GPU
    // probe.
    "owner-service-manager=1 "
    // launchd jobs this platform cannot serve (DroidVMLaunchPolicy.c): no watchdog backend, the services sheng found in a cycle
    // of synchronous XPC waits on first boot (UniXNU docs/guide/skip-setup.md), and fairplayd, which exits at once without the
    // FairPlayIOKit kext (it does not prelink against this kernel); its MachServices stay registered meanwhile, so lockdownd's
    // first FairPlay call waited forever, and with it accountsd and every app that asks it (Notes, Photos stayed blank). A
    // label without a "com." domain is under com.apple. bluetoothd runs: without its MachServices audiomxd's Bluetooth audio
    // plugin retried com.apple.BTAudioHALPlugin.xpc about 110 times a second (95% of the unified log), and it needs only the
    // keychain. accessoryupdaterd updates the firmware of Apple accessories, which a VM has none of, and kept trustd checking
    // certificates (about 9 OCSP requests a second, trustd at 20-30% of a CPU); a disabled job's MachServices are not registered,
    // so its clients get no answer at once rather than wait.
    "droidvm-disable-jobs=watchdogd,identityservicesd,locationd,CommCenter,fairplayd.A2,accessoryupdaterd "
    // Metal's 3D through the virtio GPU's DRM context (drm2kgsl on the host): turnip asks the kernel (DroidVMVirtioGpu3D.cpp,
    // DROIDVM_VIRTGPU_PARAM_DEFAULT_3D). DroidVMGpu.cpp publishes NativeAdreno, whose accelerator's user client serves the virtio
    // GPU's opens, and the accelerator.
    "vgpu-3d=1 "
    // the AGXAccelerator for the original Metal discovery, and the plugin bundle (staged in the Data seed by
    // add-gpu-plugin-data.py) mounted over /System/Library/Extensions
    "droidvm-gpu-metal=1 sheng-gpu-addon=1 "
    // the processes besides backboardd that get a Metal device of their own (DroidVMLaunchPolicy.c writes the plugin's clients
    // file): WidgetRenderer draws the widgets through RenderBox, and iconservicesagent the app icons, whose CoreImage context
    // otherwise falls back to OpenGL ES and crashes in CI::GLContext on every icon (sheng disables the job); *Poster* ("*X*"
    // names every process whose name holds X) is PosterBoard and each wallpaper's poster extension, which render the wallpaper
    // and the gallery's previews with Metal (AegirPoster, KaleidoscopePoster crashed without a device); and WebKit's processes,
    // com.apple.WebKit.* (a name ending in '*' names every process it begins): CoreGraphics draws a page's wide-color layers
    // ('b3a8', 'w30r') only through the GPU, so without a device WebContent drew nothing into them and Safari's pages were
    // black. Settings (Preferences) reads the wallpaper's snapshot, which PosterBoard writes in its device's texture layout (an
    // ASTC ATX file); ImageIO reads such a file back only through a device's layout, and Settings > Wallpaper trapped on the
    // missing image. The kernel's grant and the plugin take the same forms (UniXNU patch zzw, build-adreno-metal-plugin.py).
    // Maps is not one: VectorKit lost its device (DeviceLost) within seconds, and the render server's frames were wedged until
    // a reboot; without one its map is blank but the app runs. MetalCheck and app_ios (GFXBench) are the Metal tests, gfxprobe
    // the rig's probe (`gfxprobe ci` checks Core Image on the device against its software renderer)
    "droidvm-gpu-clients=WidgetRenderer_Default,*Poster*,iconservicesagent,com.apple.WebKit.*,Preferences,MetalCheck,app_ios,"
    "gfxprobe";
