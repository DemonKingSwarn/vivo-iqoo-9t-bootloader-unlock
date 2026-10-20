# firmware/ — not stored in this repository

The firmware images (~6.8 GB) were removed to keep this repo small. You do not
need them to root or unlock; you only need one to patch Magisk or to restore a
stock image.

## Needed for Magisk

Preferred: dump the **live** boot image from the phone — it matches the exact
installed build and avoids version drift.

```bash
tools/host/rootctl.py arm
tools/host/pull-stock-boot.sh          # -> artifacts/boot_a.img (or boot.img)
```

If you cannot root first, use the stock `boot.img` from a firmware package with
the **same build number** as the phone.

## Known sources

| File | Notes |
|---|---|
| `Vivo_iQOO_9T_PD2217F_EX_A_15.3.19.3.W30.V000L1_vivo_osc_CM_20240905150000_qcom_Flash_File_RMM.7z` | the build matching this write-up (kernel `5.10.233-...`) |
| `iQOO_9T_PD2217F_EX_A_12.1.10.7.W30.V000L1_..._Split QFIL.zip` | older/vendor QFIL package, Google Drive id `1v9jzR2dm4e8GeZcMSGQeyzKWG_4h1MIo` |

Both are vivo "RMM"/QFIL packages. A 7z can be inspected without extracting
everything:

```bash
7z l <package>.7z | grep -iE 'boot|devinfo|partition'
7z x <package>.7z 'boot.img' 'vendor_boot.img' 'rawprogram*.xml'
```

## Which image does Magisk patch?

Android 13+ devices often carry the ramdisk in `init_boot` instead of `boot`.
List what the phone actually has:

```bash
adb shell ls /dev/block/by-name | grep -E '^(boot|init_boot|vendor_boot)'
```

Patch **`init_boot`** if it exists, otherwise **`boot`**. Keep the untouched
image — it is your restore path.

## Build-number check

```bash
adb shell getprop ro.build.display.id
adb shell getprop ro.build.fingerprint
```

Flashing a boot image from a different build is the fastest way to a boot loop.
