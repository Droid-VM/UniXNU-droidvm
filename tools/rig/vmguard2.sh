#!/system/bin/sh
# vmguard2.sh [SPINS]   Guards the host against a guest vCPU that holds its core: every 0.25 s it reads each UniXNU crosvm
# vCPU thread's voluntary switches and CPU time (shell builtins only, no forks but sleep), and stops crosvm with SIGSTOP when
#  - a started secondary vCPU has not switched voluntarily once since it began to run, for 1 s (a core stuck in bootstrap, which
#    took the host down 3-10 s later), or
#  - every started secondary has run SPINS samples (default 240, about a minute) without a voluntary switch.
# Stopped vCPUs leave the guest, and its memory stays readable (peek.py). One status line a second; GUARD lines at once.
SPINS=${1:-240}; P=""; n=0
now_cs() { read up rest < /proc/uptime; T=${up%.*}${up#*.}; }
while :; do
  N=$P
  if [ -z "$N" ] || [ ! -d /proc/$N ]; then
    N=""; for p in $(pidof crosvm); do grep -q UniXNU /proc/$p/cmdline 2>/dev/null && N=$p; done
  fi
  if [ "$N" != "$P" ]; then P=$N; frozen=0; for i in 1 2 3 4 5 6 7; do eval "base_$i=-1 since_$i=0 lv_$i=-1 run_$i=0"; done; fi
  now_cs
  if [ -z "$P" ]; then [ $((n % 4)) = 0 ] && echo "$up novm"; n=$((n + 1)); sleep 0.25; continue; fi
  L="$up"; stuck=""; allspin=1; started=0
  for t in /proc/$P/task/*; do
    read -r c < $t/comm 2>/dev/null
    case "$c" in crosvm_vcpu*) ;; *) continue ;; esac
    i=${c#crosvm_vcpu}
    read -r st < $t/stat 2>/dev/null; st=${st#*) }; set -- $st; s=$1; tk=$((${12} + ${13}))
    vol=0; while IFS=' :	' read k v rest; do [ "$k" = voluntary_ctxt_switches ] && vol=$v; done < $t/status
    L="$L v$i:$s:$tk:$vol"
    [ "$i" = 0 ] && continue
    eval "b=\$base_$i; sc=\$since_$i; lv=\$lv_$i; r=\$run_$i"
    if [ "$tk" -gt 0 ]; then
      started=1
      [ "$b" = -1 ] && { b=$vol; sc=$T; }
      if [ "$vol" = "$b" ] && [ $((T - sc)) -ge 100 ]; then stuck="$stuck v$i"; fi
      if [ "$vol" = "$lv" ]; then r=$((r + 1)); else r=0; fi
      [ "$r" -lt "$SPINS" ] && allspin=0
    fi
    eval "base_$i=$b since_$i=$sc lv_$i=$vol run_$i=$r"
  done
  if [ "$frozen" = 0 ] && { [ -n "$stuck" ] || { [ "$started" = 1 ] && [ "$allspin" = 1 ]; }; }; then
    kill -STOP $P; frozen=1
    [ -n "$stuck" ] && echo "$L | GUARD: secondary$stuck never slept since it started, crosvm $P stopped" || echo "$L | GUARD: secondaries spun $SPINS samples, crosvm $P stopped"
  elif [ $((n % 4)) = 0 ]; then echo "$L"; fi
  n=$((n + 1)); sleep 0.25
done
