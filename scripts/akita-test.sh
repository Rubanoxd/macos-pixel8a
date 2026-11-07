#!/bin/bash
# RAM-boot a boot image on the Pixel 8a and wait for it to come back to fastboot on its own
# (the akita firmware arms a long watchdog and sets the reboot mode to bootloader).
#   akita-test.sh IMAGE [MAX_SECONDS]
set -u
IMAGE=${1:?image}
MAX=${2:-300}

# A PSCI reset comes back through Android; send it to the bootloader.
for i in $(seq 1 600); do
  fastboot devices | grep -q . && break
  adb devices 2>/dev/null | grep -q -w device && adb reboot bootloader >/dev/null 2>&1
  sleep 1
done
fastboot devices | grep -q . || { echo "no device in fastboot"; exit 1; }

start=$(date +%s)
echo "boot $(date +%T) $IMAGE"
fastboot boot "$IMAGE" 2>&1 | tail -1
sleep 3
while :; do
  now=$(date +%s)
  if fastboot devices | grep -q .; then
    echo "back in fastboot after $((now - start)) s"
    exit 0
  fi
  if adb devices 2>/dev/null | grep -q -w device; then
    echo "android booted after $((now - start)) s"
    exit 2
  fi
  if [ $((now - start)) -ge "$MAX" ]; then
    echo "still running after $MAX s"
    exit 3
  fi
  sleep 2
done
