// SPDX-License-Identifier: GPL-2.0-or-later
// macOS 26.5 on the Pixel 8a (akita) - port by @1421rubano
// akita (Generated-by: LLM): DisplayPort over USB-C, step A: a read-only look at the blocks the video output needs.
// Nothing is written. Each stage is its own sysctl (kern.akita_acpm = 10100000 + stage) so the log flushes in between; a block
// whose clock gate reads "manual, off" is not read (a read of an unclocked block hangs the phone), + 50 reads it anyway.
//   1  CMU_HSI0 gates of the DP link and the USB/DP combo PHY, CMU_TOP's DP oscillator clock, the PMU's PHY isolation words
//      (0x3eb0 USB, 0x3eb4 DP, through the SMC as the Wi-Fi code does: direct PMU reads hang)
//   2  the DP link (0x110f0000): version, reset, clocks, lanes, HPD, PLL lock, interrupts
//   3  the combo PHY's Samsung wrapper (0x11130000): reset, CR port, config, SRAM, TCA, power-down, the DP side's config
//   4  the PHY's Type-C assist (0x11140000): clocks, interrupts, mux control and its debug state
//   5  DPUB: its gates and the first words of DECON0 (the panel's, running) and DECON1 (the one DP would use)
//   6  DPUF0: the clock gates of its layers' block (0x19802000..0x19802100) and its Q-channels. HUNG THE PHONE in fullos292
//      (2026-11-14): refused unless forced (56)
// Step B (writes; the dongle with the HDMI monitor on the hub):
//   10 read-only: the SBU mux pin (gpa7-5), the TCPC's SBU switch (0x94) and orientation (0x19), the PD/DP state
//   11 SBU to the DP AUX: gpa7-5 output = orientation (0 normal, 1 flipped), TCPC SBUSW_CTRL = 0x09 (Google's tcpci_max77759.c)
//   12 the combo PHY on (Synopsys sequence: reset, MPLLA, RAM mode, TCA to USB) and its registers printed
//   13 dp_hw_init: DP link reset, OSC clock, AUX on, MPLLB (HBR), TCA to USB3 + DP 2 lanes (pin D), PLL lock, HPD forced
//   14 DPCD 0x000..0x00f over AUX; 15 EDID block 0 over AUX
//   fullos293: the TCA's USB -> DP switch timed out (XA_TIMEOUT): in controller-synced mode it waits for the USB3 controller's
//   PIPE handshake, and our DWC3 runs USB 2 only. Ways round it, each its own stage (then 13 again):
//   16 TCA_CTRLSYNCMODE_CFG0: USB_ACK_CHECK_BYPASS_EN (bit 12); 17 + DP_ACK_CHECK_BYPASS_EN (bit 13); both clear TCA_INTR_STS
//   18 the TCA in system configuration mode (GCFG op_mode 0), connection mode 3 (USB31 + DP 2 lanes) set directly
//   19 a TCA timeout no longer stops dp_hw_init (to see whether the PLL locks anyway); 20 the TCA's state
//   fullos294: in system configuration mode the crossbar did switch (GEN_STATUS 0x103, USB31 + DP 2 lanes), but the DP TX
//   lanes never took a request (DP_CONFIG12 req 0xf stuck, tx0/tx1 ack high) and the PLL did not lock. More light:
//   21 read-only: the eUSB/PHY supplies on the S2MPG14 (LDO8M 0x32 and LDO_CTRL1 0x44, LDO9M 0x33, LDO19M 0x3d; ACPM channel 2)
//   22 read-only: every documented PHY wrapper word (0x100..0x178, 0x200..0x250) and TCA word (0x00..0x44)
//   23 the CAL set to 4 lanes (pin C/E layout, TCA DP_ALT4) for the next 13 / 24; 24 the TCA in system mode with the CAL's lanes
//   25 the combo PHY enable runs again on the next 12
//   fullos296: all four DP lanes acked but never finished, MPLLB never locked, and USB3's RX calibration never finished either:
//   the PHY's analog side has no reference clock. Its ref (USB32DRD REF_CLK, CMU_HSI0 MUX_CLK_HSI0_USB32DRD = 0) comes from
//   DIV_CLK_HSI0_USB <- PLL_USB, which the eUSB (on TCXO) does not need. Google's cmucal: PLL_USB 614.4 MHz (FRD4312: M 200, P 4,
//   S 1 from OSCCLK 24.576 MHz, lock time P x 150), DIV_CLK_HSI0_USB ratio 31 -> 19.2 MHz (the DT's phy_ref_clock).
//   26 read-only: PLL_USB (LOCKTIME 0x0, CON3 0x10c, CON8 0x120), the HSI0 muxes and dividers on that path
//   27 PLL_USB on (only when it is off: ENABLE bit 31, MUX_SEL bit 4, wait STABLE bit 29) and DIV_CLK_HSI0_USB = 31
//   fullos297: PLL_USB is on and stable (M 200 P 4 S 1) but its output mux is in bypass (MUX_SEL 0): DIV_CLK_HSI0_USB hands the
//   PHY OSCCLK / 32 = 768 kHz instead of 19.2 MHz. 28 sets MUX_SEL (bit 4) and waits for MUX_BUSY (bit 7) to clear.
//   fullos298: MUX_SEL does not stick (no such bit on this FRD4312): PLL_USB's output is live, the reference is 19.2 MHz.
//   Next suspect: the resistor tune (REXT) the eUSB2 PHY and the combo PHY share. Google's exynos_usbcon_ready_to_pipe3_phy
//   (exynos-usb-blkcon.c) hands it to the combo PHY: USB sub-control (0x11100000) HSP_MISC (0x114) sel_res_tune_mux = 1,
//   set_req/ack_in1/in2 = 0; it also clears LINKCTRL.force_pipe_en and sets LINK_CLKRST.link_pclk_sel, which we leave alone
//   (the DWC3 would lose its clock: it runs USB 2 for the keyboard and mouse).
//   29 read-only: sub-control 0x00..0x1c and HSP_MISC; 30 HSP_MISC as Google sets it
//   31 the whole step B in a row: 11, the PHY afresh (25 + 12), 18, 19, 13, 14, 15
//   fullos300 (after 30 + 31): calibration done, MPLLB locked, dp_hw_init returned 0; the AUX gets no reply from the sink.
//   32 tries the eight AUX orientations (SBU mux gpa7-5, link AUX_CONTROL PN invert bit 1, PHY DP_AUX_CONFIG0 dn swap bit 0)
//      with a one-byte DPCD read each, and leaves the first one that answers; 33 read-only: pin banks 0xa0, 0xc0, 0xe0
//   fullos301: no orientation answered (AUX_MONITOR_1 cmd status 2 = timeout, nothing detected on the line). The CAL takes the
//   DP link's OSC clock as 40 MHz (regs-dp.h OSC_CLK; Google's displayport_dp_zuma.c: "DPOSC clock is supposed to be 40 MHz")
//   and times the AUX from it, but CMU_HSI0 PLL_CON0_MUX_CLKCMU_HSI0_DPOSC_USER (0x670) is 0: OSCCLK 24.576 MHz.
//   34 selects CLKCMU_HSI0_DPOSC (CMU_TOP: PLL_SHARED2_D2 / 10 = 40 MHz): bit 4, waits for BUSY (bit 16); then 13, 14, 15
//   fullos302: AUX works (DPCD 1.2, HBR2 x 2 lanes, TPS3, enhanced framing; EDID ASUS VP228 1080p60). Step C:
//   40 link training as Google's exynos_drm_dp.c does it (sink to D0, link config in DPCD 0x100.., clock recovery with TPS1,
//      channel equalization with TPS3/TPS2, swing and pre-emphasis as the sink asks; HBR2, then HBR, then RBR on failure)
//   41 BIST colour bars, 1080p60 (the DP link is the timing master: no DECON needed); 42 stops the video stream
//   45 the whole thing from a fresh boot: 11, 30, 25 + 12, 18, 19, DPOSC 40 MHz, 13, 14, 40, 41
//   fullos303: BIST bars on the monitor, but the link trains only at RBR (HBR2: lane 0 CR only; HBR: none). Differences from
//   Google's driver: PHY charge-pump boost was off (dp_phy_boost defaults to true: on from fullos304), and DP_CONFIG12 asks all
//   four PHY lanes to apply new settings while USB owns two of them in system mode (req 0xf never clears). Knobs, then 40 again:
//   47 SSC on (the sink allows 0.5 % down-spread, Google enables it); 48 ask only lanes 0-1 (akita_dp_req_mask 0x3);
//   49 ask all four again (0xf); 44 read-only: DP_CONFIG11/12/13/17 and the TCA's PSTATE
//   fullos304: boost on, 2-lane request (DP_CONFIG12 clears now) and SSC: still RBR only. The combo PHY's reference clock is
//   19.2 MHz (MUX_CLK_HSI0_USB32DRD 0 = DIV_CLK_HSI0_USB), but Google's cmucal runs it at 40 MHz (VCLK 40000 kHz, gate name
//   USB32DRD_REF_CLK_40): mux 2 = MUX_CLKCMU_HSI0_USB32DRD_USER <- CMU_TOP CLKCMU_HSI0_USB32DRD (mux 1, div 9: 400 / 10 MHz).
//   60 (before the PHY is turned on, i.e. before 45) moves it: HSI0 PLL_CON0 USB32DRD_USER (0x650) bit 4, then
//      CLK_CON_MUX_CLK_HSI0_USB32DRD (0x1008) select 2; waits for BUSY (bit 16) on both. The eUSB's own ref (0x1004) stays.
//   fullos307: keep_acbyp defaults to 1 (45 trains HBR2 by itself); 13 and 40 refuse unless the PHY/link came up; 45 runs once
//   per boot (306: a second 45 failed the PHY calibration and the 40 that followed hung the phone). Never run 60 (USB dies).
//   Step D (DECON1 + a DPP layer -> DPIF -> the DP link):
//   62 read-only survey of the display blocks (all DPUB/DPUF0, clocked; only offsets from Google's regs-decon.h/regs-dpp.h):
//      DECON0/1 main (GLOBAL_CON, TRIG_CON, DATA_PATH_CON_0, BLD_BG_IMG_SIZE_PRI, OF_SIZE_0, RSC_STATUS_0..11, CLOCK_CON),
//      DECON0/1 WINCON (DECON_CON_WIN 0..13: channel map + enable), the shared WIN block (FUNC_CON_0, START/END_POSITION),
//      DECON_SUB DSIMIF_SEL 0/1 and DPIF_SEL 0/1, and DPP L0's DMA (the panel's layer: format, sizes, base, stride)
//   fullos308 survey: DECON0 owns only WIN5, channel 0 (DPP L0), SRAM 0-3 and DSIMIF0; DPIF0/1 unused.
//   63 (after 45) step D1a: BIST off, DECON1 (Google's cal_9865_decon_reg.c) in video mode to DPIF0 with WIN0 as a solid green
//      colour map (no DPP layer, no memory, no sysmmu), then the DP link fed from DPIF; 64 stops DECON1
//      fullos309: DECON1 ran (GLOBAL_CON 0x13, DPIF_SEL0 2) but stopped after 1 frame and the monitor kept the bars: the DP
//      link was still in BIST (the CAL never clears it). fullos310: akita_dp_bist_off() before the video config.
//   fullos310: solid green on the monitor (DECON1 -> DPIF0 -> DP at 60 Hz). Step D1b (after 45 + 63):
//   65 DPP L1 (channel 1, DPUF0) reads the panel's own framebuffer (0xfac00000, 1080x2400 XRGB, which DPP L0 already reads)
//      1:1: its middle 1080x1080 square in WIN0 at x 420 of the 1920x1080 frame
//   66 a 1920x1080 XRGB framebuffer of our own in the low DRAM bank (0xD1000000, 8 MB, write-combined), filled with a test
//      pattern (8 colour bars, a white border, a grey ramp), shown full frame through L1
//   fullos312: 66 shows the test pattern perfectly, 65 the phone's screen with the right colours (IDMA format 0). Step D done.
//   Step E2 (monitor mode): 67 = 66 without drawing anything (the display driver writes the framebuffer at kMonitorFb);
//   boot-arg akita-dp=1 starts dp::autoTick, which waits for the PD's DisplayPort configuration + HPD, then runs 45, 63, 67 once
//   (the macOS side: NativeGOPDisplay with akita-dp-monitor=1 draws its 1920x1080 desktop into the same framebuffer)
//   fullos315 hot-plug: dp::autoTick is a state machine (waiting -> up -> teardown on unplug/HPD low -> waiting); 68 = teardown
//   61 keeps the lanes' AC-capacitor bypass at HBR/HBR2 (DP_CONFIG17; Google clears it above RBR)
// Steps 12.. run Google's CAL (cal_9865_dp_reg.c, phy-exynos-snps-usbdp.c, GPL-2.0, built aside: scratch/dp/build-dpcal.sh).
// Sources: Google's zuma device tree (zuma-drm-dpu.dtsi drmdp@110F0000, zuma-usb.dtsi phy@11100000), cal-if/zuma/cmucal-sfr.c,
// exynos_drm_dp.c and cal_9865/regs-dp.h, regs-usbdpphy_ctrl.h, regs-usbdpphy_tca_ctrl.h, regs-decon.h
// (android-gs-akita-5.15-android15-qpr1).

