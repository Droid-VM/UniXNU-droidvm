#!/usr/bin/env python3
"""Read a file or list a directory of the running guest through the kernel's file mailbox (DroidVMFileExport.c).

  export.py ls /private/var/mobile/Library/Logs/CrashReporter
  export.py tree /private/var/db/uuidtext          (every file below it with its size: "<d_type> <size> <path>")
  export.py cat /private/var/log/some.log [--offset N]
  export.py get /private/var/db/file OUTPUT        (the whole file, in 1 MiB requests)
  export.py ps                                     (pid, parent pid, name of every process)
  export.py kill PID [SIGNAL]                      (default 9; launchd restarts what it keeps alive)
  export.py mem PID ADDRESS [LENGTH]               (hex dump of a process's user memory)
  export.py put LOCAL /private/var/ngpu/... [MODE]  (write a file, root-owned, default mode 0644; only under /private/var/ngpu/)

The request is written into droidvm_export_box through crosvm's /proc/<pid>/mem; the droidvm-monitor worker answers
within ~100 ms. Paths are in the guest's namespace (Data is mounted at /private/var)."""
import argparse
import struct
import sys
import time

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import peek

BOX = struct.Struct('<IIIiQQQ512s')     # request, done, kind, status, offset, length, total, path
DATA_BYTES = 1 << 20


def physical(name):
    return peek.symbol(name) - peek.KERNEL_VA + peek.KERNEL_PA


def write_guest(pid, maps, pa, data):
    host = peek.host_address(maps, pa, len(data))
    if len(data) <= 512:
        escaped = ''.join(f'\\{byte:03o}' for byte in data)
        peek.adb(f"printf '{escaped}' | dd of=/proc/{pid}/mem bs=1 seek={host} conv=notrunc 2>/dev/null")
        return
    # Larger pieces (a file for `put`) travel as a file on the phone: a command line holds only so much.
    import subprocess, tempfile
    with tempfile.NamedTemporaryFile() as piece:
        piece.write(data)
        piece.flush()
        subprocess.run(['adb', '-s', peek.PHONE, 'push', piece.name, '/data/local/tmp/droidvm-export-piece'], check=True,
                       capture_output=True, timeout=120)
    peek.adb(f'dd if=/data/local/tmp/droidvm-export-piece of=/proc/{pid}/mem bs=65536 seek={host} oflag=seek_bytes conv=notrunc 2>/dev/null; '
             'rm -f /data/local/tmp/droidvm-export-piece')


def read_guest(pid, maps, pa, length):
    host = peek.host_address(maps, pa, length)
    if host % 4096 == 0 and length % 4096 == 0:
        return peek.adb(f'dd if=/proc/{pid}/mem bs=4096 skip={host // 4096} count={length // 4096} 2>/dev/null')
    return peek.adb(f'dd if=/proc/{pid}/mem bs=1 skip={host} count={length} 2>/dev/null')


def request(kind, path, offset=0, length=0, total=0, payload=b''):
    pid, maps = peek.crosvm_maps()
    box, data = physical('droidvm_export_box'), physical('droidvm_export_data')
    current = BOX.unpack(read_guest(pid, maps, box, BOX.size))
    number = (current[0] + 1) & 0xffffffff
    if payload:
        write_guest(pid, maps, data, payload)
    # Everything but the counters first, then the request counter: the worker reads the fields after it sees it change.
    write_guest(pid, maps, box + 8, struct.pack('<IiQQQ512s', kind, 0, offset, length, total, path.encode()[:511]))
    write_guest(pid, maps, box, struct.pack('<I', number))
    for _ in range(100):
        answer = BOX.unpack(read_guest(pid, maps, box, BOX.size))
        if answer[1] == number:
            break
        time.sleep(0.2)
    else:
        sys.exit('the guest did not answer (is droidvm-monitor running?)')
    status, length, total = answer[3], answer[5], answer[6]
    if status:
        sys.exit(f'{path}: error {status}')
    body = b''
    if length:
        body = read_guest(pid, maps, data, (length + 4095) & ~4095)[:length]
    return body, total


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['ls', 'tree', 'cat', 'get', 'ps', 'kill', 'mem', 'put'])
    parser.add_argument('path', nargs='?', default='')
    parser.add_argument('output', nargs='?')
    parser.add_argument('length', nargs='?')
    parser.add_argument('--offset', type=int, default=0)
    args = parser.parse_args()
    if args.command == 'put':
        source = open(args.path, 'rb').read()
        mode = int(args.length or '0644', 8)
        # In 1 MiB pieces: the first creates or truncates the file and sets its mode, the rest append at their offsets.
        for offset in range(0, max(len(source), 1), DATA_BYTES):
            piece = source[offset:offset + DATA_BYTES]
            request(5, args.output, offset, len(piece), mode, piece)
        print(f'{args.output}: {len(source)} bytes, mode {mode:o}')
    elif args.command == 'mem':
        body, _ = request(4, args.path, int(args.output, 16), int(args.length, 0) if args.length else 64)
        for at in range(0, len(body), 16):
            print(f'{int(args.output, 16) + at:016x}: {body[at:at + 16].hex(" ")}')
    elif args.command == 'kill':
        request(3, '', int(args.path), int(args.output or 9))
        print(f'signal {args.output or 9} sent to {args.path}')
    elif args.command == 'ps':
        body, _ = request(2, '')
        sys.stdout.write(body.decode(errors='replace'))
    elif args.command == 'ls':
        body, _ = request(1, args.path, args.offset)
        sys.stdout.write(body.decode(errors='replace'))
    elif args.command == 'tree':
        first = args.offset
        while True:   # a full answer goes on from the entry after its last
            body, listed = request(6, args.path, first)
            if not listed:
                break
            sys.stdout.write(body.decode(errors='replace'))
            first += listed
    elif args.command == 'cat':
        body, total = request(0, args.path, args.offset)
        sys.stdout.buffer.write(body)
        if args.offset + len(body) < total:
            print(f'\n[{total - args.offset - len(body)} more bytes; --offset {args.offset + len(body)}]', file=sys.stderr)
    else:
        if not args.output:
            sys.exit('get needs an OUTPUT file')
        with open(args.output, 'wb') as out:
            offset = 0
            while True:
                body, total = request(0, args.path, offset)
                out.write(body)
                offset += len(body)
                if not body or offset >= total:
                    break
        print(f'{args.output}: {offset} bytes')


if __name__ == '__main__':
    main()
