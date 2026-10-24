#!/usr/bin/env bash
# Bring the DroidVM app's native display of the UniXNU VM to the front of the phone (what its VM card's console opens),
# waking the phone and dismissing an insecure keyguard first. A VM restart closes that activity.
set -euo pipefail
. "$(dirname "$0")/rigenv.sh"
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
VM=${VM:?set VM to the id of the UniXNU VM}
A="adb -s $PHONE"
RIG=$(cd "$(dirname "$0")" && pwd)
read -r screen_id screen_width screen_height < <(python3 - "$RIG/unixnu-vm.config.json" <<'PY'
import json, sys
screens = json.load(open(sys.argv[1]))['config']['screens']
for name in ('gpu-0', 'simplefb'):
    screen = screens.get(name, {})
    if screen.get('enabled') and screen.get('exporter', '').lower() == 'native':
        print(name, int(screen['width']), int(screen['height']))
        break
else:
    sys.exit('the VM has no enabled native screen')
PY
)
$A shell 'input keyevent KEYCODE_WAKEUP; dumpsys window | grep -q "isKeyguardShowing=true" && input keyevent 82; true'
# --activity-clear-task: a fresh activity each time. Otherwise am start resumes one of the earlier instances (they pile up in the
# task), whose input is still bound to the crosvm of the VM's previous start, so no touch reaches a restarted guest.
$A shell "su -c 'am start --activity-clear-task -n cn.classfun.droidvm/.ui.vm.display.nativedisplay.display.VMNativeDisplayActivity \
  --es vm_id $VM --es vm_name \"UniXNU (DroidVM)\" --es screen $screen_id --ez input_enabled true \
  --ei display_width $screen_width --ei display_height $screen_height'" >/dev/null
sleep 2
$A shell 'dumpsys activity activities | grep -m1 topResumedActivity'
