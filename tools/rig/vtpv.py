#!/usr/bin/env python3
"""vtpv.py [--dump N]: the guest's VideoToolbox paravirtualization messages as DroidVMVTParavirt.cpp records them.
Counters, the record ring, and the first messages whole (the capture region at droidvm_vtpv_capture_pa), with their atoms
listed (FigAtom: 32-bit big-endian size and FourCC type, nested where the payload is itself atoms)."""
import struct, sys
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import peek

def u32(name):
    return struct.unpack('<I', peek.read(name, 4)[1])[0]

def fourcc(v):
    s = struct.pack('>I', v)
    return s.decode('latin1') if all(32 <= b < 127 for b in s) else f'{v:#010x}'

def atoms(data, depth=0, limit=40):
    at, out = 0, []
    while at + 8 <= len(data) and len(out) < limit:
        size, kind = struct.unpack('>I4s', data[at:at + 8])
        if size < 8 or at + size > len(data) or not all(32 <= b < 127 for b in kind):
            out.append('  ' * depth + f'raw {data[at:at + 48].hex()}' + ('...' if len(data) - at > 48 else ''))
            break
        body = data[at + 8:at + size]
        line = '  ' * depth + f"'{kind.decode()}' {size}"
        inner = atoms(body, depth + 1, limit) if len(body) >= 8 and all(32 <= b < 127 for b in body[4:8]) else None
        if inner and not inner[-1].lstrip().startswith('raw'):
            out.append(line)
            out += inner
        else:
            out.append(line + f'  {body[:40].hex()}' + ('...' if len(body) > 40 else ''))
        at += size
    return out

def main():
    dump = int(sys.argv[sys.argv.index('--dump') + 1]) if '--dump' in sys.argv else 8
    stage, opens, messages = u32('droidvm_vtpv_stage'), u32('droidvm_vtpv_opens'), u32('droidvm_vtpv_messages')
    selectors = struct.unpack('<8I', peek.read('droidvm_vtpv_selectors', 32)[1])
    print(f'stage {stage} opens {opens} messages {messages} refused {u32("droidvm_vtpv_refused")} uuids {u32("droidvm_vtpv_uuids")}'
          f' selectors {list(selectors[:5])}')
    try:
        print(f'notify calls {u32("droidvm_vtpv_notify_calls")} type {u32("droidvm_vtpv_notify_type")} result '
              f'{u32("droidvm_vtpv_notify_result"):#x} buffer fail step {u32("droidvm_vtpv_buffer_fail")} closes {u32("droidvm_vtpv_closes")}')
    except SystemExit:
        pass
    try:
        host = {n: u32('droidvm_vthost_' + n) for n in ('sessions', 'frames_in', 'frames_out', 'errors', 'error_stage', 'replies', 'unknown')}
        host['error_value'] = struct.unpack('<i', peek.read('droidvm_vthost_error_value', 4)[1])[0]
        host['last_unknown'] = fourcc(u32('droidvm_vthost_last_unknown'))
        print('host', host, 'sent', u32('droidvm_vtpv_sent'), 'dropped', u32('droidvm_vtpv_dropped'))
        count = u32('droidvm_vthost_trace_count')
        ring = peek.read('droidvm_vthost_trace', 16 * 112)[1]
        for i in range(max(0, count - 16), count):
            print('  trace', i, ring[(i % 16) * 112:(i % 16 + 1) * 112].split(b'\0')[0].decode(errors='replace'))
    except SystemExit:
        pass
    record = struct.Struct('<4I2IQ16s5II64s')
    ring = peek.read('droidvm_vtpv_log', 32 * record.size)[1]
    for i in range(min(messages, 32)):
        seq, pid, length, nsurf, kind, flags, request, uuid, *rest = record.unpack_from(ring, i * record.size)
        surfaces = rest[:5][:nsurf]
        print(f'#{seq} pid {pid} {fourcc(kind)} flags {flags:#x} request {request} uuid {uuid.hex()} {length} bytes'
              f' surfaces {surfaces}')
    pa = struct.unpack('<Q', peek.read('droidvm_vtpv_capture_pa', 8)[1])[0]
    if not pa:
        return
    for i in range(min(messages, dump, 16)):
        slot = peek.read(f'{pa + i * 16384:#x}', 16384)[1]
        length = struct.unpack('<I', slot[:4])[0]
        print(f'--- message {i}: {length} bytes')
        for line in atoms(slot[4:4 + min(length, 16380)]):
            print('  ' + line)

main()
