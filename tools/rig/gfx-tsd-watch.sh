#!/usr/bin/env bash
# Runs GFXBench tests (gfx-suite.sh) one after another and, after each, starts `gfxprobe tsd` twice: a new process's main-thread
# TSD slots as it starts. A slot that is 0 when the system is healthy (objc's 41-49 among them) holding a value in a fresh process
# means the shared-cache __DATA page holding libsystem_pthread's _main_thread was written in its shared source.
#   gfx-tsd-watch.sh ID...
cd "$(dirname "$0")" && . ./rigenv.sh
for t in "$@"; do
  ./gfx-suite.sh "$t" 2>&1 | grep -v 'Bad substitution'
  for n in 1 2; do
    echo "   tsd after $t ($n): $(GSSH_TIMEOUT=60 ./guest-ssh.sh '/private/var/ngpu/bin/gfxprobe tsd' 2>&1 | grep slot | sed 's/  slot *//; s/ = 0x0*/=/' | tr '\n' ' ')"
  done
done
