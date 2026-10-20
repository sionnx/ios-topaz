# Build helpers for oriole images, sourced by probe/build.sh and shim/build.sh.
# Toolchain: Apple/LLVM clang + ld.lld + aarch64-elf binutils (Homebrew).
# AVBTOOL: AOSP external/avb avbtool.py (e.g. "python3 -I research/avbtool.py").

COMMON=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CC="clang -target aarch64-none-elf"
CFLAGS="-O2 -march=armv8-a -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
AVBTOOL=${AVBTOOL:-avbtool}

# build_image SRC... -> out/image.elf, out/Image (truncated to _end)
build_image() {
	local objs=() f o
	mkdir -p out
	for f in "$COMMON/start.S" "$COMMON/con.c" "$@"; do
		o=out/$(basename "${f%.*}").o
		$CC $CFLAGS -c "$f" -o "$o"
		objs+=("$o")
	done
	# Runs wherever the bootloader puts it with no relocation pass: only the
	# header's image_size (start.o, linked at 0) may be an absolute address.
	local abs
	abs=$(aarch64-elf-readelf -r "${objs[@]:1}" | grep -c ABS64 || true)
	[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; return 1; }
	ld.lld -T "$COMMON/image.ld" --build-id=none -N -static "${objs[@]}" -o out/image.elf
	aarch64-elf-objcopy -O binary out/image.elf out/Image
	local size=$((16#$(aarch64-elf-nm out/image.elf | awk '$3=="_end"{print $1}')))
	truncate -s "$size" out/Image
}

# wrap_bootimg IMAGE OUT: boot image for `fastboot boot`.
# Header v4, no ramdisk, os_version 0, empty cmdline, arm64 image_size at
# least 2 MiB (as the oriole U-Boot port's mkbootimg.py). The stock bootloader
# (slider-16.4) also needs an AVB hash footer, even unlocked: without one
# `fastboot boot` answers OKAY and silently stays in fastboot (2026-10-20:
# the stock boot_a.img minus its footer is refused the same way). Unsigned
# (algorithm NONE) is fine; the device just boots "orange". DTB and vendor
# ramdisk come from vendor_boot of the active slot.
wrap_bootimg() {
	python3 - "$1" "$2" <<'EOF'
import struct, sys
k = bytearray(open(sys.argv[1], 'rb').read())
assert k[56:60] == b'ARMd', 'no arm64 Image header'
struct.pack_into('<Q', k, 16, max(0x200000, (len(k) + 0x1fffff) & ~0x1fffff))
hdr = b'ANDROID!' + struct.pack('<IIII4II', len(k), 0, 0, 1584, 0, 0, 0, 0, 4)
hdr += bytes(1536) + struct.pack('<I', 0)
pad = lambda b: bytes(b) + bytes(-len(b) % 4096)
open(sys.argv[2], 'wb').write(pad(hdr) + pad(k))
EOF
	$AVBTOOL add_hash_footer --image "$2" --partition_name boot --partition_size 67108864
}
