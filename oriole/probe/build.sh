#!/bin/bash
# Build the oriole probe -> out/probe-boot.img for `fastboot boot`.
set -euo pipefail
cd "$(dirname "$0")"
. ../common/lib.sh
build_image probe.c
wrap_bootimg out/Image out/probe-boot.img
ls -l out/Image out/probe-boot.img
