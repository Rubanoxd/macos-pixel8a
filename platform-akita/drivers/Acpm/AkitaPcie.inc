// SPDX-License-Identifier: BSD-2-Clause-Patent
// akita (Generated-by: LLM): staged look at the Wi-Fi's PCIe link (channel 1, pcie@13120000, Broadcom over one Gen2 lane).
// Included by AkitaAcpm.cpp; each stage is its own sysctl (kern.akita_acpm = 4000000 + stage) so the log flushes in between.
//   1  read only: the PMU's PCIe PHY control word (SMC, as Linux's rmw_priv_reg; the PMU hangs on direct reads) and the gph3 bank
//      (WLAN_EN gph3-4, PERST gph3-0, CLKREQ gph3-1) in pinctrl@13040000, inside the HSI2 block the UFS already uses.
//   2  read only: the first words of the controller's ELBI (0x13120000) and SOC (0x13130000) windows. If the block's clocks are
//      gated this read takes an SError and the phone panics (it comes back to fastboot by itself).
// Sources: Google's pcie-exynos-rc.c / pcie-exynos-zuma-rc-cal.c (android-gs-akita-5.15-android14-qpr3), the zuma device tree.
namespace pcie {
constexpr uint64_t kPmuPhyCtrl = 0x15460000 + 0x3ec8;   // pcie_1 pmu-offset
constexpr uint64_t kGph3 = 0x13040000;                   // pinctrl_9, first bank
constexpr uint64_t kElbi = 0x13120000, kSoc = 0x13130000;

uint64_t smcPmuRead(uint64_t phys) {
  register uint64_t x0 asm("x0") = 0x82000504;
  register uint64_t x1 asm("x1") = phys;
  register uint64_t x2 asm("x2") = 0;
  register uint64_t x3 asm("x3") = 0;
  asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
               "x15", "x16", "x17", "memory");
  return x0;
}

uint32_t ioRead(uint64_t phys, uint32_t offset) {
  volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(ml_io_map(phys & ~uint64_t(0x3fff), 0x4000));
  if (!p) return 0xdeadbeef;
  return p[((phys & 0x3fff) + offset) / 4];
}


// Mapped once: ml_io_map per access would leak mappings.
struct Win { uint64_t phys; uint32_t size; volatile uint8_t *va; };
Win gWin[] = {{0x13120000, 0x2000, nullptr},    // 0 elbi
              {0x13130000, 0x5000, nullptr},    // 1 soc
              {0x13150000, 0xd000, nullptr},    // 2 udbg
              {0x13160000, 0x3000, nullptr},    // 3 phy
              {0x13140000, 0x5000, nullptr},    // 4 pcs
              {0x13400000, 0x1000, nullptr},    // 5 dbi (own config space)
              {0x13040000, 0x1000, nullptr}};   // 6 pinctrl_9 (gph3)
enum { kE, kS, kU, kP, kC, kD, kG };
bool mapAll() {
  for (Win &w : gWin) if (!w.va && !(w.va = reinterpret_cast<volatile uint8_t *>(ml_io_map(w.phys, w.size)))) return false;
  return true;
}
inline uint32_t rd(unsigned w, uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(gWin[w].va + off); }
inline void wr(unsigned w, uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(gWin[w].va + off) = v; __asm__ volatile("dsb sy" ::: "memory"); }
uint64_t smcPmuWrite(uint64_t phys, uint32_t value) {   // TENSOR_SMC_PMU_SEC_REG, TENSOR_PMUREG_WRITE (Linux exynos-pmu.c, gs101)
  register uint64_t x0 asm("x0") = 0x82000504;
  register uint64_t x1 asm("x1") = phys;
  register uint64_t x2 asm("x2") = 1;
  register uint64_t x3 asm("x3") = value;
  asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
               "x15", "x16", "x17", "memory");
  return x0;
}
unsigned pollBit(unsigned w, uint32_t off, unsigned bit) {   // check_exynos_pcie_reg_status: 1000 x 10 us
  for (unsigned i = 0; i < 1000; ++i) { IODelay(10); if (rd(w, off) & (1u << bit)) return i + 1; }
  return 0;
}
// gph3 pin n: function nibble in CON, level in DAT.
void pinFunc(unsigned n, unsigned fn) { wr(kG, 0, (rd(kG, 0) & ~(0xfu << (4 * n))) | (fn << (4 * n))); }
void pinLevel(unsigned n, unsigned level) { wr(kG, 4, (rd(kG, 4) & ~(1u << n)) | (level << n)); }

