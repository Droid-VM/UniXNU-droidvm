#!/usr/bin/env python3
"""Print the XNU kernel message buffer (struct msgbuf at _msgbufp) of the running UniXNU VM."""
import struct, subprocess, sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import peek
_, ptr = peek.read('msgbufp', 8)
msgbuf = struct.unpack('<Q', ptr)[0]
_, head = peek.read(f'va:{msgbuf:x}', 24)
magic, size, bufx, bufr, bufc = struct.unpack('<iiiiQ', head)
print(f'msgbuf @ {msgbuf:#x}: magic {magic:#x} size {size} bufx {bufx} bufr {bufr} bufc {bufc:#x}', file=sys.stderr)
_, data = peek.read(f'va:{bufc:x}', size)
text = data[bufx:] + data[:bufx]
sys.stdout.write(text.replace(b'\0', b'').decode(errors='replace'))
