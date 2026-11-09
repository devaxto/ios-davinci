#!/usr/bin/env python3
"""Build 0100: real DPU flip (surface pitch + SRC_XY crop + latch check).

Rewrites three iokit/Kernel/arm files from their post-0099 state:

  * SweetDisplay.cpp  - probe reads SRC_SIZE/DST_SIZE/SRC_XY, derives the
                        size encoding, flips with SRC_ADDR + YSTRIDE0 +
                        SRC_XY, flushes every live CTL, restores and
                        disables the flip when the write does not latch.
  * SweetDisplay.h    - new sweet_display_flip() signature.
  * SweetFramebuffer.cpp - the flip now carries the panel-sized window and
                        the surface pitch (crop == the CPU crop-copy rect).

    python tools/gpu-src/gen_0100_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0100-sweet-dpu-flip-geometry.patch"

FILES = [
    ("iokit/Kernel/arm/SweetDisplay.cpp", "SweetDisplay.0099.cpp", "SweetDisplay.cpp"),
    ("iokit/Kernel/arm/SweetDisplay.h", "SweetDisplay.0099.h", "SweetDisplay.h"),
    ("iokit/Kernel/arm/SweetFramebuffer.cpp", "SweetFramebuffer.0099.cpp",
     "SweetFramebuffer.cpp"),
]


def file_hunk_replace(path: str, old: str, new: str) -> str:
    old_l, new_l = old.splitlines(), new.splitlines()
    out = [
        f"--- a/{path}\n",
        f"+++ b/{path}\n",
        f"@@ -1,{len(old_l)} +1,{len(new_l)} @@\n",
    ]
    for line in old_l:
        out.append("-" + line + "\n")
    for line in new_l:
        out.append("+" + line + "\n")
    return "".join(out)


def main() -> None:
    parts = []
    for path, old_name, new_name in FILES:
        old = (SRC / old_name).read_text(encoding="utf-8").split("\n")
        new = (SRC / new_name).read_text(encoding="utf-8").split("\n")
        if old == new:
            raise SystemExit(f"{path}: no change, refusing to write an empty hunk")
        parts.append(file_hunk_replace(path, "\n".join(old), "\n".join(new)))
    OUT.write_text("".join(parts), encoding="utf-8", newline="\n")
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
