#!/usr/bin/env python3
"""The verified disks of the iPadOS 27 board: raw OS and SystemOS images and the owner's signed root policies for them.

The guest reads the IPSW's OS and Cryptex1,SystemOS disk images, unchanged, as two read-only virtio-mmio disks
(NativeVirtioBlock, in the source storage of build-droidvm-source-storage.py). For each this writes:

  build/droidvm-root-images/<name>.img     the raw disk, decoded from the UDIF image (upstream's udif.py, nothing mounted),
                                           checked against the OS profile's [root] image digest -- the file the phone serves
  build/verified-root-<build>/<name>/      VerifiedRoot's policy (manifest.bin: signed header and block hash table,
                                           trusted-key.bin), checked by the real C verifier block by block (verification.json)

build-droidvm-source-storage.py bundles the policies into root-policies.bin, the firmware's RootPolicies handoff. The images are
the decrypted .dmg files of the IPSW cache (unixnu ipsw fetch, then decrypted as unixnu's ipsw.disk_image does). Like the
iPadOS 17 board's build-owner-os-policy.py, generalised to the OS profile's [root] values.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tomllib
import uuid

ROOT = Path(__file__).resolve().parents[1]


class View(ctypes.Structure):
    _fields_ = [('hashes', ctypes.c_void_p), ('image_bytes', ctypes.c_uint64), ('block_count', ctypes.c_uint64),
                ('generation', ctypes.c_uint64), ('volume_uuid', ctypes.c_uint8 * 16), ('image_sha256', ctypes.c_uint8 * 32)]


def raw_image(udif, dmg, raw, pin):
    """The raw disk of a UDIF image, written once and checked against its pin; returns its size."""
    record = raw.with_suffix('.json')
    if raw.is_file() and record.is_file() and json.loads(record.read_text()).get('sha256') == pin:
        return raw.stat().st_size
    image = udif.Image(dmg)
    digest, size = hashlib.sha256(), 0
    partial = raw.with_suffix('.partial')
    with partial.open('wb') as target:
        for block in image.blocks():
            digest.update(block)
            target.write(block)
            size += len(block)
    if size != image.size or digest.hexdigest() != pin:
        partial.unlink()
        raise SystemExit(f'{dmg.name}: raw disk sha256 {digest.hexdigest()} ({size} bytes), the OS profile pins {pin}')
    partial.rename(raw)
    record.write_text(json.dumps({'source': str(dmg), 'size': size, 'sha256': pin}, indent=2) + '\n')
    return size


def policy(manifest_tool, native, raw, size, volume, keys, out):
    """Sign the raw image's block hashes, then authenticate every block of it with the real C verifier."""
    with raw.open('rb', buffering=1 << 20) as stream:
        record = manifest_tool.make(stream, size, keys / 'owner.pem', keys / 'owner-public.pem', volume, 1, out)
    encoded = ctypes.create_string_buffer((out / 'manifest.bin').read_bytes())
    anchor = ctypes.create_string_buffer((out / 'trusted-key.bin').read_bytes())
    view = View()
    if native.vr_host_open(encoded, len(encoded) - 1, anchor, 1, ctypes.byref(view)) or bytes(view.volume_uuid) != volume.bytes:
        raise SystemExit(f'{out.name}: signature or volume authentication failed')
    complete = hashlib.sha256()
    with raw.open('rb', buffering=1 << 20) as stream:
        for index in range(view.block_count):
            block = stream.read(min(4096, view.image_bytes - index * 4096))
            if native.vr_host_block(ctypes.byref(view), index, block, len(block)) != 0:
                raise SystemExit(f'{out.name}: block {index} failed verification')
            complete.update(block)
        if stream.read(1):
            raise SystemExit(f'{out.name}: the image is longer than its policy')
    if complete.digest() != bytes(view.image_sha256) or complete.hexdigest() != record['image_sha256']:
        raise SystemExit(f'{out.name}: the verified digest differs from the image')
    report = dict(image_bytes=view.image_bytes, blocks_verified=view.block_count, image_sha256=complete.hexdigest(),
                  volume_uuid=str(volume).upper(), generation=view.generation, source_format='UDIF',
                  trusted_key_sha256=record['trusted_key_sha256'], real_c_signature_and_block_verification=True,
                  original_image_modified=False, host_only=True, boot_root_authenticated=False)
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{out.name}: signature and all {view.block_count} blocks verified, sha256 {complete.hexdigest()}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=ROOT, help='the board\'s build tree (out/.../tree)')
    parser.add_argument('--profile', type=Path, required=True, help='the OS profile (UniXNU profiles/os/<name>/profile.toml)')
    parser.add_argument('--os', type=Path, required=True, help='the decrypted OS disk image (.dmg)')
    parser.add_argument('--systemos', type=Path, required=True, help='the decrypted Cryptex1,SystemOS disk image (.dmg)')
    args = parser.parse_args()
    tree = args.tree.resolve()
    profile = tomllib.loads(args.profile.read_text())
    root, build_id = profile.get('root', {}), profile.get('ipsw', {}).get('build')
    if not build_id or not all(root.get(k) for k in ('os_volume_uuid', 'os_image_sha256', 'systemos_volume_uuid',
                                                    'systemos_image_sha256')):
        raise SystemExit(f'{args.profile}: needs [ipsw] build and [root] os/systemos volume UUIDs and image digests')
    sys.path.insert(0, str(tree / 'scripts'))
    from unixnu import udif
    spec = importlib.util.spec_from_file_location('verified_manifest', tree / 'kits/VerifiedRoot/tools/manifest.py')
    manifest_tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manifest_tool)
    host = tree / 'build/verified-root-host'
    native = ctypes.CDLL(str(host / 'verified-root.dylib'))
    native.vr_host_open.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(View)]
    native.vr_host_block.argtypes = [ctypes.POINTER(View), ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
    images = tree / 'build/droidvm-root-images'
    images.mkdir(parents=True, exist_ok=True)
    for name, dmg, prefix in (('OS', args.os, 'os'), ('SystemOS', args.systemos, 'systemos')):
        raw = images / f'{name.lower()}.img'
        size = raw_image(udif, dmg, raw, root[f'{prefix}_image_sha256'])
        policy(manifest_tool, native, raw, size, uuid.UUID(root[f'{prefix}_volume_uuid']), host / 'keys',
               tree / f'build/verified-root-{build_id}' / name)


if __name__ == '__main__':
    main()
