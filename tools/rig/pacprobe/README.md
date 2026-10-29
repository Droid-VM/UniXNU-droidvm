# pacprobe: can a Gunyah guest run arm64e?

iPadOS 18 and later ship arm64e user space only, so the guest's EL1 must have working ARMv8.3 pointer authentication. `pacprobe.bin`
is a tiny Linux-Image-format payload that crosvm boots exactly as the app boots the UniXNU VM (`--hypervisor gunyah
--protected-vm-pseudo-unprotected --hugepages`, the droidvm-shim first) with one vCPU pinned to one physical core. With the MMU
off it reads the ID registers, writes and reads back the five key registers, runs PACGA, enables EnIA/EnIB/EnDA/EnDB, runs
PACIA/IB/DA/DB and their AUTs, an AUT with a wrong modifier, BLRAA, RETAA and LDRAA, then the same at EL0, and prints
`PACPROBE VERDICT usable` or `NOT-usable` on the 8250 at 0x3f8. Every exception is counted and stepped over; past 64 it powers
off. It never waits in WFE: a guest stuck in WFE and killed from the host once reset a phone.

    ./build.sh                                     # pacprobe.bin
    adb push pacprobe.bin run-probe.sh /data/local/tmp/   # then, as root, move both to /data/local/tmp/pacprobe/
    su -c 'sh /data/local/tmp/pacprobe/run-probe.sh 0 1 2 3 4 5 6 7'

`run-probe.sh` follows the hugepage procedure (needs 64 free pool pages and no other VM up; 10 s after each stop), stops a run
still up after 20 s through its control socket (`crosvm stop`, never a signal) and ends the series after such a run. Each core's
serial output is `out-N.txt`, crosvm's log `crosvm-N.log`. Run it with hostwatch.py up. Results of 2026-10-29: the meta repo's
`plans/IPADOS27_BUMP.md`.