namespace dp {

constexpr uint64_t kCmuHsi0 = 0x11000000, kCmuTop = 0x26040000, kCmuDpub = 0x19400000, kCmuDpuf0 = 0x19800000;
constexpr uint64_t kLink = 0x110F0000, kPhy = 0x11130000, kTca = 0x11140000;
constexpr uint64_t kDecon0 = 0x19470000, kDecon1 = 0x19471000;
constexpr uint64_t kPmu = 0x15460000;

volatile uint8_t *map(uint64_t phys, uint32_t size) {
  return reinterpret_cast<volatile uint8_t *>(ml_io_map(phys, size));
}
inline uint32_t rd(volatile uint8_t *b, uint32_t o) { return *reinterpret_cast<volatile uint32_t *>(b + o); }

// a CLK_CON_GAT word: bit 28 automatic gating, bit 21 the manual value, bit 20 manual mode
bool gateOpen(uint32_t v) { return !(v & (1u << 20)) || (v & (1u << 21)); }
const char *gateText(uint32_t v) {
  if (v & (1u << 20)) return (v & (1u << 21)) ? "manual on" : "MANUAL OFF";
  return (v & (1u << 28)) ? "auto" : "on";
}

struct Reg { uint32_t off; const char *name; };

void dumpRegs(const char *who, volatile uint8_t *b, const Reg *r, unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    printf("dp: %s %s (+%#x) = %#x\n", who, r[i].name, r[i].off, rd(b, r[i].off));
    IODelay(50);
  }
}

uint32_t gate(volatile uint8_t *cmu, uint32_t off, const char *name) {
  const uint32_t v = rd(cmu, off);
  printf("dp: gate %s (+%#x) = %#x, %s\n", name, off, v, gateText(v));
  return v;
}

const Reg kHsi0Gates[] = {
  {0x2040, "DP_LINK OSC_CLK"}, {0x2050, "DP_LINK GTC_CLK"}, {0x2054, "DP_LINK PCLK"},
  {0x20a0, "USB32DRD USBSUBCTL_APB_PCLK"}, {0x20a4, "USB32DRD USBLINK_ACLK"}, {0x20ac, "USB32DRD REF_CLK_40"},
  {0x20b4, "USB32DRD USBDPPHY_CTRL_PCLK"}, {0x20b8, "USB32DRD EUSB_CTRL_PCLK"}, {0x20c0, "USB32DRD USBDPPHY_TCA_APB_CLK"},
  {0x20d8, "USB32DRD EUSB_APB_CLK"}, {0x20dc, "USB32DRD EUSB_PHY_REFCLK_26"}, {0x20e4, "USB32DRD SUSPEND_CLK_26"},
};

int stage1() {
  volatile uint8_t *h = map(kCmuHsi0, 0x4000), *t = map(kCmuTop, 0x4000);
  if (!h || !t) { printf("dp: stage 1: CMU not mapped\n"); return -1; }
  for (auto &g : kHsi0Gates) gate(h, g.off, g.name);
  printf("dp: HSI0 DPOSC_USER mux con0 %#x con1 %#x, USB32DRD_USER con0 %#x con1 %#x, mux USB32DRD %#x div %#x, "
         "QCH DP_LINK_PCLK %#x\n", rd(h, 0x670), rd(h, 0x674), rd(h, 0x650), rd(h, 0x654), rd(h, 0x1008), rd(h, 0x1808),
         rd(h, 0x303c));
  printf("dp: TOP HSI0_DPOSC gate %#x (%s) mux %#x div %#x; HSI0_USB32DRD gate %#x (%s) mux %#x div %#x\n",
         rd(t, 0x20b8), gateText(rd(t, 0x20b8)), rd(t, 0x1090), rd(t, 0x1888), rd(t, 0x20c4), gateText(rd(t, 0x20c4)),
         rd(t, 0x109c), rd(t, 0x1894));
  printf("dp: PMU USB PHY (0x3eb0) %#llx, DP PHY (0x3eb4) %#llx\n", (unsigned long long)pcie::smcPmuRead(kPmu + 0x3eb0),
         (unsigned long long)pcie::smcPmuRead(kPmu + 0x3eb4));
  return 0;
}

