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
  - /arm-io/data-store: the "virtio,mmio" node at --data-store-mmio instead, the writable disk of the Data
    store window (package/DroidVMDataStore.c): compatible "droidvm,data-store", so NativeVirtioBlock,
    which serves only verified read-only images, does not bind it
  - /arm-io/input-N: the "virtio,mmio" nodes at --input-mmio instead, the input devices of crosvm
    --virtio-mmio-input (package/DroidVMVirtioInput.c), compatible "droidvm,virtio-input"
  - /arm-io/virtio-gpu: --virtio-gpu-mmio, the virtio GPU (package/DroidVMVirtioGpu*.cpp). Its drm-host-pool
    property carries the absolute GPA and size of the FDT's pre-shared drm2kgsl_host pool.
  - /arm-io/virtio-snd: --snd-mmio, the sound card of crosvm --virtio-mmio-snd (package/DroidVMVirtioSnd.c),
    compatible "droidvm,virtio-snd"
  - /arm-io/virtio-media-N: --media-mmio, the media devices of crosvm --virtio-mmio-media (package/DroidVMVirtioMedia.c),
    compatible "droidvm,virtio-media". Their media-host-pool property carries the absolute GPA and size of the FDT's
    pre-shared media_host pool (--pre-alloc media-host-mb), where the devices' MMAP buffers live.
  - /arm-io/battery: --battery, crosvm's goldfish battery (--battery type=goldfish, package/DroidVMBattery.cpp),
    compatible "google,goldfish-battery"
  - /chosen darwinboot-fdt (--dynamic): DarwinBoot completes the tree at boot from the firmware's FDT (DarwinBoot FdtAdt.c):
    bit 0 the RAM (/chosen dram-base and dram-size from /memory, and /pram, darwinboot-memory-limit, darwinboot-video-memory and
    kern.maxvnodes recomputed from its end), bit 1 the devices (the /arm-io nodes below of every virtio-mmio window, by the
    device ID it reports, with the pools, and the battery), bit 2 the CPUs (/cpus from the FDT's, by MPIDR, the first the boot
    CPU, and /arm-io/intc's GICv3 windows and /psci's conduit in place: crosvm moves the redistributors with the vCPU count).
    The tree then leaves those nodes out -- /cpus holds cpu0 alone, the template of the others, and /psci is always there --
    and the window arguments (--data-store-mmio, --input-mmio, --net-mmio, --virtio-gpu-mmio, --snd-mmio, --media-mmio,
    --battery) and --cpu-regs are not needed; the RAM, CPU and GIC values written here are placeholders of the same size.
  - /chosen/darwinboot-video-memory: --boot-video-mib reserves ordinary RAM below the panic
    region for a BLT-only GOP handoff. It needs no crosvm simplefb device or framebuffer MMIO.
  - /chosen darwinboot-memory-limit (--reclaim-memory): DarwinBoot also gives the kernel the RAM after its window that the
    firmware frees at ExitBootServices, up to this address (the panic region); /defaults kern.maxvnodes is sized for it
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


def read_adt(blob, offset=0):
    """An Apple device tree as (properties, children); properties map a name to its bytes. A length's top bit marks a
    template property (newer trees), not part of its size."""
    properties_count, children_count = struct.unpack_from('<II', blob, offset)
    offset += 8
    properties = {}
    for _ in range(properties_count):
        name = blob[offset:offset + 32].split(b'\0')[0].decode()
        length = struct.unpack_from('<I', blob, offset + 32)[0] & 0x7fffffff
        offset += 36
        properties[name] = blob[offset:offset + length]
        offset += (length + 3) & ~3
    children = []
    for _ in range(children_count):
        child, offset = read_adt(blob, offset)
        children.append(child)
    return (properties, children), offset


# The /product properties of the identity tree a virtual machine does not have: the exclaves of Apple's SoCs, and the panel's
# gamut, which --display-gamut sets.
IDENTITY_PRODUCT_DROPPED = {'name', 'AAPL,phandle', 'exclaves-enabled', 'has-exclaves', 'artwork-display-gamut'}


