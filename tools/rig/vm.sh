#!/usr/bin/env bash
# Run the UniXNU VM on the phone and say how it is.
#   vm.sh restart [WAIT]   stop it, start it, wait (up to WAIT s, default 240) for the first frame or a panic, put its
#                          display on the phone's screen, unlock it, and print health.py
#   vm.sh start [WAIT]     the same, without stopping it first
#   vm.sh stop             stop it
#   vm.sh attach           put its display on the phone's screen again, until a touch reaches the guest
#   vm.sh health           health.py
# The VM is stopped the way the app's own stop does it, but without the daemon (which can abort on a VM stop;
# daemon-recover.sh brings it back): crosvm's `stop` on the VM's control socket, so crosvm exits through its devices'
# teardown -- the GPU device frees what the host's GPU holds of guest memory before that memory goes. Only a crosvm still
# there after STOP_WAIT seconds is killed. Each boot rewrites out/handoff.env from DarwinBoot's hand-off, which peek.py reads.
set -uo pipefail
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
VM=${VM:?set VM to the id of the UniXNU VM}
IPC=${DVMIPC:?set DVMIPC to the path of the DroidVM daemon IPC client dvmipc.py}
A="adb -s $PHONE"
RUN=/data/data/cn.classfun.droidvm/run
APP_USR=/data/data/cn.classfun.droidvm/usr
STOP_WAIT=${STOP_WAIT:-20}
POOL_SETTLE=${POOL_SETTLE:-10}
ACQUIRE_WAIT=${ACQUIRE_WAIT:-180}
POOL=/sys/module/gh_hugepage_reserve/parameters
export RIG_OUT=${RIG_OUT:-$RIG/out}

# The guest's RAM is served only from gh_hugepage_reserve's pool of 2 MiB pages (pool_want of them), never from the host's
# free memory. A stopped VM's pages go back to the pool by its release worker: what Gunyah still pins once the owner has been
# dead for its grace time is abandoned and stays out of the pool, and a VM started on a short pool is served short (the boot
# just before the 2026-10-22 host crash was that day's only one with 2 MiB collapses failing and a framebuffer not
# pool-served). So stop gives the module POOL_SETTLE s to reclaim and reports the pool, and start checks it first and, when
# short, asks the module to gather (acquire=3) before it starts anything. See hugepage-harness.md; hostwatch.sh watches runs.
pool() {
  $A shell "su -c 'cat $POOL/refill_stat $POOL/served_summary 2>/dev/null'" | tr -d '\r' | python3 -c "
import sys
kv = dict(line.strip().split('=', 1) for line in sys.stdin if '=' in line)
print(*(kv.get(k, '?') for k in ('pool_avail', 'pool_want', 'active_vms', 'live', 'orphan', 'acquire_active', 'state')))"
}
pool_line() {
  local avail want vms live orphan acquiring state
  read -r avail want vms live orphan acquiring state < <(pool)
  echo "pool $avail/$want 2 MiB pages, active VMs $vms, live $live, orphan $orphan, acquiring $acquiring"
}
have_pool() { [ -n "$($A shell "su -c 'test -d $POOL && echo y'")" ]; }
pool_whole() {
  local avail want vms live orphan acquiring state
  read -r avail want vms live orphan acquiring state < <(pool)
  echo "$(date '+%F %T') $1: avail=$avail want=$want vms=$vms live=$live orphan=$orphan acquiring=$acquiring" >> "$RIG_OUT/pool.log"
  [ "$avail" -ge "$want" ] 2>/dev/null && [ "$vms" = 0 ]
}
settle_pool() {
  have_pool || return 0
  sleep "$POOL_SETTLE"
  pool_whole after-stop && echo "pool whole ${POOL_SETTLE}s after the stop: $(pool_line)" \
    || echo "pool short ${POOL_SETTLE}s after the stop: $(pool_line); start will gather (acquire=3) first"
  $A shell "su -c 'cat $POOL/vm_owners'" | tr -d '\r' | sed 's/^/  owner: /'
}
# Before a start: the pool must be whole; when it is not, acquire=3 (sweep with per-window evict) and wait for that run to end.
ensure_pool() {
  local i
  have_pool || { echo "no gh_hugepage_reserve on the phone: no pool to check"; return 0; }
  pool_whole before-start && return 0
  echo "pool short before the start ($(pool_line)): acquire=3"
  $A shell "su -c 'echo 3 > $POOL/acquire'" || echo "acquire refused (a run is active, or the mode is unsupported)"
  for i in $(seq 1 "$ACQUIRE_WAIT"); do
    sleep 1
    read -r _ _ _ _ _ acquiring _ < <(pool)
    [ "$acquiring" = 0 ] && break
  done
  pool_whole after-acquire && { echo "pool whole after acquiring: $(pool_line)"; return 0; }
  echo "not starting: the pool is still short after acquiring ($(pool_line))"
  return 1
}

crosvm_pid() {
  $A shell "su -c 'for p in \$(pidof crosvm); do tr \"\\0\" \" \" < /proc/\$p/cmdline | grep -q UniXNU && echo \$p; done'" | tr -dc '0-9\n' | head -1
}

