#!/bin/bash
# Arranca en RAM (fastboot boot) una imagen de macOS para el Pixel 8a y deja todo listo para trabajar sin cámara:
#   1. espera a que el teléfono esté en fastboot y arranca la imagen
#   2. espera la interfaz USB (gadget CDC-ECM) y guarda el log del kernel en vivo (akita-live.py log)
#   3. espera a que el guion de entrada termine (Terminal abierto con  nc -l 2323|bash|nc -l 2324 )
#   4. deja sshd listo (akita-usb-shell.sh setup)
# Uso: akita-boot.sh <dir de imagen, p. ej. out/akita-macos-live3>  [etiqueta]
set -u
BASE=/Volumes/datos2/unixnu-akita
FS=$BASE/ref/macOS-ForSheng
IMG=${1:?imagen}
case "$IMG" in /*) ;; *) IMG=$FS/$IMG;; esac
TAG=${2:-$(basename "$IMG")}
LOG=$BASE/scratch/live/kernel-$TAG-$(date +%H%M).log
mkdir -p "$BASE/scratch/live"
until fastboot devices | grep -q fastboot; do sleep 3; done
echo "arranque $(date +%H:%M:%S) $IMG"
fastboot boot "$IMG/akita-macos26-boot.img" 2>&1 | tail -1
IF=""
for _ in $(seq 1 240); do
  IF=$(for i in $(ifconfig -l); do ifconfig "$i" 2>/dev/null | grep -q "ether 02:a5:1a:00:00:01" && echo "$i"; done | head -1)
  [ -n "$IF" ] && ifconfig "$IF" | grep -q "inet " && break
  sleep 5
done
[ -z "$IF" ] && { echo "sin interfaz USB"; exit 1; }
echo "red $IF $(date +%H:%M:%S)"
pkill -f "akita-live.py log" 2>/dev/null
nohup python3 "$BASE/akita-live.py" log "$LOG" > /dev/null 2>&1 &
sleep 2; python3 "$BASE/akita-live.py" replay
echo "log en $LOG"
for _ in $(seq 1 180); do grep -aq "script done" "$LOG" && break; sleep 10; done
echo "terminal $(date +%H:%M:%S)"
sleep 5
"$BASE/akita-usb-shell.sh" setup 2>&1 | tail -1
