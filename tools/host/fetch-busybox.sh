#!/usr/bin/env bash
# fetch-busybox.sh — fetch the static aarch64 busybox used by the bring-up
# initramfs. The binary is not vendored (tools/.toolchain/ is ignored).
#
# Result: tools/.toolchain/busybox-aarch64.static
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DEST=$ROOT/tools/.toolchain/busybox-aarch64.static
VER=${BUSYBOX_VER:-1.36.1-r31}
URL="https://dl-cdn.alpinelinux.org/alpine/v3.20/main/aarch64/busybox-static-$VER.apk"
WORK=${TMPDIR:-/tmp}/iqoo9t-busybox

mkdir -p "$(dirname "$DEST")" "$WORK"
if [ ! -f "$DEST" ]; then
    aria2c -q -x 16 -s 16 -d "$WORK" -o busybox.apk "$URL"
    tar -xzf "$WORK/busybox.apk" -C "$WORK"
    install -m 0755 "$WORK/bin/busybox.static" "$DEST"
fi
file "$DEST"
