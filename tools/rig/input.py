#!/usr/bin/env python3
"""Touch the guest's screen through the phone, the way a finger on the DroidVM app's display does.

  input.py tap X Y                    a tap at guest pixel X,Y (of the framebuffer, as fbgrab.sh shows it)
  input.py long X Y [MS]              a press held MS milliseconds (default 1000): context menus, edit mode
  input.py swipe X0 Y0 X1 Y1 [MS]     a swipe taking MS milliseconds (default 300)
  input.py unlock                     swipe up from the bottom edge (lock screen to home screen, or Home in an app)
  input.py text STRING                type into the focused field (the app's virtio keyboard)

Android's `input` reaches the guest as the app's touch screen does (virtio-input), with real timing: touch.py writes the
digitizer through guest RAM one adb round trip per step, which a busy guest can take for a long press. Where the guest's
picture is on the phone depends on the app's layout (title bar and letterbox, or fullscreen), so each command takes the
mapping from SurfaceFlinger: the visible VM surface's toDisplayTransform, guest pixel to screen pixel."""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rigenv  # noqa: F401

PHONE = os.environ['PHONE']
GUEST_WIDTH, GUEST_HEIGHT = 1280, 800
try:   # the framebuffer of the last boot, from DarwinBoot's hand-off (vm.sh writes it)
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out', 'handoff.env')) as handoff:
        facts = dict(line.strip().partition('=')[::2] for line in handoff if '=' in line)
    GUEST_WIDTH, GUEST_HEIGHT = int(facts['FB_WIDTH']), int(facts['FB_HEIGHT'])
except (OSError, KeyError, ValueError):
    pass


def adb(*command):
    return subprocess.run(['adb', '-s', PHONE, 'shell', *command], capture_output=True, text=True, timeout=30).stdout


def transform():
    """(scale x, scale y, offset x, offset y) of the composited VM display surface; without one, the whole screen."""
    lines = adb('dumpsys', 'SurfaceFlinger').splitlines()
    for at, line in enumerate(lines):
        if not ('SurfaceView[cn.classfun.droidvm/' in line and 'VMNativeDisplayActivity' in line and '(BLAST)' in line
                and line.lstrip().startswith('Layer [')):
            continue
        # The layer's block: its lines up to the next layer. Earlier instances of the activity leave invisible surfaces.
        for following in lines[at + 1:at + 6]:
            if following.lstrip().startswith('Layer [') or 'invisible' in following:
                break
            found = re.search(r'toDisplayTransform=\{ scale x=([\d.]+) y=([\d.]+)\s+tx=([-\d.]+) ty=([-\d.]+)', following)
            if found:
                return tuple(float(v) for v in found.groups())
    width, height = map(int, adb('wm', 'size').split(':')[-1].strip().split('x'))
    return max(width, height) / GUEST_WIDTH, min(width, height) / GUEST_HEIGHT, 0.0, 0.0


MAPPING = None


def phone(x, y):
    global MAPPING
    MAPPING = MAPPING or transform()
    scale_x, scale_y, offset_x, offset_y = MAPPING
    return str(int(float(x) * scale_x + offset_x)), str(int(float(y) * scale_y + offset_y))


def main():
    command, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ('', [])
    if command == 'tap' and len(args) == 2:
        adb('input', 'tap', *phone(*args))
    elif command == 'long' and len(args) in (2, 3):
        x, y = phone(args[0], args[1])
        adb('input', 'swipe', x, y, x, y, args[2] if len(args) == 3 else '1000')
    elif command == 'swipe' and len(args) in (4, 5):
        adb('input', 'swipe', *phone(args[0], args[1]), *phone(args[2], args[3]), args[4] if len(args) == 5 else '300')
    elif command == 'unlock' and not args:
        # From inside the guest's bottom 4% (DroidVMVirtioInput.c starts such a touch at the edge, for the Home gesture) but
        # above the phone's own gesture strip: a swipe from the phone's last few pixels is Android's Home, never the app's.
        adb('input', 'swipe', *phone(GUEST_WIDTH / 2, GUEST_HEIGHT * 0.9625), *phone(GUEST_WIDTH / 2, GUEST_HEIGHT / 3), '250')
    elif command == 'text' and len(args) == 1:
        adb('input', 'text', args[0].replace(' ', '%s'))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
