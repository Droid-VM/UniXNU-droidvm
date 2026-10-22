#!/usr/bin/env bash
# Run the UniXNU VM on the phone and say how it is.
#   vm.sh restart [WAIT]   stop it, start it, wait (up to WAIT s, default 240) for the first frame or a panic, put its
#                          display on the phone's screen, unlock it, and print health.py
#   vm.sh start [WAIT]     the same, without stopping it first
#   vm.sh stop             kill its crosvm
#   vm.sh attach           put its display on the phone's screen again, until a touch reaches the guest
#   vm.sh health           health.py
# The VM is stopped by killing its crosvm, not with vm_stop: the daemon can abort on a VM stop (daemon-recover.sh), which then
# brings it back and recreates the VM. Each boot rewrites out/handoff.env from DarwinBoot's hand-off, which peek.py reads.
set -uo pipefail
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
VM=${VM:?set VM to the id of the UniXNU VM}
IPC=${DVMIPC:?set DVMIPC to the path of the DroidVM daemon IPC client dvmipc.py}
A="adb -s $PHONE"
RUN=/data/data/cn.classfun.droidvm/run
export RIG_OUT=${RIG_OUT:-$RIG/out}

crosvm_pid() {
  $A shell "su -c 'for p in \$(pidof crosvm); do tr \"\\0\" \" \" < /proc/\$p/cmdline | grep -q UniXNU && echo \$p; done'" | tr -dc '0-9\n' | head -1
}

stop() {
  local pid
  pid=$(crosvm_pid)
  [ -n "$pid" ] && $A shell "su -c 'kill -9 $pid'" && echo "stopped crosvm $pid"
  sleep 2
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
  *) sed -n '2,9p' "$0"; exit 2 ;;
esac
