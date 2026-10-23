#!/usr/bin/env bash
# The guest's newest unified log, parsed: fetch only the newest N Persist tracev3 files (plus timesync) into a fresh archive that
# hard-links the uuidtext/dsc of a full mirror (logarchive.py LOG_BASE once, default out/guest.logarchive), parse it with
# mandiant/macos-UnifiedLogs' unifiedlog_iterator (UNIFIEDLOG) into OUT.jsonl, and fetch any uuidtext the mirror lacks (a rebuilt
# Metal plugin or owner tool) before parsing again. Much faster than a full mirror when only the last minutes matter.
#   guest-newlogs.sh [N] [OUT.jsonl]       defaults: 2, out/newlogs.jsonl
# A new Data seed restarts the guest's tracev3 numbering: mirror LOG_BASE afresh then, or old and new files mix.
set -u
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
cd "$RIG"
N=${1:-2}; OUT=${2:-$RIG/out/newlogs.jsonl}
PARSER=${UNIFIEDLOG:?set UNIFIEDLOG to the unifiedlog_iterator binary}
A=${LOG_BASE:-$RIG/out/guest.logarchive}; B=$RIG/out/newlogs.logarchive
[ -d "$A" ] || { echo "no $A: run logarchive.py $A first"; exit 1; }
rm -rf "$B"; mkdir -p "$B/Persist" "$B/Special" "$B/timesync"
for e in "$A"/*; do n=$(basename "$e"); case $n in Persist|Special|Signpost|HighVolume|timesync) ;; *) cp -al "$e" "$B/" ;; esac; done
for f in $(timeout 100 python3 export.py ls /private/var/db/diagnostics/timesync 2>/dev/null | awk '{print $2}'); do
  timeout 100 python3 export.py get "/private/var/db/diagnostics/timesync/$f" "$B/timesync/$f" >/dev/null 2>&1; done
for f in $(timeout 100 python3 export.py ls /private/var/db/diagnostics/Persist 2>/dev/null | awk '{print $2}' | sort | tail -"$N"); do
  timeout 300 python3 export.py get "/private/var/db/diagnostics/Persist/$f" "$B/Persist/$f" >/dev/null 2>&1; done
rm -f "$OUT"
timeout 900 "$PARSER" -m log-archive -i "$B" -o "$OUT" -f jsonl >/dev/null 2>&1
MISSING=$(grep -oE 'UUIDText file: [0-9A-F]{32}' "$OUT" | awk '{print $NF}' | sort -u)
if [ -n "$MISSING" ]; then
  for u in $MISSING; do d=${u:0:2}; f=${u:2}; mkdir -p "$A/$d" "$B/$d"
    timeout 200 python3 export.py get "/private/var/db/uuidtext/$d/$f" "$A/$d/$f" >/dev/null 2>&1 && ln -f "$A/$d/$f" "$B/$d/$f"; done
  rm -f "$OUT"
  timeout 900 "$PARSER" -m log-archive -i "$B" -o "$OUT" -f jsonl >/dev/null 2>&1
fi
ls -la "$OUT"
