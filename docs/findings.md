# iQOO 9T (I2201 / vivo PD2217F) — Bootloader Unlock & Root Recon

**Date:** 2026-10-19 (updated 2026-10-20)
**Host:** Linux, adb/fastboot platform-tools 37.0.1
**Device serial:** 15903686880004Q (USB, adb authorized)
**Scope:** §1–§12 are read-only reconnaissance (no partition written, no `flashing
unlock` issued). **From §13 onward the device was modified**: temporary kernel
root, then a manual bootloader unlock by writing the `devinfo` partition. See
§15–§17 for the unlock. **Outcome: bootloader unlocked (`devinfo[0x0d]=[0x0e]=1`,
confirmed via both Android and fastboot), Magisk 30.7 root, a crDroid GSI
installed, and an own Linux kernel fork started — see §18–§20.** Note: from §19
onward the running OS reports `ro.boot.flash.locked=1` /
`vbmeta.device_state=locked` / `verifiedbootstate=green` (GSI property
spoofing); `devinfo` remains the source of truth.

---

> **Repository note (post-write-up reorganisation).** Everything needed is in
> this repository: `tools/device/` holds the C payloads, `tools/host/` the
> Python control scripts, `tools/build.sh` rebuilds the binaries, and generated
> images live under `artifacts/` (git-ignored). No host `$TMPDIR` paths are
> referenced anywhere; `/tmp` is not persistent across reboots. The step-by-step
> procedure is [`guide.md`](guide.md).

---

## 1. Device identity

| Property | Value |
|---|---|
| Model / device | `I2201` / `I2201` |
| Product | `I2201i` |
| Marketing name | iQOO 9T |
| vivo project | `PD2217F_EX` (India, `PD2217F_EX_IN_NULL_NULL`) |
| SoC | Qualcomm **SM8475** (Snapdragon 8+ Gen 1), platform `taro` |
| ABI | arm64-v8a |
| Android | 15 (`AP3A.240905.015.A2`) |
| Build | `compiler250917212650`, fingerprint `iQOO/I2201i/I2201:15/AP3A.240905.015.A2/compiler250917212650:user/release-keys` |
| Build type / tags | `user` / `release-keys` |
| Kernel | `5.10.233-android12-9-00001-g3f8134695cb5-ab13232202` (built 2025-03-17) |
| Android security patch | **2025-09-01** |
| Vendor security patch | 2025-03-05 |
| Installed packages | 453 |

---

## 2. Bootloader / verified boot state

Collected from Android:

```
ro.boot.flash.locked        = 1
ro.boot.vbmeta.device_state = locked
ro.boot.verifiedbootstate   = green
ro.boot.secureboot_enable   = 1
ro.boot.securebootkey       = qcom_mobile
ro.boot.securebootkeyver    = 6
ro.boot.securebootkeyhash   = 2e8bd2f5e3c859720e04cd04f4255fc1b96b3c00e83cfb3bc10442306c9f6134
ro.boot.vbmeta.avb_version  = 1.0
ro.boot.vbmeta.size         = 13248
ro.boot.vbmeta.digest       = 6170c27f7d591a55cf09d5d6e5816e315f0288c89d8296e8502a250a44d8b696
```

Collected from the bootloader (`adb reboot bootloader`, read-only queries):

```
(bootloader) unlocked:no
(bootloader) parallel-download-flash:yes
(bootloader) variant:SM_ UFS
(bootloader) hw-revision:10000
```

**Interpretation:** production secure boot (Qualcomm PK hash, key version 6), AVB 1.0, verified boot green, flash locked. Unsigned `boot`/`init_boot`/`vbmeta` images cannot be flashed and will not boot.

---

## 3. Fastboot command surface (observed)

| Command | Result |
|---|---|
| `fastboot devices` | device visible |
| `fastboot getvar all` | **works**, returns partition map + `unlocked:no` |
| `fastboot oem device-info` | **no response** (timed out, rc=124) |
| `fastboot flashing get_unlock_ability` | **no response** (timed out, rc=124) |
| `fastboot getvar unlocked` | **no response** (timed out) |
| `fastboot getvar current-slot` | **no response** (timed out) |
| `fastboot getvar product` | **no response** (timed out) |

Only the generic `getvar all` query is answered. The AOSP `flashing ...` handler and vivo `oem ...` handler do not reply at all. This is consistent with a stripped/BBK-customized ABL rather than a standard AOSP fastboot.

> Inference (not proof): the standard `fastboot flashing unlock` path is not usable on this ABL. I did **not** send `flashing unlock` because, if it did work, it erases userdata.

---

## 4. OEM-unlock plumbing inside Android

| Item | Observed |
|---|---|
| `oem_lock` binder service | **present**: `android.service.oemlock.IOemLockService` |
| `cmd oem_lock` | `No shell command implementation.` |
| `settings get global oem_unlock_enabled` | `null` (never set) |
| `settings get global system_unlock_state` | `0` (vivo-specific switch, present but off) |
| `sys.oem_unlock_allowed` | `1` |
| `persist.vivo.systemunlock.version` | `1` |
| Developer options enabled | `1` |
| Files named `*unlock*` in system/vendor/product | only wallpapers + audio; **no unlock binary/daemon** |

The framework OEM-lock service exists, but shell cannot drive it, and vivo exposes no `systemunlock` executable. The `system_unlock_state` setting is the closest vivo-specific artifact; it is a settings flag, not a secure-boot bypass.

---

## 5. Privilege / attack-surface inventory

### adb shell context

```
uid=2000(shell) gid=2000(shell)
groups=2000(shell),1004(input),1007(log),1011(adb),1015(sdcard_rw),1028(sdcard_r),
       1078(ext_data_rw),1079(ext_obb_rw),3001(net_bt_admin),3002(net_bt),3003(inet),
       3006(net_bw_stats),3009(readproc),3011(uhid),3012(readtracefs)
context=u:r:shell:s0
```

### Root / debug

