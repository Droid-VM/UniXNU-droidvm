#!/usr/bin/env python3
"""Check what the screen shows, from both ends of the display path: the phone's own composition (screencap, cropped to
the VM's surface: what a person sees) and every virtio-gpu scanout surface the guest composed into (crosvm's udmabufs,
read with dmabufgrab). A frame is compared with another cell by cell (16x16 pixels, grey levels; the status bar left
out, as its clock moves): the fraction of cells that differ.

  screen-check.py capture OUTDIR                     both ends, and which guest surface the phone shows
  screen-check.py reference NAME [--home] [--add]    keep the phone's frame as out/visual/ref-NAME.png (look at it first);
                                                     --add keeps another right frame of the same screen (ref-NAME.2.png...)
  screen-check.py expect NAME[,NAME...] [--home] [--settle S]
                                                     the phone must show one of the references; else say which end is wrong

--home presses Home first and waits --settle seconds (default 3). One screen may have several right frames: the home
screen stays in portrait after a portrait-only app (no orientation sensor turns it back), so keep both and expect
home,home-portrait; and a dynamic wallpaper (Kaleidoscope) draws another pattern after each boot, so --add the home screen
with a second wallpaper: a reference is then compared only where its frames agree (icons, dock, widgets). expect writes out/visual/expect-NAMES-TIME/report.json and exits 1 when the phone shows none of them: "display path" when a guest surface does (crosvm or Android did not
put it on screen), "guest composition" when the phone shows a guest surface that is not the reference (the render server
or the app is wrong), "unknown frame" when the phone shows no guest surface at all. Needs /data/local/tmp/dmabufgrab
(dmabufgrab.c, static aarch64 build).
"""
import argparse
import io
import json
import os
import subprocess
import sys
import tarfile
import time
from pathlib import Path

import rigenv  # noqa: F401
import input as rig
from PIL import Image

RIG = Path(__file__).resolve().parent
OUT = RIG / 'out' / 'visual'
PHONE = os.environ['PHONE']
CELL, STATUS_BAR, CELL_LEVELS, SAME = 16, 24, 24, 0.08


def adb(*args, **kw):
    return subprocess.run(['adb', '-s', PHONE, *args], check=True, capture_output=True, **kw).stdout


def phone_frame():
    """The phone's composition of the VM surface, at the guest's resolution."""
    image = Image.open(io.BytesIO(adb('exec-out', 'screencap', '-p'))).convert('RGB')
    scale_x, scale_y, offset_x, offset_y = rig.transform()
    box = (max(0, int(offset_x)), max(0, int(offset_y)), min(image.width, int(offset_x + rig.GUEST_WIDTH * scale_x)),
           min(image.height, int(offset_y + rig.GUEST_HEIGHT * scale_y)))
    return image.crop(box).resize((rig.GUEST_WIDTH, rig.GUEST_HEIGHT), Image.BOX)


def guest_frames():
    """{name: image} of every scanout-sized udmabuf crosvm holds (rows as far apart as the buffer is tall allows)."""
    adb('push', str(RIG / 'scanout-grab-phone.sh'), '/data/local/tmp/scanout-grab-phone.sh')
    # adb joins its arguments into one shell line: su's command is one quoted word of it.
    tar = adb('exec-out', "su -c 'sh /data/local/tmp/scanout-grab-phone.sh /data/local/tmp/scanout'")
    archive = tarfile.open(fileobj=io.BytesIO(tar))
    frames = {}
    width, height = rig.GUEST_WIDTH, rig.GUEST_HEIGHT
    for member in archive.getmembers():
        if not member.isfile():
            continue
        raw = archive.extractfile(member).read()
        stride = len(raw) // height
        if stride < width * 4:
            continue
        rows = b''.join(raw[y * stride:y * stride + width * 4] for y in range(height))
        frames['fd' + Path(member.name).stem] = Image.frombytes('RGBA', (width, height), rows, 'raw', 'BGRA').convert('RGB')
    return frames


