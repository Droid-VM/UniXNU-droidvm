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
| `REPO` | `deploy.sh`, `swap-adt.sh`: the device repository, when the rig is not its `tools/rig` |

| Tool | Use |
| --- | --- |
| `vm.sh restart\|start\|stop\|attach\|health` | Restart the VM (killing its crosvm, which a daemon that aborts on `vm_stop` survives), wait for the first frame or a panic, put its display on the phone until a touch reaches the guest, unlock, print `health.py` |
| `health.py` | One line each for the kernel (panic?), Data, userland, display and input, the network (en0, the guest's addresses on the phone's wlan0) and new crash reports |
| `panic.py [--raw]` | Whether the kernel has panicked, and its panic log with kernel addresses symbolized: a panic does not stop this VM, only the panicking vCPU spins |
| `crashes.py [--all\|--list] [NAME...]` | The guest's new crash reports (ReportCrash's .ips), fetched to `out/crashes/` and summarized: exception, where an uncaught exception was thrown, the crashed thread; a JetsamEvent's killed processes and their reasons |
| `input.py tap\|long\|swipe\|unlock\|text` | Touch the guest screen through the app's display (Android `input`, real timing), in pixels of the 1280x800 framebuffer; the mapping comes from SurfaceFlinger |
| `deploy.sh [--ubsan keep\|nop]` | Fetch the kernel built on the Mac, add the early probes, regenerate the ADT, refresh the boot disk, push it |
| `reboot-vm.sh [WAIT]` | Restart the VM through the DroidVM daemon and print the serial console (UEFI, UniXNU, DarwinBoot) |
| `peek.py SYMBOL\|0xPA\|va:VA [LEN] [--q\|--str]` | Read guest memory by kernel symbol, physical or kernel virtual address |
| `dmesg.py` | Print the XNU message buffer (`msgbufp`) |
| `sym.py ADDR...` | Symbolize kernel addresses with `out/kernel.nm` |
| `fbgrab.sh OUT.png` | Coherent framebuffer capture (`memgrab`: mmap the guest memfd, `dc civac`, copy) |
| `memgrab.c` | The phone-side helper of `fbgrab.sh` (static aarch64 build) |
| `swap-adt.sh` | Regenerate only the ADT on the boot disk |
| `console-watch.sh [SECONDS]` | Keep the newest serial console on this machine while the VM boots, stamped with host time (survives a phone that goes down) |
| `procs.py`, `trace.py` | The droidvm-monitor worker's process samples and UI thread traces (`droidvm-monitor=<ms>`) |
| `trace.py --focus NAME [--symbolize]` | Trace any process's threads (role T) while the guest runs, e.g. an app that hangs; `--symbolize` names the frames with `dsc_sym.py` on `BUILD_HOST` |
| `export.py ls\|cat\|get\|ps` | Read guest files, directories and the process list through the kernel's file mailbox |
| `dsc_sym.py CACHE_DIR SLIDE ADDR...` | Symbolize user addresses against the SystemOS dyld shared cache (run on the Mac) |
| `home.py` | Press Home |
| `touch.py tap X Y`, `touch.py swipe X0 Y0 X1 Y1` | Touch the guest screen (pixels of the 1280x800 framebuffer) by writing NativeHidInput's finger set |
| `load-modules.sh` | Load the DroidVM app's kernel modules after the phone itself restarted |
| `show-vm.sh` | Bring a fresh display of the VM to the front of the phone (a VM restart closes it; `vm.sh attach` also checks that input arrives) |
| `export.py kill PID [SIGNAL]` | Signal a guest process, e.g. a SpringBoard deadlocked at launch (launchd restarts it) |
| `export.py mem PID ADDRESS [LENGTH]`, `umem.py` | Read a guest process's user memory (hex dump; `umem.read`/`umem.q` for scripted pointer walks) |
| `logarchive.py OUT` | Mirror the guest's unified log (diagnostics + uuidtext) for a tracev3 parser such as mandiant/macos-UnifiedLogs (`unifiedlog_iterator -m log-archive`); reruns fetch only what changed |
| `make-data-store.sh [GIB]` | Make the labelled sparse disk of the persistent Data store on the phone |
| `daemon-recover.sh` | Restart droidvmd the way the app does and recreate the VM from its saved config |

After the phone itself restarts, open the DroidVM app once (or `insmod` the modules of its autostart set from
`files/../usr/lib/modules/<kmi>/`): a pseudo-unprotected VM shares its RAM through `/dev/gunyah_share`, and
without the Gunyah Host Share module crosvm exits with "failed to initialize virtual machine (os error 2)".

The panic log lands in the `/pram` region of the ADT (default: the last 512 KiB of guest RAM); `panic.py` finds it
from crosvm's RAM mapping and prints it.
