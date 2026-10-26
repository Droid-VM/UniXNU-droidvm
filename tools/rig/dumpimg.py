#!/usr/bin/env python3
"""Turn NATIVE_METAL_DUMP_IMAGE lines of a gfx.err into PNGs.  dumpimg.py GFX_ERR OUTDIR
Float formats are shown as x/(1+x) per channel (or min..max stretched for one-channel ones); depth min..max stretched."""
import base64, os, re, sys
import numpy as np
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
parts = {}
pat = re.compile(r'native-metal dump image cb (\d+): (.*?)\|(\d+)x(\d+) f(\d+) bpp (\d+)\|(\d+)/(\d+)\|(\S*)')
for line in open(src, errors='replace'):
    m = pat.search(line)
    if not m:
        continue
    cb, where, w, h, fmt, bpp, i, n, data = m.groups()
    key = (int(cb), where, int(w), int(h), int(fmt), int(bpp), int(n))
    parts.setdefault(key, {})[int(i)] = data


def r11g11b10(words):
    def f(v, mbits):
        e = (v >> mbits) & 0x1f
        mant = v & ((1 << mbits) - 1)
        val = np.where(e == 0, mant / (1 << mbits) * 2.0 ** -14,
                       (1 + mant / (1 << mbits)) * 2.0 ** (e.astype(np.float64) - 15))
        return np.where(e == 31, np.nan, val)
    r = f(words & 0x7ff, 6)
    g = f((words >> 11) & 0x7ff, 6)
    b = f((words >> 22) & 0x3ff, 5)
    return np.stack([r, g, b], -1)


def tone(x):
    x = np.nan_to_num(x, nan=0.0, posinf=1e6)
    return (np.clip(x / (1 + np.abs(x)), 0, 1) * 255).astype(np.uint8)


def stretch(x):
    x = np.nan_to_num(x.astype(np.float64), nan=0.0, posinf=0.0, neginf=0.0)
    lo, hi = np.percentile(x, 0.5), np.percentile(x, 99.5)
    return (np.clip((x - lo) / max(hi - lo, 1e-9), 0, 1) * 255).astype(np.uint8), lo, hi


for (cb, where, w, h, fmt, bpp, n), chunks in sorted(parts.items()):
    if len(chunks) != n:
        print(f'cb {cb} {where}: {len(chunks)} of {n} parts, skipped')
        continue
    raw = base64.b64decode(''.join(chunks[i] for i in range(1, n + 1)))
    a = np.frombuffer(raw, np.uint8)
    note = ''
    if fmt in (70, 71):
        img = a.reshape(h, w, 4)[:, :, :3]
    elif fmt in (80, 81):
        img = a.reshape(h, w, 4)[:, :, 2::-1]
    elif fmt == 10:
        img = a.reshape(h, w)
    elif fmt == 25:
        img, lo, hi = stretch(np.frombuffer(raw, np.float16).reshape(h, w)); note = f' {lo:.3g}..{hi:.3g}'
    elif fmt in (55, 252):
        img, lo, hi = stretch(np.frombuffer(raw, np.float32).reshape(h, w)); note = f' {lo:.4g}..{hi:.4g}'
    elif fmt == 65:
        v = np.frombuffer(raw, np.float16).reshape(h, w, 2).astype(np.float64)
        s = np.nan_to_num(np.abs(v)).max() or 1
        img = np.zeros((h, w, 3), np.uint8)
        img[:, :, :2] = (np.clip(np.abs(v) / s, 0, 1) * 255).astype(np.uint8); note = f' |max| {s:.3g}'
    elif fmt == 92:
        img = tone(r11g11b10(np.frombuffer(raw, np.uint32).reshape(h, w)))
    elif fmt == 115:
        img = tone(np.frombuffer(raw, np.float16).reshape(h, w, 4)[:, :, :3].astype(np.float64))
    else:
        print(f'cb {cb} {where}: format {fmt} not shown')
        continue
    name = f'cb{cb}-' + re.sub(r'[^A-Za-z0-9]+', '-', where).strip('-') + '.png'
    Image.fromarray(img).save(os.path.join(out, name))
    print(f'{name} {w}x{h} f{fmt}{note}')
