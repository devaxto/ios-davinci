#!/usr/bin/env python3
"""Build 0094: soft-fail GPU polls + daily-must-stay-sweetgpu<=1 warning.

    python tools/gpu-src/gen_0094_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0094-sweet-gpu-soft-fail.patch"


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
    old = (SRC / "sweet_gpu.0093.c").read_text(encoding="utf-8")
    new = (SRC / "sweet_gpu.c").read_text(encoding="utf-8")
    if old.replace("\r\n", "\n") == new.replace("\r\n", "\n"):
        raise SystemExit("identical to 0093")
    OUT.write_text(
        file_hunk_replace("bsd/dev/sweet_gpu.c", old, new),
        encoding="utf-8",
        newline="\n",
    )
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
