#!/usr/bin/env python3
"""Print the traced UI roles' thread rows the droidvm-monitor worker publishes (droidvm_trace_rows, BootUiTraceProbe.c).

  trace.py                         every traced role's rows
  trace.py --focus NAME            trace process NAME (e.g. Preferences, MobileSafari) as role T from the next pass on, wait
                                   for two passes, and print its rows; --focus '' gives role T back to the boot arguments
  trace.py --role R                only role R's rows (B, S, T, U)
  trace.py --symbolize             name the user addresses: dsc_sym.py on the build Mac (BUILD_HOST) against the dyld shared
                                   cache of the SystemOS cryptex mounted there; an app's own code stays an address

STK<role> rows: the main thread (pid, shared cache slide, image header, pc/lr, the pid its Mach message goes to), its
registers and frames, then one row per thread group: #index xcount p<peer pid> pc lr frames. HOT rows: the busiest
threads. Roles: B = backboardd, S = SpringBoard, T/U = boot-ui-focus3/4; --focus sets T while the guest runs
(boot_ui_trace_focus, BootProcessProbe.c)."""
import argparse
import os
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rigenv  # noqa: F401
import export
import peek

ROWS, ROW = 96, 120
# On BUILD_HOST: the dyld cache of the mounted SystemOS cryptex (a volume named ...SystemCryptex), or DSC_CACHE.
CACHE_ON_HOST = ('C=${DSC_CACHE:-$(mount | sed -n "s/^.* on \\(.*SystemCryptex\\) (.*$/\\1/p" | head -1)'
                 '/System/Library/Caches/com.apple.dyld}')


def passes():
    _, data = peek.read('droidvm_monitor_passes', 4)
    return struct.unpack('<I', data)[0]


def focus(name):
    pid, maps = peek.crosvm_maps()
    export.write_guest(pid, maps, export.physical('boot_ui_trace_focus'), name.encode()[:31].ljust(32, b'\0'))
    start = passes()
    for _ in range(120):
        time.sleep(0.5)
        if passes() >= start + 2:
            return
    sys.exit('the monitor did not take two passes in a minute (droidvm-monitor=<ms>?)')


def rows():
    _, count = peek.read('droidvm_trace_count', 4)
    count = min(struct.unpack('<I', count)[0], ROWS)
    _, data = peek.read('droidvm_trace_rows', ROWS * ROW)
    return [data[i * ROW:(i + 1) * ROW].split(b'\0')[0].decode(errors='replace') for i in range(count)]


def symbolize(lines):
    """Each STK row's user addresses, named by dsc_sym.py with the slide of its role's header row."""
    slides, wanted = {}, {}
    for line in lines:
        role = re.match(r'STK(\w)', line)
        if not role:
            continue
        found = re.search(r'cache\+([0-9a-f]+)', line)
        if found:
            slides[role[1]] = found[1]
        elif role[1] in slides:
            for address in re.findall(r'\b(1[0-9a-f]{8,9})\b', line):
                wanted.setdefault(role[1], set()).add(address)
    names = {}
    script = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dsc_sym.py')).read()
    for role, addresses in wanted.items():
        command = (f'{CACHE_ON_HOST}; [ -d "$C" ] || {{ echo "no SystemOS cryptex mounted (or DSC_CACHE)" >&2; exit 1; }}; '
                   f'python3 - "$C" {slides[role]} {" ".join(sorted(addresses))}')
        out = subprocess.run(['ssh', os.environ['BUILD_HOST'], command], input=script, capture_output=True, text=True, timeout=120)
        if out.returncode:
            print(out.stderr.strip(), file=sys.stderr)
        for line in out.stdout.splitlines():
            address, _, name = line.strip().partition(' ')
            names[address.lower().removeprefix('0x').lstrip('0')] = name.strip()
    for line in lines:
        print(line)
        if re.match(r'STK\w( \d\d|#\d)', line):
            for address in re.findall(r'\b(1[0-9a-f]{8,9})\b', line):
                if address.lstrip('0') in names:
                    print(f'      {address}  {names[address.lstrip("0")]}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--focus')
    parser.add_argument('--role')
    parser.add_argument('--symbolize', action='store_true')
    args = parser.parse_args()
    if args.focus is not None:
        focus(args.focus)
        args.role = args.role or ('T' if args.focus else None)
    lines = rows()
    if args.role:
        lines = [line for line in lines if line.startswith(f'STK{args.role}')]
        if args.focus and not lines:
            print(f'{args.focus}: not running (no role T rows)')
    if args.symbolize:
        symbolize(lines)
    else:
        print('\n'.join(lines))


if __name__ == '__main__':
    main()
