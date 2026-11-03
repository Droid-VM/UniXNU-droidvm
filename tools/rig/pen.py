#!/usr/bin/env python3
"""Draw with a synthetic pen on the guest: evdev records for the screen's pen device (crosvm --input stylus, the app's PEN
channel), sent through the DroidVM daemon's vm_input as the app's console would send them.

  pen.py stroke X0 Y0 X1 Y1 [options]   approach (hover), touch down, a straight stroke, lift, leave
  pen.py hover X Y [options]            in range above (X, Y) for a moment, no contact
  pen.py tap X Y [options]
Positions are fractions (0..1) of the guest's panel, its own portrait frame (x right, y down).
  --pressure P0[:P1]   0..1 at the start and the end of the stroke (default 0.5)
  --tilt TX:TY         degrees from the normal towards +x and +y (default 0:0)
  --steps N            moves along the stroke (default 30), --ms M between them (default 8)
  --eraser             the eraser end; --barrel holds the barrel button
  --distance D         hover distance 1..255 (default 40)
  --screen ID          the screen whose pen it is (default gpu-0)
"""
import argparse
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import time
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rigenv  # noqa: E402,F401  (fills os.environ from rig.env)

PHONE, VM = os.environ['PHONE'], os.environ['VM']
RUN = '/data/data/cn.classfun.droidvm/run'
PEN = 5   # NativeDisplay.PEN
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
ABS_X, ABS_Y, ABS_PRESSURE, ABS_DISTANCE, ABS_TILT_X, ABS_TILT_Y = 0x00, 0x01, 0x18, 0x19, 0x1a, 0x1b
BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOUCH, BTN_STYLUS = 0x140, 0x141, 0x14a, 0x14b
MAX, PRESSURE_MAX = 0x7FFF, 4095   # crosvm's NORMALIZED_ABS_MAX and STYLUS_PRESSURE_MAX


def su(command):
    return subprocess.run(['adb', '-s', PHONE, 'shell', f"su -c '{command}'"], capture_output=True, text=True,
                          timeout=30).stdout.strip()


class Daemon:
    def __init__(self):
        port = int(su(f'cat {RUN}/droidvmd-port.txt'))
        token = su(f'cat {RUN}/droidvmd-token.txt')
        subprocess.run(['adb', '-s', PHONE, 'forward', f'tcp:{port}', f'tcp:{port}'], capture_output=True, check=True)
        self.sock = socket.create_connection(('127.0.0.1', port), 10)
        self.sock.settimeout(30)
        self.request('auth', token=token)

    def _recv(self):
        def exactly(n):
            data = b''
            while len(data) < n:
                chunk = self.sock.recv(n - len(data))
                if not chunk:
                    raise EOFError
                data += chunk
            return data
        data = exactly(struct.unpack('<I', exactly(4))[0])
        return json.loads(data[:-1] if data.endswith(b'\0') else data)

    def request(self, command, **kw):
        rid = str(uuid.uuid4())
        body = json.dumps(dict(type='request', request_id=rid, command=command, **kw)).encode()
        self.sock.sendall(struct.pack('<I', len(body)) + body)
        while True:
            message = self._recv()
            if message.get('type') == 'response' and message.get('request_id') == rid:
                return message


class Pen:
    def __init__(self, args):
        self.daemon, self.args, self.sent = Daemon(), args, 0
        self.tool = BTN_TOOL_RUBBER if args.eraser else BTN_TOOL_PEN
        self.tilt = [int(v) for v in args.tilt.split(':')]

    def frame(self, *events):
        records = b''.join(struct.pack('<HHi', t, c, v) for t, c, v in (*events, (EV_SYN, 0, 0)))
        reply = self.daemon.request('vm_input', vm_id=VM, screen=self.args.screen, channel=PEN,
                                    data=base64.b64encode(records).decode())
        if not (reply.get('data') or reply).get('delivered', False):
            sys.exit(f'vm_input not delivered: {json.dumps(reply)[:300]}')
        self.sent += 1

    def at(self, x, y, pressure=None, distance=None):
        events = [(EV_ABS, ABS_X, round(x * MAX)), (EV_ABS, ABS_Y, round(y * MAX)),
                  (EV_ABS, ABS_TILT_X, self.tilt[0]), (EV_ABS, ABS_TILT_Y, self.tilt[1])]
        if pressure is not None:
            events.append((EV_ABS, ABS_PRESSURE, round(max(0.0, min(1.0, pressure)) * PRESSURE_MAX)))
        if distance is not None:
            events.append((EV_ABS, ABS_DISTANCE, distance))
        return events

    def enter(self, x, y):
        self.frame((EV_KEY, self.tool, 1), *self.at(x, y, 0, self.args.distance),
                   *([(EV_KEY, BTN_STYLUS, 1)] if self.args.barrel else []))
        time.sleep(0.05)

    def leave(self):
        self.frame(*([(EV_KEY, BTN_STYLUS, 0)] if self.args.barrel else []), (EV_KEY, self.tool, 0))

    def stroke(self, x0, y0, x1, y1):
        p0, p1 = ([float(v) for v in self.args.pressure.split(':')] * 2)[:2]
        self.enter(x0, y0)
        self.frame((EV_KEY, BTN_TOUCH, 1), *self.at(x0, y0, p0, 0))
        steps = max(1, self.args.steps)
        for i in range(1, steps + 1):
            f = i / steps
            time.sleep(self.args.ms / 1000)
            self.frame(*self.at(x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, p0 + (p1 - p0) * f, 0))
        self.frame((EV_KEY, BTN_TOUCH, 0), *self.at(x1, y1, 0, self.args.distance))
        time.sleep(0.05)
        self.leave()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('what', choices=('stroke', 'hover', 'tap'))
    parser.add_argument('coords', type=float, nargs='+')
    parser.add_argument('--pressure', default='0.5')
    parser.add_argument('--tilt', default='0:0')
    parser.add_argument('--steps', type=int, default=30)
    parser.add_argument('--ms', type=float, default=8)
    parser.add_argument('--eraser', action='store_true')
    parser.add_argument('--barrel', action='store_true')
    parser.add_argument('--distance', type=int, default=40)
    parser.add_argument('--screen', default='gpu-0')
    args = parser.parse_args()
    pen = Pen(args)
    c = args.coords
    if args.what == 'stroke':
        if len(c) != 4:
            sys.exit('stroke X0 Y0 X1 Y1')
        pen.stroke(*c)
    elif args.what == 'hover':
        pen.enter(c[0], c[1])
        for i in range(args.steps):
            time.sleep(args.ms / 1000)
            pen.frame(*pen.at(c[0], c[1], 0, args.distance))
        pen.leave()
    else:
        args.steps = 1
        pen.stroke(c[0], c[1], c[0], c[1])
    print(f'{pen.sent} frames delivered')


if __name__ == '__main__':
    main()