| Check | Result |
|---|---|
| `adb root` | `adbd cannot run as root in production builds` |
| `su` in PATH / `/system/bin` / `/system/xbin` / `/sbin` | absent |
| setuid binaries in `/system/bin`, `/system/xbin`, `/vendor/bin`, `/system_ext/bin`, `/product/bin`, `/odm/bin` | **none** |
| `/data/adb` (Magisk) | permission denied (not present for shell) |
| `/sbin/.magisk`, `/system/etc/init/magisk*` | absent |
| SELinux | **Enforcing** |
| `ro.debuggable` / `ro.secure` | `0` / `1` |

### Block devices / partitions

```
/dev/block/by-name/boot -> /dev/block/sde13
brw------- root root 8, 77 /dev/block/sde13
brw------- root root 8,  0 /dev/block/sda
dd if=/dev/block/by-name/boot of=/dev/null -> Permission denied
```

Shell cannot read a single partition, so on-device extraction of `abl`/`xbl`/`boot` is not possible without root.

### Kernel hardening (from `/proc/config.gz`, readable)

```
CONFIG_CFI_CLANG=y                     (CFI_PERMISSIVE not set)
CONFIG_CFI_CLANG_SHADOW=y
CONFIG_SHADOW_CALL_STACK=y
CONFIG_ARM64_PTR_AUTH=y
CONFIG_RANDOMIZE_BASE=y                (KASLR)
CONFIG_STACKPROTECTOR_STRONG=y
CONFIG_HARDENED_USERCOPY=y
CONFIG_INIT_ON_ALLOC_DEFAULT_ON=y
CONFIG_SLAB_FREELIST_RANDOM=y
CONFIG_SLAB_FREELIST_HARDENED=y
CONFIG_SECURITY_SELINUX=y              (bootparam/disable off)
CONFIG_USER_NS is not set
CONFIG_BPF_SYSCALL=y
CONFIG_KALLSYMS=y / KALLSYMS_ALL=y
CONFIG_DEBUG_FS=y / DEBUG_FS_ALLOW_ALL=y
CONFIG_IO_URING=y
```

Runtime:

```
perf_event_paranoid = -1
unprivileged_bpf_disabled = 0
dmesg_restrict / kptr_restrict -> Permission denied (restricted)
dmesg -> klogctl: Permission denied
randomize_va_space -> Permission denied
/sys/kernel/debug -> dr-xr-xr-x root root (not traversable by shell)
/sys/fs/pstore    -> dr-xr-x--- system log
```

`perf_event_paranoid=-1` and `unprivileged_bpf_disabled=0` are the only "permissive" kernel knobs seen. They are reachable only through SELinux-allowed syscalls for the `shell` domain and do not by themselves give privilege escalation.

### Vendor services (running)

`vivo_daemon` (root), `vendor.factory.hardware.vivoatcmd`, `vendor.factory.hardware.vivoem`, `vendor.aftersale.hardware.vivoasem`, `vendor.vivo.hardware.bspservice`, `qseecomd`, `diag-router`, keymaster 4.0/4.1 (strongbox), and ~40 other vivo HALs. Binder services include `engineer_utile`, `system_defence_service`, `phonelock_service`, `oem_lock`, `ivocl`.

These are the natural "clue" surface: factory/engineering HALs and diagnostic services are the components that historically carry privileged or debug-only interfaces. They are SELinux-separated, only reachable with the right binder permissions, and none of them is documented as unlocking the bootloader.

---

## 6. Verdict against the original questions

| Question | Answer | Evidence |
|---|---|---|
| Is the bootloader locked? | **Yes** | `ro.boot.flash.locked=1`, `vbmeta.device_state=locked`, `getvar unlocked:no` |
| Is OEM unlocking available in the UI? | A framework `oem_lock` service exists, but no user-visible state has ever been set (`oem_unlock_enabled=null`), and shell cannot toggle it | §4 |
| Does vivo's fastboot implement standard unlock? | **No evidence it does**; `flashing get_unlock_ability` and `oem device-info` do not even answer | §3 |
| Can we read/flash partitions from adb? | **No** — all block devices are `0600 root:root` | §5 |
| Is there a local root path from `adb shell`? | **No practical one.** `user` build, SELinux enforcing, no setuid, no su, patched 2025-09 kernel with CFI/SCS/PAC/INIT_ON_ALLOC, no user namespaces. A working LPE for this exact build would have to be written and is not obtainable from reconnaissance. | §5 |
| Can REA "find" an unlock? | **No.** The gate is a vivo-signed token verified by ABL/secure boot, not a logic bug in a local binary. Private keys are not on the device. | §2, §3 |

### Why third-party "unlock code" services are not a safe answer

They either (a) use a vivo-authorized account/tool to mint a token (you are trusting a stranger with your IMEI/serial and paying for a service vivo does not sanction), or (b) are scams that take payment and deliver nothing or a bricked device. Nothing in this device's state suggests a free/offline bypass exists.

### Honest bottom line

The iQOO 9T cannot be bootloader-unlocked or rooted from `adb shell`, and there is no public exploit chain for this build. The only legitimate unlock path is a vivo-issued unlock authorization, which vivo has discontinued for this model. Any "solution" claiming otherwise is either a paid unauthorized service or a brick risk.

---

## 7. What could still be investigated (with more artifacts)

1. **ABL / XBL strings** — pull `abl`/`xbl` from a full OTA package (`PD2217F_EX` payload) and inspect the fastboot command table (`bbk`/`oem` handler, unlock token format). This tells you *which* command and token ABL expects, but not the private key needed to satisfy it.
2. **Settings APK** — confirm whether the OEM-unlocking preference is hidden/removed in the India firmware (`com.android.settings`), and whether `system_unlock_state` is consumed by any privileged app.
3. **vivo engineer/factory HALs** — REA/JADX analysis of the APKs that talk to `engineer_utile` / `vivoasem` / `vivoatcmd` for debug-only interfaces. Still bounded by SELinux and secure boot.
4. **REA MCP setup** — the REA MCP server is **not registered** for opencode on this host (`doctor --client opencode` reports `config_drift`), and JADX/Ghidra/IDA providers are absent. Android APK analysis therefore cannot run until those are supplied. See §8.

