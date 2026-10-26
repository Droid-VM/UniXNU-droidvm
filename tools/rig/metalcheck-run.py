#!/usr/bin/env python3
"""Run the preinstalled Metal Check CLI and record GPU faults and memory pressure.

Requires a running guest and hostwatch.py. --stress-seconds enables the sustained
render/compute/blit loop; --memory-mib is additional resident GPU-visible memory.
No driver settings or fence policy are changed. UI usage does not need this tool.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import time

import export
import peek

RIG = Path(__file__).resolve().parent
FIELDS = {
    'kernel_debugger_entry_count': 'I', 'droidvm_virtio_display_frames': 'Q',
    'droidvm_virtio_gpu_errors': 'Q', 'droidvm_virtio_gpu_timeouts': 'Q',
    'native_legacy_early_failures': 'Q', 'droidvm_virtio_present_wait': 'I',
    'native_adreno_pool_stats': '8I', 'native_adreno_kept': '6Q',
    'vm_page_free_count': 'I', 'vm_page_active_count': 'I',
    'vm_page_inactive_count': 'I', 'vm_page_wire_count': 'I',
    'vm_page_purgeable_count': 'I', 'memorystatus_level': 'I',
    'droidvm_virtgpu_stats': '8Q',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--benchmark', action='store_true')
    mode.add_argument('--stress-seconds', type=int)
    parser.add_argument('--memory-mib', type=int, default=128)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.stress_seconds is not None and not 1 <= args.stress_seconds <= 86400:
        parser.error('--stress-seconds must be 1..86400')
    if not 0 <= args.memory_mib <= 512:
        parser.error('--memory-mib must be 0..512')
    out = args.output or RIG / 'out' / ('metalcheck-' + time.strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True, exist_ok=False)
    pid, maps = peek.crosvm_maps()
    addresses = {name: peek.symbol(name) - peek.KERNEL_VA + peek.KERNEL_PA for name in FIELDS}

    def snapshot():
        row = {'time': time.time(), 'crosvm_pid': pid}
        pages = {}
        for name, fmt in FIELDS.items():
            pa = addresses[name]
            size = struct.calcsize('<' + fmt)
            body = b''
            for page in range(pa & ~4095, ((pa + size - 1) & ~4095) + 1, 4096):
                if page not in pages:
                    pages[page] = export.read_guest(pid, maps, page, 4096)
                    if len(pages[page]) != 4096:
                        raise RuntimeError('short kernel sample')
                body += pages[page]
            values = struct.unpack_from('<' + fmt, body, pa & 4095)
            row[name] = values[0] if len(values) == 1 else list(values)
        row['host_memory'] = peek.adb('cat /proc/pressure/memory /proc/meminfo').decode(errors='replace')
        return row

    baseline = snapshot()
    if baseline['kernel_debugger_entry_count']:
        raise SystemExit('guest must be healthy')
    if baseline['native_legacy_early_failures'] or baseline['droidvm_virtio_present_wait'] != 1:
        raise SystemExit('guest must wait for GPU producers, with no failed display copy')
    guest_report = '/private/var/ngpu/metalcheck-' + str(time.time_ns()) + '.json'
    command = ['/private/var/ngpu/apps/MetalCheck.app/MetalCheck', '--cli', '--output', guest_report]
    if args.benchmark:
        command.append('--benchmark')
    if args.stress_seconds is not None:
        command += ['--stress-seconds', str(args.stress_seconds), '--memory-mib', str(args.memory_mib)]
    failures = []
    rows = [baseline]
    env = dict(os.environ, GSSH_TIMEOUT=str((args.stress_seconds or 0) + 600))
    with (out / 'stdout.log').open('w') as stdout, (out / 'progress.log').open('w') as stderr, (out / 'pressure.jsonl').open('w') as samples:
        samples.write(json.dumps(baseline) + '\n')
        with subprocess.Popen([str(RIG / 'guest-ssh.sh'), shlex.join(command)], env=env, stdout=stdout, stderr=stderr) as proc:
            while True:
                try:
                    row = snapshot()
                    rows.append(row)
                    samples.write(json.dumps(row) + '\n')
                    samples.flush()
                    for name in ('kernel_debugger_entry_count', 'droidvm_virtio_gpu_errors', 'droidvm_virtio_gpu_timeouts', 'native_legacy_early_failures'):
                        if row[name] != baseline[name] and name not in failures:
                            failures.append(name)
                except Exception as error:
                    failures.append('sampling: ' + str(error))
                if proc.poll() is not None:
                    break
                time.sleep(5)
            returncode = proc.returncode
    result = subprocess.run([str(RIG / 'guest-ssh.sh'), '/private/var/ngpu/bin/toybox cat ' + shlex.quote(guest_report)],
                            capture_output=True, text=True, timeout=100)
    app = None
    try:
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
        app = json.loads(result.stdout)
        (out / 'app.json').write_text(json.dumps(app, indent=2) + '\n')
    except (ValueError, RuntimeError) as error:
        failures.append('could not retrieve app report: ' + str(error))
    if returncode != 0:
        failures.append('guest command exit ' + str(returncode))
    if not app or not app.get('passed'):
        failures.append('Metal Check did not pass')
    report = {'passed': not failures, 'failures': failures, 'guest_report': guest_report,
              'validation_scope': 'Metal CPU oracles and kernel error counters; onscreen correctness requires a separate visual check',
              'visual_check': 'NOT_ASSESSED',
              'sample_count': len(rows), 'app': app, 'baseline': baseline, 'final': rows[-1],
              'guest_page_size': 4096, 'minimum_guest_free_mib': min(r['vm_page_free_count'] for r in rows) / 256,
              'memory_note': 'Free pages alone exclude reclaimable memory and do not establish OOM.'}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(('PASS API/counters; display not validated' if report['passed'] else 'FAIL') + ': ' + str(out / 'report.json'), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
