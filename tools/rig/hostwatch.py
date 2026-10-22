#!/usr/bin/env python3
"""Watch the phone for what a VM run must not miss, one line per event (for a monitor; nothing while all is well).
  hostwatch.py [--interval S] [--boot-wait S] [--settle S]
  WATCHING ...          the first look: boot id, uptime, pool
  HOST REBOOTED ...     the phone's boot id changed or its uptime went back: the host died (watchdog bite, panic)
  PHONE UNREACHABLE / PHONE BACK   adb lost / found the phone again
  VM UP pid=...         a process took pages from gh_hugepage_reserve's pool (vm_owners), and is alive
  VM GONE pid=...       it left vm_owners or its process is gone: "requested stop" after vm.sh stop, otherwise UNEXPECTED
                        (crosvm crashed or was killed)
  POOL ...              the pool --settle s after a VM went: avail/want, active VMs, what the module abandoned
  BOOT FAILED ...       vm.sh asked for a start and no VM took pool pages within --boot-wait s
vm.sh writes out/vm-request ("start|stop <epoch>") so a requested stop is told from a crash. See hugepage-harness.md."""
import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rigenv  # noqa: F401

PHONE = os.environ['PHONE']
OUT = os.environ.get('RIG_OUT', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out'))
POOL = '/sys/module/gh_hugepage_reserve/parameters'
OWNER = re.compile(r'pid=(\d+) vm_count=(\d+) served=(\d+) abandoned=(\d+) comm=(.*)')


def say(text):
    print(time.strftime('%H:%M:%S'), text, flush=True)


def snapshot():
    """(boot id, uptime s, {pid: owner fields}, live pids, refill_stat dict), or None when adb cannot reach the phone."""
    command = (f"cat /proc/sys/kernel/random/boot_id; cut -d. -f1 /proc/uptime; "
               f"su -c 'cat {POOL}/vm_owners; echo ===; cat {POOL}/refill_stat; echo ===; ls /proc'")
    try:
        text = subprocess.run(['adb', '-s', PHONE, 'shell', command], capture_output=True, text=True, timeout=10).stdout
    except subprocess.TimeoutExpired:
        return None
    lines = text.replace('\r', '').split('\n')
    if len(lines) < 3 or not re.fullmatch(r'[0-9a-f-]{36}', lines[0].strip()) or not lines[1].strip().isdigit():
        return None
    body = '\n'.join(lines[2:]).split('===')
    owners = {}
    for line in body[0].splitlines():
        found = OWNER.match(line.strip())
        if found:
            owners[int(found[1])] = dict(vm_count=int(found[2]), served=int(found[3]), abandoned=int(found[4]), comm=found[5])
    stat = dict(l.split('=', 1) for l in body[1].split() if '=' in l) if len(body) > 1 else {}
    alive = {int(p) for p in body[2].split() if p.isdigit()} if len(body) > 2 else set()
    return lines[0].strip(), int(lines[1].strip()), owners, alive, stat


def request():
    try:
        kind, at = open(os.path.join(OUT, 'vm-request')).read().split()
        return kind, int(at)
    except (OSError, ValueError):
        return None, 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--interval', type=float, default=3)
    parser.add_argument('--boot-wait', type=int, default=90)
    parser.add_argument('--settle', type=int, default=10)
    args = parser.parse_args()
    boot = uptime = None
    up = {}                      # pid -> owner fields of the VMs seen alive
    settling = {}                # pid -> (time it went, owner fields) for the POOL line
    reachable = True
    reported_start = None
    while True:
        snap = snapshot()
        now = time.time()
        if snap is None:
            if reachable:
                say('PHONE UNREACHABLE')
            reachable = False
            time.sleep(args.interval)
            continue
        if not reachable:
            say('PHONE BACK')
        reachable = True
        boot_id, seconds, owners, alive, stat = snap
        if boot is None:
            say(f'WATCHING boot_id={boot_id[:8]} uptime={seconds}s pool={stat.get("pool_avail")}/{stat.get("pool_want")} '
                f'active_vms={stat.get("active_vms")} owners={sorted(owners)}')
        elif boot_id != boot or seconds < uptime:
            say(f'HOST REBOOTED: boot_id {boot[:8]} -> {boot_id[:8]}, uptime {seconds}s; VMs lost: {sorted(up) or "none"}')
            up.clear()
            settling.clear()
        boot, uptime = boot_id, seconds
        for pid, fields in owners.items():
            if pid in alive and pid not in up and fields['vm_count'] > 0:
                up[pid] = fields
                say(f'VM UP pid={pid} comm={fields["comm"]} vm_count={fields["vm_count"]} served={fields["served"]}')
        for pid in list(up):
            if pid in owners and pid in alive:
                up[pid] = owners[pid]
                continue
            kind, at = request()
            why = 'requested stop' if kind == 'stop' and now - at < 120 else 'UNEXPECTED: crosvm crashed or was killed'
            where = 'left vm_owners' if pid not in owners else 'process gone'
            say(f'VM GONE pid={pid} ({where}; {why}) served={up[pid]["served"]}')
            settling[pid] = (now, up.pop(pid))
        for pid, (went, fields) in list(settling.items()):
            if now - went >= args.settle:
                row = owners.get(pid)
                say(f'POOL {args.settle}s after pid {pid}: {stat.get("pool_avail")}/{stat.get("pool_want")} '
                    f'active_vms={stat.get("active_vms")} acquire_active={stat.get("acquire_active")} '
                    f'abandoned={row["abandoned"] if row else "(owner row gone)"}')
                del settling[pid]
        kind, at = request()
        if kind == 'start' and at != reported_start and now - at > args.boot_wait:
            if not up:
                say(f'BOOT FAILED: start asked {int(now - at)}s ago and no VM took pool pages')
            reported_start = at
        time.sleep(args.interval)


if __name__ == '__main__':
    main()