stop() {
  local pid name
  pid=$(crosvm_pid)
  [ -n "$pid" ] || return 0
  # The control socket is run/<VM name>.sock (CrosvmBackendInstance); the app's crosvm needs its own libraries.
  name=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['config']['name'])" "$RIG/unixnu-vm.config.json")
  $A shell "su -c 'LD_LIBRARY_PATH=$APP_USR/lib $APP_USR/bin/crosvm stop \"$RUN/$name.sock\"'" >/dev/null 2>&1
  echo "stop $(date +%s)" > "$RIG_OUT/vm-request"
  for i in $(seq 1 "$STOP_WAIT"); do
    [ -z "$(crosvm_pid)" ] && { echo "stopped crosvm $pid through its control socket"; settle_pool; return 0; }
    sleep 1
  done
  $A shell "su -c 'kill -9 $pid'" && echo "crosvm $pid had not exited after ${STOP_WAIT}s; killed it"
  settle_pool
}

ipc() {
  local port token
  port=$($A shell "su -c 'cat $RUN/droidvmd-port.txt'" | tr -dc 0-9)
  token=$($A shell "su -c 'cat $RUN/droidvmd-token.txt'" | tr -dc 0-9a-f)
  $A forward tcp:$port tcp:$port >/dev/null
  python3 "$IPC" "$port" "$token" "$@"
}

start() {
  local wait=${1:-240} pid="" frames=""
  [ -z "$(crosvm_pid)" ] || { echo "a UniXNU crosvm is already running"; return 1; }
  ensure_pool || return 1
  echo "start $(date +%s)" > "$RIG_OUT/vm-request"
  "$RIG/daemon-recover.sh" >/dev/null || { echo "the DroidVM daemon did not come back"; return 1; }
  ipc vm_console_clear "{\"vm_id\":\"$VM\"}" >/dev/null 2>&1
  for attempt in 1 2 3; do
    ipc vm_start "{\"vm_id\":\"$VM\"}" >/dev/null 2>&1
    for i in $(seq 1 10); do pid=$(crosvm_pid); [ -n "$pid" ] && break; sleep 1; done
    [ -n "$pid" ] && break
  done
  [ -n "$pid" ] || { echo "crosvm did not start"; return 1; }
  echo "crosvm $pid started"
  # DarwinBoot's hand-off (kernel and framebuffer addresses) is on the serial console a few seconds in.
  for i in $(seq 1 30); do
    port=$($A shell "su -c 'cat $RUN/droidvmd-port.txt'" | tr -dc 0-9); token=$($A shell "su -c 'cat $RUN/droidvmd-token.txt'" | tr -dc 0-9a-f)
    python3 "$RIG/console.py" "$port" "$token" "$VM" serial1 > "$RIG_OUT/console.txt" 2>/dev/null
    grep -q 'DARWINBOOT: video base' "$RIG_OUT/console.txt" && break
    sleep 2
  done
  python3 - "$RIG_OUT/console.txt" <<'PY'
import os, re, sys
text = open(sys.argv[1]).read()
arm = re.findall(r'DARWINBOOT: ARM64 va=0x([0-9A-F]+) pa=0x([0-9A-F]+)', text)
video = re.findall(r'DARWINBOOT: video base=0x([0-9A-F]+) display=\d+ row=(\d+) width=(\d+) height=(\d+)', text)
if arm and video:
    va, pa = arm[-1]
    base, row, width, height = video[-1]
    open(os.environ['RIG_OUT'] + '/handoff.env', 'w').write(
        f'KERNEL_VA=0x{va}\nKERNEL_PA=0x{pa}\nFB_BASE=0x{base}\nFB_STRIDE={row}\nFB_WIDTH={width}\nFB_HEIGHT={height}\n')
else:
    print('no DarwinBoot hand-off on the console; out/handoff.env kept')
PY
  for i in $(seq 1 $((wait / 5))); do
    sleep 5
    if [ "$(python3 "$RIG/peek.py" kernel_debugger_entry_count 4 2>/dev/null | awk '{print $2}')" != "00" ]; then
      echo "the kernel panicked while booting:"; python3 "$RIG/panic.py"; return 1
    fi
    frames=$(python3 "$RIG/peek.py" native_legacy_frame_count 8 --q 2>/dev/null | awk '{print $2}')
    [ -n "$frames" ] && [ "$frames" != 0x0000000000000000 ] && break
  done
  [ -n "$frames" ] && [ "$frames" != 0x0000000000000000 ] || echo "no frame after ${wait}s"
  sleep 15
  attach
  python3 "$RIG/health.py"
}

# Put the VM's display on the phone and make sure a touch reaches the guest: the first display after a restart often gets no
# input (the earlier activity, torn down after the new one binds, seems to take the VM's input with it), a second one does.
# The unlock swipe is the test: it must raise the guest's input event count.
attach() {
  local before after
  for attempt in 1 2 3; do
    "$RIG/show-vm.sh" >/dev/null 2>&1
    sleep 5
    before=$(python3 "$RIG/peek.py" droidvm_input_events 8 --q 2>/dev/null | awk '{print $2}')
    python3 "$RIG/input.py" unlock
    sleep 3
    after=$(python3 "$RIG/peek.py" droidvm_input_events 8 --q 2>/dev/null | awk '{print $2}')
    [ -n "$after" ] && [ "$after" != "$before" ] && { echo "display on the phone, input reaches the guest"; return 0; }
  done
  echo "display on the phone, but no input reaches the guest"
  return 1
}

case "${1:-}" in
  restart) stop; start "${2:-240}" ;;
  start) start "${2:-240}" ;;
  stop) stop ;;
  attach) attach ;;
  health) python3 "$RIG/health.py" ;;
  *) sed -n '2,12p' "$0"; exit 2 ;;
esac
