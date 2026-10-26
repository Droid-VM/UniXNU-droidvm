#!/bin/bash
# boot-cycles.sh N: restart the VM N times (stop, 12 s, start) under vmguard2; when the guard stops a VM, record its early trace,
# ArmCpu bring-up state and per-CPU activity in out/cycles/, then kill it. One summary line per boot on stdout.
cd "$(dirname "$0")"; . ./rigenv.sh; mkdir -p out/cycles
F=out/live/$(cut -d' ' -f3 out/live/current)
for i in $(seq 1 "${1:-5}"); do
  before=$(grep -c GUARD "$F"); timeout 300 ./vm.sh stop >/dev/null 2>&1; sleep 12
  timeout 900 ./vm.sh start > out/cycles/start-$i.log 2>&1
  timeout 120 ./show-vm.sh > /dev/null 2>&1   # a restart closes the app's display of the VM; bring it back
  sleep 30
  after=$(grep -c GUARD "$F"); stamp=$(date +%H%M%S)
  if [ "$after" -gt "$before" ]; then
    grep GUARD "$F" | tail -1 | cut -c1-220 > out/cycles/$stamp.txt
    timeout 200 python3 early-trace.py >> out/cycles/$stamp.txt 2>&1
    timeout 200 python3 - >> out/cycles/$stamp.txt 2>&1 <<'PY'
import peek, struct
_, a = peek.read('armcpu_registered', 0x248)
arr = lambda s, f='I': struct.unpack_from('<4' + f, a, s - 0x4a8)
print('smp step', arr(0x548), 'fail', arr(0x5a8), 'psci_rc', arr(0x680, 'i'), 'core_timer', arr(0x4f0))
PY
    adb -s $PHONE shell su -c 'sh /data/local/tmp/vcpustat.sh' >> out/cycles/$stamp.txt 2>&1
    adb -s $PHONE shell su -c 'sh /data/local/tmp/killvm.sh' > /dev/null 2>&1; sleep 12
    echo "boot $i: GUARD -> out/cycles/$stamp.txt: $(sed -n 2,3p out/cycles/$stamp.txt | tr '\n' ' ' | cut -c1-200)"
  else
    echo "boot $i: ok $(grep -E '^userland' out/cycles/start-$i.log | cut -c1-40) $(python3 vcpu-busy.py "$F" | cut -c1-90)"
  fi
done
