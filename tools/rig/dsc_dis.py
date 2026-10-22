#!/usr/bin/env python3
"""Disassemble code of the iPadOS dyld shared cache, with branch targets named (runs where the cache is, the Mac).

  dsc_dis.py CACHE_DIR SYMBOL [IMAGE] [--around OFFSET] [--count N]
      SYMBOL as the cache names it: an exact name ('-[CIContext initWithOptions:]', '_MTLCreateSystemDefaultDevice'), or a
      part of one (a C++ name is mangled: 'GLContextC2'); IMAGE limits the search to images whose path contains it. --around
      shows N instructions (default 40) centred on SYMBOL+OFFSET, the "symbol + offset" a crash report or trace.py prints.
  dsc_dis.py CACHE_DIR 0xADDRESS [--count N]    an unslid address

CACHE_DIR as for dsc_sym.py. The bytes come from the cache files and llvm-mc (Homebrew's llvm) disassembles them; each bl
and b to a known address is followed by the name dsc_sym.py gives it."""
import argparse
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsc_sym


def find(cache, wanted, image_filter):
    exact, partial = [], []
    for header, path in cache.images:
        if image_filter and image_filter not in path:
            continue
        try:
            _, _, symbols = cache.image(header)
        except (TypeError, struct.error):   # load commands that run past the end of a mapping
            continue
        for value, name in symbols:
            if name == wanted:
                exact.append((value, name, path))
            elif wanted in name and len(partial) < 20:
                partial.append((value, name, path))
    return exact or partial


def llvm_mc():
    prefix = subprocess.run(['brew', '--prefix', 'llvm'], capture_output=True, text=True).stdout.strip()
    return os.path.join(prefix, 'bin', 'llvm-mc') if prefix else 'llvm-mc'


def disassemble(cache, start, count):
    data = cache.read(start, count * 4)
    if not data:
        sys.exit(f'{start:#x}: not in the cache')
    words = ' '.join(f'0x{b:02x}' for b in data)
    out = subprocess.run([llvm_mc(), '--disassemble', '-triple=arm64e-apple-ios'], input=words, capture_output=True,
                         text=True).stdout
    lines = [line.strip() for line in out.splitlines() if line.strip() and not line.strip().startswith('.')]
    for index, text in enumerate(lines[:count]):
        address = start + index * 4
        note = ''
        found = re.match(r'(b|bl|b\.\w+|cbz|cbnz|tbz|tbnz)\s+.*?#(-?0x[0-9a-f]+|-?\d+)\s*$', text)
        if found:
            target = address + int(found[2], 0)
            # An image whose load commands the reader cannot map (a branch into another cache file's text) is named by
            # address alone rather than ending the listing.
            try:
                name = cache.symbolize(target)
            except (TypeError, ValueError, struct.error):
                name = '(not named)'
            note = f'    -> {target:#x} {name}'
        print(f'{address:#x}  {text}{note}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('cache')
    parser.add_argument('symbol')
    parser.add_argument('image', nargs='?')
    parser.add_argument('--around', type=lambda v: int(v, 0))
    parser.add_argument('--count', type=int, default=40)
    args = parser.parse_args()
    cache = dsc_sym.Cache(args.cache)
    if args.symbol.startswith('0x'):
        disassemble(cache, int(args.symbol, 16), args.count)
        return
    found = find(cache, args.symbol, args.image)
    if not found:
        sys.exit(f'{args.symbol}: no such symbol')
    if len(found) > 1 and found[0][1] != args.symbol:
        for value, name, path in found:
            print(f'{value:#x}  {name}  ({path.rsplit("/", 1)[-1]})')
        sys.exit('several symbols match; name one exactly, or add IMAGE')
    value, name, path = found[0]
    print(f'{name}  ({path})')
    start = value + args.around - args.count // 2 * 4 if args.around is not None else value
    disassemble(cache, start, args.count)


if __name__ == '__main__':
    main()
