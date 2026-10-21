#!/usr/bin/env bash
# Bring the DroidVM app's native display of the UniXNU VM to the front of the phone (what its VM card's console opens),
# waking the phone and dismissing an insecure keyguard first. A VM restart closes that activity.
set -euo pipefail
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
VM=${VM:?set VM to the id of the UniXNU VM}
A="adb -s $PHONE"
$A shell 'input keyevent KEYCODE_WAKEUP; dumpsys window | grep -q "isKeyguardShowing=true" && input keyevent 82; true'
$A shell "su -c 'am start -n cn.classfun.droidvm/.ui.vm.display.nativedisplay.display.VMNativeDisplayActivity \
  --es vm_id $VM --es vm_name \"UniXNU (DroidVM)\" --es screen simplefb --ez input_enabled true \
  --ei display_width 1280 --ei display_height 800'" >/dev/null
sleep 2
$A shell 'dumpsys activity activities | grep -m1 topResumedActivity'
