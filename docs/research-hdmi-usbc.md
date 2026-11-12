# Video out (HDMI/DisplayPort) over the Pixel 8a's USB-C — research

Date: 2026-11-11. Research only: neither the phone nor the port's code was touched.

## Conclusion

- **It is possible; the hardware is there.** The Pixel 8a (Tensor G3 "zuma") has its USB-C wired for **DisplayPort Alt Mode**.
  Google enabled it in Android with the June 2024 Feature Drop (before that it was off behind the property
  `persist.vendor.usb.displayport.enabled`).
- **"HDMI" is really DisplayPort.** The phone does not output HDMI. A USB-C→HDMI cable or adapter has a DP→HDMI converter
  inside. For us it is the same as a DisplayPort monitor.
- **Google publishes all the code needed** (GPL-2.0) in the Pixel kernel: DP controller, PHY, DECON and Type-C.
  It is the reference for the port; no reverse engineering is needed.
- **It is a big project**: five new pieces are needed in our kernel (I2C, Type-C/PD, PHY, DP link, DECON).
  None exists in the port today. Rough estimate: 4,000–6,000 ported lines, in phases that can be tested one by one.
- **Extra benefit:** the DECON reads the frame straight from memory and gives a real vsync. Today we copy each frame
  to the panel with the CPU (2.5–3 ms) and the "vsync" is a timer.

## What the hardware has (akita device tree)

| Piece | Node | Address / data |
|---|---|---|
| DP controller (link) | `drmdp@110F0000`, `samsung,exynos-dp` | `0x110F0000` (0x6000), IRQ SPI 0x185, 40 MHz `dposc_clk`, power domain `pd_hsi0` |
| Combined USB/DP PHY (Synopsys) | reg `phy` | `0x11130000` |
| PHY mux (TCA) | reg `phy-tca` | `0x11140000` |
| USB PHY (owner of the combo PHY) | `phy@11100000`, `samsung,exynos-usbdrd-phy` | `0x11100000`–`0x11140000`, `has_combo_phy = 1`, `usbdp_mode = 1` |
| USB (dwc3) | `samsung,exynos9-dwusb` | `0x11210000` |
| Display controller for DP | `drmdecon@19471000` (DECON1) | akita enables it with `connector = <0x10>` (DP); IRQ SPI 0xf8/0xf7/0xf6/0x11a |
| Layers (DPP) | `drmdpp0..13` | `0x19900000…`, `0x19D00000…` |
| Type-C / PD controller | `max77759tcpc@25` on `hsi2c_11` | I2C `0x10CB0000`, address 0x25, IRQ on `gpa8-3` |
| SBU (AUX) mux | `sbu-mux-sel-gpio` | `gpa7-5` |

On akita `drmdp` has `status = "okay"`. The declared alternate mode is `svid = 0xff01`, `vdo = 0x1cc6`:
DP source (DFP_D), receptacle, pin assignments **C, D and E**.

- Pin C/E: 4 DP lanes, USB 2.0 only.
- Pin D: 2 DP lanes + USB 3.

## How Android does it (Google kernel + USB HAL)

