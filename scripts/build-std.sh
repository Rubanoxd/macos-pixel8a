#!/bin/bash
# Standard full-OS image (GPU add-on, 9 CPUs, ACPM/DVFS, touch, persistent Data disk). Usage: build-std.sh <N> [compile-names...]
export AKITA_EXTRA_BA="akita-cryptex-preboot=1 akita-nobrowse=1 akita-cryptex-fallback=1 akita-m2v-log=1 akita-acpm=1 akita-dvfs=1 akita-touch=1${AKITA_MORE_BA:+ $AKITA_MORE_BA}"
export AKITA_CPUS=${AKITA_CPUS:-9}
export AKITA_GPU_ADDON=${AKITA_GPU_ADDON:-/Volumes/datos2/unixnu-akita/scratch/akita-gpu.addon}
exec bash /Volumes/datos2/unixnu-akita/scratch/build-fullos.sh "$@"
