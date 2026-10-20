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

## State

| Stage | State |
| --- | --- |
| Kernel build | in progress |
| DarwinBoot hand-off in the VM | not tried |
| Storage (virtio-pci block) | missing: NativeVirtioBlock is virtio-mmio only, crosvm aarch64 offers virtio-pci |
| Input (virtio-input) | missing |
| GPU | missing: NativeAdreno drives the real Adreno; a VM needs a virtio-gpu native-context path |
