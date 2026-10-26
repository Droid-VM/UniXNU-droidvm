#!/system/bin/sh
# Pin every CPU policy to one frequency for comparable benchmark runs, or give the limits back.
#   phone-cpu-pin.sh pin [BIG_KHZ [LITTLE_KHZ]]   scaling_min_freq = scaling_max_freq = the available frequency nearest the
#                                                 target (default 2400000 for the big policies, 1800000 for policy0), then the
#                                                 two files read-only, so the vendor's power daemons (OPLUS capped the big cores
#                                                 at 1.71 GHz) cannot move them; the old values go to /data/local/tmp/cpu-pin.saved
#   phone-cpu-pin.sh unpin                        the saved values back, the files writable again (the values of when pin ran,
#                                                 a vendor cap of that moment included)
#   phone-cpu-pin.sh free                         every policy back to its hardware range (cpuinfo_min/max_freq), writable: the
#                                                 governor and the vendor's caps decide, as before any pin
#   phone-cpu-pin.sh show                         each policy's limits and current frequency
# The kernel's own thermal limits (freq QoS of the cooling devices) still apply when pinned.
SAVED=/data/local/tmp/cpu-pin.saved
nearest() {   # nearest available frequency of policy $1 to $2
  best=""; bestd=""
  for f in $(cat $1/scaling_available_frequencies); do
    d=$(( f > $2 ? f - $2 : $2 - f ))
    if [ -z "$bestd" ] || [ $d -lt $bestd ]; then best=$f; bestd=$d; fi
  done
  echo $best
}
show() {
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    echo "${p##*/} min=$(cat $p/scaling_min_freq) max=$(cat $p/scaling_max_freq) cur=$(cat $p/scaling_cur_freq) hw=$(cat $p/cpuinfo_max_freq) mode=$(stat -c %a $p/scaling_max_freq)"
  done
}
case "$1" in
pin)
  big=${2:-2400000}; little=${3:-1800000}
  [ -f $SAVED ] || for p in /sys/devices/system/cpu/cpufreq/policy*; do echo "$p $(cat $p/scaling_min_freq) $(cat $p/scaling_max_freq)"; done > $SAVED
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    case ${p##*/} in policy0) want=$little;; *) want=$big;; esac
    f=$(nearest $p $want)
    chmod 644 $p/scaling_min_freq $p/scaling_max_freq
    # max first when raising, min first when lowering: min may never exceed max
    if [ $f -ge $(cat $p/scaling_max_freq) ]; then echo $f > $p/scaling_max_freq; echo $f > $p/scaling_min_freq
    else echo $f > $p/scaling_min_freq; echo $f > $p/scaling_max_freq; fi
    chmod 444 $p/scaling_min_freq $p/scaling_max_freq
  done
  show;;
unpin)
  [ -f $SAVED ] || { echo "nothing saved"; exit 1; }
  while read p min max; do
    chmod 644 $p/scaling_min_freq $p/scaling_max_freq
    echo $min > $p/scaling_min_freq; echo $max > $p/scaling_max_freq; echo $min > $p/scaling_min_freq
  done < $SAVED
  rm -f $SAVED
  show;;
free)
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    chmod 644 $p/scaling_min_freq $p/scaling_max_freq
    echo $(cat $p/cpuinfo_max_freq) > $p/scaling_max_freq; echo $(cat $p/cpuinfo_min_freq) > $p/scaling_min_freq
  done
  rm -f $SAVED
  show;;
*) show;;
esac
