#!/usr/bin/env python3
# Turn the Mali-G715 CSF firmware from your own Pixel 8a's vendor image into the C include Mali.cpp embeds.
# The firmware is ARM/Google property and is not redistributed here. Pull it from the phone (vendor partition):
#   adb pull /vendor/firmware/mali_csffw-r54p2.bin   (expected sha256 be93bec3f63d9e4f85a1b5e9c7dd358bdb2e1021bf4590f0945b19c5f4174edc)
# Usage: make-mali-csffw-inc.py mali_csffw-r54p2.bin platform-akita/drivers/Mali/mali_csffw.inc
import hashlib, os, sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
with open(dst, "w") as out:
    out.write(f"// Generated from the Pixel 8a vendor image /firmware/{os.path.basename(src)} ({len(data)} bytes, "
              f"sha256 {hashlib.sha256(data).hexdigest()}). Do not edit.\n")
    for i in range(0, len(data), 32):
        out.write(",".join(f"0x{b:02x}" for b in data[i:i + 32]) + ",\n")
