#!/usr/bin/env python3
"""Build 0090: GMU firmware load (sweetgpu=4) for Adreno 618 Metal track B1.

    python tools/gpu-src/embed_fw.py   # needs fw/a630_gmu.bin
    python tools/gpu-src/gen_0090_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0090-davinci-gmu-fw-load.patch"


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


def file_hunk_new(path: str, content: str) -> str:
    lines = content.splitlines()
    out = ["--- /dev/null\n", f"+++ b/{path}\n", f"@@ -0,0 +1,{len(lines)} @@\n"]
    for line in lines:
        out.append("+" + line + "\n")
    return "".join(out)


old = (SRC / "sweet_gpu.0085.c").read_text(encoding="utf-8")
new = (SRC / "sweet_gpu.c").read_text(encoding="utf-8")
fw = (SRC / "sweet_gpu_fw.h").read_text(encoding="utf-8")
if not (SRC / "sweet_gpu_fw.h").exists() or "sweet_gpu_gmu_fw" not in fw:
    raise SystemExit("run embed_fw.py first")

OUT.write_text(
    file_hunk_replace("bsd/dev/sweet_gpu.c", old, new)
    + file_hunk_new("bsd/dev/sweet_gpu_fw.h", fw),
    encoding="utf-8",
    newline="\n",
)
print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
