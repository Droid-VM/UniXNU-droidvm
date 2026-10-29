#!/system/bin/sh
# run-probe.sh CPUS...: boot pacprobe.bin once per physical CPU (one vCPU pinned there) as DroidVM boots its guests (Gunyah,
# pseudo-unprotected, hugepages), each with a 128 MiB RAM from the hugepage pool, and keep each run's serial output.
# Follows the hugepage procedure: the pool must hold the run and no other VM may be up before it starts; 10 s after each
# stop for the module to reclaim. A run still up after 20 s is stopped through its control socket (crosvm stop), never
# signalled; only one still there 20 s after that is killed. Any run that needed stopping ends the series.
D=/data/local/tmp/pacprobe
P=/sys/module/gh_hugepage_reserve/parameters
CROSVM=/data/data/cn.classfun.droidvm/usr/bin/crosvm
export LD_LIBRARY_PATH=/data/data/cn.classfun.droidvm/usr/lib
NEED=64
for c in "$@"; do
  avail=$(grep '^pool_avail=' $P/refill_stat | cut -d= -f2)
  active=$(grep '^active_vms=' $P/refill_stat | cut -d= -f2)
  if [ "${avail:-0}" -lt $NEED ] || [ "${active:-1}" != 0 ]; then echo "cpu$c: pool_avail $avail active_vms $active, stop"; break; fi
  rm -f $D/out-$c.txt $D/s-$c.sock
  $CROSVM run --name "pacprobe-$c" --mem 128 --cpus 1 --cpu-affinity 0=$c --hypervisor gunyah \
    --protected-vm-pseudo-unprotected --no-balloon --no-usb --disable-sandbox --hugepages --prepare-lend-mthp-mode single \
    --socket $D/s-$c.sock --serial type=file,hardware=serial,num=1,path=$D/out-$c.txt,console \
    $D/pacprobe.bin > $D/crosvm-$c.log 2>&1 &
  pid=$!
  t=0; while [ $t -lt 20 ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
  hung=0
  if kill -0 $pid 2>/dev/null; then
    hung=1; echo "cpu$c: still up after 20 s, crosvm stop"
    $CROSVM stop $D/s-$c.sock > /dev/null 2>&1
    t=0; while [ $t -lt 20 ] && kill -0 $pid 2>/dev/null; do sleep 1; t=$((t + 1)); done
    kill -0 $pid 2>/dev/null && { echo "cpu$c: still up 20 s after stop, kill -9"; kill -9 $pid; }
  fi
  wait $pid; rc=$?
  echo "cpu$c: crosvm exit $rc ($(grep -c PACPROBE $D/out-$c.txt 2>/dev/null) probe lines) $(grep VERDICT $D/out-$c.txt 2>/dev/null)"
  sleep 10
  [ $hung = 1 ] && { echo "series ended after a stuck run"; break; }
done
grep '^pool_avail=\|^active_vms=\|^state=' $P/refill_stat
