#!/usr/bin/env python3
"""Exercise Today View/blur and Spotlight/keyboard without changing the driver's sync policy.

Run after vm.sh start, with hostwatch.py active. Records counters, compositor PID, new crash
reports and phone screenshots in out/display-soak-<time>/ (or --output). A changed compositor,
panic, stopped display, disabled producer wait or failed command fails the run.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import time

import rigenv  # noqa: F401
import crashes
import export
import input as rig_input
import peek

RIG = Path(__file__).resolve().parent
COUNTERS = {
    'kernel_debugger_entry_count': 4,
    'droidvm_virtio_display_state': 4,
    'droidvm_virtio_present_wait': 4,
    'droidvm_virtio_display_frames': 8,
    'droidvm_virtio_display_boot_frames': 8,
    'droidvm_virtio_present_probes': 8,
    'droidvm_virtio_present_busy': 8,
    'droidvm_virtio_present_wait_us': 8,
    'droidvm_virtio_gpu_errors': 8,
    'droidvm_virtio_gpu_timeouts': 8,
    'native_legacy_copy_n': 8,
    'native_legacy_gpu_fallbacks': 4,
    'native_legacy_early_retires': 8,
    'native_legacy_early_failures': 8,
}


def snapshot():
    values = {}
    for name, size in COUNTERS.items():
        raw = peek.read(name, size)[1]
        if len(raw) != size:
            raise RuntimeError(f'short read of {name}: {len(raw)}/{size} bytes')
        values[name] = int.from_bytes(raw, 'little')
    raw = peek.read('droidvm_virtgpu_stats', 64)[1]
    if len(raw) != 64:
        raise RuntimeError('short read of the virtio GPU 3D counters')
    values['virtgpu_fenced_submissions'] = struct.unpack('<8Q', raw)[5]
    body, _ = export.request(2, '')
    procs = [line.split(None, 2) for line in body.decode(errors='replace').splitlines()]
    values['backboardd_pid'] = next((int(p[0]) for p in procs if len(p) == 3 and p[2] == 'backboardd'), None)
    return values


def validate(values, baseline=None):
    if values['kernel_debugger_entry_count'] or values['droidvm_virtio_display_state'] != 2:
        raise RuntimeError('kernel debugger entered or virtio display stopped')
    if values['droidvm_virtio_present_wait'] != 1:
        raise RuntimeError('producer fence waiting is not enabled')
    if values['native_legacy_early_failures']:
        raise RuntimeError('a frame retired on offer failed its display copy')
    if not values['backboardd_pid']:
        raise RuntimeError('backboardd is absent')
    if baseline:
        if values['backboardd_pid'] != baseline['backboardd_pid']:
            raise RuntimeError('backboardd restarted during the run')
        for name in ('droidvm_virtio_gpu_errors', 'droidvm_virtio_gpu_timeouts',
                     'native_legacy_copy_n', 'native_legacy_gpu_fallbacks'):
            if values[name] != baseline[name]:
                raise RuntimeError(f'{name} changed during the run')


def require_consumer():
    text = subprocess.check_output(['adb', '-s', os.environ['PHONE'], 'shell', 'dumpsys', 'SurfaceFlinger'], text=True)
    lines = text.splitlines()
    for index, line in enumerate(lines[:-1]):
        if ('Layer [' in line and 'SurfaceView[cn.classfun.droidvm/' in line and
                'VMNativeDisplayActivity](BLAST)' in line and 'visible reason= buffer=' in lines[index + 1]):
            return
    raise RuntimeError('no visible native display buffer; reattach the display before testing')


def home():
    subprocess.run(['python3', str(RIG / 'home.py')], check=True, stdout=subprocess.DEVNULL)
    time.sleep(1.5)  # HID poll + press/release + animation


def swipe(x0, y0, x1, y1, ms):
    subprocess.run(['adb', '-s', os.environ['PHONE'], 'shell', 'input', 'swipe',
                    *rig_input.phone(x0, y0), *rig_input.phone(x1, y1), str(ms)], check=True)
    time.sleep(0.7)


def shot(path):
    with path.open('wb') as stream:
        subprocess.run(['adb', '-s', os.environ['PHONE'], 'exec-out', 'screencap', '-p'], check=True, stdout=stream)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cycles', type=int, default=30)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000:
        parser.error('--cycles must be 1..1000')
    out = args.output or RIG / 'out' / time.strftime('display-soak-%Y%m%d-%H%M%S')
    out.mkdir(parents=True, exist_ok=False)
    report = {'requested_cycles': args.cycles, 'completed_cycles': 0, 'samples': [], 'passed': False}
    before_crashes = set()
    began = time.monotonic()
    try:
        require_consumer()
        home()
        home()
        before = snapshot()
        validate(before)
        report['before'] = before
        before_crashes = set(crashes.listing(crashes.REPORTS))
        shot(out / 'start.png')
        width, height = rig_input.GUEST_WIDTH, rig_input.GUEST_HEIGHT
        previous_frames = before['droidvm_virtio_display_frames']
        previous_submit = before['virtgpu_fenced_submissions']
        for cycle in range(1, args.cycles + 1):
            rig_input.MAPPING = None
            swipe(width * .22, height * .51, width * .88, height * .51, 700)
            if cycle in (1, args.cycles):
                shot(out / f'today-{cycle}.png')
            swipe(width * .88, height * .51, width * .22, height * .51, 500)
            swipe(width * .51, height * .29, width * .51, height * .83, 700)
            if cycle in (1, args.cycles):
                shot(out / f'search-{cycle}.png')
            home()
            values = snapshot()
            report['samples'].append({'cycle': cycle, 'elapsed': time.monotonic() - began, **values})
            validate(values, before)
            if values['droidvm_virtio_display_frames'] <= previous_frames:
                raise RuntimeError('display did not advance during the gesture cycle')
            previous_frames = values['droidvm_virtio_display_frames']
            if values['virtgpu_fenced_submissions'] == previous_submit:
                raise RuntimeError('3D GPU submissions did not advance; displayed swaps may be stale surfaces')
            previous_submit = values['virtgpu_fenced_submissions']
            report['completed_cycles'] = cycle
            if cycle % 5 == 0 or cycle == args.cycles:
                require_consumer()
                print(f'cycle {cycle}/{args.cycles}: {values["droidvm_virtio_display_frames"] - before["droidvm_virtio_display_frames"]} frames, no command errors or fallback', flush=True)
            (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        report['after'] = snapshot()
        validate(report['after'], before)
        report['delta'] = {k: report['after'][k] - before[k] for k in COUNTERS if COUNTERS[k] == 8}
        report['new_crashes'] = sorted(set(crashes.listing(crashes.REPORTS)) - before_crashes)
        relevant = ('backboardd-', 'SpringBoard-', 'Preferences-', 'MobileSafari-', 'com.apple.WebKit.')
        if any(Path(name).name.startswith(relevant) for name in report['new_crashes']):
            raise RuntimeError('a compositor or tested app wrote a new crash report')
        _, raw = peek.read('droidvm_virtio_present_samples', 64 * 40)
        report['present_tail'] = sorted([struct.unpack_from('<QIIIIQQ', raw, i * 40) for i in range(64)])
        shot(out / 'end.png')
        report['passed'] = True
    except (Exception, SystemExit) as error:
        report['error'] = str(error)
        raise
    finally:
        report['elapsed_seconds'] = time.monotonic() - began
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(out, flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
