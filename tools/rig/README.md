# Host-side rig for a DroidVM guest

A DroidVM guest has no XNU serial console, but the host can read guest RAM: crosvm maps it from a memfd,
and the guest PA of a byte is its memfd offset plus 0x80000000. These tools read the running kernel from
the phone over adb (root). They assume the lab setup (phone, VM id and paths in each script's header or
environment: `PHONE`, `VM`); adjust before use elsewhere.

| Tool | Use |
| --- | --- |
| `deploy.sh [--ubsan keep\|nop]` | Fetch the kernel built on the Mac, add the early probes, regenerate the ADT, refresh the boot disk, push it |
| `reboot-vm.sh [WAIT]` | Restart the VM through the DroidVM daemon and print the serial console (UEFI, UniXNU, DarwinBoot) |
| `peek.py SYMBOL\|0xPA\|va:VA [LEN] [--q\|--str]` | Read guest memory by kernel symbol, physical or kernel virtual address |
| `dmesg.py` | Print the XNU message buffer (`msgbufp`) |
| `sym.py ADDR...` | Symbolize kernel addresses with `out/kernel.nm` |
| `fbgrab.sh OUT.png` | Coherent framebuffer capture (`memgrab`: mmap the guest memfd, `dc civac`, copy) |
| `memgrab.c` | The phone-side helper of `fbgrab.sh` (static aarch64 build) |
| `swap-adt.sh` | Regenerate only the ADT on the boot disk |

The panic log lands in the `/pram` region of the ADT (default: the last 512 KiB of guest RAM,
`peek.py 0x17fb80000 0x1000` for a 4 GiB pseudo-unprotected VM).