def cells(image):
    grey = image.convert('L').crop((0, STATUS_BAR, image.width, image.height))
    return grey.resize((grey.width // CELL, grey.height // CELL), Image.BOX).tobytes()


def differ(a, b, stable=None):
    """The fraction of cells whose grey level differs by more than CELL_LEVELS (of the stable cells, when given)."""
    ca, cb = cells(a), cells(b)
    kept = [i for i in range(len(ca)) if stable is None or stable[i]]
    return sum(abs(ca[i] - cb[i]) > CELL_LEVELS for i in kept) / max(len(kept), 1)


def reference_frames(name):
    """A reference's frames (ref-NAME.png, ref-NAME.2.png, ...) and the cells all of them agree on."""
    paths = [OUT / f'ref-{name}.png'] + sorted(OUT.glob(f'ref-{name}.[0-9]*.png'))
    frames = [Image.open(path).convert('RGB') for path in paths]
    first = cells(frames[0])
    stable = [all(abs(cells(frame)[i] - first[i]) <= CELL_LEVELS for frame in frames[1:]) for i in range(len(first))]
    return frames[0], stable


def press_home(settle):
    subprocess.run([sys.executable, str(RIG / 'home.py')], check=True, capture_output=True)
    time.sleep(settle)


def capture(out):
    out.mkdir(parents=True, exist_ok=True)
    phone, guests = phone_frame(), guest_frames()
    phone.save(out / 'phone.png')
    for name, image in guests.items():
        image.save(out / f'{name}.png')
    shown = {name: round(differ(phone, image), 3) for name, image in guests.items()}
    return phone, guests, shown


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=('capture', 'reference', 'expect'))
    parser.add_argument('name')
    parser.add_argument('--home', action='store_true')
    parser.add_argument('--settle', type=float, default=3.0)
    parser.add_argument('--add', action='store_true')
    args = parser.parse_args()
    if args.action == 'capture':
        _, _, shown = capture(Path(args.name))
        print(json.dumps({'phone_vs_guest': shown}, indent=1))
        return 0
    names = args.name.split(',')
    if args.home:
        press_home(args.settle)
    if args.action == 'reference':
        OUT.mkdir(parents=True, exist_ok=True)
        path = OUT / f'ref-{names[0]}.png'
        if args.add:
            path = OUT / f'ref-{names[0]}.{len(list(OUT.glob(f"ref-{names[0]}.[0-9]*.png"))) + 2}.png'
        phone_frame().save(path)
        print(path)
        return 0
    wants = {name: reference_frames(name) for name in names}
    out = OUT / f'expect-{"+".join(names)}-{time.strftime("%Y%m%d-%H%M%S")}'
    phone, guests, shown = capture(out)
    phone_vs_refs = {name: round(differ(phone, want, stable), 3) for name, (want, stable) in wants.items()}
    best = min(phone_vs_refs, key=phone_vs_refs.get)
    phone_vs_ref = phone_vs_refs[best]
    guest_vs_ref = {name: round(min(differ(image, want, stable) for want, stable in wants.values()), 3)
                    for name, image in guests.items()}
    if phone_vs_ref <= SAME:
        verdict = 'PASS'
    elif any(v <= SAME for v in guest_vs_ref.values()):
        verdict = 'FAIL: display path (a guest surface holds the reference, the phone shows something else)'
    elif any(v <= SAME for v in shown.values()):
        verdict = 'FAIL: guest composition (the phone shows a guest surface that is not the reference)'
    else:
        verdict = 'FAIL: unknown frame (the phone shows no guest surface)'
    report = {'verdict': verdict, 'references': names, 'closest_reference': best, 'phone_vs_references': phone_vs_refs,
              'guest_vs_reference': guest_vs_ref, 'phone_vs_guest': shown, 'same_below': SAME,
              'compared_cells': {name: round(sum(stable) / len(stable), 3) for name, (_, stable) in wants.items()},
              'metric': f'fraction of {CELL}x{CELL} cells differing by more than {CELL_LEVELS} grey levels, '
                        f'status bar ({STATUS_BAR} rows) and cells the reference frames disagree on left out'}
    (out / 'report.json').write_text(json.dumps(report, indent=1) + '\n')
    print(f'{verdict}  phone vs {best} {phone_vs_ref:.3f}  ({out}/report.json)')
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
