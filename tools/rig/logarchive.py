#!/usr/bin/env python3
"""Copy the guest's unified log into a .logarchive the Mac's `log show --archive` reads.

  logarchive.py OUT.logarchive [--sizes]

Mirrors /private/var/db/diagnostics (Persist, Special, Signpost, HighVolume, timesync, logdata.statistics) and
/private/var/db/uuidtext (format strings of every binary, dsc/ for the shared cache) through the kernel's file mailbox,
then writes the Info.plist an archive needs. Each tree is listed with its file sizes in one request (kind 6), and files
already copied with the right size are skipped, so a second run only fetches what logd wrote since. --sizes only prints
how much there is.

The Info.plist carries the time references `log collect` records (each store's oldest and the archive's end, as boot UUID,
continuous time and wall time), taken from the archive's timesync records: macOS 27's `log show` reads nothing from an
archive without them ("partial or missing metadata")."""
import argparse
import plistlib
import struct
import sys
import uuid
from pathlib import Path

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from export import request

SOURCES = ('/private/var/db/diagnostics', '/private/var/db/uuidtext')
DT_REG = '8'


def tree(path):
    """{path below `path`: size} of every regular file in the tree; a full answer is continued from where it stopped."""
    files, first = {}, 0
    while True:
        body, listed = request(6, path, first)
        if not listed:
            return files
        for line in body.decode(errors='replace').splitlines():
            kind, size, name = line.split(' ', 2)
            if kind == DT_REG:
                files[name] = int(size)
        first += listed


def fetch(path, out, total):
    if out.exists() and out.stat().st_size == total:
        return 0
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, 'wb') as f:
        offset = 0
        while offset < total:
            body, _ = request(0, path, offset)
            if not body:
                break
            f.write(body)
            offset += len(body)
    return offset


def time_references(archive):
    """(oldest, end) time references of the archive's timesync files: a boot record (0xbbb0: boot UUID, timebase, boot wall time
    in ns) and its sync records ('Ts': continuous time in ticks, wall time in ns), in the order logd wrote them."""
    references = []
    for path in sorted((archive / 'timesync').glob('*.timesync')):
        data, at, boot = path.read_bytes(), 0, None
        while at + 4 <= len(data):
            if struct.unpack_from('<H', data, at)[0] == 0xbbb0:
                size = struct.unpack_from('<H', data, at + 2)[0]
                boot = str(uuid.UUID(bytes=data[at + 8:at + 24])).upper()
                references.append({'UUID': boot, 'ContinuousTime': 0, 'WallTime': struct.unpack_from('<q', data, at + 32)[0]})
                at += size
            elif data[at:at + 4] == b'Ts \0' and boot:
                ticks, wall = struct.unpack_from('<Qq', data, at + 8)
                references.append({'UUID': boot, 'ContinuousTime': ticks, 'WallTime': wall})
                at += 32
            else:
                break
    if not references:
        raise SystemExit(f'{archive}: no timesync records')
    return references[0], references[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--sizes', action='store_true')
    args = parser.parse_args()
    grand = 0
    for source in SOURCES:
        files = tree(source)
        if args.sizes:
            print(f'{source}: {len(files)} files, {sum(files.values()) / 1e6:.1f} MB')
            grand += sum(files.values())
            continue
        for relative, total in sorted(files.items()):
            got = fetch(f'{source}/{relative}', args.archive / relative, total)
            if got:
                print(f'{relative}: {got} bytes', flush=True)
    if args.sizes:
        print(f'total {grand / 1e6:.1f} MB')
        return
    oldest, end = time_references(args.archive)
    info = {'OSArchiveVersion': 4, 'ArchiveIdentifier': str(uuid.uuid5(uuid.NAMESPACE_URL, 'droidvm-unixnu')).upper(),
            'EndTimeRef': end}
    for store in ('Persist', 'Special', 'Signpost', 'HighVolume', 'Live', 'TTL'):
        info[store + 'Metadata'] = {'OldestTimeRef': oldest}
    plistlib.dump(info, open(args.archive / 'Info.plist', 'wb'))
    print(f'{args.archive} ready')


if __name__ == '__main__':
    main()
