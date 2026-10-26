# Host-side rig for a DroidVM guest

A DroidVM guest has no XNU serial console, but the host can read guest RAM: crosvm maps it from a memfd,
and the guest PA of a byte is its memfd offset plus 0x80000000. These tools read the running kernel from
the phone over adb (root). Run them from this directory; they keep their working files in `out/` next to
them, and take the setup from the environment, or from `rig.env` beside them (`KEY=value` lines, kept out of git;
the environment wins):

| Variable | Meaning |
| --- | --- |
| `PHONE` | adb serial of the phone (`adb devices`) |
| `VM` | id of the UniXNU VM in the DroidVM daemon (`vm_list`) |
| `DVMIPC` | path of the DroidVM daemon's IPC client, `dvmipc.py` |
| `BUILD_HOST`, `BUILD_OUT` | `deploy.sh`: the build Mac (`user@host`) and `out/droidvm` of its UniXNU checkout |
| `REPO` | `deploy.sh`, `swap-adt.sh`, `payload-redeploy.sh`, `mac-sync-check.sh`: the device repository, when the rig is not its `tools/rig` |
| `BUILD_REPO` | `payload-redeploy.sh`: the device repository's checkout on the build Mac (board.toml, make-droidvm-disk.sh) |
| `FORK` | `plugin-redeploy.sh`, `mac-sync-check.sh`: the local UniXNU fork checkout (plugin scripts and metal2vulkan patches are copied from it; the Mac's UniXNU checkout must match it) |
| `SYNC_CHECK`, `ALLOW_CHANGE` | `payload-redeploy.sh`, `plugin-redeploy.sh`: `SYNC_CHECK=0` builds although the Mac's checkouts differ from here; `ALLOW_CHANGE="darwinboot uefi seed"` lets a new boot disk replace those parts of the running one |
| `DATA_SEED` | `payload-redeploy.sh`: the Data seed in the tree's `build/droidvm-data` (default `data-12g-skip-ov.dbr`; another seed starts Data again) |
| `GUEST_IP`, `GUEST_KEY` | `guest-ssh.sh`: the guest's IPv4 (`health.py`) and the ssh key (default `out/ssh/id_ed25519`) |
| `CROSVM_BIN` | `crosvm-deploy.sh`: the crosvm build to install (the meta repo's `crosvm_out/crosvm`) |
| `UNIFIEDLOG`, `LOG_BASE` | `guest-newlogs.sh`: mandiant/macos-UnifiedLogs' `unifiedlog_iterator`, and a full mirror made by `logarchive.py` (default `out/guest.logarchive`) |

| Tool | Use |
| --- | --- |
| `vm.sh restart\|start\|stop\|attach\|health` | Run the VM: stop it through crosvm's control socket, give the hugepage pool 10 s to reclaim and report it; before a start make sure the pool is whole (`acquire=3` when short, refuse if still short); start, wait for the first frame or a panic, put its display on the phone until a touch reaches the guest, unlock, print `health.py` (the procedure: `hugepage-harness.md` at the root of the DroidVM meta repo) |
| `hostwatch.py [--boot-wait S]` | One line per event while a VM runs, for a monitor: host reboot (boot id or uptime), adb lost, VM up/gone in the hugepage pool's `vm_owners` (requested stop vs crash), the pool 10 s after, a start with no VM (boot failure) |
| `health.py` | One line each for the kernel (panic?), Data, userland, display and input (including virtio display state and producer-wait mode), the network (en0, the guest's addresses on the phone's wlan0) and new crash reports |
| `panic.py [--raw]` | Whether the kernel has panicked, and its panic log with kernel addresses symbolized: a panic does not stop this VM, only the panicking vCPU spins |
| `crashes.py [--all\|--list] [NAME...]` | The guest's new crash reports (ReportCrash's .ips), fetched to `out/crashes/` and summarized: exception, where an uncaught exception was thrown, the crashed thread; a JetsamEvent's killed processes and their reasons |
| `input.py tap\|long\|swipe\|unlock\|text` | Touch the guest screen through the app's display (Android `input`, real timing), in pixels of the framebuffer (handoff.env: 1366x1024, the iPad Pro 12.9-inch's points); the mapping comes from SurfaceFlinger |
| `deploy.sh [--kernel-only] [patch-early-probe.py args]` | Fetch the Mac kernel, add the early probes and push it. `--kernel-only` keeps the rig's boot disk, including its ADT, trust, Data seed and DarwinBoot; deploy extracts the ADT to `out/`, while `swap-adt.sh` rebuilds it |
| `reboot-vm.sh [WAIT]` | Restart the VM through the DroidVM daemon and print the serial console (UEFI, UniXNU, DarwinBoot) |
| `peek.py SYMBOL\|0xPA\|va:VA [LEN] [--q\|--str]` | Read guest memory by kernel symbol, physical or kernel virtual address |
| `dmesg.py` | Print the XNU message buffer (`msgbufp`) |
| `sym.py ADDR...` | Symbolize kernel addresses with `out/kernel.nm` |
| `fbgrab.sh OUT.png` | Coherent capture of the simple framebuffer (`memgrab`: mmap the guest memfd, `dc civac`, copy); with the virtio-gpu display it reads the old boot framebuffer, not the screen: use `screen-check.py` |
| `memgrab.c` | The phone-side helper of `fbgrab.sh` (static aarch64 build) |
| `screen-check.py capture DIR`, `reference NAME [--home]`, `expect NAME[,NAME...] [--home]` | What the screen shows, from both ends: the phone's composition (screencap) and every virtio-gpu scanout surface the guest composed (crosvm's udmabufs). `expect` passes when the phone shows a kept reference, else says whether the display path (a guest surface holds it, the phone does not show it) or the guest's composition is wrong. Keep `home` and `home-portrait`: after a portrait-only app the home screen stays portrait |
| `dmabufgrab.c`, `scanout-grab-phone.sh` | Their phone side: read a dma-buf crosvm holds (pidfd_getfd, `dc civac`; static aarch64 build to `/data/local/tmp/dmabufgrab`), all scanout-sized udmabufs as one tar |
| `swap-adt.sh [--dry-run]` | Regenerate only the ADT on the boot disk, with the payload's own command (board.toml `dram_size` and `[payload.adt]`, `package/crosvm-gunyah.dtb`), and print how it differs from the disk's |
| `phone-cpu-pin.sh pin [BIG_KHZ [LITTLE_KHZ]] \| unpin \| show` (on the phone, as root) | Pin every CPU policy to one frequency for comparable benchmark runs (default 2.4/1.8 GHz, but never above the vendor's freq-QoS cap: OPLUS caps the big cores by shell temperature, 1.36-1.71 GHz at night), sysfs files read-only so its daemons cannot move them; `unpin` restores the saved limits |
| `gfx-suite.sh ID...` | Run GFXBench tests (`lsdiag launch-args`, ids from gfxbench40's config, e.g. `metal_4 metal_5_high`) one after another: each prints its score, fps and frames, or NEW CRASH with the report `app_ios` left |
| `gfx-soak.sh ID...` | `gfx-suite.sh`, and after each test the host's KGSL mapped bytes, crosvm's dmabuf fds (`phone-hostmem.sh`) and the guest's virtio-gpu close counters (`droidvm_virtgpu_closes`): what accumulates from run to run |
| `gfx-wired.sh ID...` | `gfx-suite.sh`, and before and after each test the guest's free, wired, throttled and compressed pages (`vm_stat`) and its virtio-gpu clients' totals |
| `vgpu-clients.py [--sizes]` | The guest's virtio-gpu 3D clients from guest RAM: per client its DRM context, live resources (user-page blobs, host-visible ones), bytes, submissions in flight and closed resources waiting for them; `--sizes` adds each client's largest resources |
| `metalcheck-run.py [--benchmark | --stress-seconds S [--memory-mib M]] [--output DIR]` | Run the preinstalled guest Metal Check CLI and collect its report, progress, guest GPU-loss counters, and memory-pressure samples; requires a running VM and active `hostwatch.py` |
| `display-soak.py [--cycles N]` | Exercise Today View and Spotlight; checks command/GPU errors, fallback counters, compositor (`backboardd`) PID and crash reports. Requires active `hostwatch.py` and an attached VM display |
| `display-profile.py [--cycles N] [--compare-no-wait]` | Default runs only with producer wait ON using existing guest swap/present timing rings. `--compare-no-wait` runs a possibly flickering ON/OFF/OFF/ON diagnostic and restores ON in `finally`. Measures guest swap cadence, not panel FPS; the <=100 ms subset filters pauses and can also hide severe stalls |
| `console-watch.sh [SECONDS]` | Keep the newest serial console on this machine while the VM boots, stamped with host time (survives a phone that goes down) |
| `procs.py`, `trace.py` | The droidvm-monitor worker's process samples and UI thread traces (`droidvm-monitor=<ms>`) |
| `trace.py --focus NAME [--symbolize]` | Trace any process's threads (role T) while the guest runs, e.g. an app that hangs; `--symbolize` names the frames with `dsc_sym.py` on `BUILD_HOST` |
| `export.py ls\|tree\|cat\|get\|ps` | Read guest files, directories (`tree`: a whole tree with file sizes) and the process list through the kernel's file mailbox |
| `dsc_sym.py CACHE_DIR SLIDE ADDR...` | Symbolize user addresses against the SystemOS dyld shared cache (run on the Mac) |
| `dsc_dis.py CACHE_DIR SYMBOL [IMAGE] [--around OFF]` | Disassemble a function of the dyld shared cache, at a crash report's "symbol + offset", with branch targets named (run on the Mac, Homebrew llvm-mc) |
| `home.py` | Press Home |
| `touch.py tap X Y`, `touch.py swipe X0 Y0 X1 Y1` | Touch the guest screen (pixels of the framebuffer) by writing NativeHidInput's finger set |
| `load-modules.sh` | Load the DroidVM app's kernel modules after the phone itself restarted |
| `show-vm.sh` | Bring a fresh native VM display to the front; selects the enabled `gpu-0` or `simplefb` screen and dimensions from config (a VM restart closes it) |
| `export.py kill PID [SIGNAL]` | Signal a guest process, e.g. a SpringBoard deadlocked at launch (launchd restarts it) |
| `export.py mem PID ADDRESS [LENGTH]`, `umem.py` | Read a guest process's user memory (hex dump; `umem.read`/`umem.q` for scripted pointer walks) |
| `logarchive.py OUT` | Mirror the guest's unified log (diagnostics + uuidtext) for a tracev3 parser such as mandiant/macos-UnifiedLogs (`unifiedlog_iterator -m log-archive`) or macOS `log show --archive`; each tree is listed with its sizes in one request (`export.py tree`), and reruns fetch only what changed |
| `logs.sh [--no-mirror] LOG-SHOW-OPTIONS...` | The guest's unified log through macOS's own `log show` on `BUILD_HOST`: mirror, copy the archive there, show it, e.g. `logs.sh --last 10m --predicate 'process == "Preferences"'` |
| `make-data-store.sh [GIB]` | Make the labelled sparse disk of the persistent Data store on the phone |
| `guest-ssh.sh [--key\|COMMAND]` | A root shell or command in the guest over its dropbear (port 2222), through `nc` on the phone; `--key` makes the key and puts it into the guest (after every new Data seed). stdin passes through, so it also copies files in |
| `guest-newlogs.sh [N] [OUT.jsonl]` | Only the newest N tracev3 files of the guest's unified log, parsed to JSON lines with the uuidtext of a full mirror; fetches uuidtext the mirror lacks (rebuilt plugin or tools) |
| `frame-burst.sh OUTDIR N PRE swipe X0 Y0 X1 Y1 MS\|tap X Y` | N framebuffers back to back (~25 ms apart) with the phone itself starting the input after PRE of them: what an animation really shows (tearing, stale layers), plus a contact sheet and per-frame change counts; simple framebuffer only, like `fbgrab.sh` |
| `mac-sync-check.sh` | Whether the Mac builds the source here: its device repository and UniXNU checkout at the same commits, uncommitted edits and submodules as `REPO` and `FORK`, and its build tree's copies of scripts and patches equal to their sources. The Mac's checkouts are moved with git bundles (pushes go to wip branches only) |
| `payload-redeploy.sh [--keep-trust] [TOOL...]` | Check the Mac's source (`mac-sync-check.sh`), remake the owner trust bundle and pin it in board.toml (here and on the Mac), build the payload around `DATA_SEED`, make the boot disk and compare it with the running one (what changes, boot-args added and removed; a new DarwinBoot, UEFI loader or Data seed needs `ALLOW_CHANGE`), keep the running one as `out/droidvm-disk.prev.img`, deploy, restart, put the ssh key, stage owner tools |
| `plugin-redeploy.sh` | Build the Metal plugin on the Mac from `FORK`, keep the running plugin trusted, stage the new one into the guest's Data, flush it, `payload-redeploy.sh`, and check it survived the reboot |
| `crosvm-deploy.sh` | Install `CROSVM_BIN` as the app's crosvm (VM stopped), keeping a backup and the old binary's owner, mode and SELinux label; guest and crosvm must agree on the droidvm-gpu wire version |
| `daemon-recover.sh` | Restart droidvmd the way the app does and recreate the VM from its saved config (`unixnu-vm.config.json`, rig-local); change a running daemon's copy with `vm_modify` |
| `vmguard2.sh [SPINS]`, `guard-start.sh` (on the phone, as root) | Guard the host against a guest vCPU that holds its core: SIGSTOP crosvm when a started secondary never sleeps, or when every one spins SPINS samples. `guard-start.sh` runs it detached, logging to `/data/local/tmp/vmguard.log`. `vmguard.sh` is the earlier sampler |
| `killvm.sh` (on the phone) | SIGKILL a stopped UniXNU crosvm after the guard froze it |
| `early-trace.py` | The early boot trace of a VM the guard stopped: ArmCpu bring-up stages and where each CPU was |
| `boot-cycles.sh N` | Restart the VM N times under the guard; record the early trace of any boot it stops |
| `guestcpus.py`, `vcpu-busy.py` | The guest CPUs' state from guest RAM, and which crosvm vCPU threads are busy |
| `crosvm-tail.sh STREAM VM_ID` (on the phone) | Follow one console log of a VM (`stderr`, `serial1`) from now on |
| `display-ab.sh LABEL` | Whether the phone's display follows the guest (Settings, then Home; changed-pixel fraction) |
| `dumpimg.py GFX_ERR OUTDIR` | Turn NativeMetal's `NATIVE_METAL_DUMP_IMAGE` lines of a gfx.err into PNGs |

After the phone itself restarts, open the DroidVM app once (or `insmod` the modules of its autostart set from
`files/../usr/lib/modules/<kmi>/`): a pseudo-unprotected VM shares its RAM through `/dev/gunyah_share`, and
without the Gunyah Host Share module crosvm exits with "failed to initialize virtual machine (os error 2)".

The panic log lands in the `/pram` region of the ADT (default: the last 512 KiB of guest RAM); `panic.py` finds it
from crosvm's RAM mapping and prints it.

## Metal Check

Start `hostwatch.py` in one terminal while the VM is running, then use another terminal in this directory:

```sh
python3 metalcheck-run.py --benchmark --output out/metalcheck-guest-benchmark
python3 metalcheck-run.py --stress-seconds 300 --memory-mib 128 \
  --output out/metalcheck-guest-stress-300s-128mib
```

`--benchmark` runs correctness plus the three benchmark workloads. `--stress-seconds` accepts 1–86400 and runs correctness before stress; all required correctness cases must pass before stress begins. `--memory-mib` accepts 0–512 and defaults to 128. Use either `--benchmark` or `--stress-seconds`. The output directory must not already exist; without `--output`, the runner creates a timestamped directory under `out/`.

Each run directory contains `report.json`, `app.json`, `stdout.log`, `progress.log`, and `pressure.jsonl`. The pressure samples include guest GPU-loss and GPU-error counters, guest page counts and memory level, and host memory-pressure data. The runner marks changed guest GPU-loss counters and other monitored GPU/kernel errors as failures.

The enhanced guest correctness-and-benchmark report at `../../../droidvm/out/metalcheck-guest-enhanced/report.json` records 28 `PASS`, 0 `FAIL`, 0 `SKIP`, passing render/compute/blit benchmarks, and no observed guest GPU loss. This does not establish complete Metal conformance. The first 304-second guest stress run passed 9 rounds. The optimized run passed 20 API/oracle rounds in 302 seconds, but its visual review failed: stale frames and text corruption remained despite zero loss counters. See `out/metalcheck-guest-stress-final/report.json`; `report-automated.json` retains the original API/counter result. The runner explicitly labels its automatic result as API/counters only, with `visual_check=NOT_ASSESSED`; final displayed pixels need independent checking.
