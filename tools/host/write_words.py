#!/usr/bin/env python3
"""
write_words.py — retrying multi-dword writer (compatibility wrapper).

    write_words.py PA WORD [WORD...]

Equivalent to `poke.py words`.  Kept as a separate entry point because the
original notes and muscle memory reference it.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import poke  # noqa: E402


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    pa = int(sys.argv[1], 0)
    words = [int(x, 0) for x in sys.argv[2:]]
    return poke.do_words(pa, words)


if __name__ == "__main__":
    raise SystemExit(main())
