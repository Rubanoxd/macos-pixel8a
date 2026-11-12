# Early research (Nov 3, 2026) — historical

Before the port worked we collected two research reports (one on running macOS on the Pixel 8a, one on the Mali-G715 GPU).
They were written with AI help and parts of them turned out wrong. This is an English summary of what they said and what
actually happened. The originals (Spanish) are in this repository's history.

## What the reports recommended, and what happened

| Report said | What happened |
|---|---|
| Use the `VMAPPLE` (virtual Mac) kernel, patch out its paravirtual PAC hypercalls, add `/filesystems/DoNotUseSEP` | We used macOS-ForSheng's bare-metal kernel and boot path instead (see `GUIDE.md`). |
| WindowServer has no software fallback, so aim for a headless/console macOS | macOS-ForSheng's Recovery drew with a software path through page patches; later WindowServer ran on our GPU stack. |
| Secondary cores start at EL2 through PSCI `CPU_ON` and need a trampoline to EL1 | Correct: XNU's `reset_vector` was patched to drop to EL1 (`GUIDE.md` §3.2). |
| Cluster 0 watchdog at `0x10060000` reboots the phone | Correct (`GUIDE.md` §3.4). |
| The Tensor G3 supports 16 KiB pages (`TGran16 = 1`) | Correct. |
| GPU acceleration is 9–18 months away; PanVK on Valhall v11 lacks what metal2vulkan needs and has never been built for Mach-O | Done in about a week: our IOKit CSF driver, PanVK built for Darwin arm64e, darwin-drm, metal2vulkan; WindowServer composited on the Mali on Nov 8, 2026. |
| USB device mode needs a DWC3 gadget driver with CDC-NCM/ECM | Done (`AkitaUsbGadget.h`): CDC-ECM, `en0` at 169.254.7.2, SSH. |
| Display: reuse the UEFI GOP framebuffer instead of driving DECON/DSIM | Correct: `NativeGOPDisplay` copies frames into the GOP framebuffer. |

## GPU facts that held

- **Mali-G715 MC7**, Valhall v11, **CSF** (Command Stream Frontend): an MCU inside the GPU runs firmware and processes queues.
- Registers at **`0x1F000000`** (GPU control, MCU control at `+0x700`, MMU address spaces from `+0x2400`, doorbells from
  `+0x80000` in 64 KiB pages). Reading them with the GPU unpowered faults the bus.
- Power domains `pd_g3d` and `pd_embedded_g3d`; the PMU is protected on Tensor and is accessed through
  `SMC 0x82000504` (like Linux's `tensor_sec_reg_read`). GPU clocks go through ACPM DVFS (`ACPM_DVFS_G3D`, `ACPM_DVFS_G3DL2`).
- The CSF firmware from the phone's vendor partition (`mali_csffw-r54p2.bin`) is **not encrypted**; its sections are loaded
  into the MCU's address space (AS0) and the MCU is started with `MCU_CONTROL`, then the global interface version is read.
- Boot sequence that worked, as in Linux's `panthor`: soft reset → L2 power-on → AS0 page tables → firmware sections →
  `MCU_CONTROL = 1` → global interface handshake.
- Shader core mask `0x7F` (7 cores). An S2MPU sits in front of the GPU; the bootloader left it usable.

## Pixel 8a peripherals (from Google's device trees)

| Peripheral | Part | Bus |
|---|---|---|
| Touch | Goodix "nottingham" (Berlin) | SPI (USI) — working, see `research-touch.md` |
| Audio amplifier | Cirrus Logic CS35L41 | I2C + I2S |
| Haptics | Cirrus Logic CS40L26A | I2C |
| PMICs | Samsung S2MPG14 (main) / S2MPG15 (sub), Maxim MAX77759 (charger, Type-C) | ACPM / I2C |
| Wi-Fi / Bluetooth | Broadcom (Wi-Fi 6E) | PCIe / UART |
| Security | Google Titan M2 | SPI |

## Legal context

Apple's IPSWs, cryptexes and kernelcaches are copyrighted; this project never redistributes them, and the macOS license only
covers Apple hardware. Running macOS on a Pixel is research and interoperability work.
