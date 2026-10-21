#!/usr/bin/env python3
"""Copy the guest's unified log into a .logarchive the Mac's `log show --archive` reads.

  logarchive.py OUT.logarchive [--sizes]

Mirrors /private/var/db/diagnostics (Persist, Special, Signpost, HighVolume, timesync, logdata.statistics) and
/private/var/db/uuidtext (format strings of every binary, dsc/ for the shared cache) through the kernel's file mailbox,
then writes the Info.plist an archive needs. Files already copied with the right size are skipped, so a second run
only fetches what logd wrote since. --sizes only prints how much there is."""
import argparse
import plistlib
import sys
from pathlib import Path

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from export import request

SOURCES = ('/private/var/db/diagnostics', '/private/var/db/uuidtext')


def entries(path):
    body, _ = request(1, path)
    for line in body.decode(errors='replace').splitlines():
        kind, _, name = line.partition(' ')
        if name not in ('.', '..'):
            yield int(kind), name


def size(path):
    _, total = request(0, path, 1 << 40)   # past the end: no bytes, just the size
    return total


def walk(path):
    for kind, name in entries(path):
        child = f'{path}/{name}'
        if kind == 4:
            yield from walk(child)
        elif kind == 8:
            yield child


def fetch(path, out):
    total = size(path)
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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--sizes', action='store_true')
    args = parser.parse_args()
    grand = 0
    for source in SOURCES:
        files = list(walk(source))
        if args.sizes:
            sizes = [size(f) for f in files]
            print(f'{source}: {len(files)} files, {sum(sizes) / 1e6:.1f} MB')
            grand += sum(sizes)
            continue
        for path in files:
            relative = path[len(source) + 1:]
            got = fetch(path, args.archive / relative)
            if got:
                print(f'{relative}: {got} bytes', flush=True)
    if args.sizes:
        print(f'total {grand / 1e6:.1f} MB')
        return
    plistlib.dump({'OSArchiveVersion': 4, 'ArchiveIdentifier': 'droidvm-unixnu'}, open(args.archive / 'Info.plist', 'wb'))
    print(f'{args.archive} ready')


if __name__ == '__main__':
    main()