// the gate a block needs, checked before the block is read
bool clocked(uint32_t hsi0GateOff, bool force, const char *what) {
  volatile uint8_t *h = map(kCmuHsi0, 0x4000);
  if (!h) return false;
  const uint32_t v = rd(h, hsi0GateOff);
  if (gateOpen(v)) return true;
  printf("dp: %s: its clock gate (+%#x) is %#x (%s)%s\n", what, hsi0GateOff, v, gateText(v),
         force ? ", reading anyway" : ", not read (+ 50 forces it)");
  return force;
}

const Reg kLinkRegs[] = {
  {0x0000, "SYSTEM_DEVICE_VERSION"}, {0x0004, "SYSTEM_SW_RESET_CONTROL"}, {0x0008, "SYSTEM_CLK_CONTROL"},
  {0x000c, "SYSTEM_OSCLK_QCH_FUNCTION_ENABLE"}, {0x0010, "SYSTEM_MAIN_LINK_LANE_COUNT"}, {0x0014, "SYSTEM_SW_FUNCTION_ENABLE"},
  {0x0018, "SYSTEM_COMMON_FUNCTION_ENABLE"}, {0x001c, "SYSTEM_SST1_FUNCTION_ENABLE"}, {0x0024, "SYSTEM_MISC_CONTROL"},
  {0x0028, "SYSTEM_HPD_CONTROL"}, {0x002c, "SYSTEM_PLL_LOCK_CONTROL"}, {0x0100, "SYSTEM_INTERRUPT_CONTROL"},
  {0x0104, "SYSTEM_INTERRUPT_REQUEST"}, {0x0108, "SYSTEM_IRQ_COMMON_STATUS"}, {0x010c, "SYSTEM_IRQ_COMMON_STATUS_MASK"},
  {0x0200, "SYSTEM_DEBUG"},
};
const Reg kPhyRegs[] = {
  {0x0000, "PHY_RST_CTRL"}, {0x0004, "PHY_CR_PARA_CON0"}, {0x0008, "PHY_CR_PARA_CON1"}, {0x000c, "PHY_CR_PARA_CON2"},
  {0x0100, "PHY_CONFIG0"}, {0x0104, "PHY_CONFIG1"}, {0x0108, "PHY_CONFIG2"}, {0x010c, "PHY_CONFIG3"},
  {0x0110, "PHY_SRAM_CON"}, {0x0114, "PHY_EXT_CONFIG0"}, {0x016c, "TCA_CONFIG"}, {0x0170, "PHY_POWER_DOWN"},
  {0x0174, "TCA_STATE_CTRL"}, {0x0178, "PHY_LANE1_CTRL"}, {0x0200, "DP_AUX_CONFIG0"}, {0x0204, "DP_CONFIG0"},
  {0x0208, "DP_CONFIG1"}, {0x020c, "DP_CONFIG2"}, {0x0210, "DP_CONFIG3"}, {0x0214, "DP_CONFIG4"},
};
const Reg kTcaRegs[] = {
  {0x0000, "TCA_CLK_RST"}, {0x0004, "TCA_INTR_EN"}, {0x0008, "TCA_INTR_STS"}, {0x0010, "TCA_GCFG"}, {0x0014, "TCA_TCPC"},
  {0x0018, "TCA_SYSMODE_CFG"}, {0x0020, "TCA_CTRLSYNCMODE_CFG0"}, {0x0024, "TCA_CTRLSYNCMODE_CFG1"},
  {0x0028, "TCA_CTRLSYNCMODE_DBG0"},
};
const Reg kDeconRegs[] = {
  {0x0000, "DECON_VERSION"}, {0x0020, "GLOBAL_CON"}, {0x0030, "(+0x30)"}, {0x0040, "(+0x40)"},
  {0x0200, "DATA_PATH_CON_0"}, {0x0820, "GLOBAL_CON (shadow)"},
};

int stageBlock(unsigned stage, bool force) {
  switch (stage) {
    case 2: {
      if (!clocked(0x2054, force, "DP link")) return -2;
      volatile uint8_t *b = map(kLink, 0x1000);
      if (!b) return -1;
      dumpRegs("link", b, kLinkRegs, sizeof kLinkRegs / sizeof kLinkRegs[0]);
      return 0;
    }
    case 3: {
      if (!clocked(0x20b4, force, "combo PHY")) return -2;
      volatile uint8_t *b = map(kPhy, 0x1000);
      if (!b) return -1;
      dumpRegs("phy", b, kPhyRegs, sizeof kPhyRegs / sizeof kPhyRegs[0]);
      return 0;
    }
    case 4: {
      if (!clocked(0x20c0, force, "PHY TCA")) return -2;
      volatile uint8_t *b = map(kTca, 0x1000);
      if (!b) return -1;
      dumpRegs("tca", b, kTcaRegs, sizeof kTcaRegs / sizeof kTcaRegs[0]);
      return 0;
    }
    case 5: {
      volatile uint8_t *c = map(kCmuDpub, 0x4000);
      if (!c) return -1;
      const uint32_t pclk = gate(c, 0x2028, "DPUB APB_DECON_MAIN PCLKM"), aclk = gate(c, 0x202c, "DPUB ACLK_DECON");
      gate(c, 0x2004, "DPUB CMU PCLK");
      printf("dp: DPUB NOC_USER %#x DSIM_USER %#x, QCH CMU %#x\n", rd(c, 0x610), rd(c, 0x600), rd(c, 0x3060));
      if (!force && !(gateOpen(pclk) && gateOpen(aclk))) { printf("dp: DECON not read (+ 50 forces it)\n"); return -2; }
      volatile uint8_t *d0 = map(kDecon0, 0x1000), *d1 = map(kDecon1, 0x1000);
      if (!d0 || !d1) return -1;
      dumpRegs("decon0", d0, kDeconRegs, sizeof kDeconRegs / sizeof kDeconRegs[0]);
      dumpRegs("decon1", d1, kDeconRegs, sizeof kDeconRegs / sizeof kDeconRegs[0]);
      return 0;
    }
    case 6: {
      if (!force) { printf("dp: stage 6 hung the phone in fullos292 (CMU_DPUF0 read), not run (56 forces it)\n"); return -2; }
      volatile uint8_t *c = map(kCmuDpuf0, 0x4000);
      if (!c) return -1;
      for (uint32_t o = 0x2000; o < 0x2100; o += 0x20) {
        printf("dp: DPUF0 gates +%#x: %08x %08x %08x %08x %08x %08x %08x %08x\n", o, rd(c, o), rd(c, o + 4), rd(c, o + 8),
               rd(c, o + 12), rd(c, o + 16), rd(c, o + 20), rd(c, o + 24), rd(c, o + 28));
      }
      printf("dp: DPUF0 NOC_USER %#x, QCH CMU %#x\n", rd(c, 0x600), rd(c, 0x3044));
      return 0;
    }
  }
  printf("dp: no stage %u (1..6, + 50 forces a block whose gate reads off)\n", stage);
  return -1;
}

extern "C" {
void akita_dp_setup(void *link, void *phy, void *tca, int flipped, int link_rate);
int akita_dp_phy_enable(void);
int akita_dp_hw_init(void);
int akita_dp_read_dpcd(unsigned addr, unsigned len, unsigned char *buf);
int akita_dp_read_edid(unsigned char *buf);
extern int akita_tca_ignore_timeout;
void akita_dp_phy_forget(void);
void akita_dp_set_lanes(int four);
int akita_dp_write_dpcd(unsigned addr, unsigned len, unsigned char *buf);
void akita_dp_set_rate(int rate);
int akita_dp_get_rate(void);
void akita_dp_set_num_lanes(unsigned n);
void akita_dp_set_enhanced(int on);
int akita_dp_reinit(void);
void akita_dp_training_pattern(int tps);
void akita_dp_swing(unsigned char *voltage, unsigned char *pre);
void akita_dp_timing_1080p60(void);
int akita_dp_bist_on(void);
void akita_dp_video_stop(void);
void akita_dp_set_ssc(int on);
extern unsigned akita_dp_req_mask;
extern int akita_dp_keep_acbyp;
void akita_decon1_map(void *main1, void *win, void *sub, void *wincon1);
int akita_decon1_colormap(unsigned w, unsigned h, unsigned win, unsigned rgb);
int akita_decon1_stop(void);
int akita_dp_video_on(void);
int akita_dp_hw_deinit(void);
void akita_dpp1_map(void *dma, void *dpp, void *sramc, void *scl_coef, void *hdr_comm);
int akita_dpp1_show(uint64_t addr, unsigned f_w, unsigned f_h, unsigned sx, unsigned sy, unsigned w, unsigned h, unsigned win,
                    unsigned wx, unsigned wy);
unsigned akita_dpp1_irq(void);
}
bool gSsc = false;
constexpr uint64_t kMonitorFb = 0xD1000000ull;   // 1920x1080 BGRA in the low DRAM bank gap (0xC7C00000..0xD8100000; Wi-Fi: 0xD0000000 + 4 MB)
bool gPhyUp = false, gInitOk = false, gRan45 = false;
// fullos314: double buffering for the monitor (NativeGOPDisplay monitor mode): buffer A = kMonitorFb, B = kMonitorFb2. The
// display driver writes the hidden one and asks for a flip: DPP L1's base address + a DECON1 shadow update (GLOBAL | CMP | WIN0),
// which the hardware takes at the next frame start; the flip has latched once those request bits read back clear.
constexpr uint64_t kMonitorFb2 = 0xD1800000ull;
void teardown(const char *why);   // hot-plug, below
extern unsigned gAutoState;
constexpr uint32_t kFlipMask = 0x80100001u;
volatile uint8_t *gFlipDma = nullptr, *gFlipDecon = nullptr;
volatile bool gFlipReady = false;   // fullos306: 40 on a half-set PHY hung the phone

