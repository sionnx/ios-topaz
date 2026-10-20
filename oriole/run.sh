#!/bin/bash
# One test cycle on the phone: reboot to fastboot, `fastboot boot IMG`, wait
# for the image to reset the phone back into Android, save the ramoops console
# record. Usage: oriole/run.sh IMG LOG.txt [TIMEOUT_S]
# Needs Android up with adb root (KernelSU) or the phone already in fastboot.
set -euo pipefail
img=${1:?usage: run.sh IMG LOG.txt [TIMEOUT_S]}
log=${2:?usage: run.sh IMG LOG.txt [TIMEOUT_S]}
t=${3:-300}
here=$(cd "$(dirname "$0")" && pwd)
if ! fastboot devices | grep -q .; then
	adb reboot bootloader
	for i in $(seq 60); do fastboot devices | grep -q . && break; sleep 1; done
fi
fastboot boot "$img"
date
sleep 5
if timeout 3 fastboot getvar product >/dev/null 2>&1; then
	echo "bootloader still in fastboot: image refused (AVB footer?)" >&2
	exit 1
fi
timeout "$t" "$here/probe/pull.sh" "$log" > /dev/null 2>&1 || {
	echo "no Android within ${t}s: phone stuck? (Power + Volume up 30 s)" >&2
	exit 1
}
date
