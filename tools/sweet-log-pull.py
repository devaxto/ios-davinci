#!/usr/bin/env python3
"""Pull the SWEET persistent console log on any OS (adb + python only).

tools/sweet-log-pull.sh does the same read but needs a POSIX shell; this is
the Windows-friendly version:

    python tools/sweet-log-pull.py logs/boot-0087.txt
    python tools/sweet-log-pull.py out.txt -s 1234abcd

The phone must be booted into recovery (OrangeFox) with adb working: the log
is the last 1025 blocks (4 KiB each) of the userdata partition, written by
kernel patch 0047. It only reads. The block read is left in <out>.bin next to
the text so it can be decoded again with tools/sweet-log.py.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent

FIND_USERDATA = ("for d in /dev/block/by-name/userdata "
                 "/dev/block/bootdevice/by-name/userdata; do "
                 "[ -e $d ] && readlink -f $d && break; done")


def adb(argv: list[str], serial: str | None, text: bool = True):
    cmd = ["adb", *(("-s", serial) if serial else []), *argv]
    done = subprocess.run(cmd, capture_output=True, text=text)
    if done.returncode != 0:
        err = done.stderr if text else done.stderr.decode("utf-8", "replace")
        sys.exit(f"{' '.join(cmd)} failed: {err.strip()}")
    return done.stdout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", help="text file to write")
    ap.add_argument("-s", "--serial", help="adb serial (default: the only device)")
    a = ap.parse_args()

    if shutil.which("adb") is None:
        sys.exit("adb not found in PATH")
    serial = a.serial
    if serial is None:
        devices = [ln.split()[0] for ln in adb(["devices"], None).splitlines()[1:]
                   if ln.strip() and not ln.startswith("*")]
        if len(devices) != 1:
            sys.exit(f"need exactly one adb device, found {devices or 'none'}")
        serial = devices[0]

    dev = adb(["shell", FIND_USERDATA], serial).strip()
    if not dev:
        sys.exit("userdata block device not found (is the phone in recovery?)")
    size = int(adb(["shell", f"blockdev --getsize64 {dev}"], serial).strip())
    skip = size // 4096 - 1025
    blob = adb(["exec-out", f"dd if={dev} bs=4096 skip={skip} count=1025 2>/dev/null"],
               serial, text=False)

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    area = out.with_suffix(out.suffix + ".bin")
    area.write_bytes(blob)
    print(f"{dev}: {size} bytes, read blocks {skip}.. ({len(blob)} bytes)", file=sys.stderr)
    subprocess.run([sys.executable, str(ROOT / "sweet-log.py"), str(area), str(out)],
                   check=True)
    print(f"{out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
