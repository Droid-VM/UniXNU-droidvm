# GFXBench Metal local port

This package builds the pinned GFXBench 5.0.0 Metal iOS app from a local source checkout. It is a local port, not an official GFXBench release or score.

The source checkout is Droid-VM/gfxbench at `fork_revision` in `source-pin.json` (branch `wip/hackintosh`: upstream's pinned revision with the changes in `compat.patch`, as commits), or upstream at `revision`, to which the script applies `compat.patch` (`git diff revision fork_revision`).

The iPad orientations follow the panel at run time: the two landscape ones on an iPad, portrait and upside-down on the guest, whose panel is wide in portrait (1366x1024). One build serves both. The compatibility changes in `compat.patch` include the corporate Metal 5.0 test list; the build script applies them without resetting or cleaning the checkout. It runs the repository's `scripts/build-3rdparty.sh` before `scripts/build.sh`.

The full benchmark data is about 2.6 GiB. `BUNDLE_DATA=true` places it in the app. The app also compiles Metal source at runtime; that path still needs validation in the guest. Build on macOS with Xcode's iOS Metal toolchain, CMake, and the licenses in `LICENSE` in place.

Run:

```sh
scripts/build-droidvm-gfxbench.sh /path/to/gfxbench-source /path/to/output /path/to/UniXNU/out/droidvm/tree
```

The signed app is written to `output/GFXBench.app`. Its CDHash is added to the trust entry, which keeps the ones already there. The script writes a category 0 owner trust entry under `TREE_ROOT/build/owner-trust-extra/gfxbench.json`, where UniXNU's trust bundle builder reads it; it does not deploy the app or trust entry.

The compatibility patch also retains the service-ready state across loading-controller appearance, so an initialization notification arriving before `viewWillAppear` is not lost. It guards the transition against duplicate notifications.

Stage the signed/trusted app in `ngpu/apps`, then use `lsdiag owner-install` to move it into an MCM bundle container and `lsdiag -c -u 501 owner-register` to register that real path. `ngpu/apps/GFXBench.app` remains a CLI compatibility symlink. Direct UI execution from the original staging directory is blocked by the system and icon service policies. See the Metal Check README for the installation sequence.

## IPA for the guest and a real device

```sh
scripts/package-gfxbench-ipa.sh /path/to/output/GFXBench.app /path/to/ipa
```

It zips `Payload/GFXBench.app` without changing it, so the IPA's app is the guest's (same CDHash, same entitlements). On a real iPad or iPhone (iOS 17 or later; Metal, arm64), a sideloading tool (Sideloadly, AltStore, …) re-signs it with the user's certificate and its provisioning profile's entitlements, which leaves out the guest's private ones. With the bundled data (2.6 GiB) the IPA zips to about 1.53 GiB, under GitHub's 2 GiB asset limit; a larger one would also be cut into 1.9 GiB parts (`cat NAME.part-* > NAME`). The Droid-VM/gfxbench releases carry the IPA.

## Guest validation status

Build, signing, persistent MCM installation, and process launch are verified. Startup records successful service initialization, but the displayed frame can remain the old Home/loading content even after a foreground transition. The loading-ready guard has been deployed but has not established end-to-end UI success. No GFXBench workload or score has passed yet, and runtime Metal source compilation remains unverified. The owner-root UIKit command-line smoke timed out without diagnostic output and is not evidence of a shader-compiler failure. Use Metal Check for the currently verified correctness/performance/stress path.
