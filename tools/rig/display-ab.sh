#!/bin/bash
# display-ab.sh LABEL: whether the phone's display follows the guest: wake and unlock it (an idle guest's display sleeps: black),
# a frame, the guest opens Settings and then goes Home, a frame after each; prints the fraction of pixels that changed.
cd "$(dirname "$0")"; . ./rigenv.sh; S=out/display-ab; mkdir -p "$S"
shot() { timeout 60 adb -s $PHONE exec-out screencap -p > $S/ab-$1-$2.png; }
timeout 60 python3 home.py >/dev/null 2>&1; sleep 3; timeout 120 python3 input.py unlock >/dev/null 2>&1; sleep 4
shot $1 0
GSSH_TIMEOUT=60 ./guest-ssh.sh '/private/var/ngpu/bin/lsdiag -u 501 launch com.apple.Preferences' >/dev/null 2>&1; sleep 10; shot $1 1
timeout 60 python3 home.py >/dev/null 2>&1; sleep 8; shot $1 2
python3 - $S $1 <<'PY'
import sys
from PIL import Image
S, label = sys.argv[1:]
frames = [Image.open(f'{S}/ab-{label}-{i}.png').convert('L').resize((300, 212)) for i in range(3)]
d = lambda a, b: round(sum(abs(x - y) > 16 for x, y in zip(a.tobytes(), b.tobytes())) / (300 * 212), 3)
print(label, 'changed 0->1 (Settings)', d(frames[0], frames[1]), '1->2 (Home)', d(frames[1], frames[2]))
PY
