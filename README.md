# UniXNU-droidvm

Device repository of [UniXNU](https://github.com/UniXNU/UniXNU) for a DroidVM guest: crosvm on the
Gunyah hypervisor of a Snapdragon phone. It holds the board file, the kernel defconfig and the scripts
that turn the VM's own device tree into the Apple-format DT XNU reads.

The firmware is DroidVM's `edk2-gunyah`. There is no Mu-Silicium port and no Android boot image: the
payload is a FAT disk attached to the VM, with `UniXNU.efi`, `DarwinBoot.efi`, the kernel and its DT.

## Build

```sh
cd UniXNU          # branch droidvm: the bootstrap-pass fix for a build from scratch
./unixnu defconfig droidvm_defconfig -D ../UniXNU-droidvm
./unixnu fetch
./unixnu ipsw fetch
./unixnu build kernel
```

## Device tree

`scripts/make-droidvm-adt.py` reads the flattened device tree crosvm builds for the VM
(`crosvm run --dump-device-tree-blob FILE`, or `/sys/firmware/fdt` of a Linux guest with the same
settings) and writes the `.adt` sidecar: CPUs, the GICv3 windows, the PSCI conduit and the RAM base.
`package/crosvm-gunyah.dtb` is the default input; it must match the VM that boots the payload
(vCPU count, RAM size). `scripts/dump-adt.py FILE.adt` prints a generated tree.

## Data disk

The Data volume is a packed seed in the payload, served from guest RAM by NativeRamBlock. What the system
writes to it persists through NativeRamBlock's store window (`PersistOverlay.h`): the changed 4 KiB pages,
their presence bitmap and a header naming the seed. `package/DroidVMDataStore.c` provides that window over a
writable virtio-mmio disk: a sparse file on the phone (`tools/rig/make-data-store.sh`), attached as the third
`--virtio-mmio-block` and published by the ADT tool as `/arm-io/data-store` (`tool_args` in `board.toml`).
`droidvm-data-persist=2` reads and writes it; 0 keeps Data volatile. Writes are on the file when they complete,
which survives a VM stop or crash; the file is flushed (fsync) within 5 s of a write. A new seed (another
payload's Data image) does not match the stored header, and the store starts over empty;
`sheng-data-reset=1` empties it on purpose.

## Input

crosvm `--virtio-mmio-input` (DroidVM crosvm branch `wip/virtio-mmio`) puts the input devices the app attaches -- mouse,
multi-touch screen, absolute pointer, keyboard -- on the virtio-mmio transport after the disks; `tool_args`
(`--input-mmio`) names their windows for the ADT. `package/DroidVMVirtioInput.c` drives the touch screen and the
pointer and feeds NativeHidInput's digitizer; it resets the other two. A touch that starts in the bottom 4% of the
screen starts at the edge, so that the Home gesture can be made from a phone whose own gesture area covers that edge.
Every virtio-mmio device crosvm offers must be in the ADT: the two phone crashes seen so far (a vCPU stuck in the
hypervisor ~15 s after start, watchdog reset) both came with devices the guest did not know about.

## Network

The VM config's `networks` entry (the app's br-wifi network, MAC `02:00:00:aa:bb:01`) gives crosvm a `--net` tap, and
crosvm `--virtio-mmio-net` (branch `wip/virtio-mmio`) puts that device on the virtio-mmio transport after the GPU;
`tool_args` (`--net-mmio`) publishes it as `/arm-io/net`. `package/DroidVMVirtioNet.c` drives its virtqueues, and
`package/DroidVMVirtioEthernet.cpp` makes it an IOEthernetController on Apple's IONetworkingFamily, which the kernel does
not otherwise have: `scripts/build-native-legacy-display.py` fetches IONetworkingFamily-177 (the Darwin 23 release, at a
pinned commit) and compiles it in. configd names the interface en0 and IPConfiguration leases an address over DHCP. The
Data seed's network service (`seed-data-skip-setup.py`) is bound to en0, and the seeded interface entry that reserves en0
matches by MAC, so a different MAC brings the interface up as en1 with no service. br-wifi is a proxy bridge: the guest's
IPv4 and IPv6 addresses show on the phone's `wlan0` (`ip addr show wlan0`, as /32 and /128).

## State

| Stage | State |
| --- | --- |
| Kernel build | done on macOS 27 / Xcode 27.1 (with the fixes of UniXNU branch droidvm) |
| UEFI -> UniXNU -> DarwinBoot -> XNU | works on SM8650 (8 Gen 3) and SM8850 (8 Elite Gen 5), 4 KiB pages |
| Root | the original OS and SystemOS images on read-only virtio-mmio disks, verified (owner root) |
| Desktop | lock screen about a minute after boot with 4 vCPUs, home screen after a Home press; CPU-rendered (SM8650) |
| Data | persistent through the store disk (SM8650) |
| Kernel log / panic log / files | read from the host through guest RAM (tools/rig) |
| Input | the app's touch screen (and its absolute pointer) over virtio-input on virtio-mmio (crosvm `--virtio-mmio-input`); Home from the host |
| Network | en0 over virtio-net on virtio-mmio, bridged to the phone's wifi; DHCP IPv4 and IPv6 (SM8650) |
| GPU | missing: NativeAdreno drives the real Adreno; a VM needs a virtio-gpu native-context path |
