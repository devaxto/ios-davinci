#!/usr/bin/env python3
"""Build 0092: SQE + CP ring + ME_INIT/NOP (sweetgpu=5) for Adreno 618 B2.

    python tools/gpu-src/embed_fw.py      # needs fw/a630_gmu.bin + a630_sqe.fw
    python tools/gpu-src/gen_0092_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0092-davinci-sqe-ring.patch"


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
    old_c = (SRC / "sweet_gpu.0090.c").read_text(encoding="utf-8")
    new_c = (SRC / "sweet_gpu.c").read_text(encoding="utf-8")
    old_h = (SRC / "sweet_gpu_fw.0090.h").read_text(encoding="utf-8")
    new_h = (SRC / "sweet_gpu_fw.h").read_text(encoding="utf-8")

    if "sweet_gpu_sqe_fw" not in new_h:
        raise SystemExit("run embed_fw.py first (need SQE array)")
    if old_c.replace("\r\n", "\n") == new_c.replace("\r\n", "\n"):
        raise SystemExit("sweet_gpu.c identical to 0090 snapshot")

    OUT.write_text(
        file_hunk_replace("bsd/dev/sweet_gpu.c", old_c, new_c)
        + file_hunk_replace("bsd/dev/sweet_gpu_fw.h", old_h, new_h),
        encoding="utf-8",
        newline="\n",
    )
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
