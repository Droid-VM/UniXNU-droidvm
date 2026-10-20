#!/usr/bin/env bash
# Coherent framebuffer capture of the UniXNU VM (memgrab: mmap the guest memfd + dc civac), saved as PNG.
#   fbgrab.sh OUT.png [WIDTH HEIGHT STRIDE MEMFD_OFFSET]
set -euo pipefail
OUT=$1; W=${2:-1280}; H=${3:-800}; STRIDE=${4:-5120}; OFF=${5:-0xffc00000}
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
cat > /tmp/fbgrab-phone.sh <<PHONE_EOF
pid=\$(for p in \$(pidof crosvm); do tr '\0' ' ' < /proc/\$p/cmdline | grep -q UniXNU && echo \$p; done | head -1)
fd=\$(for f in /proc/\$pid/fd/*; do case "\$(readlink \$f)" in *crosvm_guest*) echo \$f; break;; esac; done)
/data/local/tmp/memgrab \$fd $OFF $((STRIDE * H)) /data/local/tmp/fb.raw
PHONE_EOF
adb -s "$PHONE" push /tmp/fbgrab-phone.sh /data/local/tmp/fbgrab-phone.sh >/dev/null
adb -s "$PHONE" shell su -c 'sh /data/local/tmp/fbgrab-phone.sh'
adb -s "$PHONE" exec-out su -c 'cat /data/local/tmp/fb.raw' > /tmp/fb.raw
python3 - "$OUT" "$W" "$H" "$STRIDE" <<'PY'
import sys
from PIL import Image
out, w, h, stride = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
raw = open('/tmp/fb.raw', 'rb').read()
rows = b''.join(raw[y * stride:y * stride + w * 4] for y in range(h))
Image.frombytes('RGBA', (w, h), rows, 'raw', 'BGRA').convert('RGB').save(out)
print(out)
PY