---

## 8. Tooling state (REA) — configured 2026-10-19

`setup --client opencode --dry-run --json` was reviewed, then applied with
`setup --client opencode --yes` (no Hopper install). Result: `status: ready`,
`appliedActions: ["configured_opencode","installed_skill"]`.

| Item | State |
|---|---|
| opencode registration | **aligned** — `mcp.rea` in `/home/swarn/.config/opencode/opencode.json`, command `npx -y rea-agents@6.7.0 mcp` |
| Backup | `/home/swarn/.config/opencode/opencode.json.rea.backup` (402 bytes, original preserved) |
| Unrelated provider config | preserved (`llama.cpp` / DemonAI block untouched) |
| Skill identity | **42**, 207 tools, `aligned` |
| Live server | handshake verified: `serverInfo {name: rea, version 6.7.0}`, protocol `2024-11-05`, `tools/list` answered |
| Analysis engines | still absent: Ghidra (`GHIDRA_INSTALL_DIR` unset), IDA MCP, JADX (`REA_JADX_MCP_JAR` unset), Kuna |
| Apktool | not on PATH (`REA_APKTOOL_COMMAND` unset) |

Working REA evidence collected after setup:

- `inspect_adb_device`-class ADB route is available (adb tools registered).
- `inspect_adb_package` on `com.android.settings` → Evidence
  `ev_ed98520d1024084be6c03765c7c857a0b28d0cfb584b0455a5e29b24647970c3`
  (`versionName 6.0`, `versionCode 160000`, `[ SYSTEM HAS CODE ]`, 160 requested permissions).
- `pull_adb_package` on `com.android.settings` → **complete**:
  `Settings.apk` (153,689,201 bytes; pulled locally, not committed).

Not yet possible: `inspect_android_package` (needs the caller-supplied JADX MCP
JAR), `decode_android_resources` (needs apktool), native `open_binary` on the
ABL (needs the image, which cannot be read from the locked device). So the
Settings APK is staged but not decompiled; the OEM-unlock preference question
remains open.

---

## 9. Commands used (reproducible, read-only)

```bash
adb devices -l
adb shell getprop
adb shell uname -a
adb shell getenforce
adb root
adb shell 'find /system/bin /system/xbin /vendor/bin /system_ext/bin /product/bin /odm/bin -maxdepth 1 -perm -4000 -type f'
adb shell 'dd if=/dev/block/by-name/boot of=/dev/null bs=4096 count=1'
adb shell 'zcat /proc/config.gz | grep -E "CONFIG_(CFI|...)"'
adb shell service list
adb shell cmd -l
adb shell cmd oem_lock help
adb shell settings list global
adb reboot bootloader
fastboot getvar all
timeout 8 fastboot oem device-info
timeout 8 fastboot flashing get_unlock_ability
fastboot reboot
```

## 10. Limitations / unknowns

- `fastboot getvar all` was captured partially (top of output); the full partition map exists on the device but was not persisted to disk.
- Individual `getvar` timeouts could be a host/ABL protocol quirk rather than proof the variable is unsupported. The `flashing get_unlock_ability` timeout is stronger evidence the AOSP unlock handler is absent, but still indirect.
- No firmware/OTA image was downloaded, so ABL internals were not inspected.
- `persist.vivo.systemunlock.version=1` and `system_unlock_state=0` are observations, not proof of any usable unlock API.
- Nothing here was tested destructively: no `flashing unlock`, no `oem unlock`, no `erase`, no partition writes.

---

## 11. Kernel exploit assessment (2026-10-19)

### Premise check

The kernel is **5.10.233-android12-9**, built **2025-03-17**. 5.10.233 is not an
old branch tip: it was the current 5.10.y stable release in early 2025. Android
security patch level is **2025-09-01** (vendor 2025-03-05). Kernel build date is
older than the system patch date, so 5.10.234+ / April–September 2025 fixes are
*not proven present*, but the version string alone does not establish that any
specific CVE is exploitable.

### Reachable configuration (observed from `/proc/config.gz`)

```
# CONFIG_USER_NS is not set          <- blocks the userns-based generic exploits
# CONFIG_NF_TABLES is not set        <- blocks nftables exploits (e.g. CVE-2024-1086)
# CONFIG_CHECKPOINT_RESTORE is not set
# CONFIG_DEVMEM is not set           <- no /dev/mem
# CONFIG_MODULE_SIG is not set       <- unsigned modules accepted, but loading needs CAP_SYS_MODULE
CONFIG_MODULES=y
CONFIG_IO_URING=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT_ALWAYS_ON=y
# CONFIG_BPF_UNPRIV_DEFAULT_OFF is not set   -> unprivileged BPF not disabled by config
CONFIG_USERFAULTFD=y
CONFIG_KCMP=y
CONFIG_VSOCKETS=y
CONFIG_OVERLAY_FS=y
CONFIG_MEMCG=y
CONFIG_KALLSYMS=y / KALLSYMS_ALL=y
```

### Runtime facts

```
uid=2000(shell)  CapEff=0000000000000000  CapBnd=00000000008000c0
NoNewPrivs=0  Seccomp=0  Seccomp_filters=0
SELinux: Enforcing, context u:r:shell:s0
/proc/kallsyms: Permission denied
dmesg: klogctl: Permission denied
unshare -Ur: Invalid argument  (user namespaces unavailable)
nft: not present; /system/bin/iptables present
perf_event_paranoid=-1; unprivileged_bpf_disabled=0
```

### What this means

- The two most widely used generic Android/Linux LPE families of 2023–2025 —
  nftables bugs and user-namespace bugs — **cannot be reached** here:
  `CONFIG_NF_TABLES` and `CONFIG_USER_NS` are both off.
