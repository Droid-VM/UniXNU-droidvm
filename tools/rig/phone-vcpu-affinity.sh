#!/system/bin/sh
# crosvm's vCPU threads: their allowed CPUs, the CPU each last ran on, and their migration counts (sampled twice)
P=$(pidof crosvm | tr ' ' '\n' | head -1)
[ -z "$P" ] && { echo "no crosvm"; exit 1; }
for round in 1 2; do
  for t in /proc/$P/task/*; do
    n=$(cat $t/comm)
    case "$n" in crosvm_vcpu*|*vcpu*) ;; *) continue;; esac
    cpu=$(awk '{print $39}' $t/stat)
    allowed=$(grep Cpus_allowed_list $t/status | awk '{print $2}')
    mig=$(grep -E '^se.nr_migrations' $t/sched 2>/dev/null | awk '{print $3}')
    echo "round $round $n tid ${t##*/} allowed $allowed last_cpu $cpu migrations $mig"
  done
  sleep 5
done
