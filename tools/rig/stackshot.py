#!/usr/bin/env python3
"""Take and print a guest kernel stackshot through the file mailbox (export.py kind 7, DroidVMFileExport.c): every thread of
a process (or of every process) with its name, state, what it waits on (and who holds that) and its kernel frames named from
out/kernel.nm. Works while the guest's user space cannot run a tool (spindump stuck making its report, ssh without a shell).

  stackshot.py PID|-1 [--all]      take one now; --all prints threads blocked in a continuation too (no kernel frames)
  stackshot.py --file F.kcdata     print one taken before
The kcdata is decoded by XNU's tools/lldbmacros/kcdata.py (KCDATA_PY overrides its path)."""
import argparse, bisect, json, os, subprocess, sys, tempfile

RIG = os.path.dirname(os.path.abspath(__file__))
KCDATA_PY = os.environ.get('KCDATA_PY', '/root/gitrs/UniXNU/refer/xnu/tools/lldbmacros/kcdata.py')
rows = []
for line in open(os.path.join(RIG, 'out', 'kernel.nm')):
    parts = line.split()
    if len(parts) == 3:
        rows.append((int(parts[0], 16), parts[2]))
rows.sort()
keys = [r[0] for r in rows]


def name(address):
    address &= 0xffffffffffffffff
    if address < 0xfffffff000000000:
        return f'{address:#x}'
    i = bisect.bisect_right(keys, address) - 1
    return f'{rows[i][1]}+{address - rows[i][0]:#x}' if i >= 0 else f'{address:#x}'


# thread_waitinfo wait_type (osfmk/kern/kern_types.h kThread*WaitType), the common ones
WAIT = {1: 'kernel mutex', 2: 'port receive', 3: 'port send in transit', 4: 'semaphore', 5: 'port receive (sync)',
        6: 'user lock', 7: 'pthread mutex', 8: 'pthread rwlock read', 9: 'pthread rwlock write', 10: 'pthread condvar',
        11: 'parked workqueue', 12: 'workloop sync wait', 13: 'on process (exit)', 14: 'sleep with inheritor', 15: 'eventlink',
        16: 'compressor segment', 17: 'kernel rwlock read', 18: 'kernel rwlock write', 19: 'kernel rwlock upgrade',
        20: 'deferred reclamation', 21: 'exclave core', 22: 'exclave scheduler', 23: 'exclave Ipc stack', 24: 'page busy'}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('pid', nargs='?', default='-1')
    parser.add_argument('--file')
    parser.add_argument('--all', action='store_true')
    args = parser.parse_args()
    path = args.file
    if not path:
        path = tempfile.mktemp(suffix='.kcdata')
        subprocess.run([sys.executable, os.path.join(RIG, 'export.py'), 'stackshot', args.pid, path], check=True,
                       stdout=subprocess.DEVNULL)
    decoded = subprocess.run([sys.executable, KCDATA_PY, path], capture_output=True, text=True, check=True).stdout
    shot = json.loads(decoded)['kcdata_stackshot']
    for pid, task in sorted(shot.get('task_snapshots', {}).items(), key=lambda kv: int(kv[0])):
        snap = task.get('task_snapshot', {})
        waits = {w['waiter']: w for w in task.get('thread_waitinfo', []) or []}
        print(f'== {pid} {snap.get("ts_p_comm", "?")}: {len(task.get("thread_snapshots", {}))} threads')
        for tid, thread in sorted(task.get('thread_snapshots', {}).items(), key=lambda kv: int(kv[0])):
            ts = thread.get('thread_snapshot', {})
            frames = thread.get('kernel_stack_frames') or []
            wait = waits.get(int(tid))
            if not frames and not wait and not args.all:
                continue
            label = thread.get('pth_name') or thread.get('dispatch_queue_label') or ''
            line = f'  {tid} {label!r} state {ts.get("ths_state", 0):#x} prio {ts.get("ths_sched_priority", "?")}'
            if ts.get('ths_continuation'):
                line += f' continuation {name(ts["ths_continuation"])}'
            if ts.get('ths_wait_event'):
                line += f' event {ts["ths_wait_event"] & 0xffffffffffffffff:#x}'
            print(line)
            if wait:
                print(f'    waits: {WAIT.get(wait.get("wait_type"), wait.get("wait_type"))} context {wait.get("context", 0):#x} '
                      f'owner {wait.get("owner", 0)}')
            for frame in frames:
                address = frame.get('lr') if isinstance(frame, dict) else frame
                print(f'    {name(address)}')


if __name__ == '__main__':
    main()
