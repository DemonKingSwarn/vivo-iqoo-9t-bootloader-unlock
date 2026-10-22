#!/usr/bin/env python3
"""
mkbootimg.py — minimal Android boot image packer/unpacker for bring-up work.

Only what this project needs: legacy header v0 (with an appended DTB, which
is what Qualcomm's ABL looks for when no vendor_boot is involved) and the
modern header v4 (GKI layout, kernel + generic ramdisk).

Usage:
    mkbootimg.py -o boot.img --header-version 0 \
        --kernel Image --dtb board.dtb --ramdisk initramfs.cpio.gz \
        --cmdline "console=ttyMSM0,115200n8"
    mkbootimg.py --print boot.img
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

MAGIC = b"ANDROID!"
PAGE = 4096
BASE = 0x80000000
KERNEL_OFFSET = 0x00008000
RAMDISK_OFFSET = 0x01000000
TAGS_OFFSET = 0x00000100


def _pad(data: bytes, page: int) -> bytes:
    rem = len(data) % page
    return data if rem == 0 else data + b"\0" * (page - rem)


def _id(kernel: bytes, ramdisk: bytes, second: bytes) -> bytes:
    import hashlib

    h = hashlib.sha1()
    for chunk in (kernel, ramdisk, second):
        h.update(chunk)
        h.update(struct.pack("<I", len(chunk)))
    return h.digest() + b"\0" * 12


def pack_v0(kernel: bytes, ramdisk: bytes, cmdline: str, page: int,
            base: int = BASE, os_version: int = 0x18000193) -> bytes:
    kernel_addr = base + KERNEL_OFFSET
    # An uncompressed arm64 Image plus appended DTB is ~40 MiB; a fixed
    # 0x81000000 ramdisk address would land inside it, so place the ramdisk
    # after the kernel with a 2 MiB gap.
    ramdisk_addr = ((kernel_addr + len(kernel) + 0x1FFFFF) & ~0x1FFFFF) + 0x200000
    tags_addr = base + TAGS_OFFSET
    name = b"iqoo9t\0".ljust(16, b"\0")
    cmd = cmdline.encode().ljust(512, b"\0")[:512]
    extra = b"\0" * 1024
    hdr = (
        MAGIC
        + struct.pack(
            "<8I",
            len(kernel), kernel_addr, len(ramdisk), ramdisk_addr,
            0, 0, tags_addr, page,
        )
        + struct.pack("<2I", 0, os_version)
        + name
        + cmd
        + _id(kernel, ramdisk, b"")
        + extra
    )
    assert len(hdr) == 1632, len(hdr)
    return _pad(hdr, page) + _pad(kernel, page) + _pad(ramdisk, page)


def pack_v4(kernel: bytes, ramdisk: bytes, cmdline: str, page: int,
            os_version: int = 0x18000193) -> bytes:
    # Header v4 is a fixed 1584 bytes: magic, sizes, os_version, header_size,
    # reserved[4], header_version, cmdline[1536].
    hdr = (
        MAGIC
        + struct.pack("<4I", len(kernel), len(ramdisk), os_version, 1584)
        + struct.pack("<4I", 0, 0, 0, 0)
        + struct.pack("<I", 4)
        + cmdline.encode().ljust(1536, b"\0")[:1536]
    )
    assert len(hdr) == 1584, len(hdr)
    return _pad(hdr, page) + _pad(kernel, page) + _pad(ramdisk, page)


def print_image(path: str) -> int:
    with open(path, "rb") as f:
        data = f.read(4096)
    if data[:8] != MAGIC:
        print(f"{path}: not an Android boot image")
        return 1
    version, = struct.unpack("<I", data[40:44])
    if version == 4:
        ksize, rsize, osver, hsize = struct.unpack("<4I", data[8:24])
        cmd = data[44:1580].split(b"\0")[0].decode()
        print(f"{path}: header v4, kernel={ksize} ramdisk={rsize} "
              f"os_version={osver:#x} header_size={hsize}")
    else:
        (ksize, kaddr, rsize, raddr, ssize, saddr, tags, page) = struct.unpack(
            "<8I", data[8:40])
        osver, = struct.unpack("<I", data[44:48])
        cmd = data[64:576].split(b"\0")[0].decode()
        print(f"{path}: header v{version}, kernel={ksize} @{kaddr:#x} "
              f"ramdisk={rsize} @{raddr:#x} tags={tags:#x} page={page}")
    print(f"  cmdline: {cmd!r}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output")
    ap.add_argument("--print", dest="inspect")
    ap.add_argument("--header-version", type=int, default=0, choices=(0, 4))
    ap.add_argument("--kernel")
    ap.add_argument("--ramdisk")
    ap.add_argument("--dtb", help="appended to the kernel (v0 only)")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--pagesize", type=int, default=PAGE)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=BASE)
    args = ap.parse_args()

    if args.inspect:
        return print_image(args.inspect)
    if not args.output or not args.kernel:
        ap.error("--output and --kernel are required")

    kernel = open(args.kernel, "rb").read()
    if args.dtb:
        if args.header_version != 0:
            ap.error("--dtb is only used with header version 0")
        kernel += open(args.dtb, "rb").read()
    ramdisk = open(args.ramdisk, "rb").read() if args.ramdisk else b""

    if args.header_version == 0:
        image = pack_v0(kernel, ramdisk, args.cmdline, args.pagesize, args.base)
    else:
        image = pack_v4(kernel, ramdisk, args.cmdline, args.pagesize)

    with open(args.output, "wb") as f:
        f.write(image)
    print(f"wrote {args.output} ({len(image)} bytes)")
    return print_image(args.output)


if __name__ == "__main__":
    sys.exit(main())
