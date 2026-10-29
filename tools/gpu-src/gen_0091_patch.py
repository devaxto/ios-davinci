#!/usr/bin/env python3
"""Build 0091: splash-pool IOSurfaces + DPU flip default on (feel like 60 Hz).

    python tools/gpu-src/gen_0091_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0091-sweet-splash-flip.patch"


def file_hunk_replace(path: str, old: str, new: str) -> str:
    old_l, new_l = old.splitlines(), new.splitlines()
    out = [f"--- a/{path}\n", f"+++ b/{path}\n",
           f"@@ -1,{len(old_l)} +1,{len(new_l)} @@\n"]
    for line in old_l:
        out.append("-" + line + "\n")
    for line in new_l:
        out.append("+" + line + "\n")
    return "".join(out)


parts = []
for rel, old_n, new_n in (
    ("iokit/Kernel/arm/SweetSurface.h", "SweetSurface.0090.h", "SweetSurface.h"),
    ("iokit/Kernel/arm/SweetSurface.cpp", "SweetSurface.0090.cpp", "SweetSurface.cpp"),
    ("iokit/Kernel/arm/SweetFramebuffer.cpp", "SweetFramebuffer.0090.cpp", "SweetFramebuffer.cpp"),
):
    old = (SRC / old_n).read_text(encoding="utf-8")
    new = (SRC / new_n).read_text(encoding="utf-8")
    if old.replace("\r\n", "\n") == new.replace("\r\n", "\n"):
        raise SystemExit(f"{rel}: identical")
    parts.append(file_hunk_replace(rel, old, new))

OUT.write_text("".join(parts), encoding="utf-8", newline="\n")
print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
