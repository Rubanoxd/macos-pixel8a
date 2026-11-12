# Persistent storage and memory (Nov 12, 2026)

Two changes that make the Pixel 8a feel more like a real Mac: the user's files and settings survive a reboot, macOS has
real swap on disk, and it sees almost 6 GiB of RAM instead of 4.

## Persistent home and swap

Until now every boot started from a clean slate: the system volume is read-only (as on a Mac) and the Data side was a
tmpfs in RAM. Nothing persisted, there was no swap (`low swap: failed to create swapfile`, errno 30) and user data used RAM.

**Kernel side** — `platform-akita/drivers/NativeUfs/AkitaDataDisk.h`, included by `NativeUfs.cpp`:

- Publishes a plain block device, "AkitaData", over a fixed window of the `userdata` partition: 64 GiB starting 32 GiB in.
  On our phone `userdata` is 109.66 GiB, the flashed system image ends at 22.26 GiB and the last GiB is the reserve of the
  persistent kernel log, so the window is clear of both. The driver refuses the window if it would overlap either.
- Boot-arg `akita-data-disk=1` publishes it read-only (probe); `akita-data-disk=2` read-write. Every write still goes
  through `hostWrite4k`, which only accepts blocks inside an explicitly opened window.
- 4096-byte blocks; a flush from APFS becomes a UFS `SYNCHRONIZE CACHE`.
- It is published right after the first root window, never before, so it can never be taken for the root volume.

**Userland side** — `rc.server` (in the XNU patch, `kern_exec.c`):

- On first use it creates an APFS container with two volumes, **AkitaData** and **VM** — but only if the first 32 KiB of the
  window are zero. It never formats anything else.
- **VM** is mounted on `/System/Volumes/VM`, where XNU puts its swapfiles: real swap on flash.
- **AkitaData** is mounted on **`/Users/akita`**, the user's home, after the Data template is copied in.
  - A mount point under `/private` failed (the `/private` union is still bare at that point).
  - A home under `/System/Volumes/Data/...` logged in, but the Dock never started. At `/Users/akita` everything starts.
- Preferences written by the boot script (dark mode) are written only once, so the user's own changes are kept.

**Measured:** sequential write ~220 MB/s, read ~530 MB/s; under `memory_pressure -l critical` macOS created 2–6 GB of
swapfiles; a file left on the Desktop was there after a reboot.

## RAM: 4 → 5.98 GiB

The Pixel 8a has 8 GiB: a 2 GiB bank at `0x80000000` cut up by Android's carve-outs (secure world, modem, AOC, TPU, logs,
framebuffer) and a 6 GiB bank at `0x880000000`. DarwinBoot gives macOS the largest free contiguous run.

| Step | Change | macOS sees |
|---|---|---|
| before | — | 4.00 GiB (fixed window) |
| fullos183 | ask DarwinBoot for up to 8 GiB (`AKITA_OWNED_MEMORY=0x200000000`) | 5.26 GiB |
| fullos190 | move the UEFI DXE heap (60 MiB) from the top of the high bank to the low bank (`0xC4000000`, a gap with no carve-outs) | 5.48 GiB |
| fullos191 | declare all 8 GiB to the UEFI (`PcdSystemMemorySize = 0x200000000`); the "ABL reserve at the top of DRAM" was copied from another Pixel and is ordinary RAM on akita | **5.98 GiB** |

Checks on fullos191: `memory_pressure -l critical` (6 GB of swap used, no panics) and `tools/ramtest/memcheck.c` (4.5 GiB
filled with an address-derived pattern, 3 rounds of 30 s, 0 bad words).

The UEFI changes are in `uefi-akita/Platforms/Google/akitaPkg` (`MemoryMapLib.c`, `akita.dsc`).

What is left: ~1.2 GiB free in the low bank, in pieces of 80–500 MiB between carve-outs. XNU assumes contiguous RAM, and
with real swap on flash it is not worth the risk.