constexpr uint64_t kPinAlive = 0x154D0000;
constexpr uint32_t kGpa7 = 0xC0;   // seventh bank of the alive block (gpa0..gpa4, gpa6, gpa7 in the zuma DT): CON, DAT, PUD, DRV
constexpr uint8_t kTcpc = 0x25, kTcpcControl = 0x19, kSbuSw = 0x94;
bool gSetup = false;
int gFlipped = -1;

int flipped() {
  uint8_t ctl = 0;
  if (i2c::readRegs(kTcpc, kTcpcControl, &ctl, 1)) return -1;
  return ctl & 1;   // TCPC_CONTROL bit 0: the plug on CC2 (AkitaTypec.inc sets it from CC_STATUS)
}

void hex(const char *what, const uint8_t *b, unsigned n) {
  for (unsigned i = 0; i < n; i += 16) {
    char line[16 * 3 + 1]; unsigned k = 0;
    for (unsigned j = i; j < i + 16 && j < n; ++j) k += static_cast<unsigned>(snprintf(line + k, sizeof line - k, "%02x ", b[j]));
    line[k] = 0;
    printf("dp: %s +%03x: %s\n", what, i, line);
  }
}

bool setup() {
  if (gSetup) return true;
  volatile uint8_t *l = map(kLink, 0x6000), *p = map(kPhy, 0x1000), *t = map(kTca, 0x1000);
  if (!l || !p || !t) { printf("dp: not mapped\n"); return false; }
  gFlipped = flipped();
  if (gFlipped < 0) { printf("dp: orientation unknown (TCPC not answering)\n"); return false; }
  akita_dp_setup(const_cast<uint8_t *>(l), const_cast<uint8_t *>(p), const_cast<uint8_t *>(t), gFlipped, 1 /* HBR */);
  gSetup = true;
  printf("dp: CAL set up: plug %s, pin D (2 lanes), HBR\n", gFlipped ? "flipped (CC2)" : "normal (CC1)");
  return true;
}

