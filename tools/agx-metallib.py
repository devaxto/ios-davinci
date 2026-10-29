#!/usr/bin/env python3
"""Inspect Apple metallib / AIR containers (Metal track B5 start).

iOS ships precompiled AGX machine code inside .metallib files. This tool
only *reads* the container so we can map sections and hashes - it does not
translate AGX→a6xx yet. Point it at a framework pulled from the iOS root:

    python tools/agx-metallib.py /path/to/default.metallib
    python tools/agx-metallib.py --scan E:/ios-root/System/Library/Frameworks

See docs/METAL-PORT.md.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

# metallib typically starts with "metallib" magic (ASCII) then version words.
MAGICS = (b"metallib", b"MTLB", b"AIR\0")


def sniff(data: bytes) -> str:
    for m in MAGICS:
        if data.startswith(m):
            return m.rstrip(b"\0").decode("ascii", "replace")
    return "unknown"


def summarize(path: Path) -> dict:
    data = path.read_bytes()
    return {
        "path": str(path),
        "size": len(data),
        "magic": sniff(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "head": data[:32].hex(),
        "u32": [f"0x{struct.unpack_from('<I', data, i)[0]:08x}"
                for i in range(0, min(32, len(data) - 3), 4)],
    }


def scan(root: Path) -> list[Path]:
    hits = []
    for p in root.rglob("*"):
        if p.is_file() and p.suffix.lower() in (".metallib", ".air", ".metal"):
            hits.append(p)
        elif p.is_file() and p.name == "default.metallib":
            hits.append(p)
    return sorted(hits)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path", nargs="?", help="metallib file or directory")
    ap.add_argument("--scan", metavar="DIR", help="walk DIR for metallibs")
    a = ap.parse_args()
    targets: list[Path] = []
    if a.scan:
        targets = scan(Path(a.scan))
        print(f"{len(targets)} metallib-like file(s) under {a.scan}")
    elif a.path:
        p = Path(a.path)
        targets = scan(p) if p.is_dir() else [p]
    else:
        ap.print_help()
        return 2
    for p in targets[:64]:
        try:
            info = summarize(p)
        except OSError as exc:
            print(f"{p}: {exc}", file=sys.stderr)
            continue
        print(f"{info['sha256'][:12]}  {info['size']:8d}  {info['magic']:8s}  {info['path']}")
        if len(targets) == 1:
            print("  head:", info["head"])
            print("  u32:", " ".join(info["u32"]))
    if len(targets) > 64:
        print(f"... ({len(targets) - 64} more)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
