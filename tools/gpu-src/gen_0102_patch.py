#!/usr/bin/env python3
"""Build 0102: program the SSPP window with the real CAF register map.

0101's flip moved SRC_ADDR and YSTRIDE0 and nothing else, and the panel kept
the bootloader's rectangle. The boot log says why:

    SWEET DPU: plane src_size 0x000237ff dst_size 0x03020001 src_xy 0x00000000
    dst_xy 0x00000000 fmt 0x80000000 -> src 0x0 dst 0x0 sizes are unknown
    SWEET DPU: flip ready on vig0 ... cmd-or-idle geom-locked sizes unknown

0x000237ff at +0x30 is a 32bpp RGBA format word (C0..C3 = 8 bpc, SRCC3_EN,
bpp-1 = 3, unpack_count-1 = 3, unpack_tight, interleaved), not a size. The
offsets were shifted by 0x30: SRC_FORMAT was read as SRC_SIZE, UNPACK_PATTERN
as OUT_SIZE, YSTRIDE1 as SRC_XY. The geometry never decoded, so it was never
written.

0102, from the post-0101 state:

  * SweetDisplay.cpp - the SSPP offsets are the CAF SDE ones for this SoC
                       (drivers/gpu/drm/msm/sde/sde_hw_sspp.c, SRC sub-block
                       base 0): SRC_SIZE +0x00, SRC_XY +0x08, OUT_SIZE +0x0c,
                       OUT_XY +0x10, SRC0_ADDR +0x14, SRC_YSTRIDE0 +0x24,
                       SRC_FORMAT +0x30, SRC_UNPACK_PATTERN +0x34,
                       SRC_OP_MODE +0x38, MULTIRECT_OPMODE +0x170.
                       The probe reads the live plane, decodes the panel rect
                       against OUT_SIZE over the four packings (vendor
                       (h << 16) | w as the fallback, never "unknown"), logs
                       format/unpack/opmode/multirect, and the flip programs
                       SRC_SIZE/OUT_SIZE/SRC_XY/YSTRIDE0 whenever they differ
                       from the wanted window instead of inheriting them.
                       SRC_ADDR and OUT_SIZE are read back; a write that did
                       not land restores the splash plane and disables the
                       flip. The first 8 flips log the whole plane.
  * SweetDisplay.h   - the comment described the "only SRC_ADDR and YSTRIDE0"
                       behaviour that this replaces.

Boot arg sweetfb_geom=0 keeps the old read-only behaviour (only ADDR/stride),
for A/B against a kernel that programs the window.

    python tools/gpu-src/gen_0102_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0102-sweet-dpu-rect-map.patch"

FILES = [
    ("iokit/Kernel/arm/SweetDisplay.cpp", "SweetDisplay.0101.cpp", "SweetDisplay.cpp"),
    ("iokit/Kernel/arm/SweetDisplay.h", "SweetDisplay.0101.h", "SweetDisplay.h"),
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
