#!/usr/bin/env bash
# Runs each GFXBench test id in turn and prints its result line (score, fps, frames, status), and NEW CRASH with the report
# app_ios left when one appeared.
#   gfx-suite.sh ID...    e.g. metal_4 metal_5_high (the ids are gfxbench40's, its config/*.json)
#   GFX_ENV="NAME=VALUE;..."  adds to the app's environment (lsdiag LSDIAG_LAUNCH_ENV), e.g. TU_DARWIN_VIRTGPU=0
cd "$(dirname "$0")" && . ./rigenv.sh
for t in "$@"; do
  timeout 60 python3 home.py >/dev/null 2>&1; sleep 2; timeout 60 python3 input.py unlock >/dev/null 2>&1; sleep 3
  before=$(GSSH_TIMEOUT=30 ./guest-ssh.sh '/private/var/ngpu/bin/toybox ls -t /private/var/mobile/Library/Logs/CrashReporter/ | /private/var/ngpu/bin/toybox grep "^app_ios-" | /private/var/ngpu/bin/toybox head -1' 2>/dev/null | tail -1)
  GSSH_TIMEOUT=60 ./guest-ssh.sh "for p in \$(/bin/ps -A -o pid,comm | /private/var/ngpu/bin/toybox grep 'GFXBench.app/app_ios' | /private/var/ngpu/bin/toybox cut -c1-6); do kill \$p; done; /private/var/ngpu/bin/toybox rm -f /private/var/tmp/gfx.out /private/var/tmp/gfx.err; sleep 2; LSDIAG_LAUNCH_ENV='${GFX_ENV:-}' /private/var/ngpu/bin/lsdiag -u 501 launch-args /private/var/tmp/gfx org.droidvm.gfxbench -t $t" >/dev/null 2>&1
  R=""; start=$(date +%s)
  for i in $(seq 1 120); do
    sleep 10
    R=$(GSSH_TIMEOUT=30 ./guest-ssh.sh '/private/var/ngpu/bin/toybox cat /private/var/tmp/gfx.out 2>/dev/null' 2>/dev/null | grep -o -E '"(score|fps|frame_count|status)"[^,}]*|ERROR.*' | tr '\n' ' ')
    case "$R" in *status*|*ERROR*) break;; esac
    A=$(GSSH_TIMEOUT=30 ./guest-ssh.sh '/bin/ps -A -o comm | /private/var/ngpu/bin/toybox grep -c GFXBench.app' 2>/dev/null | tail -1)
    [ "$A" = "0" ] && break
  done
  # The app writes its result and exits within one poll: read the output once more after it has gone.
  case "$R" in *status*|*ERROR*) ;; *) R=$(GSSH_TIMEOUT=30 ./guest-ssh.sh '/private/var/ngpu/bin/toybox cat /private/var/tmp/gfx.out 2>/dev/null' 2>/dev/null | grep -o -E '"(score|fps|frame_count|status)"[^,}]*|ERROR.*' | tr '\n' ' ');; esac
  after=$(GSSH_TIMEOUT=30 ./guest-ssh.sh '/private/var/ngpu/bin/toybox ls -t /private/var/mobile/Library/Logs/CrashReporter/ | /private/var/ngpu/bin/toybox grep "^app_ios-" | /private/var/ngpu/bin/toybox head -1' 2>/dev/null | tail -1)
  crash=""; [ "$after" != "$before" ] && crash="NEW CRASH $after"
  # A failed run's standard error is kept (the next launch clears gfx.err): /private/var/tmp/gfx-err-TEST-TIME.txt
  case "$R" in *'"OK"'*) ;; *) GSSH_TIMEOUT=30 ./guest-ssh.sh "/private/var/ngpu/bin/toybox cp /private/var/tmp/gfx.err /private/var/tmp/gfx-err-$t-$(date +%H%M%S).txt" >/dev/null 2>&1;; esac
  echo "$t: ${R:-no result} $crash ($(( $(date +%s) - start )) s)"
done
