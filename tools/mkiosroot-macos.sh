#!/bin/bash
# macOS version of tools/mkiosroot.sh: the same case-sensitive, journaled
# HFS+ image of an iOS OS root plus its cryptexes, for the userdata partition
# (rd=ufsroot0), built with the system's own APFS and HFS+ support instead of
# apfs-fuse/hfsprogs. Run as root (owners, modes and the _logd/mobile files):
#
#   sudo tools/mkiosroot-macos.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE] [--tweaks]
#
# SIZE defaults to 16g (truncate syntax). --tweaks also runs
# tools/sweet-root-tweaks.py on the new root before it is unmounted.
#
# The IPSW's APFS volumes hold transparently compressed files; rsync reads
# them through read() and writes plain data (no decmpfs xattr or
# UF_COMPRESSED flag): the kernel's HFS has no decompressor for them. The
# script checks that nothing compressed reached the image. The raw OUT.img
# can be flashed as it is (fastboot sparses it on the fly).
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,15p' "$0"; exit 2; }
[[ $(id -u) == 0 ]] || { echo "run as root (sudo)" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
os=$1 sysos=$2 appos=$3 out=$4
size=16g tweaks=0
for a in "${@:5}"; do
	case $a in
	--tweaks) tweaks=1 ;;
	*) size=$a ;;
	esac
done
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }

work=$(mktemp -d /tmp/iosroot.XXXXXX)
devs=()
cleanup() {
	for d in "${devs[@]}"; do
		hdiutil detach -force "$d" >/dev/null 2>&1 || true
	done
	rmdir "$work"/* "$work" 2>/dev/null || true
}
trap cleanup EXIT

# Attach an IPSW image read-only at $work/NAME. The OS image is UDIF; the
# cryptexes are raw APFS, which hdiutil only takes as CRawDiskImage.
attach_ro() {
	local img=$1 name=$2 dev
	mkdir "$work/$name"
	dev=$(hdiutil attach -readonly -nobrowse -noverify -noautofsck -owners on \
		-mountpoint "$work/$name" "$img" 2>/dev/null | awk '$1 ~ /^\/dev\/disk[0-9]+$/ {print $1; exit}') ||
		true
	if [[ -z $dev ]]; then
		dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -readonly -nobrowse -noverify \
			-noautofsck -owners on -mountpoint "$work/$name" "$img" |
			awk '$1 ~ /^\/dev\/disk[0-9]+$/ {print $1; exit}')
	fi
	[[ -n $dev ]] && [[ -d $work/$name/System || -d $work/$name/usr || -n $(ls -A "$work/$name") ]] ||
		{ echo "cannot attach $img" >&2; exit 1; }
	devs+=("$dev")
	echo "attached $img as $dev at $work/$name"
}
attach_ro "$os" os
attach_ro "$sysos" sysos
attach_ro "$appos" appos

truncate -s "$size" "$out"
tdev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$out" | awk 'NR==1 {print $1}')
devs=("$tdev" "${devs[@]}")
newfs_hfs -s -J -v iOSRoot "$tdev"
mkdir "$work/root"
mount -t hfs -o nobrowse "$tdev" "$work/root"
diskutil enableOwnership "$work/root" >/dev/null
# No Spotlight index or fsevents log on the new volume.
touch "$work/root/.metadata_never_index"
mkdir -p "$work/root/.fseventsd" && touch "$work/root/.fseventsd/no_log"
root=$work/root

copy() {
	echo "copying $1 -> ${2#"$root"}"
	mkdir -p "$2"
	rsync -aH --numeric-ids "$1/" "$2/"
}
copy "$work/os" "$root"
copy "$work/sysos" "$root/private/preboot/Cryptexes/OS"
copy "$work/appos" "$root/private/preboot/Cryptexes/App"

# Shared cache where dyld looks at ignition level 1: hard links, no extra space.
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
mkdir -p "$root/System/Library/Caches/com.apple.dyld"
for f in "$root/$dsc"/*; do
	ln "$f" "$root/System/Library/Caches/com.apple.dyld/${f##*/}"
done

# dyld does not search the cryptexes at ignition level 1: hard-link every
# cryptex file that has no counterpart in the OS root to its root path.
for c in OS App; do
	(cd "$root/private/preboot/Cryptexes/$c" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if [[ ! -e $root/$f && ! -L $root/$f ]]; then
			mkdir -p "$root/$(dirname "$f")"
			ln "$root/private/preboot/Cryptexes/$c/$f" "$root/$f"
			echo "linked $c:/$f"
		fi
	done
done

# logd's log stores, normally made at restore (owner _logd 272, .migrated).
db=$root/private/var/db
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	: > "$db/$d/.migrated"
	chown 272:272 "$db/$d/.migrated"
	chmod 0644 "$db/$d/.migrated"
done

# System keybag for the built-in AppleKeyStore (no passcode).
install -d -m 0700 -o 0 -g 0 "$root/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' \
	> "$root/private/var/keybags/systembag.kb"
chown 0:0 "$root/private/var/keybags/systembag.kb"
chmod 0600 "$root/private/var/keybags/systembag.kb"

if [[ $tweaks == 1 ]]; then
	python3 "$here/sweet-root-tweaks.py" "$root"
fi

n=$(find "$root" -xdev -flags compressed 2>/dev/null | wc -l | tr -d ' ')
[[ $n == 0 ]] || { echo "error: $n compressed files in the image" >&2; exit 1; }
rm -rf "$root/.fseventsd" "$root/.metadata_never_index" "$root/.Spotlight-V100" "$root/.Trashes"
df -h "$root"
umount "$root"
fsck_hfs -fy "$tdev" || true
fsck_hfs -fn "$tdev"
echo "done: $out"