def device_tree_bytes(path):
    """A device tree from a file: a decoded tree as it is, or the IPSW's IM4P (DeviceTree.<board>.im4p), whose payload is
    LZFSE-compressed (decoded by macOS compression_tool)."""
    data = Path(path).read_bytes()
    if data[:1] != b'\x30':
        return data

    def element(at):
        tag, length = data[at], data[at + 1]
        at += 2
        if length & 0x80:
            count = length & 0x7f
            length = int.from_bytes(data[at:at + count], 'big')
            at += count
        return tag, at, at + length

    _, at, end = element(0)
    fields = []
    while at < end and len(fields) < 4:
        tag, start, stop = element(at)
        fields.append((tag, data[start:stop]))
        at = stop
    if len(fields) < 4 or fields[0] != (0x16, b'IM4P') or fields[3][0] != 0x04:
        raise SystemExit(f'{path}: neither a device tree nor an IM4P')
    payload = fields[3][1]
    if payload[:4] in (b'bvx2', b'bvxn', b'bvx-', b'bvx1'):
        import subprocess
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            source, target = Path(directory) / 'in', Path(directory) / 'out'
            source.write_bytes(payload)
            subprocess.run(['compression_tool', '-decode', '-a', 'lzfse', '-i', str(source), '-o', str(target)], check=True)
            payload = target.read_bytes()
    return payload


