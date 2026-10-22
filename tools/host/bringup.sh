#!/usr/bin/env bash
# bringup.sh — build the iQOO 9T bring-up payload and boot it over fastboot.
#
# Usage:
#   tools/host/bringup.sh build      # kernel Image + DTB + initramfs + boot.img
#   tools/host/bringup.sh boot       # fastboot boot the last build (no flash)
#   tools/host/bringup.sh both
#
# Environment:
#   NDK        path to the Android NDK LLVM bin directory (has clang/lld)
#   BUSYBOX    aarch64 static busybox (for the initramfs)
#   JOBS       make -j value (default 7)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
KDIR=$ROOT/linux-iqoo9t
OUT=$KDIR/out
ART=$ROOT/artifacts
JOBS=${JOBS:-7}
NDK=${NDK:-$ROOT/tools/.toolchain/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin}

export PATH=$NDK:$PATH

config() {
    echo "== config (defconfig + iqoo9t fragment)"
    make -C "$KDIR" ARCH=arm64 LLVM=1 LLVM_IAS=1 O=out defconfig
    "$KDIR/scripts/kconfig/merge_config.sh" -m -O "$OUT" \
        "$OUT/.config" "$KDIR/arch/arm64/configs/iqoo9t.config"
    make -C "$KDIR" ARCH=arm64 LLVM=1 LLVM_IAS=1 O=out olddefconfig
}

build() {
    echo "== kernel"
    make -C "$KDIR" ARCH=arm64 LLVM=1 LLVM_IAS=1 O=out -j"$JOBS" Image
    echo "== dtb"
    make -C "$KDIR" ARCH=arm64 LLVM=1 LLVM_IAS=1 O=out -j"$JOBS" \
        qcom/sm8450-iqoo9t.dtb
    echo "== initramfs"
    "$ROOT/initramfs/build.sh" -o "$ART/initramfs-iqoo9t.cpio.gz"
    echo "== boot.img (v0, Image + DTB appended)"
    "$HERE/mkbootimg.py" -o "$ART/boot-iqoo9t.img" --header-version 0 \
        --kernel "$OUT/arch/arm64/boot/Image" \
        --dtb "$OUT/arch/arm64/boot/dts/qcom/sm8450-iqoo9t.dtb" \
        --ramdisk "$ART/initramfs-iqoo9t.cpio.gz" \
        --cmdline "console=ttyMSM0,115200n8 console=tty0 ignore_loglevel"
}

boot() {
    [ -f "$ART/boot-iqoo9t.img" ] || { echo "run: $0 build" >&2; exit 1; }
    fastboot boot "$ART/boot-iqoo9t.img"
}

case "${1:-build}" in
    config) config ;;
    build) build ;;
    boot) boot ;;
    both) build; boot ;;
    *) echo "usage: $0 [build|boot|both]" >&2; exit 1 ;;
esac
