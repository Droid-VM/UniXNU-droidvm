#!/system/bin/sh
# vmguard.sh [SPINS [EARLY]]   Every 0.5 s, one line: host uptime; per crosvm vCPU thread its state, CPU, utime+stime ticks and
# voluntary/involuntary switches; CPU 7's timer count; the VM's age. During a VM's first 600 s, when
# every started secondary vCPU has run for about 2.5 s without one voluntary switch (the state that came before each host watchdog
# bite), crosvm is stopped with SIGSTOP: its vCPUs leave the guest, and the guest's memory stays readable (peek.py).
P=""; HZ=100; SPINS=${1:-4}; EARLY=${2:-4}   # consecutive spinning samples before the stop: after, and in, the first 60 s
while :; do
  N=""; for p in $(pidof crosvm); do tr '\0' ' ' < /proc/$p/cmdline | grep -q UniXNU && N=$p; done
  if [ "$N" != "$P" ]; then
    P=$N; frozen=0; for i in 1 2 3 4 5 6 7; do eval "vol_$i=-1; tik_$i=0; run_$i=0"; done
  fi
  now=$(cut -d' ' -f1 /proc/uptime); L="$now"
  if [ -z "$P" ]; then echo "$L novm"; sleep 0.5; continue; fi
  age=$(( ${now%.*} - $(cut -d')' -f2- /proc/$P/stat | awk '{print $20}') / HZ ))
  spin=1; started=0
  for t in /proc/$P/task/*; do
    c=$(cat $t/comm 2>/dev/null)
    case "$c" in crosvm_vcpu*)
      i=${c#crosvm_vcpu}; set -- $(cut -d')' -f2- $t/stat 2>/dev/null)
      st=$1; tk=$((${12} + ${13})); cpu=${37}
      v=$(awk '/^voluntary_ctxt/{print $2}' $t/status); nv=$(awk '/^nonvoluntary_ctxt/{print $2}' $t/status)
      L="$L v$i:$st@$cpu:$tk:$v/$nv"
      if [ "$i" != 0 ]; then
        eval "pv=\$vol_$i; pt=\$tik_$i; r=\$run_$i"
        if [ "$tk" -gt 0 ]; then
          started=1
          if [ "$v" = "$pv" ] && [ "$tk" -gt "$pt" ]; then r=$((r + 1)); else r=0; fi
          need=$SPINS; [ "$age" -lt 60 ] && need=$EARLY; [ "$r" -lt "$need" ] && spin=0
        fi
        eval "vol_$i=$v; tik_$i=$tk; run_$i=$r"
      fi
    esac
  done
  L="$L | cpu7 $(awk '/^ *11:/{print $9}' /proc/interrupts) | age $age"
  if [ "$frozen" = 0 ] && [ "$started" = 1 ] && [ "$spin" = 1 ] && [ "$age" -lt 600 ]; then
    kill -STOP $P; frozen=1; L="$L | GUARD: secondary vCPUs spun $need samples, crosvm $P stopped"
  fi
  echo "$L"
  sleep 0.5
done