- No `/dev/mem`, no readable `/proc/kallsyms`, no `dmesg`, no setuid helper, and
  `CapEff=0` in the shell domain. KASLR + CFI + shadow call stack + PAC +
  `INIT_ON_ALLOC` + SELinux enforcing raise the cost of any memory-corruption
  chain substantially.
- Unprivileged BPF and `perf_event_open` are not disabled at the kernel level,
  and `io_uring`, `userfaultfd`, `vsock`, and overlayfs are present. Those are
  *surfaces*, not proof of a bug. Using them would require a new, device-specific
  exploit, and would still have to defeat SELinux before the process gains
  meaningful root.
- The kernel image cannot be read from the locked device (`/dev/block/*` is
  root-only), so patch-presence cannot be diffed locally. A full OTA could be
  analyzed, but that is research, not a ready exploit.

### Verdict

There is **no public, applicable kernel exploit** available from this
reconnaissance, and none is provided here. Running an untrusted "universal root"
binary against a Snapdragon 8+ Gen 1 device is a realistic path to a crash,
data loss, or a hard brick (and it would still not unlock the bootloader).
Root obtained by a kernel LPE would also be lost on reboot and would not make
`fastboot` accept unsigned images.

If you want to pursue this seriously, the defensible steps are: obtain a full
`PD2217F_EX` OTA, extract and diff the kernel/ABL, and check specific CVEs
against the built-in patch set. That is a research project, not a one-command
root.

---

## 12. OEM-unlock UI and ABL re-check (2026-10-19, later)

The user confirmed OEM unlocking was **already on** before this session. This
section records the UI/ABL evidence and corrects the earlier assumption that
the Settings toggle was the blocker.

### Settings does expose the switch

- `com.android.settings` holds, granted:
  `MANAGE_USER_OEM_UNLOCK_STATE`, `OEM_UNLOCK_STATE`,
  `READ_OEM_UNLOCK_STATE`, `MANAGE_USERS`.
- The APK contains `OemLockInfoDialog`, `isBootloaderUnlocked()`,
  `OemLockManager`, `oem_unlock_enable`, `confirm_enable_oem_unlock_*`.
- Developer Options renders a real switch:
  `content-desc="OEM unlocking.Allow the bootloader to be unlocked"`,
  `checkable=true`, `enabled=true`, `bounds=[0,1485][1080,1692]`.
  The `checked` attribute read `false` even while
  `sys.oem_unlock_allowed=1`, so the attribute is not a reliable state source
  on this firmware.

### What tapping it did

- First tap changed `sys.oem_unlock_allowed` **1 → 0** and showed a vivo
  dialog: *"Please restart the device to enable device protection feature."*
  (a dialog with only `android:id/message`, no buttons; BACK dismisses it).
- Further taps were unreliable because a **PIN confirmation prompt** appeared
  (`Enter your PIN again` / `Enter your PIN to continue`) and taps were being
  consumed by it.
- After reboot, `sys.oem_unlock_allowed` is **1** again. The user states it was
  already on; treat `1` as the intended state and do not toggle it again.

### Bootloader re-check (full `getvar all`, 193 lines)

```
(bootloader) unlocked:no
(bootloader) secure:yes
(bootloader) product:taro
(bootloader) serialno:96ebe4c9
```

No `unlock-ability`, `oem-unlock`, token, or warranty variable is exposed.
`fastboot flashing get_unlock_ability` and `fastboot oem device-info` still
produce **no reply** (timeout). Only `getvar all` is answered.

### Corrected conclusion

OEM unlocking is **ON** (`sys.oem_unlock_allowed=1`). The lock that remains is
the vivo/BBK ABL plus Qualcomm secure boot (`secure:yes`), not the Android
Settings toggle. The only remaining direct test is
`fastboot flashing unlock`, which is destructive if it is implemented (it
factory-resets userdata); it has **not** been run. Everything observed so far
says the ABL will either ignore it or refuse it, but that cannot be proven
without sending it.

---

## §13 — CVE-2025-21479 Adreno KGSL temporary root (SUCCESS)

### Pivot

- `ro.vendor.build.security_patch = 2025-03-05` is before the Qualcomm
  June-2025 bulletin ⇒ **CVE-2025-21479** (Adreno GPU SDS/RB access-check flaw,
  `CP_SMMU_TABLE_UPDATE` TTBR0 overwrite) is present.
- Reference port: `RamenFast/zenfone9-root`. The ASUS ZenFone 9 is the **same
  SM8475 / Adreno 730** as the iQOO 9T. Its `perf`-based physical-address leak
  replaces the fixed `gPhyAddrs` spray lottery that caused the earlier crash.

### Why the earlier attempts failed

- The `gPhyAddrs` tables in the iQOO Neo8 / PD2232 repos are the same shared
  12 GB table. Entries above ~7.5 GB are invalid on this 8 GB device; the
  attempt at phys ~8.1 GB (`0x9e3fbe000`) exceeded installed RAM and caused the
  shutdown/reboot.
- Fix: **leak the physical address of a page we own** — no guessing.

### Device facts used

- `perf_event_paranoid = -1` ⇒ an unprivileged `PERF_TYPE_BREAKPOINT` sample's
  `PERF_SAMPLE_PHYS_ADDR` reveals the page's GPU-side address (leak_phys_page),
  value = `0x800000000 + phys`.
- Kernel Image `_text` link VA `0xffffffc008000000`, physical base `0xA8000000`,
  no physical KASLR; virtual slide recovered at runtime from `init_task.cred`.
- Symbols from the extracted `boot.img` Image:
  - `__do_sys_capset` phys `0xA8145E38`
  - `init_cred` VA `0xffffffc00a7b0a60`
  - `prepare_creds` VA `0xffffffc0081856d0`
  - `__memcpy` VA `0xffffffc00801f680`
  - `commit_creds` VA `0xffffffc008185de4`
  - `init_task.cred` phys `0xAA79C600`, real_cred `0xAA79C5F8`
  - `selinux_state` phys `0xAAA42B90`
  - slide observed on the successful boot: `0x28a4400000`

### Reliable primitive

