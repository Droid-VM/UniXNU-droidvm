// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The board's probes when the display and the GPU are a kext of the collection (ipados27/kexts/DroidVMGraphics.json). The XNU
// changes call native_legacy_display_probe (bsd_init, once IOMobileGraphicsFamily has loaded; IOUserClient.cpp's client probe) and
// native_legacy_display_late_probe (bsd_autoconf, once IOBSD is published). Built in, LegacyDisplayProbe.cpp defines both: the
// display's registration, then the late work -- the virtio GPU's display, the key store, the GPU, the board's devices and monitor
// (DroidVMMonitor27.c's native_touch_probe), the HID devices, the framebuffer. As a kext, the kext's starter runs the display and
// GPU parts of both once IOBSD is published, and the kernel keeps what is the board's: these two names, the first with nothing
// left to do, the second the rest of the late probe, in the same order. Linked in the display archive in place of
// LegacyDisplayProbe.o (build-droidvm-legacy-display.py), so the XNU changes stay as they are.
void native_legacy_display_probe(void);
void native_legacy_display_late_probe(void);
extern void native_touch_probe(void);
extern void sheng_cpufreq_probe(void);
extern void droidvm_reset_install(void);
#if !DROIDVM_KEXT_KEYSTORE
extern void native_keystore_probe(void);
#endif
#if !DROIDVM_KEXT_INPUT
extern void native_hid_input_probe(void);
#endif

void native_legacy_display_probe(void)
{
}

void native_legacy_display_late_probe(void)
{
    droidvm_reset_install();   // reboot and shutdown reach the VMM (DroidVMReset.c)
#if !DROIDVM_KEXT_KEYSTORE
    native_keystore_probe();
#endif
    sheng_cpufreq_probe();
    native_touch_probe();
#if !DROIDVM_KEXT_INPUT
    native_hid_input_probe();
#endif
}
