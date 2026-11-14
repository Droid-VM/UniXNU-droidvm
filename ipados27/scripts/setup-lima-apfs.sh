#!/bin/bash
# The APFS driver in the Linux VM that sets a macOS Data seed's owners (make-droidvm-macos-seed.py, fix-macos-data-owners.py): the
# compiler and this kernel's headers, linux-apfs-rw at a pinned commit built against them, and the module loaded. Run as root in
# the VM (lima: limactl shell NAME sudo bash setup-lima-apfs.sh); the VM's kernel, not the build Mac's or any host's, loads it.
set -euo pipefail
COMMIT=4986b63f8898d35068847852c6b6dfea8f850132      # linux-apfs/linux-apfs-rw
SRC=/var/lib/droidvm-apfs/linux-apfs-rw
if grep -qw apfs /proc/filesystems; then
    echo "apfs already loaded"
    exit 0
fi
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q build-essential git "linux-headers-$(uname -r)"
if [ ! -d "$SRC/.git" ]; then
    mkdir -p "$(dirname "$SRC")"
    git clone -q https://github.com/linux-apfs/linux-apfs-rw.git "$SRC"
fi
git -C "$SRC" fetch -q origin
git -C "$SRC" checkout -q "$COMMIT"
make -C "$SRC" -j"$(nproc)" KERNELRELEASE="$(uname -r)"
insmod "$SRC/apfs.ko"
grep -w apfs /proc/filesystems
