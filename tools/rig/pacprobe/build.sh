#!/usr/bin/env bash
# Build pacprobe.bin (a Linux-Image-format payload) with the host's clang and ld.lld.
set -euo pipefail
cd "$(dirname "$0")"
F="--target=aarch64-none-elf -march=armv8.5-a+pauth -ffreestanding -fno-builtin -nostdlib -mgeneral-regs-only -mstrict-align
   -fno-jump-tables -fno-pic -fno-pie -O2 -mbranch-protection=none"
clang $F -c start.S -o start.o
clang $F -c probe.c -o probe.o
ld.lld -T probe.ld -nostdlib -static start.o probe.o -o probe.elf
llvm-objcopy -O binary probe.elf pacprobe.bin
rm -f start.o probe.o
echo "$(pwd)/pacprobe.bin"
