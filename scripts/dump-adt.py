#!/usr/bin/env python3
"""Print an Apple-format DT (the .adt sidecar DarwinBoot hands to XNU) as an indented tree."""
import struct
import sys


def dump(data, offset=0, depth=0):
    properties, children = struct.unpack_from('<II', data, offset)
    offset += 8
    name, lines = '?', []
    for _ in range(properties):
        key, length = struct.unpack_from('<32sI', data, offset)
        offset += 36
        key = key.rstrip(b'\0').decode()
        value = data[offset:offset + length]
        offset += (length + 3) & ~3
        if key == 'name':
            name = value.rstrip(b'\0').decode()
            continue
        printable = value.rstrip(b'\0')
        if printable and value[0] != 0 and all(32 <= c < 127 or c == 0 for c in printable) and value.endswith(b'\0'):
            shown = repr(value.rstrip(b'\0').replace(b'\0', b' | ').decode())
        elif length in (4, 8):
            shown = hex(int.from_bytes(value, 'little'))
        elif length % 8 == 0 and length <= 64:
            shown = '[' + ', '.join(hex(v) for v in struct.unpack('<%dQ' % (length // 8), value)) + ']'
        else:
            shown = f'<{length} bytes>'
        lines.append(f'{key} = {shown}')
    print('  ' * depth + name + ' {')
    for line in lines:
        print('  ' * (depth + 1) + line)
    for _ in range(children):
        offset = dump(data, offset, depth + 1)
    print('  ' * depth + '}')
    return offset


blob = open(sys.argv[1], 'rb').read()
end = dump(blob)
if end != len(blob):
    raise SystemExit(f'trailing bytes: parsed {end} of {len(blob)}')
