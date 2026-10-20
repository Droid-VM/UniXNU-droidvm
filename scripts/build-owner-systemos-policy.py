#!/usr/bin/env python3
"""Sign and fully verify the original SystemOS cryptex image (the counterpart of build-owner-os-policy.py).

UniXNU's build-verified-root-probe.py --include-systemos reads build/verified-root-systemos/, but no script
of the public repositories writes it. The Cryptex1,SystemOS member is already a raw APFS container
(UDRW, no partition map, 3303014400 bytes), so it is hashed as it is, 4 KiB per block, signed with the
same owner key as the OS policy, and every block is checked again by the real C verifier.

Inputs: .cache/firmware/target-volumes/044-76034-038.dmg (the unchanged IPSW member) and the owner keys and
host library of kits/VerifiedRoot/tests/check.py in build/verified-root-host. The volume UUID is the
cryptex's own, read from the attached image, and must be the one the owner SystemOS mount looks for.
"""
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
source = ROOT / '.cache/firmware/target-volumes/044-76034-038.dmg'
out = ROOT / 'build/verified-root-systemos'
expected_uuid = uuid.UUID('8846FE78-2910-4297-AF97-D59BB6F8FC16')
expected_sha256 = 'b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f'
expected_bytes = 3303014400
if not source.is_file() or source.stat().st_size != expected_bytes:
    raise SystemExit('Require the exact original SystemOS member')
digest = hashlib.sha256()
with source.open('rb') as stream:
    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
        digest.update(chunk)
if digest.hexdigest() != expected_sha256:
    raise SystemExit('The SystemOS image must be the exact supplied iPad7,1 cryptex member')

# The volume identity comes from the image itself: attach it read-only, without mounting.
attached = plistlib.loads(subprocess.check_output(['hdiutil', 'attach', '-plist', '-nomount', '-readonly',
                                                   '-noverify', '-noautofsck', str(source)]))
devices = [e['dev-entry'] for e in attached['system-entities']]
try:
    found = set()
    for device in devices:
        info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', device]))
        if info.get('VolumeUUID'):
            found.add(info['VolumeUUID'])
finally:
    subprocess.run(['hdiutil', 'detach', min(devices, key=len)], check=True, capture_output=True)
if str(expected_uuid).upper() not in found:
    raise SystemExit(f'The cryptex volume UUID is not {expected_uuid}: {sorted(found)}')

spec = importlib.util.spec_from_file_location('verified_manifest', ROOT / 'kits/VerifiedRoot/tools/manifest.py')
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)
keys = ROOT / 'build/verified-root-host/keys'
with source.open('rb', buffering=1024 * 1024) as stream:
    record = manifest.make(stream, expected_bytes, keys / 'owner.pem', keys / 'owner-public.pem', expected_uuid, 1, out)


class View(ctypes.Structure):
    _fields_ = [('hashes', ctypes.c_void_p), ('image_bytes', ctypes.c_uint64), ('block_count', ctypes.c_uint64),
                ('generation', ctypes.c_uint64), ('volume_uuid', ctypes.c_uint8 * 16), ('image_sha256', ctypes.c_uint8 * 32)]


native = ctypes.CDLL(str(ROOT / 'build/verified-root-host/verified-root.dylib'))
native.vr_host_open.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(View)]
native.vr_host_block.argtypes = [ctypes.POINTER(View), ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
encoded = ctypes.create_string_buffer((out / 'manifest.bin').read_bytes())
anchor = ctypes.create_string_buffer((out / 'trusted-key.bin').read_bytes())
view = View()
if native.vr_host_open(encoded, len(encoded) - 1, anchor, 1, ctypes.byref(view)) or bytes(view.volume_uuid) != expected_uuid.bytes:
    raise SystemExit('Real C signature/volume authentication failed')
complete = hashlib.sha256()
with source.open('rb', buffering=1024 * 1024) as stream:
    for index in range(view.block_count):
        block = stream.read(min(4096, view.image_bytes - index * 4096))
        if native.vr_host_block(ctypes.byref(view), index, block, len(block)) != 0:
            raise SystemExit('Actual C block verification failed at ' + str(index))
        complete.update(block)
if complete.digest() != bytes(view.image_sha256) or complete.hexdigest() != expected_sha256:
    raise SystemExit('Actual complete original SystemOS digest differs')
report = dict(image_bytes=view.image_bytes, blocks_verified=view.block_count, image_sha256=complete.hexdigest(),
              volume_uuid=str(expected_uuid).upper(), generation=view.generation, source_format='UDRW raw APFS container',
              trusted_key_sha256=record['trusted_key_sha256'], real_c_signature_and_block_verification=True,
              original_image_modified=False, host_only=True, boot_root_authenticated=False,
              qemu_tested=False, hardware_tested=False)
(out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS: original SystemOS signature and every', view.block_count, 'block verified by real C/RSA')
