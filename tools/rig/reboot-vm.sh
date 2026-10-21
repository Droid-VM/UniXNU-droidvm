#!/usr/bin/env bash
# Restart the UniXNU VM on the phone and print its serial console after WAIT seconds.
#   reboot-vm.sh [WAIT]
set -uo pipefail
. "$(dirname "$0")/rigenv.sh"
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
VM=${VM:?set VM to the id of the UniXNU VM}
WAIT=${1:-30}
IPC=${DVMIPC:?set DVMIPC to the path of the DroidVM daemon IPC client dvmipc.py}
A="adb -s $PHONE"
RUN=/data/data/cn.classfun.droidvm/run
P=$($A shell "su -c 'cat $RUN/droidvmd-port.txt'" | tr -dc 0-9)
T=$($A shell "su -c 'cat $RUN/droidvmd-token.txt'" | tr -dc 0-9a-f)
$A forward tcp:$P tcp:$P >/dev/null
python3 $IPC $P $T vm_stop "{\"vm_id\":\"$VM\"}" >/dev/null 2>&1
for i in $(seq 1 30); do
  python3 $IPC $P $T vm_list 2>/dev/null | grep -A3 "\"$VM\"" >/dev/null || true
  state=$(python3 $IPC $P $T vm_list 2>/dev/null | python3 -c "import sys,json;s=sys.stdin.read();d=json.loads(s[s.index(chr(10)+'{')+1:]);print([v['state'] for v in d['data'] if v['id']=='$VM'][0])")
  [ "$state" = STOPPED ] && break
  sleep 2
done
python3 $IPC $P $T vm_console_clear "{\"vm_id\":\"$VM\"}" >/dev/null 2>&1
$A shell 'input keyevent KEYCODE_WAKEUP' >/dev/null
python3 $IPC $P $T vm_start "{\"vm_id\":\"$VM\"}" | grep -q '"success": true' || echo "vm_start failed"
sleep "$WAIT"
python3 $(dirname "$0")/console.py $P $T $VM serial1 | tee /tmp/reboot-vm-console.txt
# The last DarwinBoot hand-off of this console: where the kernel and the framebuffer are (peek.py, fbgrab.sh).
RIG_OUT="$(dirname "$0")/out" RIG_OUT="$(dirname "$0")/out" python3 - <<'PY'
import re
text = open('/tmp/reboot-vm-console.txt').read()
arm = re.findall(r'DARWINBOOT: ARM64 va=0x([0-9A-F]+) pa=0x([0-9A-F]+)', text)
video = re.findall(r'DARWINBOOT: video base=0x([0-9A-F]+) display=\d+ row=(\d+) width=(\d+) height=(\d+)', text)
if arm and video:
    va, pa = arm[-1]
    base, row, width, height = video[-1]
    open(__import__('os').environ['RIG_OUT'] + '/handoff.env', 'w').write(
        f'KERNEL_VA=0x{va}\nKERNEL_PA=0x{pa}\nFB_BASE=0x{base}\nFB_STRIDE={row}\nFB_WIDTH={width}\nFB_HEIGHT={height}\n')
PY
