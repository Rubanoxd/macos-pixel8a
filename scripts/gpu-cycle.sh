#!/bin/bash
# Boot an image when the phone is in fastboot, start Mali level 8, push the GPU test files, run the Vulkan tests.
# Usage: gpu-cycle.sh <image> [tag]
set -u
cd /Volumes/datos2/unixnu-akita
IMG=$1; TAG=${2:-cycle}
pkill -f "akita-live.py log"
until fastboot devices 2>/dev/null | grep -q fastboot; do sleep 2; done
echo "FASTBOOT $(date +%T)"
fastboot boot "$IMG" 2>&1 | tail -1
sleep 20
until ifconfig | grep -q '02:a5:1a:00:00:01'; do sleep 2; done
echo "GADGET $(date +%T)"
sleep 15
TMPDIR=/Volumes/datos2/tmp python3 -u akita-live.py log scratch/live/$TAG.log > /dev/null 2>&1 &
until nc -G 2 -z 169.254.7.2 2325 2>/dev/null; do sleep 5; done
echo "SHELL $(date +%T)"
sleep 20
scratch/akx 'sysctl -w kern.akita_mali=8' 150 | tail -1
scratch/akx 'mkdir /System/Volumes/Data/ak/gpu 2>/dev/null; vm_stat | head -3' 15
for f in mesa-build/libvulkan_panfrost.dylib mesa-build/test/vkcompute mesa-build/test/vktriangle mesa-build/test/gpu-test mesa-build/test/metaltest; do
  ok=0; for i in 1 2; do scratch/akssh "cat > /System/Volumes/Data/ak/gpu/$(basename $f)" < $f && ok=1 && break; sleep 5; done
  [ $ok = 1 ] || scratch/akput $f /System/Volumes/Data/ak/gpu/$(basename $f) 60
done
scratch/akx 'cd /System/Volumes/Data/ak/gpu && chmod +x vkcompute vktriangle gpu-test metaltest && shasum -a 256 libvulkan_panfrost.dylib metaltest && ./vkcompute ./libvulkan_panfrost.dylib 2>&1 | tail -2 && ./vktriangle ./libvulkan_panfrost.dylib t.ppm 2>&1 | tail -2; ./metaltest 2>&1 | tail -12; vm_stat | head -4' 180
shasum -a 256 mesa-build/libvulkan_panfrost.dylib
echo "DONE $(date +%T)"
