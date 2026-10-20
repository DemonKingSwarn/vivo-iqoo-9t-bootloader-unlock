#!/usr/bin/env python3
"""
extract_dtbs.py — split DTBs out of Android/Qualcomm containers.

Supported inputs
----------------
  * Android DTBO / Qualcomm dt.img  (magic 0xd7b7ab1e)
      Both the 7-field AOSP header (16-byte entries) and the Qualcomm variant
      with an explicit dt_entry_size (32-byte entries, as on this device) are
      detected.
  * vendor_boot.img v3/v4  (auto-locates the DTB section)
  * boot.img v2            (dtb_size / dtb_addr)
  * a bare FDT             (magic 0xd00dfeed)
  * a raw concatenation of FDTs (scan + totalsize)

For every extracted DTB the script reads `model` and `compatible` from the FDT
and names the file `<index>_<model-slug>.dtb`.

Usage
-----
    tools/host/extract_dtbs.py --all
    tools/host/extract_dtbs.py --dtbo DTB/raw/dtbo.img --out DTB
    tools/host/extract_dtbs.py --vendor-boot artifacts/vendor_boot.img --out DTB
    tools/host/extract_dtbs.py --fdt DTB/live/live_fdt.dtb --out DTB
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import sys

FDT_MAGIC = 0xD00DFEED
DTBO_MAGIC = 0xD7B7AB1E


# ---------------------------------------------------------------------------
# FDT helpers
# ---------------------------------------------------------------------------
def is_fdt(blob: bytes, off: int = 0) -> bool:
    return len(blob) >= off + 8 and struct.unpack(">I", blob[off:off + 4])[0] == FDT_MAGIC


def fdt_totalsize(blob: bytes, off: int = 0) -> int:
    return struct.unpack(">I", blob[off + 4:off + 8])[0]


def fdt_info(dtb: bytes) -> tuple[str | None, str | None]:
    """Return (model, compatible) from the root node of an FDT."""
    if not is_fdt(dtb):
        return None, None
    totalsize, off_struct, off_strings = struct.unpack(">3I", dtb[4:16])
    model = compatible = None

    def cstr(base: int, limit: int) -> str:
        end = dtb.find(b"\x00", base, min(limit, len(dtb)))
        if end < 0:
            end = min(limit, len(dtb))
        return dtb[base:end].decode("utf-8", "replace")

    p = off_struct
    depth = 0
    while p + 4 <= min(totalsize, len(dtb)):
        token = struct.unpack(">I", dtb[p:p + 4])[0]
        p += 4
        if token == 1:  # FDT_BEGIN_NODE
            end = dtb.find(b"\x00", p)
            if end < 0:
                break
            p = (end + 1 + 3) & ~3  # node name is padded to 4 bytes
            depth += 1
        elif token == 2:  # FDT_END_NODE
            depth -= 1
        elif token == 3:  # FDT_PROP
            if p + 8 > len(dtb):
                break
            length, nameoff = struct.unpack(">II", dtb[p:p + 8])
            p += 8
            name = cstr(off_strings + nameoff, len(dtb))
            value = dtb[p:p + length]
            p += length
            p = (p + 3) & ~3
            if depth == 1:
                if name == "model" and model is None:
                    model = value.split(b"\x00")[0].decode("utf-8", "replace")
                elif name == "compatible" and compatible is None:
                    compatible = " ".join(
                        x.decode("utf-8", "replace")
                        for x in value.split(b"\x00") if x
                    )
        elif token == 4:  # FDT_NOP
            continue
        elif token == 9:  # FDT_END
            break
        else:
            break
    return model, compatible


def slug(text: str | None, fallback: str) -> str:
    if not text:
        return fallback
    s = re.sub(r"[^A-Za-z0-9._-]+", "_", text).strip("_")
    return s[:90] or fallback


# ---------------------------------------------------------------------------
# Container parsers
# ---------------------------------------------------------------------------
def parse_dtbo(blob: bytes) -> list[tuple[int, bytes]]:
    """Split an Android DTBO / Qualcomm dt.img. Returns [(id, dtb), ...]."""
    if len(blob) < 32 or not is_fdt(blob, 0):
        if len(blob) < 32 or struct.unpack(">I", blob[:4])[0] != DTBO_MAGIC:
            raise ValueError("not a DTBO container")

    total, header_size = struct.unpack(">II", blob[4:12])
    # Two known layouts.  AOSP: count @12, entries_offset @16, page @20, ver @24.
    # Qualcomm: entry_size @12, count @16, entries_offset @20, page @24.
    a_count, a_off = struct.unpack(">II", blob[12:20])
    q_entry_size, q_count, q_off = struct.unpack(">III", blob[12:24])

    def plausible(count: int, off: int, stride: int) -> bool:
        if not (0 < count <= 4096):
            return False
        if not (32 <= off < len(blob)):
            return False
        if stride < 16 or off + count * stride > len(blob):
            return False
        return True

    if plausible(q_count, q_off, q_entry_size) and q_entry_size >= 16:
        count, off, stride = q_count, q_off, q_entry_size
    elif plausible(a_count, a_off, 16):
        count, off, stride = a_count, a_off, 16
    else:
        raise ValueError(
            f"cannot parse DTBO header (total={total}, counts={a_count}/{q_count})"
        )

    out = []
    for i in range(count):
        base = off + i * stride
        dt_size, dt_off, dt_id, dt_rev = struct.unpack(">4I", blob[base:base + 16])
        if dt_size == 0 or dt_off + dt_size > len(blob):
            continue
        out.append((dt_id, blob[dt_off:dt_off + dt_size]))
    return out


def find_fdts(blob: bytes, start: int = 0, size: int | None = None) -> list[bytes]:
    """Scan a region for sequential FDTs and slice them by totalsize."""
    end = len(blob) if size is None else min(len(blob), start + size)
    out = []
    p = start
    while p + 8 <= end:
        idx = blob.find(b"\xd0\x0d\xfe\xed", p, end)
        if idx < 0:
            break
        total = fdt_totalsize(blob, idx)
        if total < 40 or idx + total > end:
            p = idx + 4
            continue
        out.append(blob[idx:idx + total])
        p = idx + ((total + 3) & ~3)
    return out


def vendor_boot_dtb_section(blob: bytes) -> tuple[int, int]:
    if blob[:8] != b"VNDRBOOT":
        raise ValueError("not a vendor_boot image")
    header_version, page_size = struct.unpack("<II", blob[8:16])
    vendor_ramdisk_size = struct.unpack("<I", blob[24:28])[0]
    header_size = struct.unpack("<I", blob[2096:2100])[0]
    dtb_size = struct.unpack("<I", blob[2100:2104])[0]
    align = lambda x: (x + page_size - 1) // page_size * page_size
    dtb_off = align(header_size) + align(vendor_ramdisk_size)
    return dtb_off, dtb_size


def boot_dtb_section(blob: bytes) -> tuple[int, int] | None:
    """Locate a DTB in a legacy boot.img (header v0-v2)."""
    if blob[:8] != b"ANDROID!":
        return None
    page_size = struct.unpack("<I", blob[36:40])[0]
    header_version = struct.unpack("<I", blob[40:44])[0]
    if header_version > 2 or page_size == 0:
        return None
    # v2 header: dtb_size @0x65c, dtb_addr @0x660
    dtb_size = struct.unpack("<I", blob[0x65C:0x660])[0]
    if not dtb_size:
        return None
    kernel_size, ramdisk_size, second_size = struct.unpack("<III", blob[8:20])
    off = page_size + ((kernel_size + page_size - 1) // page_size * page_size)
    off += (ramdisk_size + page_size - 1) // page_size * page_size
    off += (second_size + page_size - 1) // page_size * page_size
    return off, dtb_size


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
def write_set(dtbs: list[bytes], outdir: str, prefix: str, manifest: list[str]) -> int:
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for i, dtb in enumerate(dtbs):
        model, compat = fdt_info(dtb)
        name = f"{i:02d}_{slug(model, f'{prefix}_{i:02d}')}.dtb"
        path = os.path.join(outdir, name)
        with open(path, "wb") as f:
            f.write(dtb)
        digest = hashlib.sha256(dtb).hexdigest()
        manifest.append(
            f"{os.path.relpath(path, os.path.dirname(outdir))}\t"
            f"{len(dtb)}\t{digest}\t{model or '-'}\t{compat or '-'}"
        )
        n += 1
    return n


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dtbo", help="Android DTBO / Qualcomm dt.img")
    p.add_argument("--vendor-boot", help="vendor_boot.img")
    p.add_argument("--boot", help="legacy boot.img with a DTB section")
    p.add_argument("--fdt", help="single FDT file")
    p.add_argument("--raw", help="raw blob to scan for sequential FDTs")
    p.add_argument("--all", action="store_true",
                   help="use the default repo paths (dtbo + vendor_boot + live fdt)")
    p.add_argument("--out", default="DTB", help="output directory (default: DTB)")
    args = p.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, "..", ".."))
    if args.all:
        args.dtbo = args.dtbo or os.path.join(root, "DTB", "raw", "dtbo.img")
        args.vendor_boot = args.vendor_boot or os.path.join(root, "artifacts", "vendor_boot.img")
        args.fdt = args.fdt or os.path.join(root, "DTB", "live", "live_fdt.dtb")

    manifest: list[str] = []
    total = 0
    did_any = False

    if args.dtbo:
        blob = open(args.dtbo, "rb").read()
        items = parse_dtbo(blob)
        dtbs = [d for _, d in items]
        n = write_set(dtbs, os.path.join(args.out, "dtbo"), "dtbo", manifest)
        print(f"dtbo: {n} DTBs from {os.path.basename(args.dtbo)}")
        total += n
        did_any = True

    if args.vendor_boot:
        blob = open(args.vendor_boot, "rb").read()
        off, size = vendor_boot_dtb_section(blob)
        dtbs = find_fdts(blob, off, size)
        n = write_set(dtbs, os.path.join(args.out, "vendor_boot"), "vendor", manifest)
        print(f"vendor_boot: {n} DTBs (section 0x{off:x}, {size} bytes)")
        total += n
        did_any = True

    if args.boot:
        blob = open(args.boot, "rb").read()
        sec = boot_dtb_section(blob)
        if not sec:
            print("boot: no DTB section (v3/v4 boot images keep it in vendor_boot)")
        else:
            off, size = sec
            dtbs = find_fdts(blob, off, size)
            n = write_set(dtbs, os.path.join(args.out, "boot"), "boot", manifest)
            print(f"boot: {n} DTBs (section 0x{off:x}, {size} bytes)")
            total += n
        did_any = True

    if args.fdt:
        blob = open(args.fdt, "rb").read()
        dtbs = [blob] if is_fdt(blob) else find_fdts(blob)
        live_dir = os.path.abspath(os.path.join(args.out, "live"))
        if (len(dtbs) == 1 and
                os.path.dirname(os.path.abspath(args.fdt)) == live_dir):
            # already stored in DTB/live — just record it in the manifest
            model, compat = fdt_info(dtbs[0])
            rel = os.path.relpath(os.path.abspath(args.fdt), os.path.abspath(args.out))
            manifest.append(
                f"{rel}\t{len(dtbs[0])}\t{hashlib.sha256(dtbs[0]).hexdigest()}\t"
                f"{model or '-'}\t{compat or '-'}"
            )
            print(f"live: 1 FDT ({os.path.basename(args.fdt)})")
            total += 1
        else:
            n = write_set(dtbs, os.path.join(args.out, "live"), "live", manifest)
            print(f"live: {n} FDTs from {os.path.basename(args.fdt)}")
            total += n
        did_any = True

    if args.raw:
        blob = open(args.raw, "rb").read()
        dtbs = find_fdts(blob)
        n = write_set(dtbs, os.path.join(args.out, "raw_split"), "raw", manifest)
        print(f"raw: {n} FDTs from {os.path.basename(args.raw)}")
        total += n
        did_any = True

    if not did_any:
        p.print_help()
        return 2

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "manifest.tsv"), "w") as f:
        f.write("file\tsize\tsha256\tmodel\tcompatible\n")
        f.write("\n".join(manifest) + "\n")

    print(f"total: {total} DTB(s); manifest: {os.path.join(args.out, 'manifest.tsv')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
