#!/usr/bin/env python3
"""Measure existing swap/producer timing rings during repeatable UI gestures.

Default: profile with producer waiting enabled. --compare-no-wait runs a diagnostic
ON/OFF/OFF/ON comparison (OFF can flicker); always restores ON in finally. No new
GPU fences, screen recorder, or per-frame guest mailbox requests are introduced.
This measures guest swap completion cadence, not physical panel presentation.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import threading
import time

import export
import peek

RIG = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('soak', RIG / 'display-soak.py')
soak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(soak)


def distribution(values):
    values = sorted(values)
    if not values:
        return {'n': 0}
    return {'n': len(values), 'mean': sum(values) / len(values),
            **{name: values[round((len(values) - 1) * q)] for name, q in
               [('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)]}}


class Reader:
    def __init__(self):
        self.pid, self.maps = peek.crosvm_maps()
        self.addresses = {}

    def read(self, name, size):
        if name not in self.addresses:
            self.addresses[name] = export.physical(name)
        pa = self.addresses[name]
        page = pa & ~4095
        length = (pa - page + size + 4095) & ~4095
        raw = export.read_guest(self.pid, self.maps, page, length)
        result = raw[pa - page:pa - page + size]
        if len(result) != size:
            raise RuntimeError(f'short read of {name}')
        return result

    def set_mode(self, mode):
        export.write_guest(self.pid, self.maps, export.physical('droidvm_virtio_present_wait'), struct.pack('<I', mode))
        if struct.unpack('<I', self.read('droidvm_virtio_present_wait', 4))[0] != mode:
            raise RuntimeError('failed to set producer wait mode')


def run(reader, mode, cycles):
    reader.set_mode(mode)
    soak.home()
    soak.home()
    soak.require_consumer()
    start_swap = struct.unpack('<I', reader.read('__ZL14swapEventCount', 4))[0]
    start_present = struct.unpack('<Q', reader.read('droidvm_virtio_present_probes', 8))[0]
    start_gpu = struct.unpack('<8Q', reader.read('droidvm_virtgpu_stats', 64))[5]   # fenced 3D submissions
    swaps, presents, failures = {}, {}, []
    stop = threading.Event()

    def collect():
        try:
            while True:
                raw = reader.read('__ZL10swapEvents', 64 * 32)
                for i in range(64):
                    submitted, offered, done, number = struct.unpack_from('<QQQI4x', raw, i * 32)
                    if number > start_swap and (number - 1) % 64 == i and submitted <= offered <= done and done:
                        swaps[number] = [submitted, offered, done]
                raw = reader.read('droidvm_virtio_present_samples', 64 * 40)
                for i in range(64):
                    row = struct.unpack_from('<QIIIIQQ', raw, i * 40)
                    if row[0] > start_present and (row[0] - 1) % 64 == i:
                        presents[row[0]] = row
                if stop.wait(.5):
                    return
        except (Exception, SystemExit) as error:
            failures.append(str(error))

    thread = threading.Thread(target=collect, daemon=True)
    began = time.monotonic()
    thread.start()
    try:
        width, height = soak.rig_input.GUEST_WIDTH, soak.rig_input.GUEST_HEIGHT
        for _ in range(cycles):
            soak.rig_input.MAPPING = None
            soak.swipe(width * .22, height * .51, width * .88, height * .51, 700)
            soak.swipe(width * .88, height * .51, width * .22, height * .51, 500)
            soak.swipe(width * .51, height * .29, width * .51, height * .83, 700)
            soak.home()
        # Let the sampler capture the final completed swaps.
        time.sleep(.7)
    finally:
        stop.set()
        thread.join(timeout=65)
    if thread.is_alive() or failures:
        raise RuntimeError(f'timing sampler failed: {failures}')
    if not swaps or not presents:
        raise RuntimeError('no completed swaps sampled')
    end_gpu = struct.unpack('<8Q', reader.read('droidvm_virtgpu_stats', 64))[5]
    if end_gpu == start_gpu:
        raise RuntimeError('3D submissions did not advance; refusing stale-surface performance results')
    if any(row[3] != mode or row[4] for row in presents.values()):
        raise RuntimeError('mixed wait modes or failed present commands')
    ordered = sorted(swaps.items())
    intervals = [(b[1][2] - a[1][2]) / 1000 for a, b in zip(ordered, ordered[1:]) if b[0] == a[0] + 1]
    active = [x for x in intervals if x <= 100]
    return {'mode': mode, 'cycles': cycles, 'elapsed_seconds': time.monotonic() - began,
            'gpu_submitted_before': start_gpu, 'gpu_submitted_after': end_gpu,
            'captured_swaps': len(swaps), 'missing_swaps': ordered[-1][0] - ordered[0][0] + 1 - len(swaps),
            'completion_interval_ms': distribution(intervals),
            'interval_at_most_100ms': distribution(active),
            'interval_over_100ms': sum(x > 100 for x in intervals),
            'interval_over_33_4ms': sum(x > 33.4 for x in intervals),
            'swap_latency_ms': distribution([(v[2] - v[0]) / 1000 for v in swaps.values()]),
            'producer_wait_ms': distribution([r[5] / 1000 for r in presents.values()]),
            'flush_ms': distribution([r[6] / 1000 for r in presents.values()]),
            'swaps': ordered, 'presents': sorted(presents.values())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cycles', type=int, default=3)
    parser.add_argument('--compare-no-wait', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.cycles <= 100:
        parser.error('--cycles must be 1..100')
    out = RIG / 'out' / time.strftime('display-profile-%Y%m%d-%H%M%S.json')
    reader = Reader()
    report = {'passes': [], 'passed': False, 'notes': 'Guest swap completion cadence. Intervals include scripted pauses; <=100 ms subset excludes idle gaps but may also exclude severe stalls.'}
    try:
        baseline = soak.snapshot()
        soak.validate(baseline)
        for mode in ([1, 0, 0, 1] if args.compare_no_wait else [1]):
            result = run(reader, mode, args.cycles)
            report['passes'].append(result)
            print(json.dumps({k: v for k, v in result.items() if k not in ('swaps', 'presents')}), flush=True)
            out.write_text(json.dumps(report, indent=2) + '\n')
        reader.set_mode(1)
        report['after'] = soak.snapshot()
        soak.validate(report['after'], baseline)
        report['passed'] = True
    finally:
        reader.set_mode(1)
        out.write_text(json.dumps(report, indent=2) + '\n')
        print(out, flush=True)


if __name__ == '__main__':
    main()
