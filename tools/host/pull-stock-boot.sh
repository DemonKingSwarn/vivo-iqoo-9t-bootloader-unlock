#!/usr/bin/env bash
# pull-stock-boot.sh — dump the live boot partitions from the phone.
#
# Use this to get a Magisk-patchable image that exactly matches the installed
# build (better than extracting from a firmware package).
#
# Requirements:
#   - adb reachable, USB debugging authorised
#   - temporary root armed:  tools/host/rootctl.py arm
#
# Usage:
#   tools/host/pull-stock-boot.sh [OUTDIR]
#
# Default OUTDIR: artifacts/
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-"$ROOT/artifacts"}
mkdir -p "$OUT"

ADB=(adb)
if [ -n "${IQOO_SERIAL:-}" ]; then
    ADB+=(-s "$IQOO_SERIAL")
fi

if ! "${ADB[@]}" get-state 2>/dev/null | grep -q '^device$'; then
    echo "error: no adb device (accept the USB-debugging prompt?)" >&2
    exit 1
fi

slot=$("${ADB[@]}" shell getprop ro.boot.slot_suffix 2>/dev/null | tr -d '\r\n')
names=$("${ADB[@]}" shell ls /dev/block/by-name 2>/dev/null | tr -d '\r')

echo "slot suffix: '${slot}'"

found=0
for part in boot init_boot vendor_boot; do
    name="${part}${slot}"
    if ! printf '%s\n' "$names" | grep -qx "$name"; then
        # fall back to the unsuffixed name (non-A/B layout)
        if printf '%s\n' "$names" | grep -qx "$part"; then
            name="$part"
        else
            continue
        fi
    fi
    blk="/dev/block/by-name/$name"
    dest="/data/local/tmp/$name.img"
    echo "== $name =="
    if ! python3 "$HERE/rootctl.py" su "dd if=$blk of=$dest bs=1M" >/dev/null 2>&1; then
        echo "  error: dd failed — is root armed? (tools/host/rootctl.py arm)" >&2
        continue
    fi
    "${ADB[@]}" pull "$dest" "$OUT/$name.img" >/dev/null
    "${ADB[@]}" shell "rm -f $dest" || true
    echo "  saved $OUT/$name.img"
    sha256sum "$OUT/$name.img"
    found=1
done

if [ "$found" -eq 0 ]; then
    echo "error: no boot/init_boot/vendor_boot partition found" >&2
    exit 1
fi
