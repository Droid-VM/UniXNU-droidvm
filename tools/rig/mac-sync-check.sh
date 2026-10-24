#!/usr/bin/env bash
# Whether the build Mac builds the source that is here: its device repository (BUILD_REPO) and UniXNU checkout (BUILD_OUT/../..)
# must be at the same commits as REPO and FORK, with the same uncommitted edits and the same submodule commits and edits.
# The Mac's checkouts are moved by hand (git bundles; pushes go to wip branches only), so they drift. Silent on a match; else one
# line per difference and exit 1. Untracked files are not compared, nor tools/rig (the rig does not build anything). The build tree's
# copies of scripts and patches must equal their sources.
#   mac-sync-check.sh          SYNC_DEBUG=1 also prints how many facts each side reported
set -u
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
MAC=${BUILD_HOST:?set BUILD_HOST to the build Mac, user@host}
M=${BUILD_OUT:?set BUILD_OUT to out/droidvm of the UniXNU checkout on the build Mac}
MREPO=${BUILD_REPO:?set BUILD_REPO to the device repository checkout on the build Mac}
F=${FORK:?set FORK to the local UniXNU fork checkout}
DEV=${REPO:-$RIG/../..}
# One "KEY VALUE" line per fact: HEAD, each edited tracked file's blob hash, each submodule's commit and edited files.
STATE=$(cat <<'SH'
cd "$1" || exit 1
blob() { if [ -e "$1" ]; then git hash-object "$1"; else echo deleted; fi; }
echo "HEAD $(git rev-parse HEAD)"
git diff --name-only --ignore-submodules=all HEAD -- . ':(exclude)tools/rig' | while IFS= read -r f; do echo "file:$f $(blob "$f")"; done
git submodule foreach --quiet 'echo "sub:$sm_path $(git rev-parse HEAD)"
  git diff --name-only HEAD | while IFS= read -r f; do
    if [ -e "$f" ]; then echo "subfile:$sm_path/$f $(git hash-object "$f")"; else echo "subfile:$sm_path/$f deleted"; fi
  done'
SH
)
status=0
for pair in "device $DEV $MREPO" "UniXNU $F $M/../.."; do
  set -- $pair
  here=$(bash -c "$STATE" _ "$2") || { echo "$1: cannot read $2"; exit 1; }
  mac=$(ssh "$MAC" bash -s -- "$3" <<<"$STATE") || { echo "$1: cannot read $3 on $MAC"; exit 1; }
  [ -n "${SYNC_DEBUG:-}" ] && printf "%s: here %s lines, Mac %s lines\n" "$1" "$(wc -l <<<"$here")" "$(wc -l <<<"$mac")"
  # A submodule checked out on one side only (e.g. DarwinBoot kept in a worktree elsewhere here) is not compared.
  python3 - "$1" "$here" "$mac" <<'PY' || status=1
import sys
label, here, mac = sys.argv[1], *(dict(l.split(' ', 1) for l in t.splitlines() if ' ' in l) for t in sys.argv[2:])
subs = {k[4:] for k in here if k.startswith('sub:')} & {k[4:] for k in mac if k.startswith('sub:')}
def compared(key):
    if key.startswith('sub:'):
        return key[4:] in subs
    if key.startswith('subfile:'):
        return any(key[8:].startswith(s + '/') for s in subs)
    return True
bad = sorted(k for k in here.keys() | mac.keys() if compared(k) and here.get(k) != mac.get(k))
for k in bad:
    print(f'{label}: {k}: here {here.get(k, "-")[:12]}, Mac {mac.get(k, "-")[:12]}')
sys.exit(1 if bad else 0)
PY
done
# The build tree (BUILD_OUT/tree) holds copies of both checkouts' scripts and UniXNU's patches; the rest of it links back.
ssh "$MAC" bash -s -- "$M" "$MREPO" <<'SH' || status=1
T=$1/tree; stale=0
for f in "$2"/scripts/* "$1"/../../scripts/* "$1"/../../patches/*/*; do
  [ -f "$f" ] || continue
  case $f in */patches/*) copy=$T/patches/${f#*/patches/} ;; *) copy=$T/scripts/${f##*/} ;; esac
  cmp -s "$f" "$copy" || { echo "build tree: ${copy#$T/} differs from $f"; stale=1; }
done
exit $stale
SH
exit $status
