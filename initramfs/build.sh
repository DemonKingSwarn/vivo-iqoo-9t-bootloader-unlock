#!/usr/bin/env bash
# initramfs/build.sh — pack the bring-up initramfs for the iQOO 9T.
#
# Usage:
#   initramfs/build.sh [-b /path/to/busybox.static] [-o out.cpio.gz]
#
# The aarch64 busybox-static binary is not vendored. tools/host/fetch-busybox.sh
# fetches and unpacks it to tools/.toolchain/busybox-aarch64.static (ignored
# by git), or pass another path with -b.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BUSYBOX=${BUSYBOX_STATIC:-$ROOT/tools/.toolchain/busybox-aarch64.static}
OUT=${1:-$ROOT/artifacts/initramfs-iqoo9t.cpio.gz}

while getopts 'b:o:' opt; do
    case "$opt" in
        b) BUSYBOX=$OPTARG ;;
        o) OUT=$OPTARG ;;
        *) exit 1 ;;
    esac
done

[ -f "$BUSYBOX" ] || { echo "error: busybox not found: $BUSYBOX" >&2; exit 1; }
file "$BUSYBOX" | grep -q 'ARM aarch64' || {
    echo "error: busybox is not aarch64: $BUSYBOX" >&2; exit 1
}

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE"/{bin,sbin,proc,sys,dev,tmp,run,etc}
cp "$BUSYBOX" "$STAGE/bin/busybox"
chmod 0755 "$STAGE/bin/busybox"
# /init is #!/bin/sh, so the shell must exist before init runs.
ln -s busybox "$STAGE/bin/sh"
cp "$HERE/init" "$STAGE/init"
chmod 0755 "$STAGE/init"

# cpio from inside the stage so paths are relative.
( cd "$STAGE" && find . -print0 | cpio --null -o -H newc --quiet ) | gzip -9 > "$OUT"

echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
