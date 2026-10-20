#!/usr/bin/env python3
"""Generate the Apple-format DT of a DroidVM guest (crosvm on Gunyah) for the first boot.

The structure follows make-sheng-adt.py of UniXNU-sheng (the nodes ArmDTPlatform and ArmCpu
consume). Every hardware value comes from the flattened device tree crosvm builds for the VM,
read from a file: `crosvm run --dump-device-tree-blob FILE`, or /sys/firmware/fdt of a Linux
guest started with the same VM settings. Nothing is assumed about the host SoC.

  - /cpus: one node per FDT cpu, reg = its MPIDR affinity (crosvm numbers them in Affinity0)
  - /arm-io/intc: the arm,gic-v3 distributor and redistributor windows
  - /psci: the FDT conduit (crosvm declares hvc)
  - /arm-io/virtio-mmio-N: every "virtio,mmio" node (crosvm --virtio-mmio-block), for
    NativeVirtioBlock: compatible exactly "virtio,mmio", reg, dma-coherent
  - /chosen dram-base, dram-size: the guest RAM. A pseudo-unprotected VM declares only its small
    boot region in the crosvm FDT; its boot shim accepts the RAM window and rewrites /memory at
    run time (console: "SHIM: /memory now BASE+SIZE"), so --dram-base and --dram-size give it.

crosvm places its MMIO windows (RTC 0x2000, PCI ECAM 0x10000, GIC below 1 GiB) in [0x1000, 1 GiB),
which /arm-io covers. Its parent base must not be 0: pexpert takes that base as the SoC base
(pe_arm_get_soc_base_phys) and pe_identify_machine returns early on 0, leaving the timebase at 0;
wfe_timeout_configure then overflows (1 << -1) in arm_init. The architected counter frequency is not in the FDT (crosvm leaves CNTFRQ_EL0 to
the hypervisor); --timebase supplies it, 19.2 MHz on every Qualcomm host seen so far.
"""
from pathlib import Path
import argparse
import os
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

ARMIO_BASE = 0x1000
ARMIO_SIZE = 0x40000000 - ARMIO_BASE
GIC_REDIST_STRIDE = 0x20000     # GICv3 without VLPI: RD_base + SGI_base frames


