#!/usr/bin/env bash
# Catch what the screen shows while it moves: grab N framebuffers back to back on the phone (memgrab, ~25 ms apart) and, after PRE
# of them, have the phone itself start the input, so the frames bracket it with no adb latency in between.
#   frame-burst.sh OUTDIR N PRE swipe X0 Y0 X1 Y1 MS
#   frame-burst.sh OUTDIR N PRE tap X Y
# Coordinates are the guest's, as input.py takes them. Writes OUTDIR/fNN.png, times.txt (phone clock per frame), sheet.png (all
# frames, 5 to a row) and prints how many pixels changed between consecutive frames (0 = the screen did not move).
set -u
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
cd "$RIG"
. out/handoff.env
[ $# -ge 5 ] || { sed -n '2,8p' "$0"; exit 2; }
OUT=$1; N=$2; PRE=$3; shift 3
CMD=$(python3 - "$@" <<'PY'
import sys
args = sys.argv[1:]
sys.argv = ['input.py']
import input as touch
if args[0] == 'swipe' and len(args) == 6:
    print('input swipe %s %s %s %s %s' % (*touch.phone(args[1], args[2]), *touch.phone(args[3], args[4]), args[5]))
elif args[0] == 'tap' and len(args) == 3:
    print('input tap %s %s' % touch.phone(args[1], args[2]))
PY
)
[ -n "$CMD" ] || { echo "unknown input: $*"; exit 2; }
W=$FB_WIDTH; H=$FB_HEIGHT; S=$FB_STRIDE; OFF=$(printf '0x%x' $(( FB_BASE - 0x80000000 )))
mkdir -p "$OUT"; rm -f "$OUT"/f*.png
cat > out/burst-phone.sh <<PH
pid=\$(for p in \$(pidof crosvm); do tr '\0' ' ' < /proc/\$p/cmdline | grep -q UniXNU && echo \$p; done | head -1)
fd=\$(for f in /proc/\$pid/fd/*; do case "\$(readlink \$f)" in *crosvm_guest*) echo \$f; break;; esac; done)
rm -rf /data/local/tmp/burst; mkdir -p /data/local/tmp/burst
i=0; while [ \$i -lt $N ]; do [ \$i = $PRE ] && { $CMD & }; /data/local/tmp/memgrab \$fd $OFF $((S*H)) /data/local/tmp/burst/f\$i.raw; date +%s.%N >> /data/local/tmp/burst/t; i=\$((i+1)); done
wait
PH
adb -s "$PHONE" push out/burst-phone.sh /data/local/tmp/burst-phone.sh >/dev/null
adb -s "$PHONE" shell su -c 'sh /data/local/tmp/burst-phone.sh'
adb -s "$PHONE" shell su -c 'cat /data/local/tmp/burst/t' > "$OUT/times.txt"
adb -s "$PHONE" exec-out su -c 'cd /data/local/tmp/burst && tar cf - f*.raw' | tar xf - -C "$OUT"
python3 - "$OUT" "$N" "$W" "$H" "$S" <<'PY'
import sys
from PIL import Image, ImageChops
out, n, w, h, s = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
frames, moved = [], []
for i in range(n):
    raw = open(f'{out}/f{i}.raw', 'rb').read()
    rows = b''.join(raw[y * s:y * s + w * 4] for y in range(h))
    image = Image.frombytes('RGBA', (w, h), rows, 'raw', 'BGRA').convert('RGB')
    image.save(f'{out}/f{i:02d}.png')
    if frames:
        diff = ImageChops.difference(image, frames[-1]).convert('L').point(lambda v: 255 if v > 30 else 0)
        moved.append(f'{i}:{diff.histogram()[255]}')
    frames.append(image)
cols, tw, th = 5, w // 4, h // 4
sheet = Image.new('RGB', (cols * tw, (n + cols - 1) // cols * th), 'red')
for i, image in enumerate(frames):
    sheet.paste(image.resize((tw - 2, th - 2)), (i % cols * tw, i // cols * th))
sheet.save(f'{out}/sheet.png')
print('changed pixels per frame:', ' '.join(moved))
PY
rm -f "$OUT"/f*.raw
python3 -c "
t=[float(x) for x in open('$OUT/times.txt')]
print('frames',len(t),'over %.2f s, %.0f ms apart; input at frame $PRE: $CMD'%(t[-1]-t[0],1000*(t[-1]-t[0])/max(1,len(t)-1)))"
