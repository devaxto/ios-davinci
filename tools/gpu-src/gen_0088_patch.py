#!/usr/bin/env python3
"""Build 0088 patch: DPU zero-copy flip + contiguous IOSurface alloc.

Sources of truth next to this file:
  SweetFramebuffer.base.cpp - as 0087 left it
  SweetFramebuffer.cpp      - after 0088 (flip present path)
  SweetDisplay.h/.cpp       - probe + flip (replaces 0087 versions)
  SweetSurface.base.h/.cpp  - as 0068 left them
  SweetSurface.h/.cpp       - contiguous phys + sweet_surface_phys()

    python tools/gpu-src/gen_0088_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0088-davinci-dpu-flip.patch"


def file_hunk_replace(path: str, old: str, new: str) -> str:
    old_l = old.splitlines()
    new_l = new.splitlines()
    out = [f"--- a/{path}\n", f"+++ b/{path}\n",
           f"@@ -1,{len(old_l)} +1,{len(new_l)} @@\n"]
    for line in old_l:
        out.append("-" + line + "\n")
    for line in new_l:
        out.append("+" + line + "\n")
    return "".join(out)


def read(name: str) -> str:
    return (SRC / name).read_text(encoding="utf-8")


parts = []
for rel, old_name, new_name in (
    ("iokit/Kernel/arm/SweetDisplay.h", "SweetDisplay.0087.h", "SweetDisplay.h"),
    ("iokit/Kernel/arm/SweetDisplay.cpp", "SweetDisplay.0087.cpp", "SweetDisplay.cpp"),
    ("iokit/Kernel/arm/SweetFramebuffer.cpp", "SweetFramebuffer.base.cpp", "SweetFramebuffer.cpp"),
    ("iokit/Kernel/arm/SweetSurface.h", "SweetSurface.base.h", "SweetSurface.h"),
    ("iokit/Kernel/arm/SweetSurface.cpp", "SweetSurface.base.cpp", "SweetSurface.cpp"),
):
    old, new = read(old_name), read(new_name)
    if old == new:
        raise SystemExit(f"{rel}: old and new are identical")
    parts.append(file_hunk_replace(rel, old, new))

OUT.write_text("".join(parts), encoding="utf-8", newline="\n")
print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
