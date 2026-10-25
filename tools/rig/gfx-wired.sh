#!/usr/bin/env bash
# Runs each GFXBench test (gfx-suite.sh) and after each prints the guest's free/wired/throttled/compressed pages (vm_stat, in MiB)
# and the virtio-gpu clients' live resources (vgpu-clients.py totals), to see what grows from run to run.
#   gfx-wired.sh ID...
cd "$(dirname "$0")" && . ./rigenv.sh
snap() {
  local v
  v=$(GSSH_TIMEOUT=60 ./guest-ssh.sh '/usr/bin/vm_stat' 2>/dev/null | awk -F: '/free|wired|throttled|stored in compressor|occupied by compressor/ {gsub(/[ .]/, "", $2); printf "%s=%dM ", $1, $2 * 4096 / 1048576}')
  echo "   $1: $v| $(timeout 300 python3 vgpu-clients.py 2>&1 | tail -1)"
}
snap start
for t in "$@"; do
  ./gfx-suite.sh "$t" 2>&1 | grep -v 'Bad substitution'
  sleep 10
  snap "after $t"
done
