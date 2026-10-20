#!/bin/bash
# Build the canoe (OnePlus 15, SM8850) probe -> out/probe-boot.img for
# `fastboot boot`. Two variants: plain boot v4, and the same with an AVB hash
# footer (the Pixel 6's ABL needed one; try plain first here).
# Toolchain: Apple/LLVM clang + ld.lld + aarch64-elf binutils (Homebrew).
set -euo pipefail
cd "$(dirname "$0")"
CC="clang -target aarch64-none-elf"
CFLAGS="-O2 -march=armv8-a -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
AVBTOOL=${AVBTOOL:-avbtool}
COMMON=../../oriole/common
mkdir -p out
$CC $CFLAGS -c "$COMMON/start.S" -o out/start.o
$CC $CFLAGS -c probe.c -o out/probe.o
abs=$(aarch64-elf-readelf -r out/probe.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in probe.o" >&2; exit 1; }
ld.lld -T "$COMMON/image.ld" --build-id=none -N -static out/start.o out/probe.o -o out/probe.elf
aarch64-elf-objcopy -O binary out/probe.elf out/Image
size=$((16#$(aarch64-elf-nm out/probe.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
# boot image v4: no ramdisk, empty cmdline, arm64 image_size >= 2 MiB.
python3 - out/Image out/probe-boot.img <<'EOF2'
import struct, sys
k = bytearray(open(sys.argv[1], 'rb').read())
assert k[56:60] == b'ARMd', 'no arm64 Image header'
struct.pack_into('<Q', k, 16, max(0x200000, (len(k) + 0x1fffff) & ~0x1fffff))
hdr = b'ANDROID!' + struct.pack('<IIII4II', len(k), 0, 0, 1584, 0, 0, 0, 0, 4)
hdr += bytes(1536) + struct.pack('<I', 0)
pad = lambda b: bytes(b) + bytes(-len(b) % 4096)
open(sys.argv[2], 'wb').write(pad(hdr) + pad(k))
EOF2
cp out/probe-boot.img out/probe-boot-avb.img
$AVBTOOL add_hash_footer --image out/probe-boot-avb.img --partition_name boot --partition_size 100663296
ls -l out/Image out/probe-boot.img out/probe-boot-avb.img
