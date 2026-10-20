# DTB/ — device tree blobs from the iQOO 9T (I2201 / PD2217F)

Extracted from the live device on 2026-10-20 with **Magisk 30.7** root
(`u:r:magisk:s0`). Bootloader unlocked, stock Android 15 build
`PD2217F_EX_A_15.3.19.3.W30`.

## Sources

| Directory | Count | Source |
|---|---|---|
| `dtbo/` | 11 | `/dev/block/by-name/dtbo` (`/dev/block/sde17`, 24 MiB) — Android DTBO / Qualcomm `dt.img` container |
| `vendor_boot/` | 6 | DTB section of `vendor_boot.img` (header v4, 5,239,577 bytes at `0x1100000`) |
| `live/` | 1 | `/sys/firmware/fdt` — the merged tree the running kernel actually uses |
| `raw/` | — | `dtbo.img`, a verbatim 24 MiB partition dump |

`boot.img` is a v3/v4 image and has **no** DTB section; on this platform the
DTBs live in `vendor_boot` + `dtbo`. `/proc/device-tree` is a symlink to
`/sys/firmware/devicetree/base` and resolves to the same data as
`live/live_fdt.dtb`.

## Device identity (from the live tree)

```
model      : Qualcomm Technologies, Inc. Cape QRD with PM8008 PD2217F_EX
compatible : qcom,cape-qrd qcom,cape qcom,qrd
```

`Cape` is Qualcomm's SM8475 platform (Snapdragon 8+ Gen 1). The `PD2217F_EX`
suffix is the vivo project for this phone. The `dtbo/` entries are compiled
**overlays** (`fragment@N` nodes); `vendor_boot/` contains the base SoC DTBs
that Qualcomm ships for the whole family (`Cape`, `Waipio`, `WaipioP`,
`Diwali`).

## Files

* `manifest.tsv` — file, size, sha256, `model`, `compatible` for every DTB.
* `SHA256SUMS` — checksums for the whole directory.

## How it was extracted

```bash
# dump the two containers (Magisk root)
adb shell "su -c 'dd if=/dev/block/sde17 of=/data/local/tmp/dtbo.img bs=1M'"
adb shell "su -c 'dd if=/sys/firmware/fdt of=/data/local/tmp/live_fdt.dtb bs=4096'"
adb pull /data/local/tmp/dtbo.img      DTB/raw/dtbo.img
adb pull /data/local/tmp/live_fdt.dtb  DTB/live/live_fdt.dtb

# split containers + vendor_boot (also works on artifacts/vendor_boot.img)
tools/host/extract_dtbs.py --all --out DTB
```

Re-extract at any time:

```bash
tools/host/extract_dtbs.py --all
tools/host/extract_dtbs.py --dtbo DTB/raw/dtbo.img --out DTB
tools/host/extract_dtbs.py --vendor-boot artifacts/vendor_boot.img --out DTB
```

The extractor understands the AOSP DTBO header (16-byte entries) and the
Qualcomm variant used here (32-byte entries: `0x20 + 11*0x20 = 0x180`, where
the first FDT starts), plus bare FDTs and concatenated FDT streams.

## Use with Linux / postmarketOS

The `live/` tree is what the kernel booted; the `vendor_boot/05_WaipioP_SoC`
and `dtbo/00_*Cape*PD2217F_EX` pair identify the base + overlay relationship.
For a mainline kernel you will need a **merged** DTB, not an overlay — either
use `live/live_fdt.dtb` (device-accurate, but includes vendor-only nodes) or
apply the PD2217F overlay to the Cape base DTB with `fdtoverlay`.
