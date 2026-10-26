#!/system/bin/sh
# Run vmguard.sh detached from adb (it survives adb reconnects), writing /data/local/tmp/vmguard.log; args pass through.
pkill -f vmguard; sleep 1
nohup setsid sh /data/local/tmp/${GUARD:-vmguard2.sh} "$@" > /data/local/tmp/vmguard.log 2>&1 < /dev/null &
sleep 1; pgrep -f vmguard
