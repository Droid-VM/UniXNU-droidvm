#!/system/bin/sh
# SIGKILL the (stopped) UniXNU crosvm.
for p in $(pidof crosvm); do tr '\0' ' ' < /proc/$p/cmdline | grep -q UniXNU && P=$p; done
[ -n "$P" ] && grep State /proc/$P/status && kill -9 $P && echo "killed $P"
