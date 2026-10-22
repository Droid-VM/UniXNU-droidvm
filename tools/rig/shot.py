#!/usr/bin/env python3
"""Capture the phone screen and crop to the VM's composited display surface. Usage: shot.py OUT.png"""
import os
import subprocess
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import rigenv  # noqa: F401
import input as rig
from PIL import Image

PHONE = os.environ['PHONE']
out = sys.argv[1]
raw = out + '.raw.png'
with open(raw, 'wb') as f:
    f.write(subprocess.run(['adb', '-s', PHONE, 'exec-out', 'screencap', '-p'], stdout=subprocess.PIPE).stdout)
image = Image.open(raw)
scale_x, scale_y, offset_x, offset_y = rig.transform()
left, top = int(offset_x), int(offset_y)
right = int(offset_x + rig.GUEST_WIDTH * scale_x)
bottom = int(offset_y + rig.GUEST_HEIGHT * scale_y)
left, top = max(0, left), max(0, top)
right, bottom = min(image.width, right), min(image.height, bottom)
image.crop((left, top, right, bottom)).save(out)
os.remove(raw)
print(f'{out}  surface {right - left}x{bottom - top} from phone {image.width}x{image.height} (scale {scale_x:.3f},{scale_y:.3f} off {offset_x:.0f},{offset_y:.0f})')
