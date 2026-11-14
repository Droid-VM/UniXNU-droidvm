#!/usr/bin/env python3
"""The [root] values of an OS profile from its IPSW's decrypted disk images: the raw disk's sha256 and its APFS volumes.

  root-image-identity.py --tree TREE --os OS.dmg --systemos SYSTEMOS.dmg

Decodes each UDIF image to TREE/build/droidvm-root-images/<os|systemos>.img with the record build-droidvm-root-policies.py keeps
beside it (so that step finds the raw disk written and pinned), and reads the APFS container in it -- a bare container or the APFS
partition of a GPT -- to its volumes: name, role and UUID, by the container's object map. Prints the [root] table for the OS
profile (os_volume_uuid: the system volume of the OS image; systemos_volume_uuid: the cryptex's volume) and the volumes found.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import uuid

APFS_PARTITION = uuid.UUID('7C3457EF-0000-11AA-AA11-00306543ECAC')
ROLES = {0: 'none', 1: 'System', 2: 'User', 4: 'Recovery', 8: 'VM', 0x10: 'Preboot', 0x20: 'Installer', 0x40: 'Data',
         0x80: 'Baseband', 0xC0: 'Update', 0x100: 'xART', 0x140: 'Hardware', 0x180: 'Backup', 0x1C0: 'Sidecar',
         0x200: 'Reserved', 0x240: 'Enterprise', 0x280: 'Reserved', 0x2C0: 'Prelogin'}


def raw_image(udif, dmg, raw):
    """The raw disk of a UDIF image, with build-droidvm-root-policies.py's record; returns (size, sha256)."""
    record = raw.with_suffix('.json')
    if raw.is_file() and record.is_file():
        known = json.loads(record.read_text())
        if known.get('source') == str(dmg) and known.get('size') == raw.stat().st_size:
            return known['size'], known['sha256']
    image = udif.Image(dmg)
    digest, size = hashlib.sha256(), 0
    partial = raw.with_suffix('.partial')
    with partial.open('wb') as target:
        for block in image.blocks():
            digest.update(block)
            target.write(block)
            size += len(block)
    if size != image.size:
        partial.unlink()
        raise SystemExit(f'{dmg.name}: decoded {size} bytes, the image says {image.size}')
    partial.rename(raw)
    record.write_text(json.dumps({'source': str(dmg), 'size': size, 'sha256': digest.hexdigest()}, indent=2) + '\n')
    return size, digest.hexdigest()


def container_offset(f):
    """Byte offset of the APFS container: 0 for a bare one, else its GPT partition's."""
    f.seek(0)
    if f.read(36)[32:36] == b'NXSB':
        return 0
    for sector in (512, 4096):
        f.seek(sector)
        header = f.read(92)
        if header[:8] != b'EFI PART':
            continue
        entries, count, size = struct.unpack_from('<QII', header, 72)
        f.seek(entries * sector)
        table = f.read(count * size)
        for i in range(count):
            entry = table[i * size:(i + 1) * size]
            if uuid.UUID(bytes_le=entry[:16]) == APFS_PARTITION:
                first, = struct.unpack_from('<Q', entry, 32)
                return first * sector
    raise SystemExit('no APFS container (bare or in a GPT)')


def volumes(path):
    with open(path, 'rb') as f:
        base = container_offset(f)
        f.seek(base)
        nx = f.read(4096)
        if nx[32:36] != b'NXSB':
            raise SystemExit(f'{path}: no container superblock')
        block, = struct.unpack_from('<I', nx, 36)

        def read(paddr):
            f.seek(base + paddr * block)
            return f.read(block)

        omap_oid, = struct.unpack_from('<Q', nx, 0xA0)
        maxfs, = struct.unpack_from('<I', nx, 0xB4)
        fs_oids = [o for o in struct.unpack_from(f'<{maxfs}Q', nx, 0xB8) if o]
        tree_oid, = struct.unpack_from('<Q', read(omap_oid), 0x30)
        mapping = {}

        def walk(paddr):
            node = read(paddr)
            flags, level, nkeys = struct.unpack_from('<HHI', node, 0x20)
            toff, tlen = struct.unpack_from('<HH', node, 0x28)
            keys = 0x38 + toff + tlen
            vend = block - (0x28 if flags & 1 else 0)   # a root node ends with the tree's info
            for i in range(nkeys):
                koff, voff = struct.unpack_from('<HH', node, 0x38 + toff + 4 * i)   # fixed-size entries: kvoff_t
                oid, xid = struct.unpack_from('<QQ', node, keys + koff)
                if level:
                    child, = struct.unpack_from('<Q', node, vend - voff)
                    walk(child)
                else:
                    _, _, physical = struct.unpack_from('<IIQ', node, vend - voff)
                    if oid not in mapping or mapping[oid][0] < xid:
                        mapping[oid] = (xid, physical)

        walk(tree_oid)
        found = []
        for oid in fs_oids:
            sb = read(mapping[oid][1])
            if sb[32:36] != b'APSB':
                raise SystemExit(f'{path}: volume {oid} is no APFS volume')
            role, = struct.unpack_from('<H', sb, 0x3C4)
            name = sb[0x2C0:0x3C0].split(b'\0')[0].decode()
            found.append({'name': name, 'role': ROLES.get(role, hex(role)), 'uuid': str(uuid.UUID(bytes=sb[0xF0:0x100])).upper()})
        return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--os', type=Path, required=True)
    parser.add_argument('--systemos', type=Path, required=True)
    args = parser.parse_args()
    tree = args.tree.resolve()
    sys.path.insert(0, str(tree / 'scripts'))
    from unixnu import udif
    images = tree / 'build/droidvm-root-images'
    images.mkdir(parents=True, exist_ok=True)
    table, report = {}, {}
    for prefix, dmg in (('os', args.os), ('systemos', args.systemos)):
        raw = images / f'{prefix}.img'
        size, digest = raw_image(udif, dmg.resolve(), raw)
        found = volumes(raw)
        report[prefix] = {'image': str(raw), 'size': size, 'sha256': digest, 'volumes': found}
        system = [v for v in found if v['role'] == 'System'] or found
        table[f'{prefix}_volume_uuid'] = system[0]['uuid']
        table[f'{prefix}_image_sha256'] = digest
    print(json.dumps(report, indent=2))
    print('\n["root"]')
    for key, value in table.items():
        print(f'"{key}" = "{value}"')


if __name__ == '__main__':
    main()