- Ported `cheese_pa` (static aarch64, source in `tools/device/cheese_pa.c`).
  `CHEESE_SELFTEST=1` passes; `CHEESE_COPY_WRITE_TEST=1` passes after two bug
  fixes: build the page table **before** staging the payload (the original
  stages then `setup_pagetables()` memsets the page), and **wait for the GPU
  completion marker** before reusing the page.
- **One GPU operation per process** is required — inside a process the SMMU
  caches the translation of the VA page used by the first op.
  - read:  `CHEESE_PROBE_ONLY=1 CHEESE_NO_RETRY=1 CHEESE_TARGET_PA=0x...`
  - write: `CHEESE_POKE=1 ... CHEESE_WRITE_PA=0x... CHEESE_WRITE_VAL=0x...`
- Helpers: `tools/host/iqoo9t.py` (verify/patch/restore),
  `tools/host/write_words.py` (retrying dword writer).
- The kernel resolves the slide from `init_task.cred`; on-device reads:
  `init_task.cred = real_cred = 0xffffffe8aebb0a60`.

### Root

- Cred-swap shellcode (16 dwords, armed one dword per process):
  `prepare_creds()` → copy `uid/caps` from `init_cred+4` (0x4c bytes) →
  `commit_creds()`. It keeps the shell SID and gives `uid=0`, but vivo's
  anti-root service (`com.vivo.sps:rms`, `vivo_daemon`) SIGKILLs the process
  shortly after `capset`, even when those daemons are SIGSTOPped first.
- Working variant: a 3-word tail-call written at `__do_sys_capset`:
  `adrp x0, page(init_cred); add x0,x0,#0xa60; b commit_creds`
  = `f0013340 91298000 1400ffe9`. It installs `init_cred` wholesale ⇒
  `uid=0` in the **`u:r:kernel:s0`** domain, which is not killed.
- Proof captured on-device:
  ```
  uid=0(root) gid=0(root) groups=0(root) context=u:r:kernel:s0
  getenforce=Permissive
  CapEff: 000001ffffffffff
  /proc/kallsyms readable; /data/data listable; write /data/local/tmp OK
  ```
  Trigger wrapper: `/data/local/tmp/rootcmd sh -c "<cmd>"`
  (`rootcmd` = `capset(NULL,NULL)` then `execvp`).

### Caveats / not done

- **Temporary only** (until reboot). No bootloader unlock ⇒ no persistent
  Magisk/KernelSU.
- Early sessions appeared to be rebooted by "vivo anti-root". This was **wrong**
  — see §14. The real cause was an `init_cred` refcount underflow.
- `fastboot flashing unlock` was still never run; the unlock token remains
  RSA-2048 server-signed and is not forgeable.
- Clean-up: the original 16 dwords of `__do_sys_capset` are in
  `tools/host/iqoo9t.py` (`ORIG`); restore with
  `tools/host/iqoo9t.py restore`, and set `selinux_state` back to `0x01010001`
  (or reboot — after the reboot `getenforce=Enforcing` again).

---

## §14 — Root stabilization (reboot root-cause fixed)

### The reboot was NOT anti-root

`__do_sys_capset` tail-calls `commit_creds(&init_cred)`. `commit_creds()`
does **not** take a reference to its argument — it assumes the caller already
owns one. `init_cred.usage` starts at **4**, so every `capset`+process-exit
decrements the *global* `init_cred` refcount. After ~4 uses it reaches 0 and
the kernel frees `init_cred` ⇒ crash/reboot.

Proof:
- `init_cred.usage` (PA `0xAA7B0A60`) read `0x00000004` on a fresh boot.
- After 12 `capset_min` runs it read `0x7ffffff3` (exactly 12 decrements).
- After inflating it to `0x7fffffff`, 12 consecutive `capset` calls completed
  and the **device stayed up** (previously ~4 uses rebooted it).

Fix: inflate `init_cred.usage` before/while the patch is armed.

### Enforcing vs Permissive

Under **Enforcing**, a userspace task carrying the kernel SID is invalid:
`capset` succeeds but the process dies right after (observed: only the
"before" line printed). dmesg shows the intended-to-be-denied exec:

```
avc: denied { execute } for comm="rootcmd" name="sh"
  scontext=u:r:kernel:s0 tcontext=u:object_r:shell_exec:s0 permissive=1
```

So keep `selinux_state.enforcing = 0` while rooted. The GPU store to
`selinux_state` is clobbered by CPU writeback (hot cache line) and needs a
few retries; it then holds.

### Stable recipe (minimal GPU ops)

Kernel addresses are physical and fixed; the 3-word entry patch is
PC-relative, so both survive reboots (no per-boot slide needed):

1. write `__do_sys_capset` (PA `0xA8145E38`):
   `0xF0013340 0x91298000 0x1400FFE9`  (`adrp x0,init_cred; add #0xa60; b commit_creds`)
2. write `init_cred.usage` (PA `0xAA7B0A60`) = `0x7FFFFFFF`
3. write `selinux_state` (PA `0xAAA42B90`) = `0x01010000` until `getenforce` = Permissive

Tooling: `tools/host/rootctl.py {status,arm,disarm,su CMD}`.

### Verified stable

- 30-second root shell survived (`id` before and after the sleep).
- `uid=0(root) ... context=u:r:kernel:s0`, `getenforce=Permissive`.
- `/proc/kallsyms` readable, `/data/data` listable, root-owned file copy OK.
- Device stayed up across many root commands.

### Remaining fragility

- The KGSL primitive can still lose the TTBR0 race; a **heavy** sequence of
  GPU writes (e.g. a disarm/re-arm test with ~40 ops) rebooted the device
  once. Normal arming (~6 GPU writes) is fine — arm once and reuse.
- A reboot clears the patch and SELinux returns to Enforcing; re-run
  `rootctl_iqoo9t.py arm`.
- Root is per-boot and relies on `capset` being patched; it is not persistent
  across reboot and there is no bootloader-level persistence.

---

## §15 — Manual bootloader unlock via the `devinfo` partition (attempt)

