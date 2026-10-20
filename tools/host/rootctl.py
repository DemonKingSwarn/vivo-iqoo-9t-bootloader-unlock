#!/usr/bin/env python3
"""
rootctl.py — arm/disarm the temporary root and inspect the device.

Subcommands
-----------
  status                 unlock state, SELinux, patch state, init_cred.usage
  selftest               verify the GPU PA leak on this boot
  arm                    patch __do_sys_capset + inflate usage + Permissive
  disarm                 restore the pristine kernel entry + Enforcing
  su CMD [ARGS...]       run CMD as uid 0 (add --shell-domain for binder)
  devinfo status|unlock|revert   drive the devinfo unlock payload
  backup-devinfo         pull /data/local/tmp/devinfo_before.img into artifacts/
  install                push tools/out/* to /data/local/tmp
  verify                 print only the bootloader unlock state

Examples
--------
  tools/host/rootctl.py install
  tools/host/rootctl.py selftest
  tools/host/rootctl.py arm
  tools/host/rootctl.py su id
  tools/host/rootctl.py su --shell-domain pm list packages | head
  tools/host/rootctl.py devinfo status
  tools/host/rootctl.py devinfo unlock
  tools/host/rootctl.py disarm
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iqoo9t as t  # noqa: E402


def cmd_status(args: argparse.Namespace) -> int:
    serial = t.serial()
    if not serial:
        print("device: NOT CONNECTED")
        return 1
    print(f"device            : {serial}")
    for key, val in t.unlock_state().items():
        print(f"{key:18s}: {val}")
    print(f"selinux           : {t.getenforce()}")
    if not args.no_gpu:
        print(f"capset patch      : {t.patch_state()}")
        usage = t.gpu_read(t.ADDR["init_cred_usage"])
        if usage is not None:
            print(f"init_cred.usage   : 0x{usage:08x}")
    return 0


def cmd_verify(_args: argparse.Namespace) -> int:
    if not t.serial():
        print("device: NOT CONNECTED")
        return 1
    ok = t.is_unlocked()
    for key, val in t.unlock_state().items():
        print(f"{key:18s}: {val}")
    print("result            :", "UNLOCKED" if ok else "LOCKED")
    return 0 if ok else 2


def cmd_selftest(_args: argparse.Namespace) -> int:
    ok = t.selftest()
    print("selftest          :", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def cmd_arm(_args: argparse.Namespace) -> int:
    t.arm()
    return 0


def cmd_disarm(_args: argparse.Namespace) -> int:
    t.disarm()
    return 0


def cmd_su(args: argparse.Namespace) -> int:
    if not args.command:
        print("usage: rootctl.py su [--shell-domain] CMD [ARGS...]", file=sys.stderr)
        return 2
    r = t.su(" ".join(args.command), shell_domain=args.shell_domain)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    return r.returncode


def cmd_devinfo(args: argparse.Namespace) -> int:
    r = t.shell(f"{t._binary('devinfo_unlock')} {args.action}", timeout=180)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    return r.returncode


def cmd_backup_devinfo(_args: argparse.Namespace) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    dest_dir = os.path.normpath(os.path.join(here, "..", "..", "artifacts"))
    os.makedirs(dest_dir, exist_ok=True)
    dest = os.path.join(dest_dir, "devinfo_before.img")
    t.pull("/data/local/tmp/devinfo_before.img", dest)
    print(f"saved {dest}")
    return 0


def cmd_install(_args: argparse.Namespace) -> int:
    t.ensure_tools()
    print("tools installed in", t.TOOLS_DIR)
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("status")
    s.add_argument("--no-gpu", action="store_true",
                   help="skip the two GPU reads (faster, no GPU fault risk)")
    s.set_defaults(func=cmd_status)

    sub.add_parser("verify").set_defaults(func=cmd_verify)
    sub.add_parser("selftest").set_defaults(func=cmd_selftest)
    sub.add_parser("arm").set_defaults(func=cmd_arm)
    sub.add_parser("disarm").set_defaults(func=cmd_disarm)
    sub.add_parser("install").set_defaults(func=cmd_install)
    sub.add_parser("backup-devinfo").set_defaults(func=cmd_backup_devinfo)

    s = sub.add_parser("su")
    s.add_argument("--shell-domain", action="store_true",
                   help="switch to u:r:shell:s0 (needed for pm/am/cmd)")
    s.add_argument("command", nargs=argparse.REMAINDER)
    s.set_defaults(func=cmd_su)

    s = sub.add_parser("devinfo")
    s.add_argument("action", choices=["status", "unlock", "revert"])
    s.set_defaults(func=cmd_devinfo)

    args = p.parse_args()
    try:
        return args.func(args)
    except t.ToolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
