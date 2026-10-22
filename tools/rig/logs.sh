#!/usr/bin/env bash
# Read the guest's unified log with macOS's own `log show`: mirror it (logarchive.py, only what changed), copy the archive
# to the build Mac (BUILD_HOST) and show it there with the given `log show` options.
#   logs.sh --last 10m --predicate 'process == "Preferences"'
#   logs.sh --last 5m --predicate 'eventMessage CONTAINS[c] "error"' --style compact
#   logs.sh --no-mirror ...     show the copy already there
# The archive on the Mac is LOG_ARCHIVE_ON_HOST (default /private/tmp/droidvm-guest.logarchive).
set -euo pipefail
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
HOST=${BUILD_HOST:?set BUILD_HOST to the build Mac, user@host}
OUT=${RIG_OUT:-$RIG/out}/guest.logarchive
THERE=${LOG_ARCHIVE_ON_HOST:-/private/tmp/droidvm-guest.logarchive}
if [ "${1:-}" = --no-mirror ]; then
  shift
else
  python3 "$RIG/logarchive.py" "$OUT" >&2
  rsync -a --delete "$OUT/" "$HOST:$THERE/" >&2
fi
# `log` is a zsh builtin in a non-interactive shell on the Mac; the tool is /usr/bin/log.
ssh "$HOST" "/usr/bin/log show --archive $(printf '%q ' "$THERE" "$@")"
