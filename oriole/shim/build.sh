#!/bin/bash
# Build the oriole shim -> out/shim-boot.img for `fastboot boot`.
# Usage: [UFS_PROBE=1] [USB_PROBE=1] [SPI_PROBE=1 GPP23_OFF=0x..] [MAX_RUN_S=N] build.sh [PAYLOAD [KERNEL]]  (PAYLOAD from tools/mkpayload.py with an
# ADT from `tools/mkadt.py OUT --board oriole`; without one the shim reports
# and resets). KERNEL: the Mach-O in the payload; its sweet_log_ring /
# sweet_log_total symbols let CPU0 copy xnu's console into ramoops.
# The payload rides behind the shim in the kernel Image at the first 4K
# boundary after _end, as on topaz; wrap_bootimg raises image_size to cover it.
# The bootloader loads the Image at 0x80000000 and the first reserved-memory
# above it is at 0x81bd0000, so Image + payload must stay below ~27 MiB.
set -euo pipefail
abspath() { [ -z "$1" ] || echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
set -- "$(abspath "${1:-}")" "$(abspath "${2:-}")"
cd "$(dirname "$0")"
. ../common/lib.sh
[ "${UFS_PROBE:-0}" = 1 ] && CFLAGS="$CFLAGS -DUFS_PROBE"
[ "${USB_PROBE:-0}" = 1 ] && CFLAGS="$CFLAGS -DUSB_PROBE"
[ "${SPI_PROBE:-0}" = 1 ] && CFLAGS="$CFLAGS -DSPI_PROBE${GPP23_OFF:+ -DGPP23_OFF=$GPP23_OFF}"
# MAX_RUN_S: WDT reset after this many seconds of xnu (default 240, 0 = never)
[ -n "${MAX_RUN_S:-}" ] && CFLAGS="$CFLAGS -DXNU_MAX_RUN_S=$MAX_RUN_S"
if [ -n "${2:-}" ]; then
	ring=$(nm "$2" | awk '$3=="_sweet_log_ring"{print $1}')
	total=$(nm "$2" | awk '$3=="_sweet_log_total"{print $1}')
	[ -n "$ring" ] && [ -n "$total" ] || { echo "error: no sweet_log_ring/total in $2" >&2; exit 1; }
	CFLAGS="$CFLAGS -DXNU_LOG_RING_VA=0x${ring}UL -DXNU_LOG_TOTAL_VA=0x${total}UL"
fi
build_image shim.c ufs.c usb.c spi.c
if [ -n "${1:-}" ]; then
	size=$(stat -f %z out/Image)
	truncate -s $(( (size + 0xfff) & ~0xfff )) out/Image
	cat "$1" >> out/Image
	size=$(stat -f %z out/Image)
	[ "$size" -lt $((0x1bd0000)) ] || { echo "error: Image + payload $size bytes reaches 0x81bd0000" >&2; exit 1; }
fi
wrap_bootimg out/Image out/shim-boot.img
ls -l out/Image out/shim-boot.img
