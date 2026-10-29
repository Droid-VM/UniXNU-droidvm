#!/usr/bin/env python3
"""Reviewed pairings of the iPadOS 27 board's fileset prelink, over the matcher's own.

XnuPrelink pairs the original kernel's functions with this kernel's by shape and names; what it cannot pair it binds to a stub
that panics when called ("fileset: original component called an unmapped kernel function"). The small accessors it misses are
named here (ipados27/fileset-reviewed.json: original address of the 24A446 kernel, native symbol, evidence) and written into the
saved pairing (out/.../kernel/kernel.map.json), which the next prelink reuses while the native kernel is unchanged:

    unixnu build kernel -D <repo>/ipados27        (links, matches, emits)
    apply-fileset-reviewed.py --tree <tree> --map <out>/kernel/kernel.map.json --reviewed <repo>/ipados27/fileset-reviewed.json
    unixnu build prelink -D <repo>/ipados27       (emits again with these pairings)

Upstream's own reviewed identities (kits/XnuPrelink/fixtures) cover what sheng's kernel needed; these are the ones this kernel's
boot reached unpaired, and (under "data") the kernel data the kexts reference that it left unpaired.
"""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--map', type=Path, required=True)
    parser.add_argument('--reviewed', type=Path, required=True)
    args = parser.parse_args()
    native = args.tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
    names = {}
    nm = next((p for p in ('/opt/homebrew/opt/llvm/bin/llvm-nm', 'llvm-nm') if Path(p).is_file() or p == 'llvm-nm'))
    for line in subprocess.check_output([nm, '--defined-only', str(native)], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            names.setdefault(parts[2], int(parts[0], 16))
    pairing = json.loads(args.map.read_text())
    reviewed = json.loads(args.reviewed.read_text())
    if reviewed['kernelcache_sha256'] != pairing['inputs']['kernelcache_sha256']:
        raise SystemExit('the reviewed pairings are for another kernelcache')
    taken = {entry['native']: original for original, entry in pairing['functions'].items()}
    for record in reviewed['functions'] + reviewed.get('data', []):
        if not record.get('evidence'):
            raise SystemExit(f'{record["original"]}: a reviewed pairing needs its evidence')
        if record['native_symbol'] not in names:
            raise SystemExit(f'{record["native_symbol"]}: not in this kernel')
    for record in reviewed['functions']:
        address = hex(names[record['native_symbol']])
        other = taken.get(address)
        if other and other != record['original']:
            print(f'note: {record["native_symbol"]} was also paired with {other}')
        pairing['functions'][record['original']] = {'native': address, 'name': record['native_symbol'], 'addend': 0,
                                                     'how': 'reviewed'}
    # Data the kexts reference that the matcher left unpaired: the emitter keeps such a reference at the original address plus the
    # collection slide, which lands in some native object (kernel.json "missing"), so each is named here too.
    for record in reviewed.get('data', []):
        pairing['data'][record['original']] = {'native': hex(names[record['native_symbol']]), 'name': record['native_symbol'],
                                                'how': 'reviewed', 'sections': record.get('sections', ['?', '?'])}
    args.map.write_text(json.dumps(pairing, indent=2) + '\n')
    print(f'{len(reviewed["functions"])} reviewed function and {len(reviewed.get("data", []))} data pairings in {args.map}')


if __name__ == '__main__':
    main()
