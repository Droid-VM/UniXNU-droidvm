#!/usr/bin/env python3
"""Copy the guest's unified log into a .logarchive the Mac's `log show --archive` reads.

  logarchive.py OUT.logarchive [--sizes]

Mirrors /private/var/db/diagnostics (Persist, Special, Signpost, HighVolume, timesync, logdata.statistics) and
/private/var/db/uuidtext (format strings of every binary, dsc/ for the shared cache) through the kernel's file mailbox,
then writes the Info.plist an archive needs. Each tree is listed with its file sizes in one request (kind 6), and files
already copied with the right size are skipped, so a second run only fetches what logd wrote since. --sizes only prints
how much there is."""
import argparse
import plistlib
import sys
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
    plistlib.dump({'OSArchiveVersion': 4, 'ArchiveIdentifier': 'droidvm-unixnu'}, open(args.archive / 'Info.plist', 'wb'))
    print(f'{args.archive} ready')


if __name__ == '__main__':
    main()
