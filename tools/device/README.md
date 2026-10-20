# tools/device — on-device payloads

Cross-compiled to static aarch64-android binaries by `../build.sh`.

| File | Origin | Purpose |
|---|---|---|
| `cheese_pa.c` | vendored, `RamenFast/zenfone9-root` (GPL-3.0) | CVE-2025-21479 GPU/SMMU primitive → arbitrary physical read/write |
| `pa_leak.c` / `pa_leak.h` | same | perf-based physical-address leak |
| `adrenaline.h` | same | KGSL ioctl definitions |
| `kallsyms_lookup.c` | same | in-kernel symbol lookup (used by the upstream auto-root path; not required by our POKE flow) |
| `rootcmd.c` | this project | `capset(NULL,NULL)` then `execvp` — root trigger |
| `capset_min.c` | this project | minimal capset trigger, prints identity |
| `devinfo_unlock.c` | this project | set `devinfo[0x0d]/[0x0e]` to unlock the ABL |

`UPSTREAM_COMMIT` records the exact upstream revision the vendored files came
from.  License: see `LICENSE` (GPL-3.0).

## Why `cheese_pa` is used with `CHEESE_POKE` / `CHEESE_PROBE_ONLY`

The upstream auto-root path (`CHEESE_ROOT`) hardcodes ZenFone 9 addresses
(`__do_sys_capset`, `init_cred`, `init_task.cred`).  The iQOO 9T build has
different addresses, so we only use the device-independent physical
read/write interface:

```
CHEESE_PROBE_ONLY=1 CHEESE_NO_RETRY=1 CHEESE_TARGET_PA=0x... cheese_pa
CHEESE_POKE=1 CHEESE_NO_RETRY=1 CHEESE_TARGET_PA=0x... \
  CHEESE_WRITE_PA=0x... CHEESE_WRITE_VAL=0x... cheese_pa
```

Both are wrapped by `tools/host/iqoo9t.py`.

## Hard constraints

* **One GPU operation per process.** The SMMU caches the first VA→PA
  translation inside a process, so a second operation reuses a stale page
  table. `iqoo9t.py` spawns a fresh process for every read/write.
* A lost TTBR0 race faults the GPU. KGSL throttles a context after 3 faults
  in 3 s, and heavy bursts can reboot the device. Keep arming short
  (~5 writes) and don't loop the primitive.