1. **Type-C/PD** (`tcpci_max77759.c` + Linux's TCPM stack): detects the cable and orientation, negotiates Power Delivery
   and enters DisplayPort alternate mode with VDM messages (Discover Identity → Discover SVIDs → Discover Modes →
   Enter Mode → DP Status → DP Configure). "HPD" is not a pin: it arrives inside those messages (Attention).
2. **USB HAL** (`device/google/zuma/usb/usb/Usb.cpp`): copies three values from Type-C to the DP driver through sysfs
   (`/sys/devices/platform/110f0000.drmdp/drm-displayport/`): `pin_assignment` (C/D/E), `orientation` and `hpd`.
   So in Android the bridge is done by user space.
3. **DP driver** (`exynos_drm_dp.c`, on `hpd = 1`):
   - turns on the DPOSC clock and calls `dp_hw_init`;
   - `dp_hw_init`: reset, `dwc3_exynos_phy_enable(1, 1)`, AUX, PHY PLL (MPLLB), USB→DP switch in the TCA
     (`DP_ALT4` or `USB31_DP_ALT2`), wait for lock, lane map from pins and orientation;
   - reads the DPCD over AUX, does *link training* (CR and EQ, lowering rate or lanes on failure);
   - reads the EDID, picks a mode, programs the video timings and starts.
4. **DECON1 + DPP** feed the pixels to the DP link.

Useful facts from the driver:

- Rates: RBR 1.62, HBR 2.7, HBR2 5.4 and HBR3 8.1 Gbps per lane; 1, 2 or 4 lanes. SST only (no MST).
- Mode validation is bandwidth only: `link_rate × lanes × 8 ≥ clock × 3 × bpc`. HBR2 × 4 fits 4K60.
- It has a **BIST mode**: the DP controller itself generates color bars without DECON. The ideal intermediate milestone.
- HDCP exists but is not required to show an image.
- The Synopsys PHY needs a block of code loaded at init (`snps-usbdp-ram-code.h`).

**Real resolution:** there is no official figure from Google. The driver supports up to HBR3 × 4. Sources about Android
disagree (some say 4K60, others that it stays at 1080p60). Not confirmed.

## What would have to be written for macOS on the Pixel 8a

| Phase | Piece | Google reference | Testable milestone |
|---|---|---|---|
| 1 | I2C driver (HSI2C/USI at `0x10CB0000`) | `i2c-exynos5`/`hsi2c` | Read the MAX77759 ID |
| 2 | TCPCI + minimal PD: CC, orientation, power contract, DP VDMs | `tcpci_max77759.c`, `tcpm.c`, `altmodes/displayport.c` | The adapter answers "DP mode configured" and HPD arrives |
| 3 | Combo PHY + TCA + AUX | `phy-exynos-snps-usbdp.c`, `cal_9865/dp_reg.c` | Read the monitor's DPCD and EDID |
| 4 | Link training + BIST video | `exynos_drm_dp.c`, `dp_reg.c` | Color bars on the monitor |
| 5 | DECON1 + one DPP layer reading a framebuffer | `cal_9865/decon_reg.c`, `dpp_reg.c` | Our own still image on the monitor |
| 6 | macOS integration: second IOMobileFramebuffer display (or replace the panel's) | our `NativeGOPDisplay` | Desktop on the monitor |
| 7 | USB host mode (dwc3 is xHCI) for keyboard, mouse and network through the dock | — | A "desktop Mac" with one cable |

Phases 1–4 do not depend on macOS: they can be tested from the kernel with logs.

## Risks and open questions

- **We lose the debug link.** Today the log, the viewer and the shell go over USB-C in device (gadget) mode.
  To enter DP mode the phone has to be the *host* (DFP). With the monitor connected there is no gadget.
  Options: network through a USB-Ethernet adapter in the dock (needs phase 7), or debug "blind" with the on-screen log.
  **This is the biggest practical obstacle.**
- **Power.** With a simple adapter the phone must supply 5 V on VBUS (the MAX77759 *boost* has to be enabled).
  With a powered dock the phone is a sink, which is simpler. Start with a powered dock.
- **PHY shared with USB.** USB owns the PHY. If our current gadget only uses USB 2.0 (eUSB), the SuperSpeed/DP PHY may be
  uninitialized and would have to be brought up entirely, including loading the RAM code.
- **Memory for the DECON.** The DPPs read by DMA through `sysmmu_dpuf0/1` (and S2MPU). The current panel works with what
  the bootloader left, so there is a working path, but how it was configured is unknown. Read it first.
- **Clocks.** The 40 MHz DPOSC and the DPU clocks for DECON1 may be off; they have to be enabled through the CMU.
- **GPU load.** 1080p has 3.2 times the pixels of today's 1200×540, and 4K 12.8 times. We do not reach 60 fps even at the
  current resolution: performance should be solved first.
- **License.** Google's code is GPL-2.0.

## Recommendation

Do it **after** 60 fps on the panel is stable. When it is time, start with phases 1–4 using a powered dock: reaching color
bars proves the whole electrical chain before investing in DECON and macOS.

## Sources

- Google kernel, branch `android-gs-akita-5.15-android14-qpr3`:
  - [display/samsung](https://android.googlesource.com/kernel/google-modules/display/+/refs/heads/android-gs-akita-5.15-android14-qpr3/samsung/) (`exynos_drm_dp.c`, `exynos_drm_decon.c`, `displayport/dp_zuma.c`)
  - [cal_9865](https://android.googlesource.com/kernel/google-modules/display/+/refs/heads/android-gs-akita-5.15-android14-qpr3/samsung/cal_9865/) (`dp_reg.c`, `decon_reg.c`, `dpp_reg.c`, `regs-usbdpphy_*.h`)
  - [phy/samsung](https://android.googlesource.com/kernel/google-modules/soc/gs/+/refs/heads/android-gs-akita-5.15-android14-qpr3/drivers/phy/samsung/) (`phy-exynos-snps-usbdp.c`, `snps-usbdp-ram-code.h`, `phy-exynos-usbdrd.c`)
  - [typec/tcpm/google](https://android.googlesource.com/kernel/google-modules/soc/gs/+/refs/heads/android-gs-akita-5.15-android14-qpr3/drivers/usb/typec/tcpm/google/) (`tcpci_max77759.c`)
- [zuma USB HAL, `Usb.cpp`](https://android.googlesource.com/device/google/zuma/+/refs/heads/main/usb/usb/Usb.cpp)
- [Android Authority: Pixel 8a display output](https://www.androidauthority.com/pixel-8a-display-output-3441590/)
- [Plugable: Android 16 desktop mode with a Pixel](https://kb.plugable.com/how-to-use-android-16-desktop-mode-with-a-pixel-phone-and-usb-c-display-or-hub)
- Device tree: `zuma-drm-dpu.dtsi`, `zuma-usb.dtsi`, `max777x9-tcpc.dtsi`, `zuma-akita-display.dtsi`
