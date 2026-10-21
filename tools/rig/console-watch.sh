#!/usr/bin/env bash
# Keep a copy of the VM's serial console on this machine while it boots: every 2 s the newest history replaces
# out/console-live.txt, and each change is stamped with the host time in out/console-live.times, so a phone that
# goes down mid-boot still leaves the last guest line and when it came.   console-watch.sh [SECONDS, default 300]
set -uo pipefail
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
VM=${VM:?set VM to the id of the UniXNU VM}
O=$(dirname "$0")/out
A="timeout 20 adb -s $PHONE"
RUN=/data/data/cn.classfun.droidvm/run
P=$($A shell "su -c 'cat $RUN/droidvmd-port.txt'" | tr -dc 0-9)
T=$($A shell "su -c 'cat $RUN/droidvmd-token.txt'" | tr -dc 0-9a-f)
$A forward tcp:$P tcp:$P >/dev/null
: > $O/console-live.times
end=$((SECONDS + ${1:-300}))
last=0
while [ $SECONDS -lt $end ]; do
  if timeout 15 python3 $(dirname "$0")/console.py $P $T $VM serial1 > $O/console-live.new 2>/dev/null; then
    size=$(stat -c %s $O/console-live.new)
    if [ "$size" != "$last" ]; then
      mv $O/console-live.new $O/console-live.txt
      echo "$(date +%T) $size $(tail -n 1 $O/console-live.txt | cut -c1-160)" >> $O/console-live.times
      last=$size
    fi
  else
    echo "$(date +%T) no answer" >> $O/console-live.times
  fi
  sleep 2
done