### Rationale

The ABL's unlock predicates were recovered from the ABL PE
(`.../file-f536d559-.../section1.pe`, disassembled offline with `llvm-objdump` (not stored)):

```
is_device_unlocked()          = devinfo[0x0d] != 0   OR  byte[0xa74d0] != 0
is_device_critical_unlocked() = devinfo[0x0e] != 0   OR  byte[0xa74d1] != 0
```

On the **valid-magic** path the ABL performs **no CRC and no signature check** on
`devinfo` — it only checks `"ANDROID-BOOT!"`. So flipping the two flag bytes in
the on-disk `devinfo` partition should make the ABL report the device unlocked.
This is a *trust-decision overwrite*, not an exploit of the RSA unlock token
(the official `flashing unlock` token remains server-signed and unforgeable).

### `devinfo` layout (as observed)

| Offset | Meaning | Value |
|---|---|---|
| `0x00` | magic `"ANDROID-BOOT!"` (13 B) | `41 4e 44 … 54 21` |
| `0x0d` | `is_unlocked` | `00` → `01` |
| `0x0e` | `is_unlock_critical` | `00` → `01` |
| `0x0f` | `is_charger_screen_enabled` | `01` |
| `0x90` | (unknown, set by ABL) | `01` |

- Block device: `/dev/block/sde28`, 4096 bytes.
- Unlocked image saved host-side: `artifacts/devinfo_unlocked.img` (`0x0d=0x0e=0x01`); the pre-unlock copy was lost with the post-unlock wipe.
- Non-zero bytes in the pristine image were only the magic, `0x0f`, and `0x90`
  (no CRC field present).

### Payload

Built as a single static ARM64 binary, **in-process** (no shell `exec`, to keep
the number of GPU operations minimal):

`tools/device/devinfo_unlock.c` → `tools/out/devinfo_unlock`

Sequence:

1. `capset(NULL, NULL)` — runs the §13/§14 kernel entry patch to obtain
   `init_cred` (uid 0, `u:r:kernel:s0`) without spawning a child.
2. GPU store to `selinux_state` (`0xAAA42B90`) = `0x01010000` until `getenforce`
   reads `Permissive` (kernel-domain root cannot write selinuxfs under Enforcing).
3. `pwrite` `devinfo` at offset `0x0d` = `1`, offset `0x0e` = `1`.

### Result

Write succeeded (`enforce_write=1`) and the readback changed as intended:

```
before: … 42 4f 4f 54 21 00 00 01
after:  … 42 4f 4f 54 21 01 01 01
```

…but after `adb reboot` the device **did not boot**. It came up on a vivo
repair/recovery screen titled **"device is corrupt"** with the options:

> Repair system · Clear all data · Restart · Power off

The device did **not** enumerate on USB in that state (no adb, no fastboot, no
Qualcomm 9008/`05c6`). Volume Down + Power returned to the same screen.

### Why it *looked* like a brick (and what the ABL actually did)

ABL strings confirm it evaluates the boot/verified-boot result and pushes state
into the TEE/keymaster:

```
Device is unlocked, Skipping boot verification
Device critical unlocked: %a
ERROR: Device State %a, AvbSlotVerify returned %a
KeyMasterSetRotAndBootState: SetFuse (TZ_HLOS_IMG_TAMPER_FUSE) fails!
KeyMasterSetRotAndBootState: SetFuse (TZ_HLOS_TAMPER_NOTIFY_FUSE) fails!
device is unlocked , rebooting edl
```

The "device is corrupt" screen was **not** a permanent fuse/brick. The real cause
was the standard post-unlock requirement: **changing the verified-boot state from
locked→green to unlocked→orange invalidates the existing userdata**, and a normal
OEM unlock performs a `userdata` wipe as part of unlocking. Our hand-written
`devinfo` flip skipped that wipe, so the system refused to boot and the recovery
offered the fix. See §16.

---

## §16 — Recovery: the bootloader was already unlocked

Retrying the boot combos produced the decisive evidence: the ABL **printed the
unlocked warning** ("bootloader is unlocked"), proceeded to the boot logo, then
fell back to the same repair menu. That means:

- `is_unlocked` / `is_unlock_critical` were read as set → the ABL *did* skip boot
  verification, and the verified-boot state became `orange`.
- The only thing standing between us and a boot was the un-wiped userdata.

**Fix:** on the repair menu, choose **`Clear all data`** (factory reset), *not*
`Repair system`.

- `Clear all data` wipes `userdata` → resolves the boot-state/data mismatch.
- `Repair system` was deliberately avoided: vivo's system repair may re-flash and
  re-lock the bootloader (and/or restore the firmware to a patched build).

After the wipe the device booted into a fresh Android setup ("hello" screen).

---

## §17 — Unlock verified (Android + fastboot)

### From Android (`adb shell getprop`)

```
ro.boot.flash.locked        = 0         <- was 1
ro.boot.verifiedbootstate   = orange    <- was green
ro.boot.vbmeta.device_state = unlocked  <- was locked
ro.boot.veritymode          = enforcing
ro.boot.secureboot_enable   = 1
ro.boot.securebootkey       = qcom_mobile
```

### From fastboot (`adb reboot bootloader`)

```
$ fastboot devices
15903686880004Q  fastboot

$ fastboot getvar unlocked
unlocked: yes                            <- was "unlocked:no"

$ fastboot getvar product
product: taro

$ fastboot getvar max-download-size
max-download-size: 805306368             (~768 MiB)

$ fastboot getvar current-slot
FAILED (remote: 'GetVar Variable Not found')   <- A/B naming differs on this ABL
```

**This is a genuine bootloader unlock.** iOS-style secure-boot is still on
(`secureboot_enable=1`, key `qcom_mobile` v6), and the verified-boot state is
`orange` (unlocked, not "red"/tampered).

### What this enables

- Unsigned `boot` / `vendor_boot` / `init_boot` / `vbmeta` images can now be
  flashed and will boot → this is the path to a **mainline Linux kernel + rootfs**
  on the iQOO 9T (SM8475).
