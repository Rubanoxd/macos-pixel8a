# DisplayPort / HDMI out over USB-C ("monitor mode")

Status 2026-11-15: **macOS shows its desktop at 1920×1080 on an external monitor** through a USB-C hub with HDMI. The
DisplayPort link comes up by itself when the hub is plugged in, the USB keyboard and mouse on the same hub keep working,
and unplugging the hub shuts the link down cleanly. Plugging it in again without rebooting is being tested (the first
attempt failed on a PHY handshake that is now non-fatal).

Tested with: a USB-C hub with HDMI, USB-A ports and power pass-through (DP alt mode pin assignment D: 2 DP lanes + USB),
and a 1920×1080 HDMI monitor.

## What it looks like

```
WindowServer (Metal -> PanVK -> Mali)  ->  1920x1080 IOSurface
NativeGOPDisplay (monitor mode)        ->  copies the changed rows into one of two framebuffers in the low DRAM bank
DPP layer L1 + DECON1 (video mode)     ->  DPIF0
DP link (HBR2 x 2 lanes) + Synopsys combo PHY  ->  USB-C (pin D)  ->  hub's DP->HDMI converter  ->  monitor
```

The phone's own panel (DECON0, DPP L0, DSIM0) is left untouched.

## How to use it

Boot-args (both needed today):

- `akita-dp=1`: the DisplayPort side. A kernel thread waits for the USB-C PD engine (`AkitaTypec.inc`) to configure DP alt
  mode with HPD high, then brings the link up; it tears it down when the hub or the monitor goes away and waits for the next
  plug.
- `akita-dp-monitor=1`: the macOS side. `NativeGOPDisplay` offers a 1920×1080 mode and draws into the monitor's framebuffer
  instead of the panel's. The phone's panel keeps the last boot frame while this is on.

Debug commands go through `sysctl kern.akita_acpm=10100000+stage` (see the comments at the top of `AkitaDp.inc`); the
automatic path runs stages 45, 63 and 67.

## Building

Our code is `platform-akita/drivers/Acpm/AkitaDp.inc` (included by `AkitaAcpm.cpp`), the display patches
`platform-akita/patches/display-0025-*` and `display-0026-*`, and `platform-akita/dp/`. The register-level code comes from
Google's Pixel kernel (GPL-2.0) and is **not** in this repo:

```bash
platform-akita/dp/fetch-google-sources.sh build-dp      # downloads pinned revisions and applies google-patches/
platform-akita/dp/build-dp-cal.sh build-dp path/to/AkitaAcpm.o   # compile, then merge into the ACPM module object
```

`fetch-google-sources.sh` takes `cal_9865/dp_reg.c`, `decon_reg.c`, `dpp_reg.c` and their headers from
`kernel/google-modules/display`, `phy-exynos-snps-usbdp.c` from `kernel/google-modules/soc/gs`, and two UAPI/DRM headers
from `kernel/common`. `platform-akita/dp/shim/` maps the few Linux interfaces they use onto xnu, and `glue/` is the C side
that `AkitaDp.inc` calls.

## What it took (the non-obvious parts)

Google's driver assumes things that the Pixel's Android boot sets up and our UEFI boot does not. Each of these was a dead
end until found:

| Symptom | Cause | Fix |
|---|---|---|
| Combo PHY never calibrates ("Not check cal done") | The calibration resistor (REXT) is shared with the eUSB2 PHY and belongs to it after boot | USB sub-control `HSP_MISC` (`0x11100114`) `sel_res_tune_mux = 1` before enabling the PHY |
| Panic right after calibration | `phy_exynos_snps_tx_gen2_deemp_set` writes USB3 link registers through a link base we never map | Skipped when there is no link (USB3 is unused) |
| USB→DP switch times out | The TCA (Type-C crossbar) waits for a PIPE handshake from the USB3 controller, which runs USB 2 only here | The TCA in "system configuration" mode, crossbar set directly |
| AUX gets no reply from the monitor | The DP link's OSC clock was 24.576 MHz; the AUX timing assumes 40 MHz | `CMU_HSI0` `DPOSC_USER` mux (`0x11000670` bit 4) to CMU_TOP's 40 MHz |
| Link trains only at RBR (1.62 Gbps) | Google clears the lanes' AC-capacitor bypass above RBR | Keep the bypass (`DP_CONFIG17`): trains at HBR2 (5.4 Gbps per lane) |
| DECON1 shows one frame, then stops | The DP link stays in BIST (test pattern) mode; the CAL never leaves it | BIST and the forced stream-valid off before the normal video config |
| Wrong colours, half the width | The IDMA format from Google's table does not match this revision for BGRA bytes | IDMA format 0, the one the panel's own layer uses |
| Second plug fails | The PHY's NC→USB synced handshake times out once the TCA is in system mode | Logged, not fatal |

Things to avoid (they hang or panic the phone): reading undocumented registers (for example PHY `0x17c`), dumping
`CMU_DPUF0`, and moving the PHY reference clock mux (`MUX_CLK_HSI0_USB32DRD`) while USB is running (it also clocks the USB
controller: keyboard and mouse stop).

The read-only survey before touching DECON1 showed the panel uses only window 5, channel 0 (DPP L0), SRAMs 0–3 and DSIMIF0,
so DECON1 takes window 0, channel 1 (DPP L1) and DPIF0.

## Monitor mode details

- **Copy.** WindowServer's surface is compared row by row with the last frame shown (`memcmp`); only changed rows are copied.
  The mouse cursor (WindowServer's layer 1) is blended in during the copy.
- **Double buffering.** Two 1920×1080 framebuffers at `0xD1000000` and `0xD1800000` (the low DRAM bank: the display DMA, like
  the Wi-Fi's PCIe, only sees that bank). The hidden one is written, then DPP L1's base address and a DECON1 shadow update
  (`GLOBAL | CMP | WIN0`) are requested; the hardware takes them at the next frame start. This removed most of the visual
  glitches of a single buffer.
- **Performance.** Up to ~20 fps while dragging windows. The limit now is reading the whole 8 MB surface every frame
  (cache maintenance + compare); macOS gives the driver no damage rectangles.

## Next

- Hot-plug of the desktop itself: switch the display between the panel (1200×540) and the monitor (1920×1080) at run time,
  as Apple's paravirtual display does when a VM window is resized, and switch the panel off in monitor mode.
- Lower resolutions (1600×900, 1280×720) scaled up by DPP L1, offered in System Settings > Displays.
- The monitor's name and size from its EDID; its brightness over DDC/CI.
- No copy at all: DPP L1 reading WindowServer's surface directly (needs the display SYSMMU).
- Not tested: the other cable orientation (CC2), 4-lane hubs (pin C/E), audio over HDMI.

See also `docs/research-hdmi-usbc.md` (the research before this work).
