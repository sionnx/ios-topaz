#!/bin/bash
# Build the canoe UEFI probe -> out/probe-efi-boot.img for `fastboot boot`.
# The OnePlus 15 bootloader is UEFI: the boot.img "kernel" must be an EFI
# PE/COFF application (starts with "MZ"), not a raw arm64 Image.
# Toolchain: Apple clang (aarch64 COFF) + lld-link (PE link).
set -euo pipefail
cd "$(dirname "$0")"
CC=$(xcrun -find clang)
AVBTOOL=${AVBTOOL:-avbtool}
mkdir -p out
$CC --target=aarch64-unknown-windows -ffreestanding -fshort-wchar -mstrict-align \
	-fno-stack-protector -mgeneral-regs-only -Wall -Wextra -O2 -c probe_efi.c -o out/probe_efi.obj
lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib \
	-base:0x10000 -align:4096 -filealign:512 out/probe_efi.obj -out:out/probe.efi
# boot image v4 with the EFI app as the "kernel".
python3 - out/probe.efi out/probe-efi-boot.img <<'EOF2'
import struct, sys
k = open(sys.argv[1], 'rb').read()
assert k[:2] == b'MZ', 'not a PE image'
hdr = b'ANDROID!' + struct.pack('<IIII4II', len(k), 0, 0, 1584, 0, 0, 0, 0, 4)
hdr += bytes(1536) + struct.pack('<I', 0)
pad = lambda b: bytes(b) + bytes(-len(b) % 4096)
open(sys.argv[2], 'wb').write(pad(hdr) + pad(k))
EOF2
cp out/probe-efi-boot.img out/probe-efi-boot-avb.img
$AVBTOOL add_hash_footer --image out/probe-efi-boot-avb.img --partition_name boot --partition_size 100663296
ls -l out/probe.efi out/probe-efi-boot.img
file out/probe.efi