- Fastboot is reachable without USB debugging (works straight from the ABL).

### What NOT to do

- **Never flash `abl`, `xbl`, `tz`, `hyp`, `keymint`.** Secure boot is enabled and
  PBL verifies the boot chain; a bad signature = hard brick.
- Avoid the recovery **`Repair system`** option unless re-locking is acceptable.
- Re-locking (if ever needed) restores `devinfo[0x0d]=[0x0e]=0` **and** normally
  requires a data wipe; the official re-lock path may also reject a hand-modified
  `devinfo`.

### Reproducibility / artifacts

| File | Purpose |
|---|---|
| `artifacts/devinfo_unlocked.img` | 4096-byte `devinfo` as written (unlock flags set) |
| `tools/device/devinfo_unlock.c` | in-process unlock payload source |
| `tools/out/devinfo_unlock` | static ARM64 payload (pushed to `/data/local/tmp/`) |
| — | ABL disassembly was generated offline, not stored |
| `tools/host/rootctl.py` | `{status,arm,disarm,su CMD}` control |

### Net result

Starting from a **locked** iQOO 9T with production Qualcomm secure boot, we:

1. obtained per-boot kernel root via CVE-2025-21479 (Adreno KGSL) — §13/§14;
2. used that root to overwrite the ABL's trusted `devinfo` unlock flags;
3. forced the required post-unlock `userdata` wipe from vivo recovery;
4. booted Android with the bootloader **unlocked** (`orange`), confirmed on both
   sides of the verified-boot boundary.

The device is now ready to receive a custom boot image (Linux) via fastboot.

---

## §18 — vbmeta / AVB layout (corrected parse; replaces the approximate §recon numbers)

Two of the four vbmeta images are real AVB images, built by vivo's
**avbtool 1.3.0** (release string at `0x80`). The other two are empty stubs:

| Partition (block dev) | Signed | Algorithm | Descriptors |
|---|---|---|---|
| `vbmeta` (`/dev/block/sde16`) | yes | `SHA256_RSA2048` | 4 chain-partition descriptors |
| `vbmeta_system` (`/dev/block/sda6`) | yes | `SHA256_RSA2048` | hash descriptors (disk/system) |
| `vbmeta_oem` (`/dev/block/sda16`) | no | `NONE` (`alg=0`) | none |
| `vbmeta_vgc` (`/dev/block/sda15`) | no | `NONE` (`alg=0`) | none |

Corrected facts (the first-pass parse had field offsets wrong):

- **Signature is RSA-2048, not RSA-4096.** Evidence is consistent across fields:
  signature size `0x100` (256 B), hash size `0x20`, public key blob
  `0x208` (520 B = 8 B header + 512 B modulus/precomputed values), and every
  embedded key prefixed with `key_num_bits = 0x800` (2048).
- Header layout (vivo build; big-endian, `u32` fields):

  | Offset | Field | `vbmeta` | `vbmeta_system` |
  |---|---|---|---|
  | `0x10` | auth block size | `0x140` (320 B) | `0x140` (320 B) |
  | `0x18` | aux block size | `0xbc0` (3008 B) | `0x10c0` (4288 B) |
  | `0x1c` | algorithm | `1` (SHA256_RSA2048) | `1` |
  | `0x2c` | hash size | `0x20` | `0x20` |
  | `0x3c` | signature size | `0x100` | `0x100` |
  | `0x4c` | public key size | `0x208` (520 B) | `0x208` |
  | `0x6c` | descriptors size | `0x9b8` (2488 B) | `0xe88` (3720 B) |

- **`rollback_index` = 0 in the header.** The earlier read-back numbers
  "3008 / 4240" were block sizes (`0xbc0` and `0x10c0`), not rollback values —
  no anti-rollback counter is exercised by flashing these partitions. Flags
  (verity/verification enable) are `0`, i.e. fully enforced.
- `vbmeta` descriptors are **all chain-partition type**, each embedding its own
  RSA-2048 public key, chaining to: `boot` (rollback_index_location `1`),
  `dtbo` (`4`), `vbmeta_system` (`13`), `vendor_boot` (`11`). So `boot`,
  `dtbo` and `vendor_boot` trust comes from here, and `system` verification
  comes via the `vbmeta_system` chain.
- Practical consequence: flashing the dumped stock `vbmeta.img` with
  `--disable-verity --disable-verification` (flags set in the header) keeps the
  user-selected `boot`/`system` content bootable; no rollback counter is
  touched.

### Artifacts

| File | Contents |
|---|---|
| `artifacts/vbmeta.img` etc. | dumped stock images (64 KiB each) |
| `artifacts/devinfo_unlocked.img` | 4096-byte `devinfo` as currently written (`magic ANDROID-BOOT!`, unlock flags set) |

---

## §19 — crDroid GSI installed (post-unlock daily driver)

A crDroid GSI (`tdgsi_arm64_ab`, crDroid 10.13 "bgN-slim" unofficial build,
Android 14) was flashed into the `system` logical partition inside `super`
(device is A-only/slotless, dynamic partitions — §recon). State after boot:

- Boots and passes daily use; `ro.build.version.release=14`,
  `ro.crdroid.version=10.13` — while the reported build fingerprint still shows
  the vendor Android-12 base string, which is normal for a GSI.
- **Magisk root kept working**: `su` gives uid 0 with SELinux context
  `u:r:magisk:s0` (Magisk 30.7).
- **Bootloader props are spoofed by the GSI**: the running system reports
  `ro.boot.flash.locked=1`, `ro.boot.vbmeta.device_state=locked`,
  `ro.boot.verifiedbootstate=green` — but reading `/dev/block/by-name/devinfo`
  directly (as root) still shows `devinfo[0x0d]=0x0e=0x01`, i.e. **unlocked**.
  After a GSI, trust `devinfo`, not the properties.
- Data was wiped by the GSI install (any `/data`-side tooling recovered from
  this repo/§13–§17 does not persist through it).

---

