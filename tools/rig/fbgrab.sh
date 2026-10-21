#!/usr/bin/env bash
# Coherent framebuffer capture of the UniXNU VM (memgrab: mmap the guest memfd + dc civac), saved as PNG.
#   fbgrab.sh OUT.png [WIDTH HEIGHT STRIDE MEMFD_OFFSET]
set -euo pipefail
. "$(dirname "$0")/rigenv.sh"
OUT=$1
[ -f $(dirname "$0")/out/handoff.env ] && . $(dirname "$0")/out/handoff.env
W=${2:-${FB_WIDTH:-1280}}; H=${3:-${FB_HEIGHT:-800}}; STRIDE=${4:-${FB_STRIDE:-5120}}
OFF=${5:-$(printf '0x%x' $(( ${FB_BASE:-0x17fc00000} - 0x80000000 )))}
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
