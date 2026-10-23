#!/usr/bin/env bash
# Replace the DroidVM app's crosvm with a local build (CROSVM_BIN, e.g. the meta repo's crosvm_out/crosvm after
# 2_build_crosvm.sh). The VM must be stopped first (vm.sh stop). Keeps the current binary as crosvm.bak-<md5 prefix>, gives the
# new one the old one's owner, mode and SELinux label, and checks the copy. The guest kernel and crosvm must agree on the
# droidvm-gpu wire version (DroidVMGpuWire.h vs devices/src/virtio/droidvm_gpu.rs).
set -eu
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
NEW=${CROSVM_BIN:?set CROSVM_BIN to the crosvm binary to install}
DST=/data/data/cn.classfun.droidvm/usr/bin/crosvm
[ -f "$NEW" ] || { echo "no $NEW"; exit 1; }
if adb -s "$PHONE" shell "su -c 'pidof crosvm'" | grep -q .; then echo "crosvm is still running: vm.sh stop first"; exit 1; fi
OLD=$(adb -s "$PHONE" shell "su -c 'md5sum $DST'" | cut -c1-8)
OWNER=$(adb -s "$PHONE" shell "su -c 'stat -c %u:%g $DST'" | tr -d '\r')
LABEL=$(adb -s "$PHONE" shell "su -c 'ls -Z $DST'" | tr -d '\r' | cut -d' ' -f1)
adb -s "$PHONE" shell "su -c 'cp $DST $DST.bak-$OLD'"
adb -s "$PHONE" push "$NEW" /data/local/tmp/crosvm.new >/dev/null
adb -s "$PHONE" shell "su -c 'cp /data/local/tmp/crosvm.new $DST && rm /data/local/tmp/crosvm.new && chown $OWNER $DST && chmod 755 $DST && chcon $LABEL $DST'"
L=$(md5sum "$NEW" | cut -c1-32); R=$(adb -s "$PHONE" shell "su -c 'md5sum $DST'" | cut -c1-32)
[ "$L" = "$R" ] && echo "crosvm deployed $L (owner $OWNER, label $LABEL, backup crosvm.bak-$OLD)" || { echo "crosvm MISMATCH"; exit 1; }