## §20 — DTB extraction and the Linux kernel fork start

### DTB inventory

`tools/host/extract_dtbs.py` (written here) splits all three container types on
the device and names the trees by their `model`/`compatible` strings:

- 11 trees from `dtbo` (device-specific overlays, one per SKU/panel/PMIC
  combination),
- 6 base SoC trees from `vendor_boot`,
- 1 live merged tree from `/sys/firmware/fdt` (model
  `Cape QRD with PM8008 PD2217F_EX`).

All are committed under `DTB/` with a `manifest.tsv` and `SHA256SUMS`.

### Upstream status (kernel 7.2.11)

- There is **no `sm8475` / "cape" platform support** in upstream 7.2.11 (nor in
  torvalds master as of 2026-10-20). The closest mainline platform is
  `sm8450.dtsi` (Waipio); the SM8475 is the same die re-binned with per-device
  fan-out, so `sm8450.dtsi` is the base.
- All the PMICs our live DTB uses have upstream `.dtsi` includes already:
  `pm8350`, `pm8350b`, `pm8350c`, `pm8450`, `pmk8350`, `pmr735a`, `pmr735b`
  (only `pm8008i`/`pm8008j` camera/display regulators are device-specific).
- Toolchain: Android NDK r29 (`tools/.toolchain/android-ndk-r29/`) with
  `LLVM=1 LLVM_IAS=1` builds the tree clean.

### Fork state

- Repo: `linux-iqoo9t/` (own fork, upstream 7.2.11 as commit 1, branch
  `iqoo9t-cape`).
- Planned next commits: add `sm8475.dtsi` + `sm8475-iqoo9t.dts` derived from the
  `DTB/live/` tree (not the vendor `.dts` files), with panel, UART
  (`qup_uart@99c000`, 115200n8 — there is no bootloader framebuffer, so early
  debug must go over a USB NCM/ACM gadget), UFS, UDC, TLMM and the PMIC
  regulator set above.
- Build target for milestone 1: `Image` + `sm8475-iqoo9t.dtb`, tested with
  `fastboot boot` only (no flash) alongside a busybox initramfs that
  configures the configfs NCM/ACM gadget for readout, since nothing else is
  guaranteed until UFS root is proven.

See §21 for what actually landed.

## §21 — Milestone 1 payload: board DTS, UFS, eUSB2 and the boot image

### Board DTS (`sm8450-iqoo9t.dts`)

Upstream has no `sm8475` platform file, but it does ship the **SM8475 UFS PHY**
programming (`drivers/phy/qualcomm/phy-qcom-qmp-ufs.c`: `sm8475_ufsphy_cfg`,
`qcom,sm8475-qmp-ufs-phy`). So the board file includes `sm8450.dtsi` and
overrides only what differs on Cape, everything taken from `DTB/live/`:

| Item | Vendor (live DTB) | Mainline base | Board override |
|---|---|---|---|
| UFS PHY | `0x1d80000`, size `0x2000`, QMP v4 Cape | `phy@1d87000`, `0x1000` | `compatible = "qcom,sm8475-qmp-ufs-phy"`, `reg = <0 0x1d80000 0 0x2000>` |
| UFS vcc | pm8350_l7 (2.4–3.54 V) | board-specific | `vreg_l7b_2p95` |
| UFS vccq / vdd-hba / vddp-ref | pm8350_l9 (1.2 V) | board-specific | `vreg_l9b_1p2` |
| UFS vdda-phy | pm8350_l5 (0.88 V) | board-specific | `vreg_l5b_0p88` |
| UFS vdda-pll | pm8350c_l10 (1.2 V) | board-specific | `vreg_l10c_1p2` |
| UFS reset | GPIO 210, active low | same convention | `reset-gpios = <&tlmm 210 GPIO_ACTIVE_LOW>` |
| HS USB PHY | `0x88e3000`, **eUSB2**, `vdd`/`vdda12` | `phy@88e3000`, "hs-7nm" | `compatible = "qcom,usb-snps-eusb2-phy"`, `reg` `0x154`, clocks `<&gcc GCC_EUSB3_0_CLKREF_EN>` |
| USB role/speed | `dr_mode = "otg"`, `maximum-speed = "high-speed"`, SS PHY disabled | OTG + role switch | peripheral, HS only, SS PHY dropped from `phys` |

The vendor DT's USB is High-Speed only: it disables the USB3/DP combo PHY and
points DWC3 at the eUSB2 HS PHY. We mirror that instead of pretending the SS
PHY works.

Two small fork patches:

1. `drivers/phy/phy-snps-eusb2.c`: accept the vendor compatible
   `qcom,usb-snps-eusb2-phy` and reuse the SM8550 eUSB2 data (same IP).
2. `arch/arm64/boot/dts/qcom/Makefile`: build `sm8450-iqoo9t.dtb`.

### Gotchas worth remembering

- UFS PHY: vendor node is at `0x1d80000` with a `0x2000` window and compatible
  `qcom,ufs-phy-qmp-v4-cape`. Mainline knows neither the address nor that
  string, so the board DTS moves the node and uses
  `qcom,sm8475-qmp-ufs-phy`.
- UFS HC: the vendor node carries three `reg` ranges; the mainline `ufshc`
  driver maps only the first, which is the one that matters.
- eUSB2 HS PHY: mainline's `phy-snps-eusb2` does not match the vendor
  compatible `qcom,usb-snps-eusb2-phy`; the fork adds it and reuses the SM8550
  data. The two I2C eUSB2 repeaters have no mainline driver and must work on
  their power-on defaults for now.
- The stock boot header is v4 and its DTB lives in `vendor_boot`; a RAM-only
  `fastboot boot` test therefore has to use a legacy v0 Image+DTB+ramdisk
  image (see `tools/host/mkbootimg.py`).
- Blind-debug scaffolding: simpledrm on the ABL splash carveout
  (`splash@b8000000`) and ramoops at `0xba000000`, so a silent panic still
  leaves a log in `/sys/fs/pstore` after the next boot.
