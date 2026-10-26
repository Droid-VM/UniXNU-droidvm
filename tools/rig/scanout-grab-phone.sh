# Phone side of screen-check.py (pushed and run as root): copy every udmabuf of the UniXNU VM's crosvm at least one
# 1366x1024 frame large (the virtio-gpu scanout surfaces) into DIR as FD-SIZE.raw, with dmabufgrab, and write DIR as a tar
# to stdout (diagnostics go to DIR.log). Only buffers up to MAX_BYTES (default 5% above MIN) count: with Metal on the virtio GPU
# (vgpu-3d=1) the guest's GPU buffers are udmabufs of crosvm too, hundreds of megabytes of them.
#   sh scanout-grab-phone.sh DIR [MIN_BYTES [MAX_BYTES]]
DIR=$1; MIN=${2:-5595136}; MAX=${3:-$((MIN + MIN / 20))}
pid=$(for p in $(pidof crosvm); do tr '\0' ' ' < /proc/$p/cmdline | grep -q UniXNU && echo $p; done | head -1)
rm -rf "$DIR"; mkdir -p "$DIR"; exec 2>"$DIR.log"
[ -n "$pid" ] || { echo "no UniXNU crosvm" >&2; exit 1; }
for f in /proc/$pid/fd/*; do
  n=${f##*/}
  grep -q udmabuf /proc/$pid/fdinfo/$n 2>/dev/null || continue
  size=$(sed -n 's/^size:[[:space:]]*//p' /proc/$pid/fdinfo/$n)
  [ "$size" -ge "$MIN" ] && [ "$size" -le "$MAX" ] || continue
  /data/local/tmp/dmabufgrab $pid $n 0 $size "$DIR/$n-$size.raw" || exit 1
done
tar -C "$DIR" -cf - .
