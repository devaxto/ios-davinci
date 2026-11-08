#!/usr/bin/env python3
"""deploy-metal-bundle.py: Deploy HakiosMetal.bundle into iOS userdata rootfs via OrangeFox recovery over adb."""

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUNDLE_DIR = ROOT / "out-bundles" / "ca" / "HakiosMetal.bundle"

def run_cmd(cmd, check=True):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, shell=True)
    if check and p.returncode != 0:
        print(f"[-] Command failed: {cmd}")
        print(f"    stderr: {p.stderr.strip()}")
        sys.exit(p.returncode)
    return p.stdout.strip()

def wait_for_adb():
    print("[*] Waiting for ADB device (OrangeFox / Recovery)...")
    while True:
        out = run_cmd("adb devices", check=False)
        lines = [l for l in out.splitlines()[1:] if l.strip()]
        for l in lines:
            parts = l.split()
            if len(parts) >= 2 and parts[1] == "recovery":
                print(f"[+] Found recovery device: {parts[0]}")
                return parts[0]
            elif len(parts) >= 2 and parts[1] == "device":
                print(f"[+] Found adb device: {parts[0]}")
                return parts[0]
        time.sleep(2)

def main():
    if not BUNDLE_DIR.exists():
        sys.exit(f"[-] Bundle path {BUNDLE_DIR} does not exist. Extract it first.")

    serial = wait_for_adb()

    print("[*] Checking partitions in recovery...")
    userdata_dev = run_cmd(f"adb -s {serial} shell \"for d in /dev/block/by-name/userdata /dev/block/bootdevice/by-name/userdata; do [ -e $d ] && readlink -f $d && break; done\"")
    if not userdata_dev:
        userdata_dev = "/dev/block/bootdevice/by-name/userdata"
    print(f"[+] Userdata partition block: {userdata_dev}")

    print("[*] Creating mountpoint /mnt/ios_root...")
    run_cmd(f"adb -s {serial} shell \"mkdir -p /mnt/ios_root\"")

    print("[*] Mounting HFS+ userdata rootfs...")
    # Try mounting hfsplus
    mount_res = run_cmd(f"adb -s {serial} shell \"mount -t hfsplus -o rw,force {userdata_dev} /mnt/ios_root || mount -o rw {userdata_dev} /mnt/ios_root\"", check=False)
    print(f"[+] Mount output: {mount_res}")

    check_sys = run_cmd(f"adb -s {serial} shell \"ls -ld /mnt/ios_root/System/Library/Extensions\"", check=False)
    print(f"[+] Target directory check: {check_sys}")
    if "No such file" in check_sys or not check_sys:
        print("[-] /System/Library/Extensions not found on /mnt/ios_root. Checking if mount failed.")
        sys.exit(1)

    print("[*] Pushing HakiosMetal.bundle into /System/Library/Extensions/...")
    run_cmd(f"adb -s {serial} push \"{BUNDLE_DIR}\" /mnt/ios_root/System/Library/Extensions/")

    print("[*] Setting root:wheel ownership and permissions...")
    run_cmd(f"adb -s {serial} shell \"chown -R 0:0 /mnt/ios_root/System/Library/Extensions/HakiosMetal.bundle\"")
    run_cmd(f"adb -s {serial} shell \"chmod -R 755 /mnt/ios_root/System/Library/Extensions/HakiosMetal.bundle\"")
    run_cmd(f"adb -s {serial} shell \"chmod 755 /mnt/ios_root/System/Library/Extensions/HakiosMetal.bundle/HakiosMetal\"")

    print("[*] Syncing filesystem buffers...")
    run_cmd(f"adb -s {serial} shell \"sync\"")

    print("[*] Unmounting /mnt/ios_root...")
    run_cmd(f"adb -s {serial} shell \"umount /mnt/ios_root\"", check=False)

    print("\n[SUCCESS] HakiosMetal.bundle deployed to iOS userdata rootfs!")
    print("[*] Rebooting back to bootloader...")
    run_cmd(f"adb -s {serial} reboot bootloader", check=False)
    print("[+] Device rebooting to fastboot mode.")

if __name__ == "__main__":
    main()