// Stage 3: power the BCM4383 and train the link (Google's exynos_pcie_rc_poweron for channel 1 with an EP_BCM_WIFI endpoint:
// phy isolation off, phy_all_pwrdn_clear, pcie_phy_config, establish_link). Writes only to the PCIe channel 1 blocks, its gph3
// pins and its PMU PHY control word. Reports PLL/CDR/OC locks and the LTSSM state.
int bringUp() {
  if (!mapAll()) { printf("pcie: stage 3: cannot map the windows\n"); return -1; }
  // pins: PERST (gph3-0) output low, CLKREQ (gph3-1) function 2, WLAN_EN (gph3-4) output high, DEV_WAKE (gph3-5) output low
  pinLevel(0, 0); pinFunc(0, 1);
  pinFunc(1, 2);
  pinLevel(5, 0); pinFunc(5, 1);
  pinLevel(4, 0); pinFunc(4, 1);
  IOSleep(10);
  pinLevel(4, 1);
  printf("pcie: stage 3: WLAN_EN high (gph3 con %#x dat %#x); waiting 200 ms for the chip\n", rd(kG, 0), rd(kG, 4));
  IOSleep(200);
  // PHY isolation off (PCIE_PHY_BYPASS, bit 0 of the PMU word)
  uint32_t pmu = static_cast<uint32_t>(smcPmuRead(kPmuPhyCtrl));
  uint64_t r = smcPmuWrite(kPmuPhyCtrl, pmu | 1);
  printf("pcie: stage 3: PMU %#x -> %#x (smc %#llx, read back %#llx)\n", pmu, pmu | 1, (unsigned long long)r, (unsigned long long)smcPmuRead(kPmuPhyCtrl));
  wr(kS, 0x4000, 0x15);
  // phy_all_pwrdn_clear, channel 1
  wr(kU, 0xC700, rd(kU, 0xC700) | 1); IODelay(100);
  const uint32_t clr[][2] = {{0x000C, 0}, {0x0928, 0x55}, {0x0580, 0x02}, {0x0208, 0}, {0x1044, 0}, {0x1048, 0}, {0x104C, 0}, {0x1050, 0}, {0x185C, 0}};
  for (auto &c : clr) wr(kP, c[0], c[1]);
  IODelay(10);
  // pcie_phy_config, channel 1
  wr(kP, 0x032C, 0x3);
  wr(kU, 0xC700, rd(kU, 0xC700) | 1);
  wr(kU, 0xC700, rd(kU, 0xC700) & ~2u);
  const unsigned extPll = pollBit(kU, 0xC734, 2);
  wr(kS, 0x4004, rd(kS, 0x4004) & ~(1u << 2));
  wr(kS, 0x4004, rd(kS, 0x4004) | (3u << 4));
  wr(kE, 0x0080, 0x4);
  wr(kE, 0x3A4, 0xF); wr(kE, 0x3A4, 0xD); IODelay(10); wr(kE, 0x3A4, 0xF); IODelay(10);
  wr(kE, 0x1404, 1); wr(kE, 0x1408, 1); wr(kE, 0x1400, 1); wr(kE, 0x1404, 0); wr(kE, 0x1408, 0); wr(kE, 0x1400, 0);
  wr(kE, 0x03D8, 1);
  const uint32_t pma[][2] = {
    {0x0000, 0x88}, {0x001C, 0x66}, {0x01F4, 0x00}, {0x0514, 0x59}, {0x051C, 0x11}, {0x062C, 0x0E}, {0x0644, 0x22}, {0x0688, 0x03},
    {0x06D4, 0x28}, {0x0788, 0x64}, {0x078C, 0x64}, {0x0790, 0x50}, {0x0794, 0x50}, {0x0944, 0x05}, {0x0948, 0x05}, {0x094C, 0x05},
    {0x0950, 0x05}, {0x0590, 0x02}, {0x07F8, 0xB0}, {0x0730, 0x08}, {0x0344, 0xC0}, {0x0040, 0x04}, {0x0204, 0x03}, {0x02D4, 0x1F},
    {0x0358, 0x10}, {0x0018, 0x01}, {0x0514, 0x5B}, {0x0608, 0x0C}, {0x060C, 0x0F}, {0x0610, 0x0F}, {0x0614, 0x0F}, {0x0618, 0x0F},
    {0x0510, 0x80}, {0x0688, 0x03}, {0x0644, 0x23}, {0x0624, 0x11}, {0x0630, 0x0f}, {0x06D0, 0x53},
    // lane 0
    {0x1140, 0x04}, {0x1144, 0x04}, {0x1148, 0x04}, {0x114C, 0x02}, {0x1150, 0x00}, {0x1154, 0x00}, {0x1158, 0x00}, {0x115C, 0x00},
    {0x12CC, 0x1C}, {0x12DC, 0x6C}, {0x130C, 0x29}, {0x13B4, 0x2F}, {0x1A64, 0x05}, {0x1A68, 0x05}, {0x1A84, 0x05}, {0x1A88, 0x05},
    {0x1A98, 0x00}, {0x1A9C, 0x00}, {0x1AA8, 0x07}, {0x1AB8, 0x00}, {0x1ABC, 0x00}, {0x1AF8, 0x90}, {0x1B34, 0x03}, {0x1BB0, 0x03},
    {0x1BB4, 0x03}, {0x1BC0, 0x06}, {0x1BC4, 0x06}, {0x1BE8, 0x01}, {0x1BF8, 0x04}, {0x1C98, 0x00}, {0x1CA4, 0x81},
    // EP_BCM_WIFI: L1SS exit link-down workaround
    {0x1B20, 0x02}, {0x1340, 0x23}, {0x002C, 0x3C}};
  for (auto &c : pma) wr(kP, c[0], c[1]);
  wr(kP, 0x1350, rd(kP, 0x1350) | 1);
  const uint32_t pcs[][2] = {{0x190, 0x100B0604}, {0x154, 0x000700D5}, {0x100, 0x16400000}, {0x104, 0x08600000}, {0x114, 0x18500000},
                             {0x124, 0x60700000}, {0x174, 0x00000007}, {0x178, 0x00000100}, {0x17c, 0x00000700}, {0x110, 0x01600202}};
  for (auto &c : pcs) wr(kC, c[0], c[1]);
  wr(kE, 0x1404, 1); IODelay(10); wr(kE, 0x1408, 1); wr(kE, 0x1400, 1);
  const unsigned pll = pollBit(kP, 0x0A80, 0), cdr = pollBit(kP, 0x15C0, 4), oc = pollBit(kP, 0x140C, 7);
  wr(kP, 0x032C, 0x0);
  wr(kE, 0x1078, 1);
  printf("pcie: stage 3: ext PLL lock %u, PLL lock %u, CDR lock %u, offset cal %u (polls of 10 us, 0 = never)\n", extPll, pll, cdr, oc);
  // establish_link
  pmu = static_cast<uint32_t>(smcPmuRead(kPmuPhyCtrl));
  smcPmuWrite(kPmuPhyCtrl, pmu | 0x400);
  wr(kU, 0xC800, 0x421);
  wr(kP, 0x32C, 0x0);
  *reinterpret_cast<volatile uint32_t *>(gWin[kD].va + 0x890) = 0x12000;   // EQ off (GEN3_RELATED)
  pinLevel(0, 1);   // PERST high
  IOSleep(20);
  wr(kE, 0x3BC, rd(kE, 0x3BC) | 0x1 | (1u << 4));
  wr(kE, 0x3A0, 1u << 1);
  wr(kE, 0x0074, 1);
  wr(kE, 0x3A8, rd(kE, 0x3A8) & ~0xFFFu);
  wr(kE, 0x474, 1);
  wr(kE, 0x1078, 1);
  wr(kE, 0xC00, rd(kE, 0xC00) & ~(1u << 2));
  wr(kE, 0xC04, 0xffffffff); wr(kE, 0xC08, 0xffffffff);
  wr(kE, 0xC00, rd(kE, 0xC00) | 0x3);
  wr(kE, 0x054, 1);   // LTSSM enable
  unsigned n = 0, state = 0;
  for (; n < 4800; ++n) { state = rd(kE, 0x2C8) & 0x3f; if (state == 0x11) break; IODelay(10); }
  printf("pcie: stage 3: LTSSM state %#x after %u polls (0x11 = L0, link up)\n", state, n);
  if (state == 0x11) {
    const uint32_t id = *reinterpret_cast<volatile uint32_t *>(gWin[kD].va + 0x0);
    printf("pcie: stage 3: root port id %#x; link status %#x\n", id, *reinterpret_cast<volatile uint32_t *>(gWin[kD].va + 0x80));
  } else {
    uint32_t h[8];
    for (unsigned i = 0; i < 8; ++i) h[i] = rd(kE, 0xC40 + i * 4);
    printf("pcie: stage 3: no link; ELBI 0x2c8 %#x, history %#x %#x %#x %#x %#x %#x %#x %#x\n", rd(kE, 0x2C8), h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
  }
  return state == 0x11 ? 0 : 1;
}

// Stage 4: the endpoint's config space through iATU outbound region 0 (CFG0, bus 1) at the DT's "config" window 0x60ffe000.
// DesignWare unrolled iATU at DBI + 0x300000 (region 0 outbound: ctrl1 0x00, ctrl2 0x04, base 0x08/0x0c, limit 0x10, target 0x14/0x18).
int readEndpoint() {
  if (!mapAll()) return -1;
  if ((rd(kE, 0x2C8) & 0x3f) != 0x11) { printf("pcie: stage 4: no link (LTSSM %#x): run stage 3 first\n", rd(kE, 0x2C8) & 0x3f); return 1; }
  static volatile uint8_t *atu, *cfg;
  if (!atu) atu = reinterpret_cast<volatile uint8_t *>(ml_io_map(0x13400000 + 0x300000, 0x1000));
  if (!cfg) cfg = reinterpret_cast<volatile uint8_t *>(ml_io_map(0x60ffe000, 0x2000));
  if (!atu || !cfg) return -1;
  auto dbi = [](uint32_t off) -> volatile uint32_t & { return *reinterpret_cast<volatile uint32_t *>(gWin[kD].va + off); };
  auto at = [](uint32_t off) -> volatile uint32_t & { return *reinterpret_cast<volatile uint32_t *>(atu + off); };
  dbi(0x18) = 0x00010100;               // primary 0, secondary 1, subordinate 1
  dbi(0x04) = dbi(0x04) | 0x6;          // memory space, bus master
  at(0x08) = 0x60ffe000; at(0x0c) = 0; at(0x10) = 0x60ffefff;
  at(0x14) = 1u << 24; at(0x18) = 0;    // bus 1, device 0, function 0
  at(0x00) = 4;                         // CFG0
  at(0x04) = 0x80000000u;               // enable
  __asm__ volatile("dsb sy" ::: "memory");
  unsigned n = 0;
  while (!(at(0x04) & 0x80000000u) && n++ < 100) IODelay(10);
  uint32_t c[16];
  for (unsigned i = 0; i < 16; ++i) c[i] = *reinterpret_cast<volatile uint32_t *>(cfg + i * 4);
  printf("pcie: stage 4: endpoint vendor %#x device %#x, class %#x rev %#x, BAR0 %#x BAR1 %#x BAR2 %#x, subsystem %#x\n", c[0] & 0xffff, c[0] >> 16,
         c[2] >> 8, c[2] & 0xff, c[4], c[5], c[6], c[11]);
  return (c[0] & 0xffff) == 0x14e4 ? 0 : 1;
}

int command(unsigned stage) {
  if (stage == 1) {
    const uint64_t phy = smcPmuRead(kPmuPhyCtrl);
    printf("pcie: stage 1: PMU PHY control (%#llx) = %#llx\n", static_cast<unsigned long long>(kPmuPhyCtrl), static_cast<unsigned long long>(phy));
    const uint32_t con = ioRead(kGph3, 0), dat = ioRead(kGph3, 4), pud = ioRead(kGph3, 8), drv = ioRead(kGph3, 0xc);
    printf("pcie: stage 1: gph3 con %#x dat %#x pud %#x drv %#x; WLAN_EN (gph3-4) fn %u level %u, PERST (gph3-0) fn %u level %u, CLKREQ (gph3-1) fn %u\n",
           con, dat, pud, drv, (con >> 16) & 0xf, (dat >> 4) & 1, con & 0xf, dat & 1, (con >> 4) & 0xf);
    return 0;
  }
  if (stage == 2) {
    printf("pcie: stage 2: reading ELBI and SOC in 2 s (an SError here means the block is gated)\n");
    IOSleep(2000);
    uint32_t e[8], s[4];
    for (unsigned i = 0; i < 8; ++i) e[i] = ioRead(kElbi, i * 4);
    printf("pcie: stage 2: ELBI %#x %#x %#x %#x %#x %#x %#x %#x\n", e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
    for (unsigned i = 0; i < 4; ++i) s[i] = ioRead(kSoc, i * 4);
    printf("pcie: stage 2: SOC %#x %#x %#x %#x\n", s[0], s[1], s[2], s[3]);
    return 0;
  }
  if (stage == 3) return bringUp();
  if (stage == 4) return readEndpoint();
  printf("pcie: unknown stage %u\n", stage);
  return -1;
}
}  // namespace pcie
