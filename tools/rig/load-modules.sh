#!/usr/bin/env bash
# Load the DroidVM app's kernel modules the way the app does at its start (its autostart set in droidvm_prefs.xml), for a
# phone that rebooted and whose app UI has not been opened since. Without the Gunyah Host Share module a pseudo-unprotected
# VM dies at once: "failed to initialize virtual machine (os error 2)".
set -euo pipefail
. "$(dirname "$0")/rigenv.sh"
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
A="adb -s $PHONE"
$A shell "su -c '
M=/data/data/cn.classfun.droidvm/usr/lib/modules/\$(ls /data/data/cn.classfun.droidvm/usr/lib/modules | grep -E \"^android\" | head -1)
uid=\$(stat -c %u /data/data/cn.classfun.droidvm)
for ko in udmabuf gh-unmovable gunyah-host-share gunyah-kvcalloc nproc-guard; do
  name=\$(echo \$ko | tr - _)
  if lsmod | grep -q \"^\$name\"; then echo \"\$ko already loaded\"; continue; fi
  args=; [ \$ko = nproc-guard ] && args=\"uid=\$uid\"
  f=\$(ls \$M/\$ko-*.ko 2>/dev/null | head -1)
  [ -n \"\$f\" ] && insmod \$f \$args && echo \"loaded \$ko\" || echo \"FAILED \$ko\"
done'"
