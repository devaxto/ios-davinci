#!/usr/bin/env python3
"""Build 0087 patch: DPU probe (SweetDisplay) + cached panel mapping default.

Sources of truth next to this file:
  SweetFramebuffer.base.cpp  - the file as 0046+0054+0056+0061+0067+0079+0086 left it
  SweetFramebuffer.cpp       - same file after 0087
  SweetDisplay.h/.cpp        - new files (read-only MDSS/SDE probe)

    python tools/gpu-src/gen_0087_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0087-davinci-dpu-probe.patch"


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


# iokit/conf/files.arm64 after 0085: SweetSurface 16, SweetFramebuffer 17,
# SweetBattery 18, SweetAccelerator 19, hid/SweetHID 20.
FILES_HUNK = """--- a/iokit/conf/files.arm64
+++ b/iokit/conf/files.arm64
@@ -16,5 +16,6 @@
 iokit/Kernel/arm/SweetSurface.cpp                                optional iokitcpp
 iokit/Kernel/arm/SweetFramebuffer.cpp                            optional iokitcpp
+iokit/Kernel/arm/SweetDisplay.cpp                                optional iokitcpp
 iokit/Kernel/arm/SweetBattery.cpp                                optional iokitcpp
 iokit/Kernel/arm/SweetAccelerator.cpp                            optional iokitcpp
 iokit/Kernel/arm/hid/SweetHID.cpp                                optional iokitcpp
"""

old_fb = (SRC / "SweetFramebuffer.base.cpp").read_text(encoding="utf-8")
new_fb = (SRC / "SweetFramebuffer.cpp").read_text(encoding="utf-8")
display_h = (SRC / "SweetDisplay.h").read_text(encoding="utf-8")
display_cpp = (SRC / "SweetDisplay.cpp").read_text(encoding="utf-8")

OUT.write_text(
    FILES_HUNK
    + file_hunk_replace("iokit/Kernel/arm/SweetFramebuffer.cpp", old_fb, new_fb)
    + file_hunk_new("iokit/Kernel/arm/SweetDisplay.h", display_h)
    + file_hunk_new("iokit/Kernel/arm/SweetDisplay.cpp", display_cpp),
    encoding="utf-8",
    newline="\n",
)
print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
