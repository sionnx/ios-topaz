#!/bin/bash
# After a probe boot: wait for Android (adb + root via KernelSU) and save the
# probe's ramoops console record. Usage: oriole/probe/pull.sh OUT.txt
set -euo pipefail
out=${1:?usage: pull.sh OUT.txt}
adb wait-for-device
until [ "$(adb shell getprop sys.boot_completed | tr -d '\r')" = 1 ]; do sleep 2; done
adb shell 'su -c "ls -l /sys/fs/pstore/"'
adb exec-out "su -c 'cat /sys/fs/pstore/console-ramoops-0'" > "$out" || true
adb shell 'su -c "dmesg | grep -iE \"pstore|ramoops\""' || true
echo "--- $out ($(wc -c < "$out") bytes)"
grep -a -A200 "ORIOLE PROBE" "$out" || echo "no probe record (DRAM not kept across the reset, or the probe never ran)"
