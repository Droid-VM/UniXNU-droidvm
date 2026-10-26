#!/system/bin/sh
# Follow one of the VM's console logs from now on (stderr: crosvm's own log; serial1: the guest's console), for a copy kept off the phone.
#   sh crosvm-tail.sh STREAM VM_ID
exec tail -n 0 -F "/data/data/cn.classfun.droidvm/cache/console_${2:?the VM id}_$1.log"
