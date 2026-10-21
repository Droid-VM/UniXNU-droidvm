#!/usr/bin/env bash
# Restart droidvmd the way the app does (DaemonHelper.startDaemon) when it has died, and recreate the UniXNU VM from
# unixnu-vm.config.json: the VM was created over IPC, so it is not in files/vms.json and a new daemon forgets it.
# The daemon can abort on a VM stop (fdsan: it closes a descriptor a FileOutputStream owns; app bug, seen 2026-10-21).
set -euo pipefail
. "$(dirname "$0")/rigenv.sh"
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
A="adb -s $PHONE"
RUN=/data/data/cn.classfun.droidvm/run
IPC=${DVMIPC:?set DVMIPC to the path of the DroidVM daemon IPC client dvmipc.py}
CONFIG=$(dirname "$0")/unixnu-vm.config.json
pid=$($A shell "su -c 'cat $RUN/droidvmd.pid 2>/dev/null'" | tr -dc 0-9)
# The pid file outlives a crash of the phone, and its number may belong to another process by now.
if [ -z "$pid" ] || ! $A shell "su -c 'grep -q cn.classfun.droidvm.daemon /proc/$pid/cmdline'" 2>/dev/null; then
  apk=$($A shell pm path cn.classfun.droidvm | sed 's/^package://' | tr -d '\r')
  lib="$(dirname "$apk")/lib/arm64:/data/data/cn.classfun.droidvm/lib"
  $A shell "su -c 'env CLASSPATH=\"$apk\" LD_LIBRARY_PATH=\"$lib\" setsid /data/data/cn.classfun.droidvm/bin/daemon /system/bin/app_process64 / cn.classfun.droidvm.daemon.Daemon </dev/null >/dev/null 2>&1 &'"
  sleep 3
fi
P=$($A shell "su -c 'cat $RUN/droidvmd-port.txt'" | tr -dc 0-9)
T=$($A shell "su -c 'cat $RUN/droidvmd-token.txt'" | tr -dc 0-9a-f)
$A forward tcp:$P tcp:$P >/dev/null
# A daemon that has just started takes a few seconds to answer; asking earlier ends in EOFError, not in an answer.
for i in $(seq 1 15); do python3 $IPC $P $T vm_list >/dev/null 2>&1 && break; sleep 2; done
id=$(python3 -c "import json;print(json.load(open('$CONFIG'))['config']['id'])")
if python3 $IPC $P $T vm_get "{\"vm_id\":\"$id\"}" | grep -q '"success": false'; then
  python3 $IPC $P $T vm_create "$(cat $CONFIG)" | grep -E '"success"'
fi
echo "daemon port $P; VM $id present"