int stageB(unsigned stage) {
  switch (stage) {
    case 10: {
      volatile uint8_t *pin = map(kPinAlive, 0x1000);
      uint8_t sbu = 0, ctl = 0, cc = 0;
      i2c::readRegs(kTcpc, kSbuSw, &sbu, 1); i2c::readRegs(kTcpc, kTcpcControl, &ctl, 1); i2c::readRegs(kTcpc, 0x1D, &cc, 1);
      if (pin) printf("dp: gpa7 con %#x dat %#x pud %#x drv %#x (pin 5: function %u, level %u)\n", rd(pin, kGpa7), rd(pin, kGpa7 + 4),
                      rd(pin, kGpa7 + 8), rd(pin, kGpa7 + 12), (rd(pin, kGpa7) >> 20) & 0xf, (rd(pin, kGpa7 + 4) >> 5) & 1);
      printf("dp: TCPC SBUSW_CTRL %#x TCPC_CONTROL %#x CC_STATUS %#x; PD: DP entered %d configured %d HPD %d status %08x\n", sbu,
             ctl, cc, typec::gS.dpEntered, typec::gS.dpConfigured, typec::gS.hpd, typec::gS.dpStatus);
      return 0;
    }
    case 11: {
      const int f = flipped();
      volatile uint8_t *pin = map(kPinAlive, 0x1000);
      if (f < 0 || !pin) { printf("dp: stage 11: orientation or pins unknown\n"); return -1; }
      volatile uint32_t *con = reinterpret_cast<volatile uint32_t *>(pin + kGpa7), *dat = reinterpret_cast<volatile uint32_t *>(pin + kGpa7 + 4);
      *dat = (*dat & ~(1u << 5)) | (static_cast<uint32_t>(f) << 5);
      *con = (*con & ~(0xfu << 20)) | (1u << 20);   // pin 5 output
      const uint8_t v = 0x09;
      const int r = i2c::writeRegs(kTcpc, kSbuSw, &v, 1);
      printf("dp: SBU mux sel = %d (gpa7 con %#x dat %#x), TCPC SBUSW_CTRL = 0x09 (%d)\n", f, *con, *dat, r);
      return r;
    }
    case 12: {
      if (!setup()) return -1;
      const int r = akita_dp_phy_enable();
      gPhyUp = r == 0;
      printf("dp: combo PHY enable returned %d\n", r);
      stageBlock(3, false); stageBlock(4, false);
      return r;
    }
    case 13: {
      if (!setup()) return -1;
      if (!gPhyUp) { printf("dp: the combo PHY is not up: dp_hw_init refused\n"); return -1; }
      const int r = akita_dp_hw_init();
      gInitOk = r == 0;
      printf("dp: dp_hw_init returned %d\n", r);
      stageBlock(2, false); stageBlock(4, false);
      return r;
    }
    case 16: case 17: case 18: case 19: case 20: {
      volatile uint8_t *t = map(kTca, 0x1000);
      if (!t) return -1;
      auto w = [&](uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(t + o) = v; __asm__ volatile("dsb sy" ::: "memory"); };
      if (stage == 16 || stage == 17) {
        const uint32_t bits = stage == 16 ? (1u << 12) : (3u << 12);
        w(0x20, rd(t, 0x20) | bits);
        w(0x08, 0xffff);
      } else if (stage == 18) {
        const int f = gFlipped >= 0 ? gFlipped : flipped();
        w(0x18, rd(t, 0x34));                                   // the current crossbar setting (GEN_STATUS) into SYSMODE_CFG first
        w(0x10, rd(t, 0x10) & ~3u);                             // op_mode 0: system configuration
        w(0x18, rd(t, 0x18) | (1u << 3));                       // typec_disable
        IODelay(2);
        w(0x18, (rd(t, 0x18) & ~7u) | 3u | (f > 0 ? 4u : 0u));  // conn_mode 3 (USB31 + DP 2 lanes), flip
        IODelay(2);
        w(0x18, rd(t, 0x18) & ~(1u << 3));                      // typec_disable off
        IODelay(1000);
      } else if (stage == 19) {
        akita_tca_ignore_timeout = 1;
        printf("dp: TCA timeouts no longer stop dp_hw_init\n");
      }
      printf("dp: TCA now: CLK_RST %#x INTR_STS %#x GCFG %#x TCPC %#x SYSMODE %#x SYNC_CFG0 %#x DBG0 %#x DBG1 %#x PSTATE %#x GEN_STATUS %#x\n",
             rd(t, 0), rd(t, 8), rd(t, 0x10), rd(t, 0x14), rd(t, 0x18), rd(t, 0x20), rd(t, 0x28), rd(t, 0x2c), rd(t, 0x30), rd(t, 0x34));
      return 0;
    }
    case 21: {
      if (!probe()) { printf("dp: ACPM not reachable\n"); return -1; }
      const struct { uint8_t reg; const char *name; } l[] = {{0x32, "L8M_CTRL"}, {0x44, "LDO_CTRL1"}, {0x33, "L9M_CTRL"}, {0x3d, "L19M_CTRL"}};
      for (auto &x : l) {
        uint32_t w[4] = {(0x01u << 8) | x.reg, 0, 0, 0};
        const int r = transfer(2, w, 4, true);
        const unsigned v = (w[1] >> 8) & 0xff, err = (w[1] >> 24) & 0xff;
        printf("dp: S2MPG14 %s (0x%02x) = %#x (transfer %d, error %u)\n", x.name, x.reg, v, r, err);
      }
      printf("dp: decode: L8M 300 mV + 12.5 x (v & 0x7f), on = LDO_CTRL1 bits 1:0; L9M 725 mV + 12.5 x (v & 0x3f), on = bits 7:6; "
             "L19M 1800 mV + 25 x (v & 0x3f), on = bits 7:6\n");
      return 0;
    }
    case 22: {
      // only the documented words: fullos295 took an SError (panic) dumping 0x160..0x17c in one go (0x17c is not a register)
      volatile uint8_t *p = map(kPhy, 0x1000), *t = map(kTca, 0x1000);
      if (!p || !t) return -1;
      static const uint16_t kPhyRegsAll[] = {0x100, 0x104, 0x108, 0x10c, 0x110, 0x114, 0x118, 0x11c, 0x120, 0x124, 0x128, 0x12c,
        0x130, 0x134, 0x138, 0x13c, 0x140, 0x144, 0x148, 0x14c, 0x150, 0x154, 0x158, 0x15c, 0x160, 0x164, 0x168, 0x16c, 0x170, 0x174,
        0x178, 0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x218, 0x21c, 0x220, 0x224, 0x228, 0x22c, 0x230, 0x234, 0x238, 0x23c, 0x240,
        0x244, 0x248, 0x24c, 0x250};
      static const uint16_t kTcaRegsAll[] = {0x00, 0x04, 0x08, 0x10, 0x14, 0x18, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x40, 0x44};
      for (auto o : kPhyRegsAll) { printf("dp: PHY +%#x = %08x\n", o, rd(p, o)); IODelay(50); }
      for (auto o : kTcaRegsAll) { printf("dp: TCA +%#x = %08x\n", o, rd(t, o)); IODelay(50); }
      return 0;
    }
    case 23: akita_dp_set_lanes(1); printf("dp: the CAL now drives 4 lanes (pin C layout)\n"); return 0;
    case 24: {
      volatile uint8_t *t = map(kTca, 0x1000);
      if (!t) return -1;
      const int f = gFlipped >= 0 ? gFlipped : flipped();
      auto w = [&](uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(t + o) = v; __asm__ volatile("dsb sy" ::: "memory"); };
      const uint32_t mode = 2;   // DP 4 lanes (stage 18 is USB31 + DP 2 lanes)
      w(0x18, rd(t, 0x34)); w(0x10, rd(t, 0x10) & ~3u); w(0x18, rd(t, 0x18) | (1u << 3)); IODelay(2);
      w(0x18, (rd(t, 0x18) & ~7u) | mode | (f > 0 ? 4u : 0u)); IODelay(2); w(0x18, rd(t, 0x18) & ~(1u << 3)); IODelay(1000);
      printf("dp: TCA system mode, connection %u: GEN_STATUS %#x PSTATE %#x DBG0 %#x\n", mode, rd(t, 0x34), rd(t, 0x30), rd(t, 0x28));
      return 0;
    }
    case 25: akita_dp_phy_forget(); printf("dp: the next 12 enables the combo PHY again\n"); return 0;
    case 26: case 27: case 28: {
      volatile uint8_t *h = map(kCmuHsi0, 0x4000);
      if (!h) return -1;
      auto w = [&](uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(h + o) = v; __asm__ volatile("dsb sy" ::: "memory"); };
      auto show = [&](const char *when) {
        const uint32_t c3 = rd(h, 0x10c);
        printf("dp: %s PLL_USB locktime %#x con3 %#x (enable %u stable %u mux_sel %u M %u P %u S %u) con8 %#x; mux USB20_REF(0x1004) %#x "
               "USB32DRD(0x1008) %#x RTCCLK(0x1010) %#x; div USB(0x1804) %#x USB32DRD(0x1808) %#x; USB20_USER %#x TCXO_USER %#x "
               "USB32DRD_USER %#x\n", when, rd(h, 0), c3, c3 >> 31, (c3 >> 29) & 1, (c3 >> 4) & 1, (c3 >> 16) & 0x3ff, (c3 >> 8) & 0x3f,
               c3 & 7, rd(h, 0x120), rd(h, 0x1004), rd(h, 0x1008), rd(h, 0x1010), rd(h, 0x1804), rd(h, 0x1808), rd(h, 0x640),
               rd(h, 0x630), rd(h, 0x650));
      };
      show("before:");
      if (stage == 26) return 0;
      if (stage == 28) {
        const uint32_t c = rd(h, 0x10c);
        if (!(c & (1u << 31)) || !(c & (1u << 29))) { printf("dp: PLL_USB not on and stable (%#x): MUX_SEL left alone\n", c); return -1; }
        w(0x10c, c | (1u << 4));
        unsigned t = 0;
        while ((rd(h, 0x10c) & (1u << 7)) && t < 1000) { IODelay(10); ++t; }
        printf("dp: PLL_USB output selected (MUX_SEL): con3 %#x after %u us\n", rd(h, 0x10c), t * 10);
        show("after:");
        return 0;
      }
      uint32_t c3 = rd(h, 0x10c);
      if (c3 & (1u << 31)) {
        printf("dp: PLL_USB already on, left alone\n");
      } else {
        w(0x0, 4 * 150);
        c3 = (c3 & ~((0x3ffu << 16) | (0x3fu << 8) | 7u)) | (200u << 16) | (4u << 8) | 1u | (1u << 4) | (1u << 31);
        w(0x10c, c3);
        unsigned t = 0;
        while (!(rd(h, 0x10c) & (1u << 29)) && t < 1000) { IODelay(10); ++t; }
        printf("dp: PLL_USB enabled: con3 %#x, %s after %u us\n", rd(h, 0x10c), (rd(h, 0x10c) & (1u << 29)) ? "stable" : "NOT stable", t * 10);
      }
      if ((rd(h, 0x1804) & 0x3f) != 31) {
        w(0x1804, (rd(h, 0x1804) & ~0x3fu) | 31u);
        IODelay(10);
      }
      show("after:");
      return 0;
    }
    case 29: case 30: {
      volatile uint8_t *c = map(0x11100000, 0x1000);
      if (!c) return -1;
      printf("dp: subctl VERSION %#x LINKCTRL %#x LINKPORT %#x LINK_CLKRST %#x UTMI_CTRL %#x ESS_CTRL %#x PTM %#x SOCBW %#x\n", rd(c, 0),
             rd(c, 4), rd(c, 8), rd(c, 0xc), rd(c, 0x10), rd(c, 0x14), rd(c, 0x18), rd(c, 0x1c));
      printf("dp: subctl HSP_MISC (0x114) %#x\n", rd(c, 0x114));
      if (stage == 29) return 0;
      const uint32_t v = (rd(c, 0x114) & ~(3u | (0xfu << 4))) | 1u;
      *reinterpret_cast<volatile uint32_t *>(c + 0x114) = v;
      __asm__ volatile("dsb sy" ::: "memory");
      IODelay(10);
      printf("dp: HSP_MISC now %#x (sel_res_tune_mux 1: the shared REXT to the combo PHY)\n", rd(c, 0x114));
      return 0;
    }
    case 31: {
      static const unsigned seq[] = {11, 25, 12, 18, 19, 13, 14, 15};
      for (unsigned st : seq) {
        printf("dp: === step %u ===\n", st);
        const int r = stageB(st);
        IOSleep(300);
        if (r && (st == 14 || st == 15)) printf("dp: step %u failed (%d)\n", st, r);
      }
      return 0;
    }
    case 32: {
      if (!gSetup) { printf("dp: run 31 first\n"); return -1; }
      volatile uint8_t *pin = map(kPinAlive, 0x1000), *l = map(kLink, 0x6000), *ph = map(kPhy, 0x1000);
      if (!pin || !l || !ph) return -1;
      auto w = [](volatile uint8_t *b, uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(b + o) = v; __asm__ volatile("dsb sy" ::: "memory"); };
      for (unsigned c = 0; c < 8; ++c) {
        const unsigned mux = c & 1, pn = (c >> 1) & 1, sw = (c >> 2) & 1;
        w(pin, kGpa7 + 4, (rd(pin, kGpa7 + 4) & ~(1u << 5)) | (mux << 5));
        w(l, 0x1000, (rd(l, 0x1000) & ~2u) | (pn << 1));
        w(ph, 0x200, (rd(ph, 0x200) & ~1u) | sw);
        IODelay(2000);
        uint8_t b[1] = {0};
        const int r = akita_dp_read_dpcd(0, 1, b);
        printf("dp: AUX try mux %u pn %u swap %u: %d (DPCD rev %#x)\n", mux, pn, sw, r, b[0]);
        if (r == 0) { printf("dp: AUX answers with mux %u pn %u swap %u\n", mux, pn, sw); return 0; }
      }
      printf("dp: no AUX orientation answered\n");
      return -1;
    }
    case 33: {
      volatile uint8_t *pin = map(kPinAlive, 0x1000);
      if (!pin) return -1;
      for (uint32_t o = 0xa0; o <= 0xe0; o += 0x20)
        printf("dp: pin bank +%#x: con %#x dat %#x pud %#x drv %#x\n", o, rd(pin, o), rd(pin, o + 4), rd(pin, o + 8), rd(pin, o + 12));
      return 0;
    }
    case 34: {
      volatile uint8_t *h = map(kCmuHsi0, 0x4000), *t = map(kCmuTop, 0x4000);
      if (!h || !t) return -1;
      printf("dp: DPOSC before: HSI0 USER mux con0 %#x con1 %#x; TOP gate %#x mux %#x div %#x\n", rd(h, 0x670), rd(h, 0x674),
             rd(t, 0x20b8), rd(t, 0x1090), rd(t, 0x1888));
      *reinterpret_cast<volatile uint32_t *>(h + 0x670) = rd(h, 0x670) | (1u << 4);
      __asm__ volatile("dsb sy" ::: "memory");
      unsigned n = 0;
      while ((rd(h, 0x670) & (1u << 16)) && n < 1000) { IODelay(10); ++n; }
      printf("dp: DPOSC now from CMU_TOP: con0 %#x (busy waited %u us)\n", rd(h, 0x670), n * 10);
      if (!gSetup) return 0;
      static const unsigned seq[] = {13, 14, 15};
      for (unsigned st : seq) { printf("dp: === step %u ===\n", st); stageB(st); IOSleep(300); }
      return 0;
    }
    case 40: {
      if (!gSetup || !gPhyUp || !gInitOk) { printf("dp: link not initialised (PHY %d, init %d): training refused\n", gPhyUp, gInitOk); return -1; }
      akita_tca_ignore_timeout = 1;
      uint8_t cap[16] = {};
      if (akita_dp_read_dpcd(0, 16, cap)) { printf("dp: training: DPCD caps not readable\n"); return -1; }
      const unsigned lanes = cap[2] & 0x1f;
      const bool enhanced = cap[2] & 0x80, tps3 = cap[2] & 0x40;
      unsigned interval = (cap[0x0e] & 0x7f) ? (cap[0x0e] & 0x7f) * 4000 : 400;   // us; 0: 400 us
      if (interval > 16000) interval = 16000;
      uint8_t v = 1;   // DPCD 0x600 SET_POWER: D0
      akita_dp_write_dpcd(0x600, 1, &v);
      IOSleep(2);
      static const struct { int cal; uint8_t bw; const char *name; } kRates[] = {{2, 0x14, "HBR2"}, {1, 0x0a, "HBR"}, {0, 0x06, "RBR"}};
      const unsigned first = cap[1] >= 0x14 ? 0 : cap[1] >= 0x0a ? 1 : 2;
      for (unsigned ri = first; ri < 3; ++ri) {
        printf("dp: training at %s x %u lanes (enhanced %d, TPS3 %d, interval %u us)\n", kRates[ri].name, lanes, enhanced, tps3, interval);
        v = 0; akita_dp_write_dpcd(0x102, 1, &v);   // training off while the PHY changes rate
        akita_dp_set_rate(kRates[ri].cal);
        akita_dp_set_num_lanes(lanes);
        akita_dp_set_enhanced(enhanced);
        const int rr = akita_dp_reinit();
        if (rr) { printf("dp: reinit at %s returned %d\n", kRates[ri].name, rr); continue; }
        stageB(44);
        uint8_t cfg[2] = {kRates[ri].bw, static_cast<uint8_t>(lanes | (enhanced ? 0x80 : 0))};
        akita_dp_write_dpcd(0x100, 2, cfg);
        v = gSsc ? 0x10 : 0; akita_dp_write_dpcd(0x107, 1, &v);   // DOWNSPREAD_CTRL: 0.5 % when SSC is on
        v = 1; akita_dp_write_dpcd(0x108, 1, &v);   // 8b/10b
        uint8_t swing[4] = {}, pre[4] = {};
        bool ok = false;
        for (int phase = 0; phase < 2; ++phase) {
          const int tps = phase == 0 ? 1 : (tps3 ? 3 : 2);
          akita_dp_training_pattern(tps);
          v = static_cast<uint8_t>(tps | 0x20);   // scrambling off while training
          akita_dp_write_dpcd(0x102, 1, &v);
          bool done = false;
          uint8_t lastReq[2] = {0xff, 0xff};
          unsigned same = 0;
          for (unsigned tries = 0; tries < (phase == 0 ? 10u : 6u); ++tries) {
            akita_dp_swing(swing, pre);
            uint8_t set[4];
            for (unsigned l = 0; l < lanes && l < 4; ++l)
              set[l] = static_cast<uint8_t>(swing[l] | (pre[l] << 3) | (swing[l] == 3 ? 4 : 0) | (pre[l] == 3 ? 0x20 : 0));
            akita_dp_write_dpcd(0x103, lanes, set);
            IODelay(phase == 0 ? 400 : interval);
            uint8_t st[6] = {};
            if (akita_dp_read_dpcd(0x202, 6, st)) { printf("dp: link status unreadable\n"); break; }
            unsigned crOk = 1, eqOk = 1;
            for (unsigned l = 0; l < lanes; ++l) {
              const unsigned nib = (st[l / 2] >> ((l & 1) * 4)) & 0xf;
              if (!(nib & 1)) crOk = 0;
              if ((nib & 7) != 7) eqOk = 0;
            }
            if (!(st[2] & 1)) eqOk = 0;
            printf("dp: %s try %u: swing %u/%u pre %u/%u -> status %02x %02x align %02x adjust %02x %02x\n", phase ? "EQ" : "CR", tries,
                   swing[0], swing[1], pre[0], pre[1], st[0], st[1], st[2], st[4], st[5]);
            if (phase == 0 ? crOk : (crOk && eqOk)) { done = true; break; }
            if (phase == 1 && !crOk) { printf("dp: clock recovery lost during EQ\n"); break; }
            if (st[4] == lastReq[0] && st[5] == lastReq[1] && ++same >= 5) break;
            if (st[4] != lastReq[0] || st[5] != lastReq[1]) same = 0;
            lastReq[0] = st[4]; lastReq[1] = st[5];
            for (unsigned l = 0; l < lanes && l < 4; ++l) {
              const unsigned adj = (st[4 + l / 2] >> ((l & 1) * 4)) & 0xf;
              swing[l] = static_cast<uint8_t>(adj & 3);
              pre[l] = static_cast<uint8_t>((adj >> 2) & 3);
            }
          }
          if (!done) { printf("dp: %s phase failed at %s\n", phase ? "EQ" : "CR", kRates[ri].name); break; }
          printf("dp: %s done at %s\n", phase ? "EQ" : "CR", kRates[ri].name);
          if (phase == 1) ok = true;
        }
        akita_dp_training_pattern(0);
        v = ok ? 0 : 0;   // training off; scrambling on for normal data
        akita_dp_write_dpcd(0x102, 1, &v);
        if (ok) { printf("dp: LINK TRAINED at %s x %u lanes\n", kRates[ri].name, lanes); return 0; }
      }
      printf("dp: link training failed at every rate\n");
      return -1;
    }
    case 41: {
      akita_dp_timing_1080p60();
      const int r = akita_dp_bist_on();
      printf("dp: BIST colour bars 1080p60: %d\n", r);
      stageBlock(2, false);
      return r;
    }
    case 42: akita_dp_video_stop(); printf("dp: video stopped\n"); return 0;
    case 45: {
      if (gRan45) { printf("dp: 45 already ran in this boot (a second PHY enable fails and the next training hangs): use 40/41, or reboot\n"); return -1; }
      gRan45 = true;
      static const unsigned seq[] = {11, 30, 25, 12, 18, 19, 1034, 13, 14, 40, 41};
      for (unsigned st : seq) {
        printf("dp: === step %u ===\n", st);
        int r;
        if (st == 1034) {   // DPOSC to 40 MHz without 34's own 13/14/15
          volatile uint8_t *h = map(kCmuHsi0, 0x4000);
          if (!h) return -1;
          *reinterpret_cast<volatile uint32_t *>(h + 0x670) = rd(h, 0x670) | (1u << 4);
          __asm__ volatile("dsb sy" ::: "memory");
          unsigned n = 0;
          while ((rd(h, 0x670) & (1u << 16)) && n < 1000) { IODelay(10); ++n; }
          printf("dp: DPOSC 40 MHz (con0 %#x)\n", rd(h, 0x670));
          r = 0;
        } else {
          r = stageB(st);
        }
        IOSleep(200);
        if (r && (st == 12 || st == 13 || st == 14 || st == 40)) { printf("dp: step %u failed (%d), stopping\n", st, r); return r; }
      }
      return 0;
    }
    case 44: {
      volatile uint8_t *p = map(kPhy, 0x1000), *t = map(kTca, 0x1000);
      if (!p || !t) return -1;
      printf("dp: DP_CONFIG11 %#x CONFIG12 %#x CONFIG13 %#x CONFIG17 %#x; TCA PSTATE %#x GEN_STATUS %#x; req mask %#x, SSC %d\n",
             rd(p, 0x230), rd(p, 0x234), rd(p, 0x238), rd(p, 0x248), rd(t, 0x30), rd(t, 0x34), akita_dp_req_mask, gSsc);
      return 0;
    }
    case 47: gSsc = true; akita_dp_set_ssc(1); printf("dp: SSC on for the next 40\n"); return 0;
    case 48: akita_dp_req_mask = 0x3; printf("dp: DP_CONFIG12 requests lanes 0-1 only\n"); return 0;
    case 49: akita_dp_req_mask = 0xF; printf("dp: DP_CONFIG12 requests all four lanes\n"); return 0;
    case 60: {
      volatile uint8_t *h = map(kCmuHsi0, 0x4000), *t = map(kCmuTop, 0x4000);
      if (!h || !t) return -1;
      auto w = [&](uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(h + o) = v; __asm__ volatile("dsb sy" ::: "memory"); };
      auto busy = [&](uint32_t o) { unsigned n = 0; while ((rd(h, o) & (1u << 16)) && n < 1000) { IODelay(10); ++n; } return n * 10; };
      printf("dp: PHY ref before: USB32DRD_USER %#x mux USB32DRD %#x div %#x; TOP USB32DRD gate %#x mux %#x div %#x\n", rd(h, 0x650),
             rd(h, 0x1008), rd(h, 0x1808), rd(t, 0x20c4), rd(t, 0x109c), rd(t, 0x1894));
      w(0x650, rd(h, 0x650) | (1u << 4));
      const unsigned b1 = busy(0x650);
      w(0x1008, (rd(h, 0x1008) & ~3u) | 2u);
      const unsigned b2 = busy(0x1008);
      printf("dp: PHY ref now 40 MHz path: USB32DRD_USER %#x (%u us) mux USB32DRD %#x (%u us)\n", rd(h, 0x650), b1, rd(h, 0x1008), b2);
      return 0;
    }
    case 61: akita_dp_keep_acbyp = 1; printf("dp: AC-capacitor bypass kept at HBRx\n"); return 0;
    case 62: {
      volatile uint8_t *m0 = map(kDecon0, 0x6000), *m1 = map(kDecon1, 0x1000), *win = map(0x19480000, 0xE000),
                       *wc0 = map(0x194A0000, 0xE000), *wc1 = map(0x194B0000, 0xE000), *sub = map(0x19490000, 0x10000),
                       *l0 = map(0x19900000, 0x1000);
      if (!m0 || !m1 || !win || !wc0 || !wc1 || !sub || !l0) { printf("dp: survey: not mapped\n"); return -1; }
      for (unsigned d = 0; d < 2; ++d) {
        volatile uint8_t *m = d ? m1 : m0;
        printf("dp: DECON%u GLOBAL_CON %#x TRIG_CON %#x DATA_PATH_CON_0 %#x BLD_BG_SIZE %#x OF_SIZE_0 %#x INT_EN %#x FRAME_COUNT %u\n", d,
               rd(m, 0x20), rd(m, 0x30), rd(m, 0x200), rd(m, 0x220), rd(m, 0x290), rd(m, 0x60), rd(m, 0x4));
        IODelay(100);
      }
      printf("dp: RSC_STATUS 0..6: %08x %08x %08x %08x %08x %08x %08x\n", rd(m0, 0x100), rd(m0, 0x104), rd(m0, 0x108), rd(m0, 0x10c),
             rd(m0, 0x110), rd(m0, 0x114), rd(m0, 0x118));
      printf("dp: RSC_STATUS 7..11 (SRAM): %08x %08x %08x %08x %08x; CLOCK_CON %#x\n", rd(m0, 0x11c), rd(m0, 0x120), rd(m0, 0x124),
             rd(m0, 0x128), rd(m0, 0x12c), rd(m0, 0x5000));
      for (unsigned w = 0; w < 14; ++w) {
        const uint32_t c0 = rd(wc0, 0x1000 * w), c1 = rd(wc1, 0x1000 * w);
        if ((c0 | c1) & 1) printf("dp: WIN%u: DECON0 con %#x DECON1 con %#x FUNC_CON_0 %#x start %#x end %#x\n", w, c0, c1,
                                  rd(win, 0x1000 * w + 4), rd(win, 0x1000 * w + 0xc), rd(win, 0x1000 * w + 0x10));
        IODelay(50);
      }
      printf("dp: DSIMIF_SEL0 %#x SEL1 %#x DPIF_SEL0 %#x SEL1 %#x\n", rd(sub, 0x8000), rd(sub, 0x9000), rd(sub, 0xC000), rd(sub, 0xD000));
      printf("dp: DPP L0 DMA: ENABLE %#x IN_CTRL_0 %#x SRC_W %#x SRC_H %#x SRC_OFF %#x IMG_SIZE %#x BASE %#x STRIDE_0 %#x; shadow BASE %#x\n",
             rd(l0, 0), rd(l0, 8), rd(l0, 0x10), rd(l0, 0x14), rd(l0, 0x18), rd(l0, 0x1c), rd(l0, 0x40), rd(l0, 0x50), rd(l0, 0x440));
      return 0;
    }
    case 63: {
      if (!gInitOk) { printf("dp: run 45 first\n"); return -1; }
      volatile uint8_t *m1 = map(kDecon1, 0x5000), *win = map(0x19480000, 0xE000), *sub = map(0x19490000, 0x10000),
                       *wc1 = map(0x194B0000, 0xE000);
      if (!m1 || !win || !sub || !wc1) { printf("dp: DECON1 not mapped\n"); return -1; }
      akita_dp_video_stop();
      if (rd(m1, 0x20) & 3) { const int sr = akita_decon1_stop(); printf("dp: DECON1 was running: stop %d\n", sr); }   // 309: stuck after 1 frame
      akita_decon1_map(const_cast<uint8_t *>(m1), const_cast<uint8_t *>(win), const_cast<uint8_t *>(sub), const_cast<uint8_t *>(wc1));
      const int r = akita_decon1_colormap(1920, 1080, 0, 0x0000FF00);
      akita_dp_video_on();
      IOSleep(100);
      printf("dp: D1a: decon %d; DECON1 GLOBAL_CON %#x DATA_PATH %#x FRAME_COUNT %u; DPIF_SEL0 %#x; DECON0 FRAME_COUNT %u\n", r,
             rd(m1, 0x20), rd(m1, 0x200), rd(m1, 0x4), rd(sub, 0xC000), rd(map(kDecon0, 0x1000), 0x4));
      stageBlock(2, false);
      return r;
    }
    case 64: { const int r = akita_decon1_stop(); printf("dp: DECON1 stop %d\n", r); return r; }
    case 65: case 66: case 67: {
      if (!gInitOk) { printf("dp: run 45 and 63 first\n"); return -1; }
      volatile uint8_t *dma = map(0x19901000, 0x1000), *dpp = map(0x19931000, 0x1000), *sramc = map(0x19951000, 0x1000),
                       *coef = map(0x19940000, 0x4000), *hdr = map(0x19961000, 0x1000), *m1 = map(kDecon1, 0x1000);
      if (!dma || !dpp || !sramc || !coef || !hdr || !m1) { printf("dp: DPP L1 not mapped\n"); return -1; }
      akita_dpp1_map(const_cast<uint8_t *>(dma), const_cast<uint8_t *>(dpp), const_cast<uint8_t *>(sramc),
                     const_cast<uint8_t *>(coef), const_cast<uint8_t *>(hdr));
      int r;
      if (stage == 65) {
        r = akita_dpp1_show(0xfac00000ull, 1080, 2400, 0, 660, 1080, 1080, 0, 420, 0);
      } else if (stage == 67) {
        r = akita_dpp1_show(kMonitorFb, 1920, 1080, 0, 0, 1920, 1080, 0, 0, 0);
        if (!r) {   // the display driver may flip between kMonitorFb and kMonitorFb2 from now on
          gFlipDma = dma;
          gFlipDecon = m1;
          __asm__ volatile("dmb sy" ::: "memory");
          gFlipReady = true;
        }
      } else {
        constexpr uint64_t kFb = kMonitorFb;
        constexpr unsigned W = 1920, H = 1080;
        volatile uint32_t *fb = reinterpret_cast<volatile uint32_t *>(ml_io_map_wcomb(kFb, W * H * 4));
        if (!fb) { printf("dp: framebuffer not mapped\n"); return -1; }
        static const uint32_t kBars[8] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000};
        for (unsigned y = 0; y < H; ++y) {
          for (unsigned x = 0; x < W; ++x) {
            uint32_t c;
            if (x < 8 || y < 8 || x >= W - 8 || y >= H - 8) c = 0xffffff;
            else if (y < H * 2 / 3) c = kBars[x * 8 / W];
            else { const uint32_t g = x * 255 / (W - 1); c = (g << 16) | (g << 8) | g; }
            fb[y * W + x] = 0xff000000u | c;
          }
        }
        __asm__ volatile("dsb sy" ::: "memory");
        r = akita_dpp1_show(kFb, W, H, 0, 0, W, H, 0, 0, 0);
      }
      IOSleep(100);
      printf("dp: D1b stage %u: show %d; DECON1 GLOBAL_CON %#x FRAME_COUNT %u; DPP1 IRQ %#x\n", stage, r, rd(m1, 0x20), rd(m1, 0x4),
             akita_dpp1_irq());
      return r;
    }
    case 68: teardown("stage 68"); gAutoState = 0; return 0;
    case 14: {
      if (!gSetup) { printf("dp: run 12 and 13 first\n"); return -1; }
      uint8_t b[16] = {};
      const int r = akita_dp_read_dpcd(0, 16, b);
      printf("dp: DPCD read %d: DPCD rev %u.%u, max link rate %#x, max lanes %u\n", r, b[0] >> 4, b[0] & 15, b[1], b[2] & 0x1f);
      hex("DPCD", b, 16);
      return r;
    }
    case 15: {
      if (!gSetup) { printf("dp: run 12 and 13 first\n"); return -1; }
      uint8_t e[128] = {};
      const int r = akita_dp_read_edid(e);
      printf("dp: EDID read %d: header %s, maker %c%c%c product %04x\n", r,
             (e[0] == 0 && e[1] == 0xff && e[7] == 0) ? "ok" : "BAD", '@' + ((e[8] >> 2) & 31), '@' + (((e[8] & 3) << 3) | (e[9] >> 5)),
             '@' + (e[9] & 31), e[10] | (e[11] << 8));
      for (unsigned d = 54; d < 126; d += 18)
        if (e[d] == 0 && e[d + 1] == 0 && e[d + 3] == 0xfc) {
          char name[14] = {}; for (unsigned i = 0; i < 13 && e[d + 5 + i] != 0x0a; ++i) name[i] = static_cast<char>(e[d + 5 + i]);
          printf("dp: monitor name: %s\n", name);
        }
      if (e[54] || e[55]) {
        const unsigned clk = (e[54] | (e[55] << 8)) * 10, ha = e[56] | ((e[58] & 0xf0) << 4), va = e[59] | ((e[61] & 0xf0) << 4);
        printf("dp: preferred mode %ux%u, pixel clock %u kHz\n", ha, va, clk);
      }
      hex("EDID", e, 128);
      return r;
    }
  }
  printf("dp: no stage %u\n", stage);
  return -1;
}

}  // namespace dp

