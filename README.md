# iQOO 9T (I2201 / PD2217F) — root & bootloader unlock

Working notes, tooling, and a step-by-step procedure for a **vivo iQOO 9T**
(model `I2201`, project `PD2217F_EX`, Qualcomm SM8475 / Snapdragon 8+ Gen 1).

**Status: bootloader unlocked and verified.**

```
ro.boot.flash.locked        = 0
ro.boot.verifiedbootstate   = orange
ro.boot.vbmeta.device_state = unlocked
```

Achieved on firmware `15.3.19.3.W30.V000L1` by:

1. temporary kernel root via **CVE-2025-21479** (Adreno/KGSL physical R/W);
2. writing the ABL's trusted `devinfo` unlock flags;
3. completing the required post-unlock `userdata` wipe from vivo recovery.

No vendor signing key or unlock token was bypassed. The phone was a spare and
its data was wiped.

---

## Start here

| I want to… | Read |
|---|---|
| **follow the procedure end-to-end** | [`docs/guide.md`](docs/guide.md) |
| understand the ABL / exploit details | [`docs/findings.md`](docs/findings.md) |
| rebuild the payloads | [`tools/`](tools/README.md) |
| patch Magisk / restore stock boot | [`firmware/README.md`](firmware/README.md) |

## Quick start

```bash
# 1. build the on-device binaries (downloads the NDK if needed)
tools/build.sh

# 2. push them (phone in Android, USB debugging on)
tools/host/rootctl.py install

# 3. check the device
tools/host/rootctl.py verify
tools/host/rootctl.py selftest

# 4. temporary root
tools/host/rootctl.py arm
tools/host/rootctl.py su id

# 5. unlock (once) — see the guide for the recovery-screen step
tools/host/rootctl.py devinfo unlock
```

Full instructions, including the `Clear all data` step, are in
[`docs/guide.md`](docs/guide.md).

## What is stored here

* `docs/` — the guide, the technical write-up, and a screenshot of the
  unlocked-bootloader confirmation.
* `tools/device/` — the on-device C payloads (the KGSL primitive is vendored
  from [RamenFast/zenfone9-root](https://github.com/RamenFast/zenfone9-root),
  GPL-3.0; see `tools/device/LICENSE` and `UPSTREAM_COMMIT`).
* `tools/host/` — Python control scripts, no third-party dependencies.
* `DTB/` — 18 device tree blobs extracted from the phone (`dtbo` overlays,
  `vendor_boot` base SoCs, and the live merged tree), with a manifest and
  checksums.
* `firmware/` — **README only**; multi-GB images are not committed.
* `artifacts/` — generated backups and boot images (git-ignored).

## Warnings

* This is not a supported or sanctioned unlock path. Use a device you can lose.
* **Never flash** `abl`, `xbl`, `tz`, `hyp`, `keymint`, or `uefi`: Qualcomm
  secure boot is still enabled and verifies the boot chain in PBL.
* Do not choose **Repair system** in vivo recovery if you want to keep the
  unlock.
* Keep `artifacts/devinfo_before.img` — it is your re-lock/restore backup.
* Root is per-boot; a reboot restores the stock kernel text and Enforcing.

## Device summary

| | |
|---|---|
| SoC | SM8475, Adreno 730, `taro` |
| Kernel | `5.10.233-android12-9-00001-g3f8134695cb5-ab13232202` |
| Android | 15, security patch 2025-09-01 |
| Vendor patch | 2025-03-05 |
| Serial used | `15903686880004Q` |
| Fastboot | `product: taro`, `unlocked: yes`, max download 768 MiB |
