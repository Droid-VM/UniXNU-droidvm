#!/system/bin/sh
# Host memory, the iOS VM crosvm's RSS, threads and dmabuf fds (3 per live virtio-gpu resource: not a leak), KGSL's
# allocated and mapped bytes, temperatures and the GPU clock. Pushed to the phone by gfx-soak.sh.
grep -E 'MemTotal|MemFree|MemAvailable|^Cached|SwapTotal|SwapFree|Shmem:|KReclaimable|Unevictable' /proc/meminfo
cat /proc/pressure/memory 2>/dev/null
for p in $(pidof crosvm); do
  if tr '\0' ' ' < /proc/$p/cmdline | grep -q UniXNU; then
    echo "crosvm $p: $(grep -E 'VmRSS|VmSwap|RssAnon|RssFile|RssShmem' /proc/$p/status | tr -s ' \t' ' ' | tr '\n' ' ')"
    echo "threads $(ls /proc/$p/task | wc -l) fds $(ls /proc/$p/fd | wc -l)"
    ls -l /proc/$p/fd 2>/dev/null | grep -c dmabuf | sed 's/^/dmabuf fds /'
    if [ -d /sys/kernel/debug/kgsl/proc/$p ]; then cat /sys/kernel/debug/kgsl/proc/$p/mem 2>/dev/null | wc -l | sed 's/^/kgsl entries /'; fi
  fi
done
cat /sys/class/kgsl/kgsl/page_alloc 2>/dev/null | sed 's/^/kgsl page_alloc /'
cat /sys/class/kgsl/kgsl/page_alloc_max 2>/dev/null | sed 's/^/kgsl page_alloc_max /'
cat /sys/class/kgsl/kgsl/mapped 2>/dev/null | sed 's/^/kgsl mapped /'
cat /sys/class/kgsl/kgsl/secure 2>/dev/null | sed 's/^/kgsl secure /'
for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/type); case $t in gpuss-0|cpu-0-0-0|skin-msm-therm) echo "$t $(cat $z/temp)";; esac; done
cat /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null
