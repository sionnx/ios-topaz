#!/bin/bash
# Build the oriole probe and wrap it in a boot image v4 (DTB and vendor
# ramdisk come from vendor_boot of the active slot). Output, for
# `fastboot boot`: out/probe-boot.img (raw Image) and out/probe-boot-lz4.img
# (Image as LZ4 legacy, like the stock kernel).
# Toolchain: Apple/LLVM clang + ld.lld + aarch64-elf binutils (Homebrew).
set -euo pipefail
cd "$(dirname "$0")"
CC="clang -target aarch64-none-elf"
CFLAGS="-O2 -march=armv8-a -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
$CC $CFLAGS -c start.S -o out/start.o
$CC $CFLAGS -c probe.c -o out/probe.o
# Runs wherever the bootloader puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-elf-readelf -r out/probe.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in probe.o" >&2; exit 1; }
ld.lld -T probe.ld --build-id=none -N -static out/start.o out/probe.o -o out/probe.elf
aarch64-elf-objcopy -O binary out/probe.elf out/Image
size=$((16#$(aarch64-elf-nm out/probe.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
python3 -c 'import struct,sys; f=open(sys.argv[1],"r+b"); f.seek(16); f.write(struct.pack("<Q", 0x200000))' out/Image
rm -f out/Image.lz4 && lz4 -l -12 -q out/Image out/Image.lz4
# Header v4, no ramdisk, os_version 0, empty cmdline, arm64 image_size raised
# to 2 MiB (as the oriole U-Boot port's mkbootimg.py). The stock bootloader
# (slider-16.4) also needs an AVB hash footer, even unlocked: without one
# `fastboot boot` answers OKAY and silently stays in fastboot (2026-10-20:
# the stock boot_a.img minus its footer is refused the same way). Unsigned
# (algorithm NONE) is fine; the device just boots "orange".
# AVBTOOL: AOSP external/avb avbtool.py.
AVBTOOL=${AVBTOOL:-avbtool}
for k in Image Image.lz4; do
	o=out/probe-boot.img
	[ $k = Image.lz4 ] && o=out/probe-boot-lz4.img
	python3 - out/$k "$o" <<'EOF2'
import struct, sys
k = bytearray(open(sys.argv[1], 'rb').read())
if k[56:60] == b'ARMd':
    struct.pack_into('<Q', k, 16, 0x200000)
hdr = b'ANDROID!' + struct.pack('<IIII4II', len(k), 0, 0, 1584, 0, 0, 0, 0, 4)
hdr += bytes(1536) + struct.pack('<I', 0)
pad = lambda b: bytes(b) + bytes(-len(b) % 4096)
open(sys.argv[2], 'wb').write(pad(hdr) + pad(k))
EOF2
	$AVBTOOL add_hash_footer --image "$o" --partition_name boot --partition_size 67108864
done
ls -l out/Image out/Image.lz4 out/probe-boot.img out/probe-boot-lz4.img