def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def empty_nvram_proxy():
    """A fresh volatile CHRP bank for upstream IODTNVRAM, with no controller."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', struct.pack('<I', len(image))),
            ('nvram-bank-count', struct.pack('<I', 1)),
            ('nvram-current-bank', struct.pack('<I', 0)),
            ('nvram-proxy-data', bytes(image))]


def parse_fdt(blob):
    """The FDT as {path: {property: bytes}}; paths are '/'-joined node names with the unit address."""
    magic, total, structure, strings, _, version, _, _, string_bytes, structure_bytes = struct.unpack_from('>10I', blob)
    if magic != 0xd00dfeed or total > len(blob) or version < 16:
        raise SystemExit('not a flattened device tree')
    nodes, stack = {}, []
    cursor = structure
    while cursor < structure + structure_bytes:
        tag = struct.unpack_from('>I', blob, cursor)[0]
        cursor += 4
        if tag == 1:
            end = blob.index(b'\0', cursor)
            stack.append(blob[cursor:end].decode())
            nodes['/' + '/'.join(stack[1:])] = {}
            cursor = (end + 4) & ~3
        elif tag == 2:
            stack.pop()
        elif tag == 3:
            length, name_offset = struct.unpack_from('>II', blob, cursor)
            cursor += 8
            name = blob[strings + name_offset:blob.index(b'\0', strings + name_offset)].decode()
            nodes['/' + '/'.join(stack[1:])][name] = blob[cursor:cursor + length]
            cursor = (cursor + length + 3) & ~3
        elif tag == 9:
            break
        elif tag != 4:
            raise SystemExit(f'unknown FDT tag {tag}')
    return nodes


def cells(data):
    return list(struct.unpack('>%dI' % (len(data) // 4), data))


def strings_of(data):
    return [item.decode() for item in data.split(b'\0') if item]


def reg_pairs(data, address_cells=2, size_cells=2):
    values = cells(data)
    step = address_cells + size_cells
    result = []
    for index in range(0, len(values), step):
        address = 0
        for value in values[index:index + address_cells]:
            address = (address << 32) | value
        size = 0
        for value in values[index + address_cells:index + step]:
            size = (size << 32) | value
        result.append((address, size))
    return result


def describe(fdt):
    """The values the ADT carries, each from its FDT source."""
    root = fdt['/']
    if (cells(root.get('#address-cells', b'\0\0\0\2')), cells(root.get('#size-cells', b'\0\0\0\2'))) != ([2], [2]):
        raise SystemExit('expected a crosvm FDT with 2 address and 2 size cells at the root')
    cpus = sorted((cells(props['reg'])[0], path) for path, props in fdt.items()
                  if path.startswith('/cpus/') and props.get('device_type') == b'cpu\0')
    if not cpus:
        raise SystemExit('FDT has no /cpus')
    gic = [props for path, props in fdt.items() if 'arm,gic-v3' in strings_of(props.get('compatible', b''))]
    if len(gic) != 1:
        raise SystemExit('FDT must have exactly one arm,gic-v3 node')
    gic_windows = reg_pairs(gic[0]['reg'])
    if len(gic_windows) < 2:
        raise SystemExit('the GICv3 node needs a distributor and a redistributor window')
    (dist, dist_size), (redist, redist_size) = gic_windows[:2]
    if redist_size < GIC_REDIST_STRIDE * len(cpus):
        raise SystemExit(f'redistributor window {redist_size:#x} is smaller than {len(cpus)} frames')
    for address, size in ((dist, dist_size), (redist, redist_size)):
        if not ARMIO_BASE <= address < address + size <= ARMIO_BASE + ARMIO_SIZE:
            raise SystemExit(f'GIC window {address:#x}+{size:#x} lies outside the 1 GiB arm-io window')
    psci = fdt.get('/psci', {})
    method = psci.get('method', b'').rstrip(b'\0').decode()
    if len(cpus) > 1 and method not in ('hvc', 'smc'):
        raise SystemExit('FDT /psci declares no hvc or smc conduit')
    memory = [pair for path, props in fdt.items() if props.get('device_type') == b'memory\0'
              for pair in reg_pairs(props['reg'])]
    if not memory:
        raise SystemExit('FDT has no memory node')
    dram_base, dram_size = min(memory)
    virtio = sorted(reg_pairs(props['reg'])[0] for path, props in fdt.items()
                    if 'virtio,mmio' in strings_of(props.get('compatible', b'')))
    for address, size in virtio:
        if not ARMIO_BASE <= address < address + size <= ARMIO_BASE + ARMIO_SIZE:
            raise SystemExit(f'virtio-mmio window {address:#x}+{size:#x} lies outside the arm-io window')
    return dict(cpu_regs=[reg for reg, _ in cpus], gic=(dist, dist_size, redist, redist_size),
                psci_method=method or 'hvc', dram_base=dram_base, dram_size=dram_size, virtio_mmio=virtio)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/droidvm/kernel.macho.adt')
    parser.add_argument('--fdt', type=Path, default=ROOT / 'kits/droidvm/crosvm-gunyah.dtb',
                        help='FDT of the VM: crosvm --dump-device-tree-blob, or /sys/firmware/fdt of a Linux guest')
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), required=True,
                        help='Guest RAM size; without --dram-base it must fit the first FDT /memory region')
    parser.add_argument('--dram-base', type=lambda value: int(value, 0), default=None,
                        help='Guest RAM base, for a VM whose boot shim rewrites /memory (pseudo-unprotected)')
    parser.add_argument('--owned-memory-size', type=lambda value: int(value, 0), default=None,
                        help='DarwinBoot contiguous window, 256 MiB..8 GiB in 2 MiB steps')
    parser.add_argument('--timebase', type=lambda value: int(value, 0), default=19200000,
                        help='CNTFRQ_EL0 of the guest (the Linux guest prints it: "arch_timer: ... running at")')
    parser.add_argument('--cpu-regs', default=None,
                        help='Use only these FDT CPUs (comma-separated MPIDR values); the first is the boot CPU')
    parser.add_argument('--psci-method', choices=['smc', 'hvc'], default=None,
                        help='Override the FDT /psci conduit')
    parser.add_argument('--panic-region', default='top',
                        help='BASE:SIZE of the panic log (/pram + /chosen/embedded-panic-log-size, read by the '
                             'XNU_DT_PANIC_REGION kernel), "top" for the last 512 KiB of guest RAM, or "none". '
                             'The BCM2837 default 0x3c0fc000 is no RAM in a crosvm guest and faults as an external abort')
    parser.add_argument('--root-match-volume-uuid', default=None,
                        help='/chosen/root-matching: select the root by the UUID of an IOMedia')
    parser.add_argument('--unique-chip-id', type=lambda value: int(value, 16), default=None,
                        help='/chosen/unique-chip-id (8 bytes) and --chip-id (4 bytes) seed XNU IOPlatformUUID')
    parser.add_argument('--chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--ipad-identity', action='store_true',
                        help='/product and /buttons describe an iPad-class device (values of the unchanged iPad7,1 tree)')
    parser.add_argument('--packed-data-ramdisk', action='store_true')
    parser.add_argument('--data-ramdisk', action='store_true',
                        help='/chosen/memory-map RAMDisk placeholder; the loader fills in the real range')
    parser.add_argument('--trust-cache', action='store_true', help='Zeroed TrustCache sidecar handoff placeholder')
    args = parser.parse_args()
    if (args.unique_chip_id is None) != (args.chip_id is None):
        parser.error('--unique-chip-id and --chip-id must be supplied together')
    if args.owned_memory_size is not None and (
            not 0x10000000 <= args.owned_memory_size <= 0x200000000 or args.owned_memory_size % 0x200000):
        parser.error('Owned memory must be 256 MiB to 8 GiB, aligned to 2 MiB')
    if not 0 < args.timebase < 1 << 32:
        parser.error('--timebase must fit the DT u32 ABI')

    vm = describe(parse_fdt(args.fdt.read_bytes()))
    if args.dram_base is None and args.dram_size > vm['dram_size']:
        parser.error(f'--dram-size {args.dram_size:#x} exceeds the FDT /memory region {vm["dram_size"]:#x}; '
                     'a pseudo-unprotected VM needs --dram-base and --dram-size from its boot shim')
    dram_base = vm['dram_base'] if args.dram_base is None else args.dram_base
    if dram_base % 0x200000 or args.dram_size % 0x200000:
        parser.error('the guest RAM base and size must be 2 MiB aligned')
    if args.owned_memory_size is not None and args.owned_memory_size > args.dram_size:
        parser.error('the DarwinBoot window cannot exceed the VM RAM')
    cpu_regs = vm['cpu_regs']
    if args.cpu_regs is not None:
        try:
            selected = [int(value, 0) for value in args.cpu_regs.split(',')]
        except ValueError:
            parser.error('--cpu-regs must be integers')
        if not selected or len(set(selected)) != len(selected) or any(reg not in cpu_regs for reg in selected):
            parser.error(f'--cpu-regs must name distinct FDT CPUs out of {cpu_regs}')
        cpu_regs = selected
    if len(cpu_regs) > 8:
        cpu_regs = cpu_regs[:8]    # ArmCpu starts at most eight cores
    psci_method = args.psci_method or vm['psci_method']
    if args.panic_region == 'none':
        panic_region = None
    elif args.panic_region == 'top':
        panic_region = (dram_base + args.dram_size - 0x80000, 0x80000)
    else:
        base, _, size = args.panic_region.partition(':')
        panic_region = (int(base, 0), int(size, 0))
    if panic_region and (panic_region[0] % 0x4000 or panic_region[1] % 0x4000 or not 0x4000 <= panic_region[1] < 1 << 32):
        parser.error('--panic-region must be 16 KiB aligned and below 4 GiB in size')
    dist, dist_size, redist, redist_size = vm['gic']

    cpus = [node('cpu%d' % index, [('state', b'running\0' if index == 0 else b'waiting\0'),
                                   ('reg', struct.pack('<I', reg)),
                                   ('device_type', b'cpu\0'), ('compatible', b'arm,armv8\0'),
                                   ('timebase-frequency', struct.pack('<I', args.timebase))])
            for index, reg in enumerate(cpu_regs)]
    tree = node('device-tree', [
        ('model', b'iPad7,1\0' if args.ipad_identity else b'droidvm-first-boot\0'),
        *([('target-type', b'J120\0')] if args.ipad_identity else []),
        ('compatible', b'droidvm,crosvm-gunyah\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        node('chosen', [('random-seed', os.urandom(64)),
                        *([('unique-chip-id', struct.pack('<Q', args.unique_chip_id)), ('chip-id', struct.pack('<I', args.chip_id))]
                          if args.unique_chip_id is not None else []),
                        ('osenvironment', b'main\0'),
                        *([('certificate-production-status', struct.pack('<I', 0)), ('effective-production-status-ap', struct.pack('<I', 0)),
                           ('certificate-security-mode', struct.pack('<I', 1)), ('effective-security-mode-sep', struct.pack('<I', 1))]
                          if args.ipad_identity else []),
                        *([('ephemeral-storage', struct.pack('<I', 1))] if (args.data_ramdisk or args.packed_data_ramdisk) else []),
                        *([('darwinboot-memory-size', struct.pack('<Q', args.owned_memory_size))] if args.owned_memory_size else []),
                        ('dram-base', struct.pack('<Q', dram_base)),
                        ('dram-size', struct.pack('<Q', args.dram_size)),
                        ('crypto-hash-method', b'sha2-384\0'),
                        *([('root-matching', ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
                                              '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
                                              args.root_match_volume_uuid.upper() + '</string></dict></dict>\0').encode())]
                          if args.root_match_volume_uuid else []),
                        ('amfi-only-platform-code', struct.pack('<I', 1)),
                        *([('embedded-panic-log-size', struct.pack('<I', panic_region[1]))] if panic_region else []),
                        *empty_nvram_proxy()],
             [node('memory-map', ([('PackedRAMDisk' if args.packed_data_ramdisk else 'RAMDisk', bytes(16))]
                                  if (args.data_ramdisk or args.packed_data_ramdisk) else []) +
                                 ([('TrustCache', bytes(16))] if args.trust_cache else [])),
              node('manifest-properties')]),
        node('cpus', children=cpus),
        *([node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', psci_method.encode() + b'\0')])] if len(cpus) > 1 else []),
        # The guest really runs under a hypervisor: XNU publishes this as kern.hv_vmm_present.
        *([node('pram', [('reg', struct.pack('<QQ', *panic_region))])] if panic_region else []),
        node('defaults', [('sleep-disabled', struct.pack('<I', 1)), ('vmm-present', struct.pack('<I', 1))]),
        node('options', []),
        *([node('buttons', [('compatible', b'buttons\0'), ('device_type', b'buttons\0'),
                            ('button-names', b'hold\0voldown\0volup\0'),
                            ('home-button-type', struct.pack('<I', 2))])] if args.ipad_identity else []),
        node('product', [('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0' if args.ipad_identity else b'DroidVM guest\0'),
                         ('model', b'droidvm,crosvm-gunyah\0'),
                         *([('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0'),
                            ('artwork-device-idiom', b'pad\0'),
                            ('artwork-device-subtype', struct.pack('<I', 2732)),
                            ('artwork-scale-factor', struct.pack('<I', 2)),
                            ('artwork-display-gamut', b'P3\0'),
                            ('compatible-device-fallback', b'iPad6,7\0'),
                            ('thin-bezel', struct.pack('<I', 1))] if args.ipad_identity else [])]),
        node('arm-io', [('device_type', b'arm-io\0'),
                        ('#address-cells', struct.pack('<I', 2)),
                        ('#size-cells', struct.pack('<I', 2)),
                        ('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, ARMIO_SIZE))],
             [node('intc', [('compatible', b'arm,gic-v3\0'),
                            ('reg', struct.pack('<4Q', dist - ARMIO_BASE, dist_size, redist - ARMIO_BASE, redist_size)),
                            ('redistributor-stride', struct.pack('<Q', GIC_REDIST_STRIDE))]),
              *[node('virtio-mmio-%d' % index, [('compatible', b'virtio,mmio\0'),
                                                 ('reg', struct.pack('<QQ', address - ARMIO_BASE, size)),
                                                 ('dma-coherent', b'')])
                for index, (address, size) in enumerate(vm['virtio_mmio'])]]),
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    print(f'droidvm ADT: {len(tree)} bytes, {len(cpu_regs)} cpu, gic {dist:#x}/{redist:#x}+{redist_size:#x}, '
          f'psci {psci_method}, dram {dram_base:#x}+{args.dram_size:#x}, timebase {args.timebase}, '
          f'panic {"none" if not panic_region else f"{panic_region[0]:#x}+{panic_region[1]:#x}"}, '
          f'virtio-mmio {[hex(a) for a, _ in vm["virtio_mmio"]]}')


if __name__ == '__main__':
    main()
