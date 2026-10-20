#!/bin/bash
# Build the oriole shim -> out/shim-boot.img for `fastboot boot`.
# Usage: build.sh [PAYLOAD]  (PAYLOAD from tools/mkpayload.py with an ADT from
# `tools/mkadt.py OUT --board oriole`; without one the shim reports and resets)
# The payload rides behind the shim in the kernel Image at the first 4K
# boundary after _end, as on topaz; wrap_bootimg raises image_size to cover it.
# The bootloader loads the Image at 0x80000000 and the first reserved-memory
# above it is at 0x81bd0000, so Image + payload must stay below ~27 MiB.
set -euo pipefail
cd "$(dirname "$0")"
. ../common/lib.sh
build_image shim.c
if [ -n "${1:-}" ]; then
	size=$(stat -f %z out/Image)
	truncate -s $(( (size + 0xfff) & ~0xfff )) out/Image
	cat "$1" >> out/Image
	size=$(stat -f %z out/Image)
	[ "$size" -lt $((0x1bd0000)) ] || { echo "error: Image + payload $size bytes reaches 0x81bd0000" >&2; exit 1; }
fi
wrap_bootimg out/Image out/shim-boot.img
ls -l out/Image out/shim-boot.img
