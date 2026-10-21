#!/usr/bin/env bash
# Make the disk of the persistent Data store on the phone (package/DroidVMDataStore.c): a sparse file whose 4096-byte block 0
# is the label "DroidVMDataStore" + version 1, the window after it. Only the blocks the guest writes take space on the phone.
#   make-data-store.sh [SIZE_GIB, default 16]      refuses to touch an existing file (delete it by hand to start over)
set -euo pipefail
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
FILE=${FILE:-/data/media/0/DroidVM/UniXNU/data-store.img}
GIB=${1:-16}
A="adb -s $PHONE"
if $A shell "su -c 'test -e $FILE'"; then
  echo "$FILE exists; not touched" >&2
  exit 1
fi
# The label in printf octal escapes: the magic, then version 1 as a little-endian u32.
label='DroidVMDataStore\001\000\000\000'
$A shell "su -c 'truncate -s ${GIB}G $FILE && printf \"$label\" | dd of=$FILE bs=4096 conv=notrunc,sync count=1 2>/dev/null && \
  chown \$(stat -c %u:%g $(dirname $FILE)) $FILE && chmod 660 $FILE && ls -ls $FILE && od -A d -c -N 20 $FILE'"
