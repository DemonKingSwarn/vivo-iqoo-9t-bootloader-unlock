# tools/host — host-side control

Python 3, stdlib only. All scripts import `iqoo9t.py`.

| Script | Purpose |
|---|---|
| `iqoo9t.py` | shared library: adb, GPU read/write, patch lifecycle, unlock checks |
| `rootctl.py` | `status / verify / selftest / arm / disarm / su / devinfo / install` |
| `poke.py` | `read / write / words` one physical address |
| `selinux.py` | `status / on / off` (Enforcing ↔ Permissive) |
| `write_words.py` | retrying multi-dword writer (compat wrapper around `poke.py`) |
| `extract_dtbs.py` | split DTBs from DTBO / vendor_boot / boot / live FDT |
| `pull-stock-boot.sh` | dump the live `boot` partition for Magisk |

## Quick start

```bash
tools/build.sh
tools/host/rootctl.py install
tools/host/rootctl.py verify      # bootloader unlock state
tools/host/rootctl.py selftest    # GPU PA leak works?
tools/host/rootctl.py arm
tools/host/rootctl.py su id
```

Set `IQOO_SERIAL` if more than one adb device is attached:

```bash
export IQOO_SERIAL=15903686880004Q
```

## Environment knobs

`iqoo9t.py` honours:

* `IQOO_SERIAL` — target serial (otherwise the sole adb device is used)
* `ADDR` — the physical address map; edit only if your kernel build differs
  (see `docs/findings.md` for how the addresses were derived)
