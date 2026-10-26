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
sleep 3
# The app hands crosvm the display Surface from the new instance before the instance it replaces stops, and the old one's
# surfaceDestroyed then calls removeSurface, which takes away the new Surface: the phone keeps the last frame while the guest
# runs on (DisplayProvider; every relaunch over a live console froze it). Sending the survivor to the background and back makes
# it remove and re-send its own Surface, in that order. SHOWVM_NO_CYCLE=1 skips it (to test an app that owns its Surface).
if [ "${SHOWVM_NO_CYCLE:-0}" != 1 ]; then
  $A shell 'input keyevent KEYCODE_HOME' >/dev/null
  sleep 2
  $A shell "su -c 'am start --activity-reorder-to-front -n cn.classfun.droidvm/.ui.vm.display.nativedisplay.display.VMNativeDisplayActivity'" >/dev/null 2>&1
  sleep 2
fi
$A shell 'dumpsys activity activities | grep -m1 topResumedActivity'
