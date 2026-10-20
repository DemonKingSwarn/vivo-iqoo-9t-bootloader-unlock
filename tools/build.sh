#!/usr/bin/env bash
# build.sh — cross-compile the on-device binaries for aarch64 Android.
#
#   tools/build.sh                 # uses $ANDROID_NDK_HOME / $NDK, or downloads r29
#   NDK=/path/to/ndk tools/build.sh
#
# Output: tools/out/{cheese_pa,rootcmd,capset_min,devinfo_unlock}
#
# The NDK is only needed at build time.  If none is found the script fetches
# android-ndk-r29 into tools/.toolchain/ (git-ignored) with aria2c when
# available.
set -euo pipefail

DIR=$(cd "$(dirname "$0")" && pwd)
DEV="$DIR/device"
OUT="$DIR/out"
TC="$DIR/.toolchain"
NDK_URL="https://dl.google.com/android/repository/android-ndk-r29-linux.zip"

mkdir -p "$OUT" "$TC"

find_ndk() {
    local c
    for c in "${ANDROID_NDK_HOME:-}" "${NDK:-}" "${ANDROID_NDK_ROOT:-}"; do
        if [ -n "$c" ] && [ -d "$c" ]; then printf '%s\n' "$c"; return 0; fi
    done
    for c in "$TC"/android-ndk-r*; do
        if [ -d "$c" ]; then printf '%s\n' "$c"; return 0; fi
    done
    return 1
}

NDK_PATH=$(find_ndk || true)
if [ -z "$NDK_PATH" ]; then
    echo "== no NDK found; downloading r29 to $TC =="
    if command -v aria2c >/dev/null 2>&1; then
        aria2c -x 16 -s 16 -d "$TC" -o android-ndk-r29-linux.zip "$NDK_URL"
    else
        curl -L --retry 3 --progress-bar -o "$TC/android-ndk-r29-linux.zip" "$NDK_URL"
    fi
    unzip -q "$TC/android-ndk-r29-linux.zip" -d "$TC"
    rm -f "$TC/android-ndk-r29-linux.zip"
    NDK_PATH=$(find_ndk)
fi

HOST_TAG=linux-x86_64
CLANG="$NDK_PATH/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android34-clang"
if [ ! -x "$CLANG" ]; then
    # r25+ ships API-versioned wrappers; pick the highest available.
    CLANG=$(ls "$NDK_PATH"/toolchains/llvm/prebuilt/"$HOST_TAG"/bin/aarch64-linux-android*-clang 2>/dev/null \
            | sort -V | tail -1 || true)
fi
if [ ! -x "$CLANG" ]; then
    echo "error: no aarch64 clang in $NDK_PATH" >&2
    exit 1
fi

CFLAGS=(-O2 -static -Wall -Wextra -Wno-unused-parameter -I"$DEV")

echo "== building device binaries =="
echo "   clang: $CLANG"
# cheese_pa.c pulls in pa_leak.h (static leak_phys_page) and kallsyms_lookup.c —
# build it on its own; pa_leak.c is a standalone self-test with its own main().
"$CLANG" "${CFLAGS[@]}" -o "$OUT/cheese_pa"      "$DEV/cheese_pa.c"
echo "   built cheese_pa"
"$CLANG" "${CFLAGS[@]}" -o "$OUT/pa_leak"        "$DEV/pa_leak.c"
echo "   built pa_leak"
"$CLANG" "${CFLAGS[@]}" -o "$OUT/rootcmd"        "$DEV/rootcmd.c"
echo "   built rootcmd"
"$CLANG" "${CFLAGS[@]}" -o "$OUT/capset_min"     "$DEV/capset_min.c"
echo "   built capset_min"
"$CLANG" "${CFLAGS[@]}" -o "$OUT/devinfo_unlock" "$DEV/devinfo_unlock.c"
echo "   built devinfo_unlock"

echo
echo "done -> $OUT"
ls -l "$OUT"
echo
echo "install on device:"
echo "  python3 $DIR/host/rootctl.py install"
