#!/usr/bin/env python3
"""Build 0093: re-embed Xiaomi vendor a618_gmu.bin + a630_sqe.fw (B0→kernel).

    python tools/gpu-src/embed_fw.py       # prefers fw/a618_gmu.bin
    python tools/gpu-src/gen_0093_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0093-davinci-vendor-gmu-sqe.patch"


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
    old_c = (SRC / "sweet_gpu.0092.c").read_text(encoding="utf-8")
    new_c = (SRC / "sweet_gpu.c").read_text(encoding="utf-8")
    old_h = (SRC / "sweet_gpu_fw.0092.h").read_text(encoding="utf-8")
    new_h = (SRC / "sweet_gpu_fw.h").read_text(encoding="utf-8")

    if "a618_gmu.bin" not in new_h.splitlines()[0]:
        raise SystemExit("run embed_fw.py first (expect a618_gmu.bin in header comment)")
    if "SWEET_GPU_SQE_FW_BYTES 32136u" not in new_h:
        raise SystemExit("expected vendor SQE size 32136")

    parts = []
    if old_c.replace("\r\n", "\n") != new_c.replace("\r\n", "\n"):
        parts.append(file_hunk_replace("bsd/dev/sweet_gpu.c", old_c, new_c))
    parts.append(file_hunk_replace("bsd/dev/sweet_gpu_fw.h", old_h, new_h))
    OUT.write_text("".join(parts), encoding="utf-8", newline="\n")
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
