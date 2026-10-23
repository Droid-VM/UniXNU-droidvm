#!/usr/bin/env python3
"""Touch the running guest's screen from the host: write the finger set NativeHidInput's digitizer reads every 8 ms
(native_touch_seq/count/ids/xs/ys, a seqlock: odd while it is being written) through crosvm's /proc/<pid>/mem.

  touch.py tap X Y                       a tap at screen pixel X,Y of the framebuffer (handoff.env)
  touch.py swipe X0 Y0 X1 Y1 [STEPS]     press, move in STEPS, lift
  touch.py down X Y | move X Y | up      single steps

The digitizer axes are 30480 (long) by 20320 (short); --raw takes coordinates on those axes instead of pixels."""
import argparse
import struct
import sys
import time

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import peek
from export import physical, read_guest, write_guest

AXIS_X, AXIS_Y = 30480, 20320
SCREEN = {'FB_WIDTH': 1280, 'FB_HEIGHT': 800}
try:   # written by reboot-vm.sh from the last DarwinBoot hand-off
    for line in open(__import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'out', 'handoff.env')):
        key, _, value = line.strip().partition('=')
        if key in SCREEN:
            SCREEN[key] = int(value)
except FileNotFoundError:
    pass


class Touch:
    def __init__(self, raw=False):
        self.pid, self.maps = peek.crosvm_maps()
        self.raw = raw
        self.at = {name: physical(f'native_touch_{name}') for name in ('seq', 'count', 'ids', 'xs', 'ys')}
        self.seq = struct.unpack('<I', read_guest(self.pid, self.maps, self.at['seq'], 4))[0] & ~1

    def scale(self, x, y):
        if self.raw:
            return x, y
        w, h = SCREEN['FB_WIDTH'], SCREEN['FB_HEIGHT']
        return min(AXIS_X - 1, x * AXIS_X // w), min(AXIS_Y - 1, y * AXIS_Y // h)

    def put(self, name, value):
        write_guest(self.pid, self.maps, self.at[name], struct.pack('<i', value))

    def tap(self, point, hold=0.05):
        # One adb shell for the whole tap: each write_guest is its own adb round trip (~0.1-0.3 s), so press and release as
        # separate calls held the finger down long enough for SpringBoard to take it as a long press (the icon context
        # menu instead of a launch). The press and the release are written by the phone itself, `hold` seconds apart.
        x, y = self.scale(*point)
        def put(name, value):
            data = struct.pack('<i', value)
            host = peek.host_address(self.maps, self.at[name], 4)
            escaped = ''.join(f'\\{byte:03o}' for byte in data)
            return f"printf '{escaped}' | dd of=/proc/{self.pid}/mem bs=1 seek={host} conv=notrunc 2>/dev/null"
        steps = [put('seq', self.seq + 1), put('ids', 1), put('xs', x), put('ys', y), put('count', 1),
                 put('seq', self.seq + 2), f'sleep {hold}',
                 put('seq', self.seq + 3), put('count', 0), put('seq', self.seq + 4)]
        peek.adb('; '.join(steps))
        self.seq += 4

    def publish(self, point):
        # Odd while the fields change, then the next even number: the reader skips odd and repeated values.
        self.put('seq', self.seq + 1)
        if point:
            x, y = self.scale(*point)
            self.put('ids', 1)
            self.put('xs', x)
            self.put('ys', y)
            self.put('count', 1)
        else:
            self.put('count', 0)
        self.seq += 2
        self.put('seq', self.seq)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=['tap', 'swipe', 'down', 'move', 'up'])
    parser.add_argument('coords', nargs='*', type=int)
    parser.add_argument('--raw', action='store_true')
    args = parser.parse_args()
    touch = Touch(args.raw)
    c = args.coords
    if args.action == 'tap':
        touch.tap((c[0], c[1]))
    elif args.action == 'swipe':
        steps = c[4] if len(c) > 4 else 8
        for i in range(steps + 1):
            touch.publish((c[0] + (c[2] - c[0]) * i // steps, c[1] + (c[3] - c[1]) * i // steps))
        touch.publish(None)
    elif args.action in ('down', 'move'):
        touch.publish((c[0], c[1]))
    else:
        touch.publish(None)
    print(f'{args.action} done; seq {touch.seq}')


if __name__ == '__main__':
    main()