// akita: called by NativeGOPDisplay (monitor mode). -1: the DisplayPort is not up yet (the driver keeps writing buffer A).
extern "C" int akita_dp_flip(uint64_t addr) {
  if (!dp::gFlipReady || (addr != dp::kMonitorFb && addr != dp::kMonitorFb2)) return -1;
  *reinterpret_cast<volatile uint32_t *>(dp::gFlipDma + 0x40) = static_cast<uint32_t>(addr);   // RDMA_BASEADDR_P0
  __asm__ volatile("dsb sy" ::: "memory");
  volatile uint32_t *req = reinterpret_cast<volatile uint32_t *>(dp::gFlipDecon + 0x50);       // SHD_REG_UP_REQ
  *req = *req | dp::kFlipMask;
  __asm__ volatile("dsb sy" ::: "memory");
  return 0;
}
extern "C" int akita_dp_ready(void) { return dp::gFlipReady ? 1 : 0; }
// 1 once the last flip has been taken by the hardware (or when the DisplayPort is not up), 0 while it is pending
extern "C" int akita_dp_flip_latched(void) {
  if (!dp::gFlipReady) return 1;
  return !(*reinterpret_cast<volatile uint32_t *>(dp::gFlipDecon + 0x50) & dp::kFlipMask);
}

