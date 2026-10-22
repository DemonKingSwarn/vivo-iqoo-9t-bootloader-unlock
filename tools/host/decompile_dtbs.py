#!/usr/bin/env python3
"""
decompile_dtbs.py — turn the committed DTB files into DTS and map them.

Outputs, under DTB/dts/:
  live/*.dts          the live merged device tree(s)
  dtbo/*.dts          bootloader device-tree overlays
  vendor_boot/*.dts   base SoC trees shipped in vendor_boot
  README.md           per-file map: model, compatibles, node counts,
                      overlay targets, and the enabled-device inventory
  inventory-live.tsv  every node with a compatible in the live tree:
                      path, compatible, reg, status
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DTB = ROOT / "DTB"
OUT = DTB / "dts"
GROUPS = ("live", "dtbo", "vendor_boot")


def find_dtc() -> str:
    env = os.environ.get("DTC")
    if env:
        return env
    built = ROOT / "linux-iqoo9t/out/scripts/dtc/dtc"
    if built.is_file():
        return str(built)
    which = shutil.which("dtc")
    if which:
        return which
    sys.exit("error: no dtc (build the kernel or install device-tree-compiler)")


def decompile(dtc: str, src: Path, dst: Path) -> bool:
    dst.parent.mkdir(parents=True, exist_ok=True)
    res = subprocess.run(
        [dtc, "-q", "-I", "dtb", "-O", "dts", "-o", str(dst), str(src)],
        capture_output=True, text=True,
    )
    if res.returncode != 0:
        print(f"  {src.name}: {res.stderr.strip().splitlines()[:1]}")
        return False
    return True


def root_strings(text: str) -> tuple[str, list[str]]:
    model = re.search(r'^\s*model = "([^"]*)"', text, re.M)
    compat = re.search(r'^\s*compatible = ((?:"[^"]*"\s*,?\s*)+);', text, re.M)
    compat_list = re.findall(r'"([^"]*)"', compat.group(1)) if compat else []
    return (model.group(1) if model else ""), compat_list


def node_count(text: str) -> tuple[int, int]:
    total = len(re.findall(r"\{\s*$", text, re.M)) - 1  # minus root
    disabled = len(re.findall(r'status = "disabled";', text))
    return max(total, 0), disabled


def overlay_targets(text: str) -> list[str]:
    return re.findall(r'target-path\s*=\s*"([^"]*)"', text) + \
        re.findall(r'target\s*=\s*<([^>]+)>', text)


def inventory(text: str) -> list[tuple[str, str, str, str]]:
    """Walk the DTS text, return (path, compatible, reg, status) per node.

    decompiled trees keep labels (dtc rebuilds them from __symbols__), so a
    node header can look like "spmi_bus: spmi@c42d000 {".
    """
    node_re = re.compile(
        r"^(\t*)(?:[A-Za-z0-9_]+: )*([A-Za-z0-9_@,+\-\.]+) \{$")
    stack: list[tuple[int, str]] = []
    paths: dict[str, str] = {}
    props: dict[str, dict[str, str]] = {}
    rows: list[tuple[str, str, str, str]] = []

    for line in text.splitlines():
        m = node_re.match(line)
        if m:
            depth = len(m.group(1))
            while stack and stack[-1][0] >= depth:
                stack.pop()
            name = m.group(2)
            path = "/" + "/".join([n for _, n in stack] + [name])
            stack.append((depth, name))
            paths[name] = path
            props[path] = {}
            continue
        if re.match(r"^\t*\};", line):
            if stack:
                stack.pop()
            continue
        if not stack:
            continue
        path = paths[stack[-1][1]]
        s = line.strip()
        for key in ("compatible", "reg", "status"):
            if s.startswith(key + " ="):
                props[path][key] = s

    for path, p in props.items():
        comp = p.get("compatible")
        if not comp:
            continue
        compat = " ".join(re.findall(r'"([^"]*)"', comp))
        reg = ""
        m = re.search(r"<([^>]*)>", p.get("reg", ""))
        if m:
            reg = m.group(1)
        status = "disabled" if 'status = "disabled"' in p.get("status", "") else "okay"
        rows.append((path, compat, reg, status))
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dtb", default=str(DTB))
    args = ap.parse_args()

    dtc = find_dtc()
    print(f"dtc: {dtc}")
    OUT.mkdir(parents=True, exist_ok=True)

    rows = []
    for group in GROUPS:
        src_dir = Path(args.dtb) / group
        if not src_dir.is_dir():
            continue
        dst_dir = OUT / group
        dst_dir.mkdir(parents=True, exist_ok=True)
        for src in sorted(src_dir.glob("*.dtb")):
            dst = dst_dir / (src.stem + ".dts")
            ok = decompile(dtc, src, dst)
            text = dst.read_text(errors="replace") if ok else ""
            model, compat = root_strings(text)
            total, disabled = node_count(text) if ok else (0, 0)
            rows.append({
                "group": group, "name": src.stem, "ok": ok, "model": model,
                "compat": compat, "nodes": total, "disabled": disabled,
                "targets": overlay_targets(text) if group == "dtbo" else [],
                "text": text,
            })
            print(f"  {group}/{src.stem}: {'ok' if ok else 'FAILED'} "
                  f"({total} nodes)")

    # Inventory of the live tree.
    live = next((r for r in rows if r["group"] == "live" and r["ok"]), None)
    inv_lines = ["path\tcompatible\treg\tstatus"]
    if live:
        for path, compat, reg, status in inventory(live["text"]):
            inv_lines.append(f"{path}\t{compat}\t{reg}\t{status}")
        (OUT / "inventory-live.tsv").write_text("\n".join(inv_lines) + "\n")

    # README map.
    md = ["# Decompiled device trees", "",
          "Generated by `tools/host/decompile_dtbs.py` from the committed DTBs.",
          "Do not edit; rerun the tool after adding DTBs.", ""]
    for group in GROUPS:
        grp = [r for r in rows if r["group"] == group]
        if not grp:
            continue
        md += [f"## {group}", "",
               "| file | model | nodes | disabled | compatible |",
               "|---|---|---:|---:|---|"]
        for r in grp:
            md.append(f"| `{r['name']}` | {r['model']} | {r['nodes']} | "
                      f"{r['disabled']} | `{'`, `'.join(r['compat'])}` |")
        if group == "dtbo":
            md += ["", "Overlay targets:", ""]
            for r in grp:
                if r["targets"]:
                    md.append(f"- `{r['name']}` → {', '.join(r['targets'])}")
        md.append("")

    if live:
        md += ["## Live tree inventory (enabled nodes with a compatible)", "",
               "Full list: `inventory-live.tsv`.", "",
               "| node | compatible | reg |", "|---|---|---|"]
        seen = set()
        for path, compat, reg, status in inventory(live["text"]):
            if status != "okay":
                continue
            key = (compat.split()[0], reg)
            if key in seen:
                continue
            seen.add(key)
            md.append(f"| `{path}` | `{compat}` | `{reg}` |")
        md.append("")

    (OUT / "README.md").write_text("\n".join(md))
    print(f"wrote {OUT/'README.md'}")
    if live:
        print(f"wrote {OUT/'inventory-live.tsv'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
