#!/usr/bin/env python3
"""
poke.py — single-dword physical read/write through the GPU primitive.

    poke.py read  PA
    poke.py write PA VALUE
    poke.py words PA VALUE [VALUE...]

Values accept 0x prefixes.  Each operation is a separate cheese_pa process
(one GPU op per process is a hard requirement on this target).

Examples
--------
    poke.py read 0xA8145E38
    poke.py write 0xAA7B0A60 0x7FFFFFFF
    poke.py words 0xA8145E38 0xF0013340 0x91298000 0x1400FFE9
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iqoo9t as t  # noqa: E402


def num(s: str) -> int:
    return int(s, 0)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("read")
    s.add_argument("pa", type=num)
    s.set_defaults(func=lambda a: do_read(a.pa))

    s = sub.add_parser("write")
    s.add_argument("pa", type=num)
    s.add_argument("value", type=num)
    s.set_defaults(func=lambda a: do_write(a.pa, a.value))

    s = sub.add_parser("words")
    s.add_argument("pa", type=num)
    s.add_argument("values", type=num, nargs="+")
    s.set_defaults(func=lambda a: do_words(a.pa, a.values))

    args = p.parse_args()
    return args.func(args)


def do_read(pa: int) -> int:
    v = t.gpu_read(pa)
    if v is None:
        print(f"0x{pa:x}: READ FAILED", file=sys.stderr)
        return 1
    print(f"0x{pa:08x} = 0x{v:08x}")
    return 0


def do_write(pa: int, value: int) -> int:
    if t.gpu_write(pa, value):
        print(f"0x{pa:08x} <- 0x{value & 0xFFFFFFFF:08x}  OK")
        return 0
    print(f"0x{pa:08x} <- 0x{value & 0xFFFFFFFF:08x}  FAILED", file=sys.stderr)
    return 1


def do_words(pa: int, values: list[int]) -> int:
    rc = 0
    for i, value in enumerate(values):
        addr = pa + 4 * i
        if t.gpu_write(addr, value):
            print(f"0x{addr:08x} <- 0x{value & 0xFFFFFFFF:08x}  OK")
        else:
            print(f"0x{addr:08x} <- 0x{value & 0xFFFFFFFF:08x}  FAILED", file=sys.stderr)
            rc = 1
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