namespace dp {
thread_call_t gAutoCall = nullptr;
unsigned gAutoWaits = 0, gAutoState = 0, gAutoFails = 0;   // state 0: waiting for a monitor, 1: up

// hot-plug (fullos315): everything 45/63/67 turned on, off again, so the next monitor starts from scratch
void teardown(const char *why) {
  printf("dp: teardown (%s)\n", why);
  gFlipReady = false;   // NativeGOPDisplay goes back to writing buffer A in place
  __asm__ volatile("dmb sy" ::: "memory");
  if (gInitOk) { akita_dp_video_stop(); IODelay(20000); }
  if (gFlipDecon) { const int r = akita_decon1_stop(); printf("dp: teardown: DECON1 stop %d\n", r); }
  if (gPhyUp || gInitOk) akita_dp_hw_deinit();   // link off, combo PHY disabled
  volatile uint8_t *t = map(kTca, 0x1000);
  if (t) {   // the crossbar back to USB31 only (system configuration mode, as 18 set it)
    auto w = [&](uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(t + o) = v; __asm__ volatile("dsb sy" ::: "memory"); };
    w(0x18, rd(t, 0x18) | (1u << 3)); IODelay(2);
    w(0x18, (rd(t, 0x18) & ~7u) | 1u); IODelay(2);
    w(0x18, rd(t, 0x18) & ~(1u << 3));
  }
  const uint8_t off = 0;
  i2c::writeRegs(kTcpc, kSbuSw, &off, 1);   // SBU away from the AUX
  gPhyUp = gInitOk = gRan45 = gSetup = false;
  gFlipDma = gFlipDecon = nullptr;
  printf("dp: teardown done\n");
}

void autoTick(thread_call_param_t, thread_call_param_t) {
  const bool present = typec::gS.dpConfigured && typec::gS.hpd;
  uint64_t deadline;
  if (gAutoState == 1) {   // up: watch for the monitor going away
    if (!present) { teardown("the dongle or the monitor went away"); gAutoState = 0; gAutoWaits = 0; }
    clock_interval_to_deadline(500, NSEC_PER_MSEC, &deadline);
    thread_call_enter_delayed(gAutoCall, deadline);
    return;
  }
  if (!present || gAutoFails >= 3) {   // waiting (after 3 failed bring-ups: wait for an unplug before trying again)
    if (!present) gAutoFails = 0;
    if ((gAutoWaits++ % 120) == 0) printf("dp: auto: waiting for the DisplayPort mode and HPD (configured %d, HPD %d, fails %u)\n",
                                          typec::gS.dpConfigured, typec::gS.hpd, gAutoFails);
    clock_interval_to_deadline(500, NSEC_PER_MSEC, &deadline);
    thread_call_enter_delayed(gAutoCall, deadline);
    return;
  }
  IOSleep(1500);   // let the hub's DP->HDMI converter settle after the PD configuration
  printf("dp: auto: monitor detected, bringing up the link\n");
  int r = stageB(45);
  if (!r) r = stageB(63);
  if (!r) r = stageB(67);
  printf("dp: auto: %s (%d)%s\n", r ? "FAILED" : "the monitor shows the framebuffer", r,
         r ? "" : " - DisplayPort on the Pixel 8a, port by @1421rubano");
  if (r) { teardown("bring-up failed"); ++gAutoFails; } else { gAutoState = 1; gAutoFails = 0; }
  clock_interval_to_deadline(r ? 3 : 1, NSEC_PER_SEC, &deadline);
  thread_call_enter_delayed(gAutoCall, deadline);
}

void boot() {
  unsigned on = 0;
  PE_parse_boot_argn("akita-dp", &on, sizeof on);
  if (!on || gAutoCall) return;
  gAutoCall = thread_call_allocate(autoTick, nullptr);
  if (!gAutoCall) return;
  uint64_t deadline;
  clock_interval_to_deadline(10, NSEC_PER_SEC, &deadline);
  thread_call_enter_delayed(gAutoCall, deadline);
  printf("dp: auto bring-up armed (akita-dp=1)\n");
}

int command(unsigned n) {
  const bool force = n >= 50 && n < 60;   // fullos305: stages 50/51 landed here by mistake; step-B knobs live at 60+
  const unsigned stage = force ? n - 50 : n;
  printf("dp: stage %u%s\n", stage, force ? " (forced)" : "");
  if ((n >= 10 && n < 50) || (n >= 60 && n < 100)) return stageB(n);   // 50..59 are the forced reads of stages 1..6 (+ 50)
  return stage == 1 ? stage1() : stageBlock(stage, force);
}

}  // namespace dp
