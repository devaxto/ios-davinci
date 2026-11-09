#!/usr/bin/env python3
"""Build 0104: a colour chart the panel cannot fake, plus the pipe after a flip.

0103's log answers the content question and leaves the geometry one open. The
probe read the CoreAnimation surface about to be flipped:

    SWEET FB: probe 1080x2340 bpr 4352 9996 samples, 0 zero, 9996 opaque,
        first 0xff000000 last 0xff000000
    SWEET FB: probe top y 0 x0/25/50/75 0xff000000 0xff000000 0xff000000
        0xff000000
    SWEET FB: probe mid y 1170 x0/25/50/75 0xff000000 0xff000000 0xffffffff
        0xff000000
    SWEET FB: probe end y 2339 x0/25/50/75 0xff000000 0xff000000 0xff000000
        0xff000000

So the buffer the plane was pointed at held a fully opaque black frame with one
white detail at the centre - a plausible early boot screen, not garbage. The
panel meanwhile showed a black screen with a full-width white band across the
middle third, no sign of the fetchtest markers (100 blue rows at the top, green
at 1000, red at the bottom, a red bar down the left edge) and a red/blue
diagonal. Markers that never appear mean the plane is not scanning the source
rectangle we programmed, even though SRC_SIZE, OUT_SIZE, SRC_XY, OUT_XY,
SRC_ADDR and YSTRIDE all read back as written and all eight flips latched.

Which leaves two ways for the pixels to arrive wrong, and both are cheap to tell
apart on the panel instead of in theory: the fetch is not 1:1 against the buffer
(a scaler or pixel-extension state the bootloader left behind at 0x0c0..0x158,
which the driver never writes because the CPU crop-copy path does not need it),
or the pixels the panel shows do not come from that address at all.

0104, from the post-0103 state:

  * SweetFramebuffer.cpp - sweetfb_fetchtest becomes sweetfb_chart=N (default
                       0). Instead of copying the panel buffer it paints twelve
                       solid horizontal bands in a fixed order - red, green,
                       blue, white, yellow, cyan, magenta, orange, violet,
                       grey, dark grey, black - framed by 8 px white rulers on
                       all four edges, over the whole surface. Read the panel:

                         12 bands + 4 rulers  the fetch is 1:1 and the earlier
                                              noise was buffer content
                         4 bands, no rulers    the plane scans a third of the
                                              source: something scales, and the
                                              bands say which third
                         bands, no rulers      pitch or window is off
                         no bands at all       this address is not what the
                                              panel shows

  * SweetDisplay.cpp - dpu_dump_sspp() dumps the live SSPP block once, right
                       after the first flip ("SWEET DPU: after flip
                       sspp+0xNNN=0x..."). The boot probe already dumped the
                       same words before any write, so the two lists differ only
                       by what a flip changes, and the words this driver never
                       touches (pixel extension at 0x100, the scaler-era values
                       at 0x0c0..0x0fc, CDP/QOS at 0x130..0x158) are visible
                       for the first time with the surface in place.

Boot arg use for the next run: sweetfb_probe=2 sweetfb_chart=8.

    python tools/gpu-src/gen_0104_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0104-sweet-dpu-chart-and-pipe-dump.patch"

FILES = [
    ("iokit/Kernel/arm/SweetFramebuffer.cpp", "SweetFramebuffer.0103.cpp",
     "SweetFramebuffer.cpp"),
    ("iokit/Kernel/arm/SweetDisplay.cpp", "SweetDisplay.0103.cpp",
     "SweetDisplay.cpp"),
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
