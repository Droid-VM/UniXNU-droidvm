#!/usr/bin/env python3
"""Read a file or list a directory of the running guest through the kernel's file mailbox (DroidVMFileExport.c).

  export.py ls /private/var/mobile/Library/Logs/CrashReporter
  export.py cat /private/var/log/some.log [--offset N]
  export.py get /private/var/db/file OUTPUT        (the whole file, in 1 MiB requests)
  export.py ps                                     (pid, parent pid, name of every process)
  export.py kill PID [SIGNAL]                      (default 9; launchd restarts what it keeps alive)

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
    escaped = ''.join(f'\\{byte:03o}' for byte in data)
    peek.adb(f"printf '{escaped}' | dd of=/proc/{pid}/mem bs=1 seek={host} conv=notrunc 2>/dev/null")


def read_guest(pid, maps, pa, length):
    host = peek.host_address(maps, pa, length)
    if host % 4096 == 0 and length % 4096 == 0:
        return peek.adb(f'dd if=/proc/{pid}/mem bs=4096 skip={host // 4096} count={length // 4096} 2>/dev/null')
    return peek.adb(f'dd if=/proc/{pid}/mem bs=1 skip={host} count={length} 2>/dev/null')


def request(kind, path, offset=0, length=0):
    pid, maps = peek.crosvm_maps()
    box, data = physical('droidvm_export_box'), physical('droidvm_export_data')
    current = BOX.unpack(read_guest(pid, maps, box, BOX.size))
    number = (current[0] + 1) & 0xffffffff
    # Everything but the counters first, then the request counter: the worker reads the fields after it sees it change.
    write_guest(pid, maps, box + 8, struct.pack('<IiQQQ512s', kind, 0, offset, length, 0, path.encode()[:511]))
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
    parser.add_argument('command', choices=['ls', 'cat', 'get', 'ps', 'kill'])
    parser.add_argument('path', nargs='?', default='')
    parser.add_argument('output', nargs='?')
    parser.add_argument('--offset', type=int, default=0)
    args = parser.parse_args()
    if args.command == 'kill':
        request(3, '', int(args.path), int(args.output or 9))
        print(f'signal {args.output or 9} sent to {args.path}')
    elif args.command == 'ps':
        body, _ = request(2, '')
        sys.stdout.write(body.decode(errors='replace'))
    elif args.command == 'ls':
        body, _ = request(1, args.path, args.offset)
        sys.stdout.write(body.decode(errors='replace'))
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
