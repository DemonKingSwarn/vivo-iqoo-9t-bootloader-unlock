#!/usr/bin/env python3
"""
selinux.py — toggle SELinux by writing the kernel's selinux_state.

    selinux.py status
    selinux.py off      # Permissive (required for the root trigger on this build)
    selinux.py on       # Enforcing

The write is done through the GPU primitive and is verified with `getenforce`.
CPU writeback can clobber the change, so the script retries.
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iqoo9t as t  # noqa: E402


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("state", choices=["status", "on", "off", "enforcing", "permissive"])
    args = p.parse_args()

    if args.state == "status":
        print(t.getenforce())
        return 0

    want_permissive = args.state in ("off", "permissive")
    ok = t.set_permissive() if want_permissive else t.set_enforcing()
    print(f"selinux: {t.getenforce()} ({'ok' if ok else 'FAILED'})")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
