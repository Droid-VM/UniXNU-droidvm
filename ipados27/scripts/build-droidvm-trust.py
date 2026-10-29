#!/usr/bin/env python3
"""The trust caches of the iPadOS 27 board: the IPSW's own static, SystemOS and App trust modules, unchanged, in one bundle.

  build-droidvm-trust.py --tree TREE --ipsw IPSW_DIR [--owner TRUST.json ...] [--name NAME]

The OS volume's binaries (launchd, dyld, ...) are ad hoc signed and run only as platform code listed in a trust cache. The IPSW
carries them as IM4P members (StaticTrustCache, Cryptex1,SystemTrustCache, Cryptex1,AppTrustCache); `unixnu ipsw fetch` records
each beside it (<member>.json: component and sha256). Each payload is a version-2 trust module, taken byte for byte, and the
bundle is the layout DarwinBoot hands the kernel as /chosen/memory-map TrustCache (as fork scripts/build-original-system-trust.py
and make-owner-trust-bundle.py write it): a count, the modules' offsets, the modules. `unixnu build payload --trust-cache` takes it.
Writes TREE/build/droidvm-trust-<build>/original.bundle and inputs.json.

--owner adds one owner module before them with the CDHashes an owner build records (build-adreno-metal-plugin.py's trust.json:
cdhash, daemon_cdhash, turnip_cdhash; or a "cdhashes" list with an optional "category"), as make-owner-trust-bundle.py does for
iPadOS 17; several records may share a CDHash (an old and a new plugin build kept trusted together). The bundle is then
NAME.bundle (default owner). The original modules stay byte for byte.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import uuid

COMPONENTS = ('StaticTrustCache', 'Cryptex1,SystemTrustCache', 'Cryptex1,AppTrustCache')


def der(data, at):
    tag, size = data[at:at + 2]
    at += 2
    if size & 0x80:
        count = size & 0x7f
        size = int.from_bytes(data[at:at + count], 'big')
        at += count
    if at + size > len(data):
        raise ValueError('DER length past the input')
    return tag, data[at:at + size], at + size


def module(path):
    """The version-2 trust module inside an IM4P (its fourth field)."""
    data = path.read_bytes()
    tag, sequence, end = der(data, 0)
    fields, cursor = [], 0
    while cursor < len(sequence):
        kind, value, cursor = der(sequence, cursor)
        fields.append((kind, value))
    if tag != 0x30 or end != len(data) or [kind for kind, _ in fields][:4] != [0x16, 0x16, 0x16, 4] or fields[0][1] != b'IM4P':
        raise SystemExit(f'{path.name}: not an IM4P trust cache')
    payload = fields[3][1]
    version, = struct.unpack_from('<I', payload)
    count, = struct.unpack_from('<I', payload, 20)
    if version != 2 or len(payload) != 24 + count * 24:
        raise SystemExit(f'{path.name}: not a complete version-2 trust module')
    return payload, count


def owner_module(paths):
    """One version-2 module of the owner CDHashes, ascending (the boot loader's and the kernel's binary search need the order)."""
    hashes, categories = [], {}
    for path in paths:
        record = json.loads(path.read_text())
        listed = [record[key] for key in ('cdhash', 'daemon_cdhash', 'turnip_cdhash') if key in record] + record.get('cdhashes', [])
        for value in listed:
            digest = bytes.fromhex(value)
            if len(digest) != 20:
                raise SystemExit(f'{path}: {value} is no 20-byte CDHash')
            # Two builds kept trusted side by side (a plugin redeployed into a running Data) share what did not change.
            if categories.setdefault(digest, record.get('category', 0)) != record.get('category', 0):
                raise SystemExit(f'{path}: {value} listed with two categories')
            if digest not in hashes:
                hashes.append(digest)
    hashes.sort()
    identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'org.blu-sharky.droidvm-owner:' + ''.join(h.hex() for h in hashes))
    return struct.pack('<I16sI', 2, identifier.bytes, len(hashes)) + b''.join(h + bytes([2, 0, categories[h], 0]) for h in hashes), hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--ipsw', type=Path, required=True, help='the IPSW cache of the build (out/ipsw/<build>)')
    parser.add_argument('--owner', type=Path, action='append', default=[], help="an owner build's trust record")
    parser.add_argument('--name', default='owner', help='the bundle name with --owner (NAME.bundle)')
    args = parser.parse_args()
    records = {}
    for path in sorted(args.ipsw.glob('*.trustcache.json')):
        info = json.loads(path.read_text())
        records[info.get('component')] = (path.with_name(path.name[:-len('.json')]), info)
    modules, inputs = [], {}
    for component in COMPONENTS:
        if component not in records:
            raise SystemExit(f'{args.ipsw}: no {component} member (unixnu ipsw fetch)')
        path, info = records[component]
        if hashlib.sha256(path.read_bytes()).hexdigest() != info['sha256']:
            raise SystemExit(f'{path.name}: differs from the digest the IPSW fetch recorded')
        payload, count = module(path)
        modules.append(payload)
        inputs[component] = dict(member=path.name, im4p_sha256=info['sha256'], entries=count,
                                 module_sha256=hashlib.sha256(payload).hexdigest())
    owner = []
    if args.owner:
        module_bytes, hashes = owner_module(args.owner)
        modules.insert(0, module_bytes)
        owner = [h.hex() for h in hashes]
    starts, at = [], 4 * (1 + len(modules))
    for payload in modules:
        starts.append(at)
        at += len(payload)
    bundle = struct.pack(f'<{1 + len(modules)}I', len(modules), *starts) + b''.join(modules)
    build_id = args.ipsw.resolve().name
    out = args.tree / f'build/droidvm-trust-{build_id}'
    out.mkdir(parents=True, exist_ok=True)
    name = args.name if args.owner else 'original'
    (out / f'{name}.bundle').write_bytes(bundle)
    (out / f'{name}.json' if args.owner else out / 'inputs.json').write_text(json.dumps(dict(
        modules=inputs, owner_cdhashes=owner, owner_records=[str(p) for p in args.owner],
        bundle_sha256=hashlib.sha256(bundle).hexdigest(), original_modules_modified=False), indent=2) + '\n')
    print(f'{out / f"{name}.bundle"}: {len(owner)} owner and {len(inputs)} unchanged modules, '
          f'{sum(m["entries"] for m in inputs.values())} original entries, sha256 {hashlib.sha256(bundle).hexdigest()}')


if __name__ == '__main__':
    main()
