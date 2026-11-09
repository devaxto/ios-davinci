#!/usr/bin/env python3
"""Build 0103: what is actually in the buffer the plane flips to.

0102's log settles the register side and leaves the picture unexplained:

    SWEET DPU: plane src_size 0x09240438 out_size 0x09240438 src_xy 0x00000000
    out_xy 0x00000000 fmt 0x000237ff unpack 0x03020001 opmode 0x80000000
    multi 0x00000000 -> src 1080x2340 out 1080x2340 at 0,0 sizes w-low
    SWEET DPU: flip 1 pa 0xb3f8c000 stride 4352 win 1080x2340 at 0,0 ->
    SRC_SIZE 0x09240438 OUT_SIZE 0x09240438 SRC_XY 0x00000000 OUT_XY
    0x00000000 YSTRIDE 0x00001100

The bootloader window was already the full panel 1:1, multirect is off, the
format is 32bpp RGBA with the BGRA unpack, and every flip latched (no "flip
did not stick") with no CPU crop-copy taking over. So the panel shows a tiled,
sheared pattern with the register state a working 0097 boot had: the plane
fetches what is at that address and pitch, and the question is what that is.

0103, from the post-0102 state:

  * SweetFramebuffer.cpp - two diagnostics, both off the hot path:
                       sweetfb_probe=N (default 3) logs, from the CA surface
                       about to be flipped, how many of its sampled pixels are
                       zero or fully opaque plus a few pixels at the top,
                       middle and last row. A UI frame is neither all zero nor
                       all opaque and varies across a row; uninitialised or
                       partially drawn memory reads as a single value.
                       sweetfb_fetchtest=N (default 0) copies the panel buffer
                       (content the DPU already scans out) into the surface
                       before the flip and paints 100 blue rows at the top,
                       green at the middle, red at the bottom and a red bar
                       down the left edge. Markers that arrive sheared or
                       repeated mean the fetch pitch/window is wrong; no
                       markers mean that physical address is not being fetched
                       at all; correct markers mean the flip works and the
                       pixels CoreAnimation drew are what is missing.

Boot arg use for the next run: sweetfb_probe=3 sweetfb_fetchtest=4.

    python tools/gpu-src/gen_0103_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0103-sweet-dpu-content-probe.patch"

FILES = [
    ("iokit/Kernel/arm/SweetFramebuffer.cpp", "SweetFramebuffer.0102.cpp",
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
