#!/usr/bin/env python3
"""vtpvdump.py FILE...: VideoToolbox paravirtualization messages decoded: the 'vtpv' header, then each atom; 'para' parameters
by type (=u32 =s32 =s64 =tim =whx =pls =dat =uui =fds ...), binary plists through plistlib, nested atoms indented."""
import plistlib, struct, sys

def cc(b):
    return b.decode('latin1') if all(32 <= x < 127 for x in b) else b.hex()

def atoms(data):
    at = 0
    while at + 8 <= len(data):
        size, kind = struct.unpack('>I4s', data[at:at + 8])
        if size < 8 or at + size > len(data):
            yield None, data[at:]
            return
        yield kind, data[at + 8:at + size]
        at += size

def plist(b):
    try:
        return plistlib.loads(b)
    except Exception as e:
        return f'<plist {e}: {b[:32].hex()}>'

def para(body, indent):
    key, kind, value = body[:4], body[4:8], body[8:]
    k = cc(key)
    t = cc(kind)
    if t == '=u32': v = struct.unpack('>I', value)[0]
    elif t == '=s32': v = struct.unpack('>i', value)[0]
    elif t == '=s64': v = struct.unpack('>q', value)[0]
    elif t == '=tim':
        val, scale, flags, epoch = struct.unpack('>qiIq', value[:24]); v = f'CMTime({val}/{scale} flags {flags:#x} epoch {epoch})'
    elif t == '=whx': v = struct.unpack('>ii', value[:8])
    elif t == '=pls': v = plist(value)
    elif t == '=uui': v = value.hex()
    elif t == '=dat': v = f'{len(value)} bytes {value[:48].hex()}'
    elif t == '=fds': v = f'ImageDescription {len(value)} bytes: ' + imagedesc(value)
    else:
        sub = list(atoms(value))
        if sub and all(s[0] for s in sub) and value:
            print(f'{indent}{k} {t}:')
            dump(value, indent + '    ')
            return
        v = f'{len(value)} bytes {value[:64].hex()}'
    print(f'{indent}{k} {t} {v}')

def imagedesc(b):
    if len(b) < 86: return b.hex()
    size, codec = struct.unpack('>I4s', b[:8])
    w, h = struct.unpack('>HH', b[32:36])
    ext = ' '.join(f"{cc(k)}({len(v)})" for k, v in atoms(b[86:size]) if k)
    return f'{cc(codec)} {w}x{h} ext {ext}'

def dump(data, indent=''):
    for kind, body in atoms(data):
        if kind is None:
            print(f'{indent}raw {body[:64].hex()}'); return
        k = cc(kind)
        if k == 'vtpv':
            mtype, flags, uuid, req = struct.unpack('>4sI16sQ', body[:32])
            print(f'{indent}vtpv {cc(mtype)} flags {flags:#x} uuid {uuid.hex()} request {req}')
        elif k == 'para':
            para(body, indent)
        elif k in ('nmbv', 'bulv', 'strk', 'datv', 'dtev'):
            print(f'{indent}{k} {plist(body)!r}')
        else:
            sub = list(atoms(body))
            if body and sub and all(s[0] for s in sub):
                print(f'{indent}{k}:')
                dump(body, indent + '    ')
            else:
                print(f'{indent}{k} {len(body)} bytes {body[:64].hex()}')

for path in sys.argv[1:]:
    data = open(path, 'rb').read()
    print(f'=== {path.split("/")[-1]} ({len(data)} bytes)')
    dump(data, '  ')
