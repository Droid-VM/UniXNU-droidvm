#!/usr/bin/env bash
# Runs GFXBench tests one after another (gfx-suite.sh) and after each prints the host's KGSL mapped bytes and crosvm dmabuf fds
# and the guest's virtio-gpu close counters (closed, deferred, waiting, connections still busy after 5 s), to see whether
# anything accumulates across runs.
#   gfx-soak.sh ID...
cd "$(dirname "$0")" && . ./rigenv.sh
adb -s "$PHONE" push phone-hostmem.sh /data/local/tmp/hostmem.sh >/dev/null || exit 1
for t in "$@"; do
  ./gfx-suite.sh "$t" 2>&1 | grep -v 'Bad substitution'
  sleep 5
  h=$(timeout 60 adb -s "$PHONE" shell su -c 'sh /data/local/tmp/hostmem.sh' 2>/dev/null | grep -E 'dmabuf fds|kgsl mapped|MemAvailable' | tr -s ' ' | tr '\n' ' ')
  g=$(timeout 60 python3 -c "
import sys, struct; sys.path.insert(0, '.'); import rigenv, peek
pa, d = peek.read('droidvm_virtgpu_closes', 32); print('closes', struct.unpack('<4Q', d))" 2>&1 | tail -1)
  echo "   after $t: $h| $g"
done
