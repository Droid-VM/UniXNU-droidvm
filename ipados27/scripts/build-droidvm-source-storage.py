#!/usr/bin/env python3
"""The disks of the iPadOS 27 board: the source storage archive (build/droidvm-source-storage/libDroidVMSourceStorage.a).

The kernel collection of 24A446 binds no class of the original IOStorageFamily. The public IOBlockStorageDevice is compiled
from IOStorageFamily's source and registers late, below the original class (package/DroidVMSourceStorage.cpp), and with it the
drivers of the guest's disks: NativeVirtioBlock (drivers/VirtioMmioBlock, verified against the root policies the firmware hands
over), NativeRamBlock (drivers/RamBlock, the Data seed) and the Data store of package/DroidVMDataStore.c. After the fork's
scripts/build-sheng-source-storage.py, which does this for sheng's UFS host.

Only the owner's public key (its 395-byte anchor) is compiled in. The signed OS and SystemOS policies go into the payload as
build/droidvm-source-storage/root-policies.bin (board.toml [payload] root_policies) when build/verified-root-<build>/ holds
them, verified for the OS profile's [root] images; the archive builds without them.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def policy_bundle(policies, key, out):
    """root-policies.bin from build/verified-root-<build>/{OS,SystemOS}, each checked against the OS profile's [root] image."""
    manifests = []
    for name, prefix in (('OS', 'UNIXNU_ROOT_OS'), ('SystemOS', 'UNIXNU_ROOT_SYSTEMOS')):
        path = policies / name / 'manifest.bin'
        if not path.is_file():
            return None
        digest, identity = os.environ.get(prefix + '_SHA256'), (os.environ.get(prefix + '_UUID') or '').upper()
        verification = json.loads(path.with_name('verification.json').read_text())
        if (not verification['real_c_signature_and_block_verification'] or verification['image_sha256'] != digest or
                verification['volume_uuid'] != identity or verification['trusted_key_sha256'] != hashlib.sha256(key).hexdigest()):
            raise SystemExit(f'{path.parent}: not a complete verification of the OS profile\'s {name} image with this owner key')
        data = path.read_bytes()
        if len(data) < 544 or data[56:88].hex() != digest:
            raise SystemExit(f'{path}: the manifest does not describe the {name} image')
        manifests.append(data)
    cursor = 32 + 16 * len(manifests)
    table = bytearray()
    for data in manifests:
        table += struct.pack('<QQ', cursor, len(data))
        cursor += len(data)
    bundle = struct.pack('<8sQQQ', b'VROOTB01', 1, len(manifests), cursor) + table + b''.join(manifests)
    (out / 'root-policies.bin').write_bytes(bundle)
    return out / 'root-policies.bin'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=ROOT)
    args = parser.parse_args()
    tree = args.tree.resolve()
    if os.environ.get('UNIXNU_XNU_ARCH') != 'arm64e' or int(os.environ.get('UNIXNU_IOS_VERSION', '0').split('.')[0]) < 26:
        parser.error('the source storage is for the arm64e iPadOS 26+ profiles; run it through unixnu build kernel')
    build_id = os.environ.get('UNIXNU_IPSW_BUILD')
    if not build_id:
        parser.error('UNIXNU_IPSW_BUILD is not set; run it through unixnu build kernel')
    compile_helper = helper('source_compile', tree / 'scripts/build-sheng-source-drivers.py')
    headers = helper('source_headers', tree / 'scripts/prepare-storage-headers.py')
    target = helper('source_target', tree / 'scripts/kernel_target.py')
    manifest_tool = helper('verified_manifest', tree / 'kits/VerifiedRoot/tools/manifest.py')
    storage = tree / 'refer/IOStorageFamily'
    package = tree / 'kits/droidvm'
    out = tree / 'build/droidvm-source-storage'
    out.mkdir(parents=True, exist_ok=True)
    # The pin of the OS profile's IOStorageFamily source (IOStorageFamily-337.100.1).
    header_report = headers.prepare(storage, '7edb88fbae296fb7c8ce2f64e115e116e566d51c', out / 'headers')

    public_key = tree / 'build/verified-root-host/keys/owner-public.pem'
    if not public_key.is_file():
        raise SystemExit(f'{public_key} missing: the owner keys come first (build-droidvm-all.py, step policies)')
    key = manifest_tool.public_anchor(public_key)
    if len(key) != 395:
        raise SystemExit(f'owner anchor is {len(key)} bytes, not 395')
    bundle = policy_bundle(tree / f'build/verified-root-{build_id}', key, out)
    anchor = out / 'RootAnchor.c'
    anchor.write_text('#include <stdint.h>\nconst uint8_t verified_root_key[395] = {' + ','.join(str(b) for b in key) + '};\n')

    base = out / 'IOBlockStorageDevice.cpp'
    original = (storage / 'IOBlockStorageDevice.cpp').read_text()
    macro = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
    if original.count(macro) != 1:
        raise SystemExit('the pinned IOBlockStorageDevice registration macro changed')
    # Registered under another name, with the original class as its parent (native_source_storage_parent); the C++ class name
    # stays, since arm64e's vtable discriminators are derived from it.
    base.write_text(original.replace(macro, '''
extern "C" const OSMetaClass *native_source_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("UniXNUBlockStorageDevice", native_source_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
'''))
    # NativeVirtioBlock as xnu-12377 takes it: kmem_alloc's KMA_* flags and kmem_free moved to vm/vm_kern_xnu.h, and
    # vm_map_protect to vm_map_internal.h, with its protection and set_max swapped and its range in vm_map_offset_ut; the public
    # mach_vm_protect does the same. Upstream VirtioMmioBlock (f4dfce6) predates xnu-12377.
    virtio = out / 'NativeVirtioBlock.cpp'
    text = (tree / 'drivers/VirtioMmioBlock/NativeVirtioBlock.cpp').read_text()
    for old_text, new_text in (('#include <vm/vm_map.h>\n', '#include <vm/vm_map.h>\n#include <vm/vm_kern_xnu.h>\n#include <mach/mach_vm.h>\n'),
                               ('vm_map_protect(kernel_map, verifiedStorage, verifiedStorage + verifiedStorageBytes,\n      VM_PROT_READ, TRUE)',
                                'mach_vm_protect(kernel_map, verifiedStorage, verifiedStorageBytes,\n      TRUE, VM_PROT_READ)')):
        if text.count(old_text) != 1:
            raise SystemExit(f'NativeVirtioBlock.cpp changed: {old_text.splitlines()[0]!r}')
        text = text.replace(old_text, new_text)
    virtio.write_text(text)
    obj = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    record = lambda path: json.loads((obj / path).read_text().strip().rstrip(','))
    cpp, c = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'), record('osfmk/DEVELOPMENT/startup.o.json')
    includes = ['-I', str(out / 'headers/include'), '-I', str(tree / 'kits/VerifiedRoot/include'),
                '-I', str(tree / 'drivers/VirtioMmioBlock'), '-I', str(tree / '.cache/xnu-work/osfmk')]
    flags = ['-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1', '-DNATIVE_VERIFIED_POLICY_HANDOFF=1']
    late = {'IOBlockStorageDevice.cpp', 'NativeVirtioBlock.cpp', 'NativeRamBlock.cpp'}
    sources = [base, virtio, tree / 'drivers/RamBlock/NativeRamBlock.cpp',
               package / 'DroidVMSourceStorage.cpp', package / 'DroidVMSourceRootPolicies.cpp', package / 'DroidVMDataStore.c',
               tree / 'kits/VerifiedRoot/src/verified_root.c', tree / 'kits/VerifiedRoot/adapters/xnu_crypto.c', anchor]
    objects, inputs = [], {}
    objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
    for source in sources:
        output = out / (source.stem + '.o')
        command = compile_helper.compile_command(c if source.suffix == '.c' else cpp, source, output, includes, flags)
        if source == base:
            command.append('-Wno-error')
        result = subprocess.run(command, cwd=(c if source.suffix == '.c' else cpp)['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / (source.stem + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
        (out / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(result.stdout[-6000:])
        target.check_object(output.read_bytes(), source.name)
        if source.name in late:
            # No static initializer at kernel start: DroidVMSourceStorage.cpp runs it once the original family has loaded.
            moved = output.with_suffix('.late.o')
            subprocess.run([objcopy, '--remove-section=__DATA,__mod_init_func',
                            f'--globalize-symbol=__GLOBAL__sub_I_{source.name}', output, moved], check=True)
            output = moved
        objects.append(output)
        inputs[str(source)] = compile_helper.sha(source)
    archive = out / 'libDroidVMSourceStorage.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
    report = dict(archive=str(archive), archive_sha256=compile_helper.sha(archive), sources=inputs, headers=header_report,
                  target=target.TRIPLE, trusted_key_sha256=hashlib.sha256(key).hexdigest(),
                  policy_bundle=str(bundle) if bundle else None, runtime_qualified=False)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(archive)
    if not bundle:
        print(f'no root policies yet (build/verified-root-{build_id}): the kernel links, a payload cannot boot its disks')


if __name__ == '__main__':
    main()
