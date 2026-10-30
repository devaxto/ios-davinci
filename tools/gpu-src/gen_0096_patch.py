#!/usr/bin/env python3
"""Build 0096: safe flip + fast logical→panel crop-copy (performance).

    python tools/gpu-src/gen_0096_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0096-sweet-fb-fast-crop-copy.patch"


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
    # 0095 introduced broken crop-flip; 0096 replaces that file state.
    old = (SRC / "SweetFramebuffer.0095.cpp").read_text(encoding="utf-8")
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
