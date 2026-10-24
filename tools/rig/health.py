#!/usr/bin/env python3
"""One look at the running guest: kernel, Data, userland, display, input, network, new crash reports.

  health.py          prints one line per part; exits 1 when the kernel has panicked or launchd is not running

Read from guest RAM (peek.py) and through the kernel's file mailbox (export.py); the guest's addresses from the phone's
wlan0, where the app's br-wifi proxy shows them (/32 and /128, noprefixroute)."""
import os
import re
import subprocess
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rigenv  # noqa: F401
import crashes
import export
import panic
import peek

DATA_MOUNTED = 4
IFF_UP, IFF_RUNNING = 0x1, 0x40
USERLAND = ['launchd', 'backboardd', 'SpringBoard', 'configd', 'runningboardd', 'lsd']


def value(name, size=4):
    try:
        return panic.word(name, size)
    except (Exception, SystemExit):
        return None


def addresses():
    out = subprocess.run(['adb', '-s', os.environ['PHONE'], 'shell', 'ip addr show wlan0'], capture_output=True, text=True,
                         timeout=30).stdout
    return (re.findall(r'inet (\S+)/32 .*noprefixroute', out), re.findall(r'inet6 (\S+)/128 .*noprefixroute', out))


def main():
    healthy = True
    entries = value('kernel_debugger_entry_count')
    if entries:
        healthy = False
        print(f'kernel    PANIC (debugger op {value("debugger_current_op")}): panic.py prints the log')
    else:
        print('kernel    running, no panic')
    stage = value('native_owner_data_stage')
    print(f'data      {"mounted" if stage == DATA_MOUNTED else f"NOT mounted (stage {stage})"}, store state {value("native_data_persist_state")}')
    try:
        body, _ = export.request(2, '')
        names = {line.split(None, 2)[2] for line in body.decode(errors='replace').splitlines() if len(line.split(None, 2)) == 3}
        missing = [n for n in USERLAND if n not in names]
        print(f'userland  {len(names)} processes; ' + (f'MISSING {", ".join(missing)}' if missing else 'launchd, backboardd, SpringBoard, configd up'))
        healthy &= 'launchd' in names
    except SystemExit as error:
        print(f'userland  the file mailbox did not answer ({error})')
        healthy = False
    print(f'display   {value("native_legacy_frame_count")} frames presented; input {value("droidvm_input_events", 8)} events')
    virtio_state = value('droidvm_virtio_display_state')
    if virtio_state:
        print(f'virtio-gpu state {virtio_state}, blob {value("droidvm_virtio_display_blob")}, '
              f'frames {value("droidvm_virtio_display_frames", 8)}, boot frames {value("droidvm_virtio_display_boot_frames", 8)}, '
              f'command errors {value("droidvm_virtio_gpu_errors", 8)}')
        wait_enabled = value('droidvm_virtio_present_wait')
        early_retires = value('native_legacy_early_retires', 8)
        print(f'present   producer wait {wait_enabled}, busy {value("droidvm_virtio_present_busy", 8)}/'
              f'{value("droidvm_virtio_present_probes", 8)}, status {value("droidvm_virtio_display_last_status")}, '
              f'early retires {early_retires}')
        healthy &= virtio_state == 2 and wait_enabled == 1 and early_retires == 0
    try:
        losses = struct.unpack('<32I', peek.read('droidvm_gpu_lost', 32 * 4)[1])
        affected = ', '.join(f'{index}:{count}' for index, count in enumerate(losses) if count) or 'none'
        caps = value('droidvm_gpu_capabilities') or 0
        mask = value('droidvm_gpu_lost_mask')
        if caps & 1 and mask is not None:
            print(f'gpu-3d    active lost mask {mask:#010x}; context losses since boot {affected}')
            healthy &= mask == 0
        else:
            print(f'gpu-3d    context losses since boot {affected}; negotiated per-client loss status unavailable')
            healthy &= not any(losses)
    except (Exception, SystemExit):
        print('gpu-3d    context loss counters unavailable')
    unit, flags = value('droidvm_ethernet_unit'), value('droidvm_ethernet_ifflags') or 0
    if unit:
        state = ', '.join(s for bit, s in ((IFF_UP, 'up'), (IFF_RUNNING, 'running')) if flags & bit) or 'down'
        ipv4, ipv6 = addresses()
        print(f'network   en{unit - 1} {state}; out {value("droidvm_ethernet_outputs")} in {value("droidvm_ethernet_inputs")} packets; '
              f'address {", ".join(ipv4 + ipv6) or "none on wlan0"}')
    else:
        print(f'network   no interface (controller stage {value("droidvm_net_if_stage")}, transport stage {value("droidvm_net_stage")})')
    seen_path = os.path.join(crashes.OUT, 'seen')
    seen = set(open(seen_path).read().split()) if os.path.exists(seen_path) else set()
    new = [n for n in crashes.listing(crashes.REPORTS) if n not in seen]
    kinds = sorted({re.sub(r'-\d{4}-\d{2}-\d{2}-.*$', '', n.split('/')[-1]) for n in new})
    print(f'crashes   {len(new)} new report(s)' + (f': {", ".join(kinds)} (crashes.py)' if new else ''))
    return 0 if healthy else 1


if __name__ == '__main__':
    sys.exit(main())
