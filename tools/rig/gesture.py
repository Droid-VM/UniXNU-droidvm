#!/usr/bin/env python3
"""Two-finger gestures on the guest's screen, through a virtual multi-touch screen on the phone (mt.c), for what adb's input cannot
make (it has one finger): the app takes them as any touch on its display.

  gesture.py scroll GX GY DY [STEPS]   two fingers 300 guest pixels apart around GX,GY, moved DY guest pixels (negative: up)
  gesture.py tap2 GX GY                two fingers down and up within 60 ms (macOS: a right click)

Guest pixels go to the phone's as input.py's do (the VM surface's layer). /data/local/tmp/mt is built (aarch64-linux-gnu-gcc
-static) and pushed when the phone has none; it runs as root (/dev/uinput)."""
import os
import subprocess
import sys

RIG = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, RIG)
import rigenv  # noqa: E402,F401  (fills os.environ from rig.env)
import input as rig_input  # noqa: E402

PHONE = os.environ['PHONE']
MT = '/data/local/tmp/mt'


def adb(*args, **kwargs):
    return subprocess.run(['adb', '-s', PHONE, *args], **kwargs)


def ensure_mt():
    if adb('shell', f'test -x {MT} && echo yes', capture_output=True, text=True).stdout.strip() == 'yes':
        return
    binary = os.path.join(RIG, 'out', 'mt')
    os.makedirs(os.path.dirname(binary), exist_ok=True)
    subprocess.run(['aarch64-linux-gnu-gcc', '-static', '-O2', '-o', binary, os.path.join(RIG, 'mt.c')], check=True)
    adb('push', binary, MT, check=True, capture_output=True)
    adb('shell', f'chmod 755 {MT}', check=True)


def run(lines):
    ensure_mt()
    size = adb('shell', 'wm', 'size', capture_output=True, text=True).stdout
    width, height = size.strip().splitlines()[-1].split(':')[-1].strip().split('x')
    adb('shell', f'su -c "{MT} {width} {height}"', input='\n'.join(lines) + '\n', text=True, check=True)


def fingers(gx, gy):
    a, b = rig_input.phone(gx - 150, gy), rig_input.phone(gx + 150, gy)
    return a, b


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ('scroll', 'tap2'):
        sys.exit(__doc__)
    kind, gx, gy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    if kind == 'scroll':
        if len(sys.argv) < 5:
            sys.exit(__doc__)
        dy, steps = int(sys.argv[4]), int(sys.argv[5]) if len(sys.argv) > 5 else 20
        a, b = fingers(gx, gy)
        lines = [f'd 0 {a[0]} {a[1]}', f'd 1 {b[0]} {b[1]}', 'w 30']
        for i in range(1, steps + 1):
            a, b = fingers(gx, gy + dy * i // steps)
            lines += [f'm 0 {a[0]} {a[1]}', f'm 1 {b[0]} {b[1]}', 'w 16']
        lines += ['u 0', 'u 1']
    else:
        a, b = fingers(gx, gy)
        lines = [f'd 0 {a[0]} {a[1]}', f'd 1 {b[0]} {b[1]}', 'w 60', 'u 0', 'u 1']
    run(lines)


if __name__ == '__main__':
    main()
