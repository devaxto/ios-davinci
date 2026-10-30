#!/usr/bin/env python3
"""Build 0095: DPU flip crops logical 1125x2436 into the panel (kill memcpy lag).

    python tools/gpu-src/gen_0095_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0095-sweet-fb-flip-logical-crop.patch"


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
    old = (SRC / "SweetFramebuffer.0093.cpp").read_text(encoding="utf-8")
    new = (SRC / "SweetFramebuffer.cpp").read_text(encoding="utf-8")
    if old.replace("\r\n", "\n") == new.replace("\r\n", "\n"):
        raise SystemExit("identical")
    OUT.write_text(
        file_hunk_replace("iokit/Kernel/arm/SweetFramebuffer.cpp", old, new),
        encoding="utf-8",
        newline="\n",
    )
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
