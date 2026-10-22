#!/usr/bin/env python3
"""
mkvendorboot.py - pack a vendor_boot.img v4 (GKI layout) for the iQOO 9T.

Usage:
  tools/host/mkvendorboot.py --dtb out/sm8450-iqoo9t.dtb \
      --cmdline 'console=ttyMSM0,115200n8 console=tty0 ignore_loglevel' \
      -o artifacts/vendor_boot-iqoo9t.img

The header is modelled on the stock PD2217F vendor_boot (page 4096,
dtb_addr 0x1f00000).  An empty vendor ramdisk is allowed; the boot.img
generic ramdisk then carries our initramfs alone.
"""

from __future__ import annotations

import argparse
import struct
import sys

MAGIC = b"VNDRBOOT"
PAGE = 4096
HEADER_SIZE = 2128
DTB_ADDR = 0x1F00000
KERNEL_ADDR = 0x8000
RAMDISK_ADDR = 0x1000000
TAGS_ADDR = 0x100
RAMDISK_TABLE_ENTRY_SIZE = 108


def pad(data: bytes, page: int = PAGE) -> bytes:
    return data + b"\0" * (-len(data) % page)


def repack_template(path: str, dtb: bytes, cmdline: str, output: str,
                    vram_override: bytes | None = None) -> int:
    d = open(path, "rb").read()
    if d[:8] != MAGIC:
        print(f"{path}: not a vendor_boot image", file=sys.stderr)
        return 1
    page, vram_size = struct.unpack_from("<I", d, 12)[0], struct.unpack_from("<I", d, 24)[0]
    hdr_size, dtb_size = struct.unpack_from("<2I", d, 2096)
    table_size, table_num, entry_size, bc_size = struct.unpack_from("<4I", d, 2112)
    vram_off = page
    vram = d[vram_off:vram_off + vram_size]
    if vram_override is not None:
        vram = vram_override
        vram_size = len(vram)
    dtb_off = (vram_off + vram_size + page - 1) // page * page
    table_off = (dtb_off + dtb_size + page - 1) // page * page
    bc_off = (table_off + table_size + page - 1) // page * page
    hdr = bytearray(d[:hdr_size])
    struct.pack_into("<I", hdr, 2100, len(dtb))
    if cmdline:
        hdr[28:28 + 2048] = cmdline.encode().ljust(2048, b"\0")[:2048]
    if vram_override is not None:
        struct.pack_into("<I", hdr, 24, len(vram))
        table = struct.pack("<3I", len(vram), 0, 1) + b"platform".ljust(32, b"\0") + b"\0" * 64
        table_size = len(table)
        table_num = 1
        struct.pack_into("<4I", hdr, 2112, table_size, table_num, 108, bc_size)
    else:
        table = d[table_off:table_off + table_size]
    image = pad(bytes(hdr))
    image += pad(vram)
    image += pad(dtb)
    if table_size:
        image += pad(table)
    if bc_size:
        image += pad(d[bc_off:bc_off + bc_size])
    with open(output, "wb") as f:
        f.write(image)
    print(f"wrote {output} ({len(image)} bytes): page={page} "
          f"vendor_ramdisk={vram_size} dtb={len(dtb)} table={table_size} "
          f"bootconfig={bc_size}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dtb", required=True)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--vendor-ramdisk")
    ap.add_argument("--bootconfig")
    ap.add_argument("--template", help="reuse a stock vendor_boot (ramdisk, "
                    "table, bootconfig) and replace only its DTB")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    dtb = open(args.dtb, "rb").read()
    if args.template:
        vram_override = None
        if args.vendor_ramdisk:
            vram_override = open(args.vendor_ramdisk, "rb").read()
        return repack_template(args.template, dtb, args.cmdline, args.output,
                               vram_override)
    vram = open(args.vendor_ramdisk, "rb").read() if args.vendor_ramdisk else b""
    bootconfig = open(args.bootconfig, "rb").read() if args.bootconfig else b""

    table = b""
    if vram:
        table = struct.pack("<3I", len(vram), 0, 1) + b"platform".ljust(32, b"\0") \
            + b"\0" * 64
        assert len(table) == RAMDISK_TABLE_ENTRY_SIZE, len(table)

    hdr = (
        MAGIC
        + struct.pack("<5I", 4, PAGE, KERNEL_ADDR, RAMDISK_ADDR, len(vram))
        + args.cmdline.encode().ljust(2048, b"\0")[:2048]
        + struct.pack("<I", TAGS_ADDR)
        + b"\0" * 16                      # name
        + struct.pack("<I", HEADER_SIZE)
        + struct.pack("<I", len(dtb))
        + struct.pack("<Q", DTB_ADDR)
        + struct.pack("<4I", len(table),
                      1 if vram else 0, RAMDISK_TABLE_ENTRY_SIZE,
                      len(bootconfig))
    )
    assert len(hdr) == HEADER_SIZE, len(hdr)

    image = pad(hdr)
    if vram:
        image += pad(vram)
    image += pad(dtb)
    if table:
        image += pad(table)
    if bootconfig:
        image += pad(bootconfig)

    with open(args.output, "wb") as f:
        f.write(image)
    print(f"wrote {args.output} ({len(image)} bytes): page={PAGE} "
          f"vendor_ramdisk={len(vram)} dtb={len(dtb)} bootconfig={len(bootconfig)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
