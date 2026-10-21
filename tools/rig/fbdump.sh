#!/usr/bin/env bash
# Read the UniXNU VM's simplefb straight out of crosvm's guest memory and save it as PNG.
#   fbdump.sh OUT.png [WIDTH HEIGHT STRIDE FBOFFSET]
set -euo pipefail
. "$(dirname "$0")/rigenv.sh"
OUT=$1; W=${2:-1280}; H=${3:-800}; STRIDE=${4:-5120}; OFF=${5:-ffc00000}
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
cat > /tmp/fbdump-phone.sh <<PHONE_EOF
pid=\$(for p in \$(pidof crosvm); do tr '\0' ' ' < /proc/\$p/cmdline | grep -q 'UniXNU' && echo \$p; done | head -1)
[ -n "\$pid" ] || { echo "no UniXNU crosvm" >&2; exit 1; }
start=\$(grep "rw-s $OFF .*crosvm_guest" /proc/\$pid/maps | head -1 | cut -d- -f1)
[ -n "\$start" ] || { echo "no fb mapping" >&2; exit 1; }
dd if=/proc/\$pid/mem of=/data/local/tmp/fb.raw bs=4096 skip=\$((0x\${start%???})) count=$(( (STRIDE * H + 4095) / 4096 )) 2>/dev/null
PHONE_EOF
adb -s "$PHONE" push /tmp/fbdump-phone.sh /data/local/tmp/fbdump-phone.sh >/dev/null
adb -s "$PHONE" shell su -c 'sh /data/local/tmp/fbdump-phone.sh'
adb -s "$PHONE" exec-out su -c 'cat /data/local/tmp/fb.raw' > /tmp/fb.raw
python3 - "$OUT" "$W" "$H" "$STRIDE" <<'PY'
import sys
from PIL import Image
out, w, h, stride = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
raw = open('/tmp/fb.raw', 'rb').read()
rows = b''.join(raw[y * stride:y * stride + w * 4] for y in range(h))
Image.frombytes('RGBA', (w, h), rows, 'raw', 'BGRA').convert('RGB').save(out)
print(out, len(raw), 'bytes')
PY
