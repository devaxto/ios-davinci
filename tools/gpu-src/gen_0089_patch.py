#!/usr/bin/env python3
"""Build 0089: fix 0088 lag (contig only with sweetfb_flip) + quiet DPU logs.

    python tools/gpu-src/gen_0089_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0089-sweet-fb-smooth.patch"


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


parts = []
for rel, old_name, new_name in (
    ("iokit/Kernel/arm/SweetSurface.cpp", "SweetSurface.0088.cpp", "SweetSurface.cpp"),
    ("iokit/Kernel/arm/SweetDisplay.cpp", "SweetDisplay.0088.cpp", "SweetDisplay.cpp"),
):
    old = (SRC / old_name).read_text(encoding="utf-8")
    new = (SRC / new_name).read_text(encoding="utf-8")
    if old.replace("\r\n", "\n") == new.replace("\r\n", "\n"):
        raise SystemExit(f"{rel}: identical")
    parts.append(file_hunk_replace(rel, old, new))

OUT.write_text("".join(parts), encoding="utf-8", newline="\n")
print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
