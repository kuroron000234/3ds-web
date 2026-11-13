#!/usr/bin/env bash
# CPU ベンチマークをビルドする。出力は bench-arm.elf。
set -euo pipefail
cd "$(dirname "$0")"
F="-O2 -mcpu=mpcore -mfloat-abi=hard -mfpu=vfpv2 -ffreestanding -nostdlib -static -fno-builtin -Wl,-T,link.ld -Wl,--build-id=none"
arm-none-eabi-gcc $F -marm bench.c -o bench-arm.elf

arm-none-eabi-gcc $F -marm jittest.c -o jittest-arm.elf