def identity_of(path):
    """The device identity of an OS's own device tree (the IPSW's DeviceTree, decoded or as its IM4P): the root's model,
    target and board names, and the /product properties MobileGestalt and the frameworks read (product-id, artwork, feature
    classes)."""
    (root, children), _ = read_adt(device_tree_bytes(path))
    product = next((props for props, _ in children if props.get('name', b'').rstrip(b'\0') == b'product'), None)
    if product is None or 'model' not in root or 'target-type' not in root:
        raise SystemExit(f'{path}: not a device tree with a model, a target-type and /product')
    return {'model': root['model'], 'target-type': root['target-type'],
            'board': root.get('compatible', b'').split(b'\0')[0] + b'\0',
            'product': [(key, value) for key, value in product.items() if key not in IDENTITY_PRODUCT_DROPPED]}


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
    drm_host = [pair for path, props in fdt.items()
                if path.startswith('/reserved-memory/drm2kgsl_host@')
                for pair in reg_pairs(props['reg'])]
    if len(drm_host) > 1:
        raise SystemExit('FDT has more than one drm2kgsl_host pool')
    media_host = [pair for path, props in fdt.items()
                  if path.startswith('/reserved-memory/media_host@')
                  for pair in reg_pairs(props['reg'])]
    if len(media_host) > 1:
        raise SystemExit('FDT has more than one media_host pool')
    for address, size in virtio:
        if not ARMIO_BASE <= address < address + size <= ARMIO_BASE + ARMIO_SIZE:
            raise SystemExit(f'virtio-mmio window {address:#x}+{size:#x} lies outside the arm-io window')
    battery = [pair for path, props in fdt.items()
               if 'google,goldfish-battery' in strings_of(props.get('compatible', b''))
               for pair in reg_pairs(props['reg'])]
    # The SBSA UART (crosvm pl011@6000, FDT stdout-path): XNU's VMAPPLE_UART path is a PL011 driver and finds it by the ADT
    # node named "uart0" (soc_base + reg). It gives XNU a working serial console on this crosvm VM -- BCM2837's own PI3 mini-UART
    # (the board default) is not emulated by crosvm, so without this XNU's console is silent. REQUIRES the kernel built with
    # VMAPPLE_UART defined for this board (not yet; see plans/UNIXNUSETTING.md). Inert otherwise -- an unused ADT node.
    pl011 = [reg_pairs(props['reg'])[0] for path, props in fdt.items()
             if 'arm,pl011' in strings_of(props.get('compatible', b''))]
    sbsa_uart = pl011[0] if pl011 else None
    if sbsa_uart and not (ARMIO_BASE <= sbsa_uart[0] < sbsa_uart[0] + sbsa_uart[1] <= ARMIO_BASE + ARMIO_SIZE):
        raise SystemExit(f'pl011 window {sbsa_uart[0]:#x}+{sbsa_uart[1]:#x} lies outside the arm-io window')
    return dict(cpu_regs=[reg for reg, _ in cpus], gic=(dist, dist_size, redist, redist_size),
                psci_method=method or 'hvc', dram_base=dram_base, dram_size=dram_size,
                virtio_mmio=virtio, drm_host_pool=drm_host, media_host_pool=media_host, battery=battery, sbsa_uart=sbsa_uart)


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
    parser.add_argument('--identity-tree', type=Path, default=None,
                        help='with --ipad-identity: the identity (root model, target-type, board, /product) of this device '
                             'tree of the target OS (its IPSW DeviceTree, the IM4P or decoded) instead of the iPad7,1 values')
    parser.add_argument('--packed-data-ramdisk', action='store_true')
    parser.add_argument('--data-ramdisk', action='store_true',
                        help='/chosen/memory-map RAMDisk placeholder; the loader fills in the real range')
    parser.add_argument('--trust-cache', action='store_true', help='Zeroed TrustCache sidecar handoff placeholder')
    parser.add_argument('--root-policies', action='store_true',
                        help='Zeroed RootPolicies placeholder: DarwinBoot hands kernel.macho.vroot (the disks\' signed root policies) '
                             'over there')
    parser.add_argument('--data-store-mmio', type=lambda value: int(value, 0), default=None,
                        help='Base of the FDT virtio-mmio window that holds the persistent Data store (/arm-io/data-store)')
    parser.add_argument('--display-gamut', choices=['P3', 'sRGB'], default='P3',
                        help='/product artwork-display-gamut: P3 is the iPad7,1 panel; sRGB keeps UIKit on 8-bit sRGB surfaces')
    parser.add_argument('--input-mmio', default='',
                        help='Bases of the FDT virtio-mmio windows of the input devices, comma-separated (/arm-io/input-N)')
    parser.add_argument('--net-mmio', type=lambda value: int(value, 0), default=None,
                        help='Base of the FDT virtio-mmio window of the virtio-net device (/arm-io/net)')
    parser.add_argument('--virtio-gpu-mmio', type=lambda value: int(value, 0), default=None,
                        help='Base of the standard virtio-gpu window (/arm-io/virtio-gpu)')
    parser.add_argument('--snd-mmio', type=lambda value: int(value, 0), default=None,
                        help='Base of the FDT virtio-mmio window of the virtio-snd card (/arm-io/virtio-snd)')
    parser.add_argument('--media-mmio', default='',
                        help='Comma-separated bases of the FDT virtio-mmio windows of the virtio-media devices (/arm-io/virtio-media-N)')
    parser.add_argument('--dynamic', action='store_true',
                        help='/chosen darwinboot-fdt: DarwinBoot fills in the RAM and the /arm-io devices from the firmware FDT at boot')
    parser.add_argument('--battery', action='store_true',
                        help='Emit /arm-io/battery from the FDT goldfish-battery node (crosvm --battery type=goldfish)')
    parser.add_argument('--boot-video-mib', type=int, default=0,
                        help='Reserve ordinary RAM for the BLT-only GOP boot framebuffer (requires --reclaim-memory)')
    parser.add_argument('--reclaim-memory', action='store_true',
                        help='/chosen/darwinboot-memory-limit: DarwinBoot also hands the kernel the RAM after its window that the '
                             'firmware frees at ExitBootServices, up to the panic region (or the end of guest RAM)')
    args = parser.parse_args()
    if args.identity_tree and not args.ipad_identity:
        parser.error('--identity-tree takes --ipad-identity')
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
    # The end of the kernel's memory when DarwinBoot extends its window: the panic region, which the kernel maps as I/O, stays out.
    memory_limit = None
    if args.reclaim_memory:
        memory_limit = (panic_region[0] if panic_region else dram_base + args.dram_size) & ~0x1fffff
    boot_video = None
    if args.boot_video_mib:
        if not args.reclaim_memory or not 2 <= args.boot_video_mib <= 64 or args.boot_video_mib % 2:
            parser.error('--boot-video-mib requires --reclaim-memory and an even size from 2 to 64 MiB')
        video_bytes = args.boot_video_mib << 20
        if memory_limit - dram_base <= video_bytes + (args.owned_memory_size or 0):
            parser.error('boot video reservation leaves no room for the DarwinBoot window')
        memory_limit -= video_bytes
        boot_video = (memory_limit, video_bytes)
    # The memory the kernel manages: its window, or about everything up to the limit when the window is extended.
    kernel_memory = memory_limit - dram_base if memory_limit else (args.owned_memory_size or args.dram_size)
    if panic_region and (panic_region[0] % 0x4000 or panic_region[1] % 0x4000 or not 0x4000 <= panic_region[1] < 1 << 32):
        parser.error('--panic-region must be 16 KiB aligned and below 4 GiB in size')
    dist, dist_size, redist, redist_size = vm['gic']
    try:
        input_bases = [int(value, 0) for value in args.input_mmio.split(',') if value]
    except ValueError:
        parser.error('--input-mmio takes comma-separated addresses')
    try:
        media_bases = [int(value, 0) for value in args.media_mmio.split(',') if value]
    except ValueError:
        parser.error('--media-mmio takes comma-separated addresses')
    virtio_mmio = [window for window in vm['virtio_mmio']
                   if window[0] not in (args.data_store_mmio, args.net_mmio, args.virtio_gpu_mmio, args.snd_mmio)
                   and window[0] not in input_bases and window[0] not in media_bases]
    data_store = [window for window in vm['virtio_mmio'] if window[0] == args.data_store_mmio]
    if args.data_store_mmio is not None and not data_store:
        parser.error(f'the FDT has no virtio-mmio window at {args.data_store_mmio:#x} for --data-store-mmio')
    inputs = [window for base in input_bases for window in vm['virtio_mmio'] if window[0] == base]
    if len(inputs) != len(input_bases) or args.data_store_mmio in input_bases:
        parser.error('every --input-mmio base must be a virtio-mmio window of the FDT, other than the data store')
    net = [window for window in vm['virtio_mmio'] if window[0] == args.net_mmio]
    if args.net_mmio is not None and (not net or args.net_mmio in input_bases or
                                      args.net_mmio == args.data_store_mmio):
        parser.error(f'--net-mmio {args.net_mmio:#x} must be a virtio-mmio window of the FDT of its own')
    virtio_gpu = [window for window in vm['virtio_mmio'] if window[0] == args.virtio_gpu_mmio]
    if args.virtio_gpu_mmio is not None and (not virtio_gpu or args.virtio_gpu_mmio in input_bases or
                                           args.virtio_gpu_mmio in (args.data_store_mmio, args.net_mmio)):
        parser.error(f'--virtio-gpu-mmio {args.virtio_gpu_mmio:#x} must be a virtio-mmio window of the FDT of its own')
    media = [window for base in media_bases for window in vm['virtio_mmio'] if window[0] == base]
    taken = (args.data_store_mmio, args.net_mmio, args.virtio_gpu_mmio, args.snd_mmio)
    if len(media) != len(media_bases) or len(set(media_bases)) != len(media_bases) or \
            any(base in input_bases or base in taken for base in media_bases):
        parser.error('each --media-mmio address must be a virtio-mmio window of the FDT of its own')
    if media and not vm['media_host_pool']:
        parser.error('--media-mmio needs the FDT media_host pool (crosvm --pre-alloc media-host-mb)')
    snd = [window for window in vm['virtio_mmio'] if window[0] == args.snd_mmio]
    if args.snd_mmio is not None and (not snd or args.snd_mmio in input_bases or
                                      args.snd_mmio in (args.data_store_mmio, args.net_mmio, args.virtio_gpu_mmio)):
        parser.error(f'--snd-mmio {args.snd_mmio:#x} must be a virtio-mmio window of the FDT of its own')

    cpus = [node('cpu%d' % index, [('state', b'running\0' if index == 0 else b'waiting\0'),
                                   ('reg', struct.pack('<I', reg)),
                                   ('device_type', b'cpu\0'), ('compatible', b'arm,armv8\0'),
                                   ('timebase-frequency', struct.pack('<I', args.timebase))])
            for index, reg in enumerate(cpu_regs[:1] if args.dynamic else cpu_regs)]
    identity = identity_of(args.identity_tree) if args.identity_tree else None
    tree = node('device-tree', [
        ('model', (identity['model'] if identity else b'iPad7,1\0') if args.ipad_identity else b'droidvm-first-boot\0'),
        *([('target-type', identity['target-type'] if identity else b'J120\0')] if args.ipad_identity else []),
        # The iPad's board name first: libMobileGestalt reads the first entry as the hardware model (HWModelStr), and CoreMotion and
        # locationd know a device's motion sensors only by it (CLPlatformInfo::queryEmbeddedSystemHardware hashes it against its
        # table; "droidvm,crosvm-gunyah" had none, so CMMotionManager reported no accelerometer or gyroscope). The audio server and
        # MediaExperience key their per-board tables on it too (UniXNU 688fa88). The board's own string stays matchable after it.
        ('compatible', ((identity['board'] if identity else b'J120AP\0') if args.ipad_identity else b'') + b'droidvm,crosvm-gunyah\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        # xnu-12377 takes 4 x 64 seed bytes on ARM and panics with fewer ("Expected 256 seed bytes from bootloader"); older
        # kernels read the first 64.
        node('chosen', [('random-seed', os.urandom(256)),
                        *([('unique-chip-id', struct.pack('<Q', args.unique_chip_id)), ('chip-id', struct.pack('<I', args.chip_id))]
                          if args.unique_chip_id is not None else []),
                        ('osenvironment', b'main\0'),
                        *([('darwinboot-fdt', struct.pack('<I', 7))] if args.dynamic else []),
                        *([('certificate-production-status', struct.pack('<I', 0)), ('effective-production-status-ap', struct.pack('<I', 0)),
                           ('certificate-security-mode', struct.pack('<I', 1)), ('effective-security-mode-sep', struct.pack('<I', 1))]
                          if args.ipad_identity else []),
                        *([('ephemeral-storage', struct.pack('<I', 1))] if (args.data_ramdisk or args.packed_data_ramdisk) else []),
                        *([('darwinboot-memory-size', struct.pack('<Q', args.owned_memory_size))] if args.owned_memory_size else []),
                        *([('darwinboot-memory-limit', struct.pack('<Q', memory_limit))] if memory_limit else []),
                        *([('darwinboot-video-memory', struct.pack('<QQ', *boot_video))] if boot_video else []),
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
                                 ([('TrustCache', bytes(16))] if args.trust_cache else []) +
                                 ([('RootPolicies', bytes(16))] if args.root_policies else [])),
              node('manifest-properties')]),
        node('cpus', children=cpus),
        *([node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', psci_method.encode() + b'\0')])]
          if len(cpus) > 1 or args.dynamic else []),
        # The guest really runs under a hypervisor: XNU publishes this as kern.hv_vmm_present.
        *([node('pram', [('reg', struct.pack('<QQ', *panic_region))])] if panic_region else []),
        # kern.maxvnodes: the vnode limit XNU sizes from memory, (bytes / 64 KiB) + 1024, over the memory the kernel manages. Without it
        # the BCM2837 configuration this 4K kernel is built from clips the limit to its CONFIG_VNODES (1024, a Raspberry Pi 3), and the
        # vnode jetsam then kills every idle process, an app's prewarmed instance among them, as soon as the system holds ~2000 vnodes.
        node('defaults', [('sleep-disabled', struct.pack('<I', 1)), ('vmm-present', struct.pack('<I', 1)),
                          ('kern.maxvnodes', struct.pack('<I', kernel_memory // 0x10000 + 1024))]),
        node('options', []),
        *([node('buttons', [('compatible', b'buttons\0'), ('device_type', b'buttons\0'),
                            ('button-names', b'hold\0voldown\0volup\0'),
                            ('home-button-type', struct.pack('<I', 2))])] if args.ipad_identity else []),
        node('product', [*(identity['product'] + [('artwork-display-gamut', args.display_gamut.encode() + b'\0')]
                           if identity and args.ipad_identity else
                           [('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0' if args.ipad_identity else b'DroidVM guest\0')]),
                         ('model', b'droidvm,crosvm-gunyah\0'),
                         *([('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0'),
                            ('artwork-device-idiom', b'pad\0'),
                            ('artwork-device-subtype', struct.pack('<I', 2732)),
                            ('artwork-scale-factor', struct.pack('<I', 2)),
                            ('artwork-display-gamut', args.display_gamut.encode() + b'\0'),
                            ('compatible-device-fallback', b'iPad6,7\0'),
                            ('thin-bezel', struct.pack('<I', 1)),
                            # MGGetProductType() looks this 20-byte id up in libMobileGestalt's product table (J120: 0x80ac7ae0);
                            # without it the product type is 0 and every client that switches on it sees an unknown device.
                            ('product-id', bytes.fromhex('e9fefef6e6f9d12a5ba1a2551866cbbd749f0ec3'))]
                           if args.ipad_identity and not identity else [])],
             # the audio server reads /product/audio (the original also sets supports-always-listening)
             [node('audio', [('wireless-splitter', struct.pack('<I', 1))])] if args.ipad_identity else []),
        node('arm-io', [('device_type', b'arm-io\0'),
                        ('#address-cells', struct.pack('<I', 2)),
                        ('#size-cells', struct.pack('<I', 2)),
                        ('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, ARMIO_SIZE))],
             [node('intc', [('compatible', b'arm,gic-v3\0'),
                            ('reg', struct.pack('<4Q', dist - ARMIO_BASE, dist_size, redist - ARMIO_BASE, redist_size)),
                            ('redistributor-stride', struct.pack('<Q', GIC_REDIST_STRIDE))]),
              *([] if args.dynamic else [
                *[node('virtio-mmio-%d' % index, [('compatible', b'virtio,mmio\0'),
                                                   ('reg', struct.pack('<QQ', address - ARMIO_BASE, size)),
                                                   ('dma-coherent', b'')])
                  for index, (address, size) in enumerate(virtio_mmio)],
                *[node('data-store', [('compatible', b'droidvm,data-store\0'),
                                      ('reg', struct.pack('<QQ', address - ARMIO_BASE, size)),
                                      ('dma-coherent', b'')])
                  for address, size in data_store],
                *[node('input-%d' % index, [('compatible', b'droidvm,virtio-input\0'),
                                             ('reg', struct.pack('<QQ', address - ARMIO_BASE, size)),
                                             ('dma-coherent', b'')])
                  for index, (address, size) in enumerate(inputs)],
                *[node('net', [('compatible', b'droidvm,net\0'),
                               ('reg', struct.pack('<QQ', address - ARMIO_BASE, size)),
                               ('dma-coherent', b'')])
                  for address, size in net],
                *[node('virtio-gpu', [('compatible', b'droidvm,virtio-gpu\0'),
                                      ('reg', struct.pack('<QQ', address - ARMIO_BASE, size)),
                                      ('dma-coherent', b''),
                                      *[('drm-host-pool', struct.pack('<QQ', *pool)) for pool in vm['drm_host_pool']]])
                  for address, size in virtio_gpu],
                *[node('virtio-snd', [('compatible', b'droidvm,virtio-snd\0'),
                                      ('reg', struct.pack('<QQ', address - ARMIO_BASE, size)),
                                      ('dma-coherent', b'')])
                  for address, size in snd],
                *[node('virtio-media-%d' % index, [('compatible', b'droidvm,virtio-media\0'),
                                                    ('reg', struct.pack('<QQ', address - ARMIO_BASE, size)),
                                                    ('dma-coherent', b''),
                                                    *[('media-host-pool', struct.pack('<QQ', *pool))
                                                      for pool in vm['media_host_pool']]])
                  for index, (address, size) in enumerate(media)],
                *[node('battery', [('compatible', b'google,goldfish-battery\0'),
                                   ('reg', struct.pack('<QQ', address - ARMIO_BASE, size))])
                  for address, size in (vm['battery'] if args.battery else [])],
                # XNU's VMAPPLE_UART (PL011) console: found by name "uart0", mapped at soc_base + reg (soc_base = ARMIO_BASE).
                *([node('uart0', [('compatible', b'arm,pl011\0'),
                                  ('reg', struct.pack('<QQ', vm['sbsa_uart'][0] - ARMIO_BASE, vm['sbsa_uart'][1]))])]
                  if vm.get('sbsa_uart') else [])
              ])]),
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    cpu_count = 'FDT' if args.dynamic else len(cpu_regs)
    print(f'droidvm ADT: {len(tree)} bytes, {cpu_count} cpu, gic {dist:#x}/{redist:#x}+{redist_size:#x}, '
          f'psci {psci_method}, dram {dram_base:#x}+{args.dram_size:#x}, timebase {args.timebase}, '
          f'panic {"none" if not panic_region else f"{panic_region[0]:#x}+{panic_region[1]:#x}"}, '
          f'virtio-mmio {[hex(a) for a, _ in virtio_mmio]}, data store {hex(data_store[0][0]) if data_store else "none"}, '
          f'input {[hex(a) for a, _ in inputs]}, net {hex(net[0][0]) if net else "none"}')


if __name__ == '__main__':
    main()
