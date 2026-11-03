#!/usr/bin/env python3
"""Keys on the guest's keyboard (the screen's virtio keyboard, the app's KEYBOARD channel) through the DroidVM daemon.
  keys.py combo META+L         modifiers and a key pressed together (META, CTRL, SHIFT, ALT)
  keys.py key ENTER            one key: ENTER, ESC, TAB, SPACE, BACKSPACE, or a letter or digit
  keys.py text STRING          US-layout text, shifted characters with SHIFT
  --screen ID                  the screen whose keyboard it is (default gpu-0)
"""
import argparse, base64, struct, sys, time
from pen import Daemon, VM

KEYBOARD = 1   # NativeDisplay.KEYBOARD
EV_SYN, EV_KEY = 0, 1
CODES = {'ESC': 1, 'BACKSPACE': 14, 'TAB': 15, 'ENTER': 28, 'SPACE': 57, 'META': 125, 'CTRL': 29, 'SHIFT': 42, 'ALT': 56,
         '-': 12, '=': 13, '[': 26, ']': 27, ';': 39, "'": 40, '`': 41, '\\': 43, ',': 51, '.': 52, '/': 53, ' ': 57}
for i, ch in enumerate('1234567890'):
    CODES[ch] = 2 + i
for row, start in (('qwertyuiop', 16), ('asdfghjkl', 30), ('zxcvbnm', 44)):
    for i, ch in enumerate(row):
        CODES[ch] = CODES[ch.upper()] = start + i
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0', '_': '-',
           '+': '=', '{': '[', '}': ']', ':': ';', '"': "'", '~': '`', '|': '\\', '<': ',', '>': '.', '?': '/'}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('what', choices=('combo', 'key', 'text'))
    parser.add_argument('value')
    parser.add_argument('--screen', default='gpu-0')
    args = parser.parse_args()
    daemon = Daemon()

    def send(*keys):   # (code, down) pairs, one report each
        for code, down in keys:
            data = struct.pack('<HHi', EV_KEY, code, 1 if down else 0) + struct.pack('<HHi', EV_SYN, 0, 0)
            reply = daemon.request('vm_input', vm_id=VM, screen=args.screen, channel=KEYBOARD, data=base64.b64encode(data).decode())
            if not (reply.get('data') or reply).get('delivered', False):
                sys.exit(f'not delivered: {reply}')
            time.sleep(0.03)

    def tap(code, shift=False):
        send(*([(CODES['SHIFT'], True)] if shift else []), (code, True), (code, False), *([(CODES['SHIFT'], False)] if shift else []))

    if args.what == 'combo':
        names = args.value.split('+')
        mods, key = [CODES[n.upper()] for n in names[:-1]], CODES[names[-1]]
        send(*[(m, True) for m in mods], (key, True), (key, False), *[(m, False) for m in reversed(mods)])
    elif args.what == 'key':
        tap(CODES[args.value])
    else:
        for ch in args.value:
            if ch in SHIFTED:
                tap(CODES[SHIFTED[ch]], True)
            else:
                tap(CODES[ch], ch.isupper())


main()
