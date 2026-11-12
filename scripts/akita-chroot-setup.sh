#!/bin/bash
# On the phone (Recovery): prepares a full-macOS chroot in "/Volumes/Macintosh HD" with the SystemOS cryptex
# at System/Volumes/Preboot/Cryptexes/OS, devfs, and a union tmpfs over System/Library with dyld -> the cryptex's cache.
R="/Volumes/Macintosh HD"; C=/Volumes/CheerF25F71.arm64eSystemCryptex; D=/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld
for i in $(seq 1 30); do [ -d "$R/System" ] && break; sleep 2; done
mount | grep -q "$R/System/Volumes/Preboot " || mount_tmpfs "$R/System/Volumes/Preboot"
mkdir -p "$R/System/Volumes/Preboot/Cryptexes/OS"
if ! mount | grep -q "Cryptexes/OS "; then diskutil unmount "$C" >/dev/null 2>&1; mount_apfs -o rdonly /dev/$(diskutil list | awk '/CheerF25F71/{print $NF}') "$R/System/Volumes/Preboot/Cryptexes/OS"; fi
mount | grep -q "on $R/dev " || mount -t devfs devfs "$R/dev"
if [ ! -d "$R/System/Library/dyld" ]; then mount_tmpfs -o union "$R/System/Library"; mkdir -p "$R/System/Library/dyld"; for f in $(ls "$R$D"); do ln -sf "$D/$f" "$R/System/Library/dyld/$f"; done; fi
mount | grep "Macintosh HD"
