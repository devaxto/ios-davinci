#!/usr/bin/env python3
"""Build 0101: a flip that actually runs on every frame.

0100 had a flip that could only ever cover one surface (the single ABL splash
slot) and rewrote SRC_SIZE/DST_SIZE from a guessed packing. 0101 fixes both,
from the post-0100 state:

  * SweetSurface.cpp/.h - reserve contiguous below-4 GB flip slots at start()
                        and hand them to every full-screen layer, so all the
                        surfaces CoreAnimation rotates through are scanout-able
                        (the overflow frames used to be copied into memory
                        nobody scanned out). Slot returned on surface free.
  * SweetDisplay.cpp    - decode w-low/w-high x size/size-1 from the live
                        DST_SIZE, write SRC_SIZE/DST_SIZE only when the window
                        size really changes, SRC_XY only when the origin does,
                        restore the bootloader origin on reset, and a cheap
                        reset no-op when the plane was never moved.
  * SweetFramebuffer.cpp - allocate the flip pool in start(), and put the plane
                        back on the splash buffer before any CPU copy path.

    python tools/gpu-src/gen_0101_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0101-sweet-fb-flip-pool.patch"

FILES = [
    ("iokit/Kernel/arm/SweetSurface.cpp", "SweetSurface.0100.cpp", "SweetSurface.cpp"),
    ("iokit/Kernel/arm/SweetSurface.h", "SweetSurface.0100.h", "SweetSurface.h"),
    ("iokit/Kernel/arm/SweetDisplay.cpp", "SweetDisplay.0100.cpp", "SweetDisplay.cpp"),
    ("iokit/Kernel/arm/SweetDisplay.h", "SweetDisplay.0100.h", "SweetDisplay.h"),
    ("iokit/Kernel/arm/SweetFramebuffer.cpp", "SweetFramebuffer.0100.cpp",
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
