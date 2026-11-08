#!/usr/bin/env python3
"""Build 0097: HakiosGPU & Adreno 618 IB1 command submit engine.

    python tools/gpu-src/gen_0097_patch.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(__file__).resolve().parent
OUT = ROOT / "xnu" / "patches" / "0097-davinci-gpu-hakios-accel.patch"


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
    old_accel = (SRC / "SweetAccelerator.0085.cpp").read_text(encoding="utf-8") if (SRC / "SweetAccelerator.0085.cpp").exists() else ""
    if not old_accel:
        # read original 0085 patch to get old SweetAccelerator.cpp
        p85 = (ROOT / "xnu" / "patches" / "0085-davinci-gpu-power-accel.patch").read_text(encoding="utf-8")
        marker = "+++ b/iokit/Kernel/arm/SweetAccelerator.cpp\n"
        idx = p85.find(marker)
        if idx != -1:
            idx = p85.find("@@", idx)
            idx = p85.find("\n", idx) + 1
            lines = []
            for l in p85[idx:].splitlines():
                if l.startswith("+"):
                    lines.append(l[1:])
                elif l.startswith("diff ") or l.startswith("--- "):
                    break
            old_accel = "\n".join(lines) + "\n"

    new_accel = (SRC / "SweetAccelerator.cpp").read_text(encoding="utf-8")

    # old sweet_gpu.c comes from 0094-sweet-gpu-soft-fail.patch
    p94 = (ROOT / "xnu" / "patches" / "0094-sweet-gpu-soft-fail.patch").read_text(encoding="utf-8")
    marker_gpu = "+++ b/bsd/dev/sweet_gpu.c\n"
    idx_gpu = p94.find(marker_gpu)
    assert idx_gpu != -1, "sweet_gpu.c not found in 0094 patch"
    idx_gpu = p94.find("@@", idx_gpu)
    idx_gpu = p94.find("\n", idx_gpu) + 1
    gpu_lines = []
    for l in p94[idx_gpu:].splitlines():
        if l.startswith("+"):
            gpu_lines.append(l[1:])
        elif l.startswith("diff ") or l.startswith("--- "):
            break
    old_gpu = "\n".join(gpu_lines) + "\n"
    new_gpu = (SRC / "sweet_gpu.c").read_text(encoding="utf-8")

    kconfig_hunk = """--- a/iokit/KernelConfigTables.cpp
+++ b/iokit/KernelConfigTables.cpp
@@ -82,6 +82,11 @@
     "     'IOProviderClass' = IOResources;"
     "     'IOMatchCategory' = SweetBattery;"
     "   },"
+    "   {"
+    "     'IOClass'         = HakiosGPU;"
+    "     'IOProviderClass' = IOPlatformDevice;"
+    "     'IONameMatch'     = 'sweet,gpu';"
+    "   },"
     "   {"
     "     'IOClass'         = AGXAccelerator;"
     "     'IOProviderClass' = IOPlatformDevice;"
"""

    parts = [kconfig_hunk]
    if old_gpu != new_gpu:
        parts.append(file_hunk_replace("bsd/dev/sweet_gpu.c", old_gpu, new_gpu))
    if old_accel != new_accel:
        parts.append(file_hunk_replace("iokit/Kernel/arm/SweetAccelerator.cpp", old_accel, new_accel))

    OUT.write_text("".join(parts), encoding="utf-8", newline="\n")
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
