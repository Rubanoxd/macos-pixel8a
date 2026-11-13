// SPDX-License-Identifier: BSD-2-Clause-Patent
// akita (Generated-by: LLM): staged look at the Wi-Fi's PCIe link (channel 1, pcie@13120000, Broadcom over one Gen2 lane).
// Included by AkitaAcpm.cpp; each stage is its own sysctl (kern.akita_acpm = 4000000 + stage) so the log flushes in between.
//   1  read only: the PMU's PCIe PHY control word (SMC, as Linux's rmw_priv_reg; the PMU hangs on direct reads) and the gph3 bank
//      (WLAN_EN gph3-4, PERST gph3-0, CLKREQ gph3-1) in pinctrl@13040000, inside the HSI2 block the UFS already uses.
//   2  read only: the first words of the controller's ELBI (0x13120000) and SOC (0x13130000) windows. If the block's clocks are
//      gated this read takes an SError and the phone panics (it comes back to fastboot by itself).
// Sources: Google's pcie-exynos-rc.c / pcie-exynos-zuma-rc-cal.c (android-gs-akita-5.15-android14-qpr3), the zuma device tree.
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);   // osfmk/arm/machine_routines.h (not in the IOKit headers)
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

// Stage 5: BARs and the chip's backplane. iATU outbound region 1 maps CPU 0x60000000..0x60efffff to the same PCI addresses (the DT's
// memory range); the root port forwards it (memory base/limit); BAR0 (64-bit) at 0x60000000, BAR2 (64-bit) at 0x60800000 (sized first).
// BAR0's first 4 KB shows the backplane core chosen by config register 0x80 (PCI_BAR0_WIN): chipcommon at 0x18000000, whose word 0
// is the chip id (low 16 bits 0x4383).
volatile uint8_t *gBar0;
int backplane() {
  if (readEndpoint() != 0) return 1;
  static volatile uint8_t *cfg;
  if (!cfg) cfg = reinterpret_cast<volatile uint8_t *>(ml_io_map(0x60ffe000, 0x2000));
  static volatile uint8_t *atu;
  if (!atu) atu = reinterpret_cast<volatile uint8_t *>(ml_io_map(0x13400000 + 0x300000, 0x1000));
  auto ec = [](uint32_t off) -> volatile uint32_t & { return *reinterpret_cast<volatile uint32_t *>(cfg + off); };
  auto at = [](uint32_t off) -> volatile uint32_t & { return *reinterpret_cast<volatile uint32_t *>(atu + off); };
  auto dbi = [](uint32_t off) -> volatile uint32_t & { return *reinterpret_cast<volatile uint32_t *>(gWin[kD].va + off); };
  // size the BARs
  uint32_t size[6];
  for (unsigned b = 0; b < 6; ++b) {
    const uint32_t old = ec(0x10 + b * 4);
    ec(0x10 + b * 4) = 0xffffffffu;
    size[b] = ec(0x10 + b * 4);
    ec(0x10 + b * 4) = old;
  }
  printf("pcie: stage 5: BAR masks %#x %#x %#x %#x %#x %#x\n", size[0], size[1], size[2], size[3], size[4], size[5]);
  ec(0x10) = 0x60000000; ec(0x14) = 0;
  ec(0x18) = 0x60800000; ec(0x1c) = 0;
  ec(0x04) = ec(0x04) | 0x6;
  dbi(0x20) = 0x60e06000;   // root port memory window 0x60000000..0x60efffff
  at(0x200 + 0x08) = 0x60000000; at(0x200 + 0x0c) = 0; at(0x200 + 0x10) = 0x60efffff;
  at(0x200 + 0x14) = 0x60000000; at(0x200 + 0x18) = 0;
  at(0x200 + 0x00) = 0;               // MEM
  at(0x200 + 0x04) = 0x80000000u;
  __asm__ volatile("dsb sy" ::: "memory");
  IODelay(100);
  if (!gBar0) gBar0 = reinterpret_cast<volatile uint8_t *>(ml_io_map(0x60000000, 0x8000));
  if (!gBar0) return -1;
  ec(0x80) = 0x18000000;   // BAR0 window: chipcommon
  __asm__ volatile("dsb sy" ::: "memory");
  const uint32_t chipid = *reinterpret_cast<volatile uint32_t *>(gBar0);
  const uint32_t caps = *reinterpret_cast<volatile uint32_t *>(gBar0 + 4);
  printf("pcie: stage 5: BAR0 %#x BAR2 %#x cmd %#x; chipcommon id %#x (chip %#x rev %u pkg %u, %u cores), caps %#x\n", ec(0x10), ec(0x18), ec(0x04) & 0xffff,
         chipid, chipid & 0xffff, (chipid >> 16) & 0xf, (chipid >> 20) & 0xf, (chipid >> 24) & 0xf, caps);
  return (chipid & 0xffff) == 0x4383 ? 0 : 1;
}

// Backplane access through BAR0's first window (config register 0x80 selects the 4 KB page).
volatile uint8_t *gCfg;
uint32_t bpRead(uint32_t addr) {
  if (!gCfg) gCfg = reinterpret_cast<volatile uint8_t *>(ml_io_map(0x60ffe000, 0x2000));
  *reinterpret_cast<volatile uint32_t *>(gCfg + 0x80) = addr & ~0xfffu;
  __asm__ volatile("dsb sy" ::: "memory");
  return *reinterpret_cast<volatile uint32_t *>(gBar0 + (addr & 0xfff));
}
void bpWrite(uint32_t addr, uint32_t value) {
  if (!gCfg) gCfg = reinterpret_cast<volatile uint8_t *>(ml_io_map(0x60ffe000, 0x2000));
  *reinterpret_cast<volatile uint32_t *>(gCfg + 0x80) = addr & ~0xfffu;
  __asm__ volatile("dsb sy" ::: "memory");
  *reinterpret_cast<volatile uint32_t *>(gBar0 + (addr & 0xfff)) = value;
  __asm__ volatile("dsb sy" ::: "memory");
}
struct Core { uint32_t id, rev, base, wrap; };
Core gCores[32];
unsigned gNumCores;
// Stage 6: walk the AXI enumeration ROM (aiutils.c ai_scan, simplified): every component's id and revision, its first slave port
// address and its first wrapper. Read only.
int enumerate() {
  if (!gBar0 && backplane() != 0) return 1;
  const uint32_t erom = bpRead(0x18000000 + 0xfc);
  printf("pcie: stage 6: EROM at %#x\n", erom);
  uint32_t at = erom;
  gNumCores = 0;
  auto next = [&at]() { const uint32_t v = bpRead(at); at += 4; return v; };
  for (unsigned guard = 0; guard < 1024 && gNumCores < 32; ++guard) {
    uint32_t cia = next();
    if ((cia & 0xe) == 0xe) break;                    // ER_END
    if ((cia & 0x7) != 0x1) continue;                 // not a valid component descriptor
    const uint32_t cib = next();
    const uint32_t cid = (cia >> 8) & 0xfff, mfg = cia >> 20, rev = cib >> 24;
    const unsigned nmp = (cib >> 4) & 0x1f, nsp = (cib >> 9) & 0x1f, nmw = (cib >> 14) & 0x1f, nsw = (cib >> 19) & 0x1f;
    for (unsigned i = 0; i < nmp; ++i) next();        // master port descriptors
    uint32_t base = 0, wrap = 0;
    // address descriptors until the next component (peek: a component descriptor has tag 0)
    for (unsigned k = 0; k < 64; ++k) {
      const uint32_t ad = bpRead(at);
      if ((ad & 0xe) != 0x4 || !(ad & 1)) break;      // not an address descriptor
      at += 4;
      if (ad & 0x8) at += 4;                          // 64-bit address: upper word
      if ((ad & 0x30) == 0x30) { const uint32_t sd = next(); if (sd & 0x8) at += 4; }   // size descriptor(s)
      const uint32_t type = ad & 0xc0, port = (ad >> 8) & 0xf, addr = ad & 0xfffff000u;
      if (type == 0x00 && port == 0 && !base) base = addr;
      if ((type == 0xc0 || type == 0x80) && !wrap) wrap = addr;
    }
    if (mfg == 0x4bf || cid >= 0x800) {               // Broadcom components (skip ARM's bus bridges without a slave)
      gCores[gNumCores++] = {cid, rev, base, wrap};
      printf("pcie: stage 6: core %#x rev %u at %#x, wrapper %#x (masters %u slaves %u mwrap %u swrap %u)\n", cid, rev, base, wrap, nmp, nsp, nmw, nsw);
    }
  }
  printf("pcie: stage 6: %u cores\n", gNumCores);
  return gNumCores ? 0 : 1;
}

// ---- Stage 7: firmware into the chip's RAM and its ARM Cortex-R4 started (bcmdhd __dhdpcie_bus_download_state for a CR4) ----
// The firmware, NVRAM and CLM blob come from the phone's own vendor partition, copied to /Users/akita/.akita/wifi/ (never shipped).
int readFile(const char *path, uint8_t *buf, int max) {
  vfs_context_t ctx = vfs_context_create(nullptr);
  vnode_t vp = nullptr;
  int err = vnode_open(path, FREAD, 0, 0, &vp, ctx);
  int total = -1;
  if (!err) {
    total = 0;
    while (total < max) {
      int resid = 0, chunk = max - total > 65536 ? 65536 : max - total;
      err = vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(buf + total), chunk, total, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
      if (err) { total = -1; break; }
      total += chunk - resid;
      if (resid) break;
    }
    vnode_close(vp, FREAD, ctx);
  }
  vfs_context_rele(ctx);
  if (total < 0) printf("pcie: cannot read %s (%d)\n", path, err);
  return total;
}
// RAM writes and reads through BAR0's window, the window moved only when the 4 KB page changes.
uint32_t gWinPage = 1;
void winTo(uint32_t addr) {
  if ((addr & ~0xfffu) == gWinPage) return;
  *reinterpret_cast<volatile uint32_t *>(gCfg + 0x80) = gWinPage = addr & ~0xfffu;
  __asm__ volatile("dsb sy" ::: "memory");
}
void ramWrite(uint32_t addr, const uint8_t *src, uint32_t len) {   // addr and len multiples of 4
  for (uint32_t i = 0; i < len; i += 4) {
    winTo(addr + i);
    uint32_t w; memcpy(&w, src + i, 4);
    *reinterpret_cast<volatile uint32_t *>(gBar0 + ((addr + i) & 0xfff)) = w;
  }
  __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t ramRead(uint32_t addr) { winTo(addr); return *reinterpret_cast<volatile uint32_t *>(gBar0 + (addr & 0xfff)); }
void ramPut(uint32_t addr, uint32_t v) { winTo(addr); *reinterpret_cast<volatile uint32_t *>(gBar0 + (addr & 0xfff)) = v; __asm__ volatile("dsb sy" ::: "memory"); }
// AI wrapper reset (aiutils.c _ai_core_reset): ioctrl +0x408, resetctrl +0x800, resetstatus +0x804.
void coreReset(uint32_t wrap, uint32_t bits, uint32_t resetbits) {
  for (unsigned i = 0; i < 300 && ramRead(wrap + 0x804); ++i) IODelay(10);
  ramPut(wrap + 0x800, 1); IODelay(10);
  ramPut(wrap + 0x408, bits | resetbits | 0x2 | 0x1); (void)ramRead(wrap + 0x408);
  for (unsigned i = 0; i < 300 && ramRead(wrap + 0x804); ++i) IODelay(10);
  for (unsigned i = 0; i < 10000 && (ramRead(wrap + 0x800) & 1); ++i) { ramPut(wrap + 0x800, 0); IODelay(1); }
  ramPut(wrap + 0x408, bits | 0x1); (void)ramRead(wrap + 0x408);
}
constexpr uint32_t kRamBase = 0x6E0000;   // CR4_4383_RAM_BASE
int download() {
  if ((!gNumCores && enumerate() != 0) || !gCfg) return 1;
  gWinPage = 1;
  const Core *cr4 = nullptr;
  for (unsigned i = 0; i < gNumCores; ++i) if (gCores[i].id == 0x83e) cr4 = &gCores[i];
  if (!cr4 || !cr4->wrap) { printf("pcie: stage 7: no ARM CR4\n"); return 1; }
  // TCM size from the CR4's banks (brcmfmac brcmf_chip_tcm_ramsize)
  const uint32_t cap = ramRead(cr4->base + 4);
  const unsigned banks = (cap & 0xf) + ((cap >> 4) & 0xf);
  uint32_t ramsize = 0;
  for (unsigned b = 0; b < banks; ++b) {
    ramPut(cr4->base + 0x40, b);
    const uint32_t info = ramRead(cr4->base + 0x44);
    ramsize += ((info & 0xff) + 1) * ((info & 0x200) ? 1024 : 8192);
  }
  printf("pcie: stage 7: CR4 cap %#x, %u banks, RAM %#x bytes at %#x\n", cap, banks, ramsize, kRamBase);
  if (!ramsize || ramsize > 0x400000) return 1;
  // files
  const int fwMax = 0x200000, nvMax = 0x10000;
  uint8_t *fw = static_cast<uint8_t *>(IOMalloc(fwMax)), *nv = static_cast<uint8_t *>(IOMalloc(nvMax)), *vars = static_cast<uint8_t *>(IOMallocZero(nvMax + 8));
  int fwLen = fw ? readFile("/Users/akita/.akita/wifi/fw.bin", fw, fwMax) : -1;
  int nvLen = nv ? readFile("/Users/akita/.akita/wifi/nvram.txt", nv, nvMax) : -1;
  int rc = 1;
  if (fwLen > 0 && nvLen > 0 && vars && static_cast<uint32_t>(fwLen) < ramsize - 0x20000) {
    // NVRAM text to "key=value\0...\0": no comments, no blank lines, trimmed
    int vlen = 0;
    for (int i = 0; i < nvLen;) {
      int e = i; while (e < nvLen && nv[e] != '\n') ++e;
      int a = i, b = e;
      while (a < b && (nv[a] == ' ' || nv[a] == '\t')) ++a;
      while (b > a && (nv[b - 1] == '\r' || nv[b - 1] == ' ' || nv[b - 1] == '\t')) --b;
      if (b > a && nv[a] != '#') { memcpy(vars + vlen, nv + a, b - a); vlen += b - a; vars[vlen++] = 0; }
      i = e + 1;
    }
    vars[vlen++] = 0;
    const uint32_t varsz = (vlen + 3) & ~3u;
    // enter download state: halt the ARM, clear the shared-address word
    coreReset(cr4->wrap, 0x20, 0x20);   // SICF_CPUHALT
    ramPut(kRamBase + ramsize - 4, 0);
    // firmware
    const uint32_t fwsz = (fwLen + 3) & ~3u;
    for (int i = fwLen; i < static_cast<int>(fwsz); ++i) fw[i] = 0;
    ramWrite(kRamBase, fw, fwsz);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < fwsz; i += 4096) { uint32_t w; memcpy(&w, fw + i, 4); if (ramRead(kRamBase + i) != w) ++bad; }
    // vars at the top, the length token in the last word
    const uint32_t varaddr = kRamBase + ramsize - 4 - varsz;
    ramWrite(varaddr, vars, varsz);
    const uint32_t words = varsz / 4, token = (~words << 16) | (words & 0xffff);
    ramPut(kRamBase + ramsize - 4, token);
    // random-seed rTLV right below the vars (type 0xFEEDC0DE, length without checksum)
    uint8_t rnd[128];
    read_random(rnd, sizeof rnd);
    const uint32_t rndaddr = varaddr - 8 - sizeof rnd;
    ramWrite(rndaddr, rnd, sizeof rnd);
    ramPut(rndaddr + sizeof rnd, sizeof rnd);
    ramPut(rndaddr + sizeof rnd + 4, 0xFEEDC0DEu);
    // reset vector: the image's first word at address 0
    uint32_t resetinstr; memcpy(&resetinstr, fw, 4);
    const uint32_t at0 = ramRead(0);
    ramPut(0, resetinstr);
    printf("pcie: stage 7: firmware %d bytes (%u page mismatches), vars %u bytes at %#x token %#x, address 0 was %#x now %#x (want %#x)\n", fwLen, bad,
           varsz, varaddr, token, at0, ramRead(0), resetinstr);
    // exit download state: let the ARM run
    coreReset(cr4->wrap, 0, 0);
    // the firmware replaces the last word with the address of its pciedev_shared structure
    uint32_t shared = token;
    unsigned ms = 0;
    for (; ms < 3000; ms += 10) { IOSleep(10); shared = ramRead(kRamBase + ramsize - 4); if (shared != token && shared != 0) break; }
    printf("pcie: stage 7: after %u ms the shared word is %#x%s\n", ms, shared, shared != token && shared ? " (firmware up)" : " (no answer)");
    if (shared > kRamBase && shared < kRamBase + ramsize) {
      uint32_t sh[8];
      for (unsigned i = 0; i < 8; ++i) sh[i] = ramRead(shared + i * 4);
      printf("pcie: stage 7: pciedev_shared flags %#x (protocol version %u) trap %#x assert %#x %#x console %#x msgtrace %#x %#x\n", sh[0], sh[0] & 0xff,
             sh[1], sh[2], sh[3], sh[4], sh[5], sh[6]);
      rc = 0;
    }
  } else {
    printf("pcie: stage 7: firmware %d bytes, nvram %d bytes: missing or too big\n", fwLen, nvLen);
  }
  if (fw) IOFree(fw, fwMax);
  if (nv) IOFree(nv, nvMax);
  if (vars) IOFree(vars, nvMax + 8);
  return rc;
}

// ---- Stage 8: the msgbuf protocol (brcmfmac pcie.c / msgbuf.c, ISC): common rings in host memory, ring indices in the chip's
// TCM, polled completions. Asks the firmware for its MAC address (WLC_GET_VAR "cur_etheraddr"). ----
unsigned gHostCap = 0, gDmaIdx = 0, gLowMem = 1;   // 4000014/15: host rings in macOS memory / in the low bank at 0xD0000000   // kern.akita_acpm 4000010/11: host_cap off/on; 4000012/13: TCM/DMA indices
struct Dma { IOBufferMemoryDescriptor *md; IOMemoryMap *map; uint8_t *va; uint64_t pa; uint32_t size; };
Dma gDma;
bool dmaAlloc(Dma &d, uint32_t size) {
  if (gLowMem) {   // a free gap of the low DRAM bank (no carve-out between 0xC3090000 and 0xD8100000; the UEFI heap ends at 0xC7C00000)
    d.md = nullptr; d.map = nullptr; d.pa = 0xD0000000; d.size = size;
    d.va = reinterpret_cast<uint8_t *>(ml_io_map_wcomb(d.pa, size));
    if (!d.va) return false;
    for (uint32_t o = 0; o < size; o += 4) *reinterpret_cast<volatile uint32_t *>(d.va + o) = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    return true;
  }
  d.md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, 0x0000000fffffffffULL & ~0xfffULL);
  if (!d.md || d.md->prepare() != kIOReturnSuccess) return false;
  IOByteCount len = 0;
  d.pa = d.md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
  d.map = d.md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache);
  if (!d.map || !d.pa || len < size) return false;
  d.va = reinterpret_cast<uint8_t *>(d.map->getVirtualAddress());
  d.size = size;
  // the buffer's cached kernel alias was zeroed at allocation: clean and invalidate it so no dirty line lands on the rings later
  if (uint8_t *cached = static_cast<uint8_t *>(d.md->getBytesNoCopy())) {
    for (uint32_t o = 0; o < size; o += 64) __asm__ volatile("dc civac, %0" : : "r"(cached + o) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
  }
  memset(d.va, 0, size);
  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}
inline void tcm16(uint32_t addr, uint16_t v) { winTo(addr); *reinterpret_cast<volatile uint16_t *>(gBar0 + (addr & 0xfff)) = v; __asm__ volatile("dsb sy" ::: "memory"); }
inline uint16_t tcm16r(uint32_t addr) { winTo(addr); return *reinterpret_cast<volatile uint16_t *>(gBar0 + (addr & 0xfff)); }
struct Ring { uint32_t off, items, itemsz, widx, ridx; uint16_t w, r; uint32_t seq; uint32_t hw, hr; };   // hw/hr: index slots in host memory (DMA indices)
Ring gRing[5];
uint32_t gShared, gPcieCore = 0x18001000;
// layout of gDma (offsets)
constexpr uint32_t kRingOff[5] = {0x0000, 0x1000, 0x9000, 0xA000, 0x10000};
constexpr uint32_t kScratch = 0x1A000, kRingUpd = 0x1B000, kIoctlReq = 0x1C000, kIoctlResp = 0x1E000 /* 8 x 8 KB */, kEvent = 0x2E000 /* 8 x 2 KB */;
constexpr uint32_t kIdx = 0x30000;   // DMA index buffer (4-byte indices): h2d w, h2d r, d2h w, d2h r
constexpr uint32_t kDmaSize = 0x400000;   // 4 MB in the low bank: rings, control buffers, flow ring, 256 RX and 256 TX buffers
constexpr uint32_t kFlowRing = 0x40000, kRxBuf = 0x100000, kTxBuf = 0x200000, kNumRx = 256, kNumTx = 256, kBufSz = 2048;
void bell() { ramPut(gPcieCore + 0xA20, 1); }   // H2D_MAILBOX_0 (PCIe2 core rev >= 64)
uint8_t *ringSlot(Ring &r, uint16_t i) { return gDma.va + r.off + i * r.itemsz; }
inline uint32_t hidx(uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(gDma.va + off); }
inline void hidxPut(uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(gDma.va + off) = v; __asm__ volatile("dsb sy" ::: "memory"); }
uint32_t ridxGet(const Ring &r, bool wr) { return gDmaIdx ? hidx(wr ? r.hw : r.hr) : tcm16r(wr ? r.widx : r.ridx); }
void ridxPut(const Ring &r, bool wr, uint16_t v) { if (gDmaIdx) hidxPut(wr ? r.hw : r.hr, v); else tcm16(wr ? r.widx : r.ridx, v); }
bool h2dPush(unsigned ring, const void *msg, uint32_t len) {
  Ring &r = gRing[ring];
  r.r = static_cast<uint16_t>(ridxGet(r, false));
  if (static_cast<uint16_t>((r.w + 1) % r.items) == r.r) return false;   // full
  uint8_t *slot = ringSlot(r, r.w);
  memset(slot, 0, r.itemsz);
  memcpy(slot, msg, len);
  slot[2] = 0;                                   // flags: the ring's phase (0 for H2D rings)
  slot[3] = static_cast<uint8_t>(r.seq % 253);   // epoch: bcmdhd numbers each H2D ring's messages from H2D_EPOCH_INIT_VAL (254)
  r.seq++;
  __asm__ volatile("dsb sy" ::: "memory");
  r.w = static_cast<uint16_t>((r.w + 1) % r.items);
  ridxPut(r, true, r.w);
  bell();
  return true;
}
uint32_t gReqId = 0x100;
void putAddr(uint8_t *p, uint64_t pa) { const uint32_t lo = static_cast<uint32_t>(pa), hi = static_cast<uint32_t>(pa >> 32); memcpy(p, &lo, 4); memcpy(p + 4, &hi, 4); }
int msgbuf() {
  if (!gBar0 || !gCfg) { printf("pcie: stage 8: run stages 3, 5-7 first\n"); return 1; }
  const Core *cr4 = nullptr;
  for (unsigned i = 0; i < gNumCores; ++i) { if (gCores[i].id == 0x83e) cr4 = &gCores[i]; if (gCores[i].id == 0x83c) gPcieCore = gCores[i].base; }
  (void)cr4;
  gShared = ramRead(kRamBase + 0x220000 - 4);
  if (gShared <= kRamBase || gShared >= kRamBase + 0x220000) { printf("pcie: stage 8: no shared structure (%#x)\n", gShared); return 1; }
  const uint32_t flags = ramRead(gShared), ver = flags & 0xff, ringInfo = ramRead(gShared + 48);
  const uint16_t maxRxPost = static_cast<uint16_t>(ramRead(gShared + 32) >> 16);
  const uint32_t ringMem = ramRead(ringInfo), h2dW = ramRead(ringInfo + 4), h2dR = ramRead(ringInfo + 8), d2hW = ramRead(ringInfo + 12), d2hR = ramRead(ringInfo + 16);
  const uint32_t maxes = ramRead(ringInfo + 52), maxCompl = ramRead(ringInfo + 56) & 0xffff;
  printf("pcie: stage 8: shared %#x flags %#x v%u, ring info %#x: ringmem %#x idx h2d w %#x r %#x d2h w %#x r %#x, max flow %u sub %u, rxpost %u\n", gShared, flags, ver,
         ringInfo, ringMem, h2dW, h2dR, d2hW, d2hR, maxes & 0xffff, maxes >> 16, maxRxPost);
  if (!gDma.va && !dmaAlloc(gDma, kDmaSize)) { printf("pcie: stage 8: no DMA memory\n"); return -1; }
  memset(gDma.va, 0, kDmaSize); __asm__ volatile("dsb sy" ::: "memory");
  printf("pcie: stage 8: host memory %#llx (%u KB)\n", static_cast<unsigned long long>(gDma.pa), kDmaSize / 1024);
  // DMA indices (the firmware reads/writes the ring indices in host memory): four arrays of 4-byte slots
  const uint32_t maxSub = maxes >> 16, hW = kIdx, hR = hW + maxSub * 4, dW = hR + maxSub * 4, dR = dW + maxCompl * 4;
  const uint64_t ia[4] = {gDma.pa + hW, gDma.pa + hR, gDma.pa + dW, gDma.pa + dR};
  for (unsigned k = 0; k < 4 && gDmaIdx; ++k) { ramPut(ringInfo + 20 + k * 8, static_cast<uint32_t>(ia[k])); ramPut(ringInfo + 24 + k * 8, static_cast<uint32_t>(ia[k] >> 32)); }
  printf("pcie: stage 8: %u completion rings; DMA indices at %#llx\n", maxCompl, static_cast<unsigned long long>(ia[0]));
  const uint32_t items[5] = {64, 1024, 64, 1024, 1024};
  const uint32_t isz[5] = {40, 32, 24, ver >= 7 ? 24u : 16u, ver >= 7 ? 40u : 32u};
  for (unsigned i = 0; i < 5; ++i) {
    Ring &r = gRing[i];
    r.off = kRingOff[i]; r.items = items[i]; r.itemsz = isz[i]; r.w = r.r = 0; r.seq = 254;
    r.widx = i < 2 ? h2dW + i * 4 : d2hW + (i - 2) * 4;
    r.ridx = i < 2 ? h2dR + i * 4 : d2hR + (i - 2) * 4;
    r.hw = i < 2 ? hW + i * 4 : dW + (i - 2) * 4;
    r.hr = i < 2 ? hR + i * 4 : dR + (i - 2) * 4;
    const uint32_t mem = ringMem + i * 16;
    const uint64_t pa = gDma.pa + r.off;
    ramPut(mem + 8, static_cast<uint32_t>(pa)); ramPut(mem + 12, static_cast<uint32_t>(pa >> 32));
    tcm16(mem + 4, static_cast<uint16_t>(r.items)); tcm16(mem + 6, static_cast<uint16_t>(r.itemsz));
    tcm16(r.widx, 0); tcm16(r.ridx, 0);
  }
  ramPut(gShared + 52, 8); ramPut(gShared + 56, static_cast<uint32_t>(gDma.pa + kScratch)); ramPut(gShared + 60, static_cast<uint32_t>((gDma.pa + kScratch) >> 32));
  // host_cap (pciedev_shared +84): IPC version 7, host ready supported (bcmdhd dhd_set_host_cap)
  if (gHostCap) ramPut(gShared + 84, 7u | ((flags & 0x10000000) ? 0x400u : 0u));
  if (flags & 0x10000000) ramPut(gPcieCore + 0xA24, 1);   // host ready (H2D_MAILBOX_1)
  IOSleep(50);
  // ioctl response and event buffers
  uint8_t m[40];
  for (unsigned i = 0; i < 8; ++i) {
    memset(m, 0, sizeof m); m[0] = 0x0B; const uint32_t id = 0x1000 + i; memcpy(m + 4, &id, 4);
    const uint16_t blen = 8192; memcpy(m + 8, &blen, 2); putAddr(m + 16, gDma.pa + kIoctlResp + i * 8192);
    if (!h2dPush(0, m, 24)) { printf("pcie: stage 8: control ring full\n"); return 1; }
    memset(m, 0, sizeof m); m[0] = 0x0D; const uint32_t eid = 0x2000 + i; memcpy(m + 4, &eid, 4);
    const uint16_t elen = 2048; memcpy(m + 8, &elen, 2); putAddr(m + 16, gDma.pa + kEvent + i * 2048);
    h2dPush(0, m, 24);
  }
  // WLC_GET_VAR "cur_etheraddr"
  memset(gDma.va + kIoctlReq, 0, 64);
  memcpy(gDma.va + kIoctlReq, "cur_etheraddr", 14);
  __asm__ volatile("dsb sy" ::: "memory");
  memset(m, 0, sizeof m); m[0] = 0x09; const uint32_t rid = 0x7777; memcpy(m + 4, &rid, 4);
  const uint32_t cmd = 262; memcpy(m + 8, &cmd, 4); const uint16_t tid = 1, inlen = 32, outlen = 32; memcpy(m + 12, &tid, 2); memcpy(m + 14, &inlen, 2); memcpy(m + 16, &outlen, 2);
  putAddr(m + 24, gDma.pa + kIoctlReq);
  h2dPush(0, m, 40);
  printf("pcie: stage 8: posted 8 ioctl and 8 event buffers and the ioctl; h2d ctrl w %u r %u (dma idx %u, host_cap %u)\n", gRing[0].w, ridxGet(gRing[0], false), gDmaIdx, gHostCap);
  // completions
  Ring &c = gRing[2];
  bool done = false;
  for (unsigned ms = 0; ms < 2000 && !done; ms += 5) {
    IOSleep(5);
    const uint16_t w = static_cast<uint16_t>(ridxGet(c, true));
    while (c.r != w) {
      const uint8_t *it = ringSlot(c, c.r);
      uint32_t reqid; memcpy(&reqid, it + 4, 4);
      uint16_t status; memcpy(&status, it + 8, 2);
      uint16_t ringid, widx; memcpy(&ringid, it + 10, 2); memcpy(&widx, it + 12, 2);
      printf("pcie: stage 8: d2h ctrl msg type %#x flags %#x epoch %u req %#x status %u ring %u widx %u\n", it[0], it[2], it[3], reqid, status, ringid, widx);
      if (it[0] == 0x0C) {
        uint16_t rlen; memcpy(&rlen, it + 12, 2);
        const uint8_t *buf = (reqid >= 0x1000 && reqid < 0x1008) ? gDma.va + kIoctlResp + (reqid - 0x1000) * 8192 : nullptr;
        if (buf) printf("pcie: stage 8: ioctl answer %u bytes: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", rlen, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
        done = true;
      }
      c.r = static_cast<uint16_t>((c.r + 1) % c.items);
      ridxPut(c, false, c.r);
    }
  }
  printf("pcie: stage 8: indices host h2d w %u r %u, d2h w %u r %u; TCM h2d w %u r %u, d2h w %u r %u\n", hidx(gRing[0].hw), hidx(gRing[0].hr),
         hidx(gRing[2].hw), hidx(gRing[2].hr), tcm16r(gRing[0].widx), tcm16r(gRing[0].ridx), tcm16r(gRing[2].widx), tcm16r(gRing[2].ridx));
  const uint32_t trap = ramRead(gShared + 4);
  printf("pcie: stage 8: %s; shared flags now %#x trap %#x\n", done ? "ioctl answered" : "no ioctl answer in 2 s", ramRead(gShared), trap);
  return done ? 0 : 1;
}

// Stage 9 (after 8): DMA loopback (MSG_TYPE_LPBK_DMAXFER 0x13): the chip copies 256 bytes from one host buffer to another,
// which proves its reads of host memory; raw dumps of a ring status and of our first ring slot.
int loopback() {
  if (!gDma.va) return 1;
  uint8_t *src = gDma.va + kIoctlReq + 0x1000, *dst = gDma.va + kIoctlReq + 0x1800;
  for (unsigned i = 0; i < 256; ++i) { src[i] = static_cast<uint8_t>(0xA5 ^ i); dst[i] = 0; }
  __asm__ volatile("dsb sy" ::: "memory");
  uint8_t m[40] = {};
  m[0] = 0x13; const uint32_t id = 0x5150; memcpy(m + 4, &id, 4);
  putAddr(m + 8, gDma.pa + kIoctlReq + 0x1000); putAddr(m + 16, gDma.pa + kIoctlReq + 0x1800);
  const uint32_t len = 256, delay = 0; memcpy(m + 24, &len, 4); memcpy(m + 28, &delay, 4); memcpy(m + 32, &delay, 4);
  const uint8_t *s0 = ringSlot(gRing[0], 0);
  printf("pcie: stage 9: our ctrl slot 0: %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x\n", s0[0], s0[1], s0[2], s0[3], s0[4], s0[5],
         s0[6], s0[7], s0[8], s0[9], s0[10], s0[11], s0[12], s0[13], s0[14], s0[15]);
  if (!h2dPush(0, m, 40)) return 1;
  Ring &c = gRing[2];
  for (unsigned ms = 0; ms < 1000; ms += 5) {
    IOSleep(5);
    const uint16_t w = static_cast<uint16_t>(ridxGet(c, true));
    while (c.r != w) {
      const uint8_t *it = ringSlot(c, c.r);
      printf("pcie: stage 9: d2h raw %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", it[0], it[1],
             it[2], it[3], it[4], it[5], it[6], it[7], it[8], it[9], it[10], it[11], it[12], it[13], it[14], it[15], it[16], it[17], it[18], it[19], it[20], it[21], it[22], it[23]);
      c.r = static_cast<uint16_t>((c.r + 1) % c.items);
      ridxPut(c, false, c.r);
    }
  }
  // error bits: endpoint status (Received Master/Target Abort), its PCIe device status; root port status; the HSI2 S2MPU
  {
    auto ec = [](uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(gCfg + off); };
    uint32_t cap = ec(0x34) & 0xff, pcap = 0;
    for (unsigned n = 0; cap && n < 16; ++n) { const uint32_t h = ec(cap); if ((h & 0xff) == 0x10) { pcap = cap; break; } cap = (h >> 8) & 0xff; }
    const uint32_t rc = *reinterpret_cast<volatile uint32_t *>(gWin[kD].va + 0x04);
    printf("pcie: stage 9: endpoint cmd/status %#x, pcie cap %#x devctl/sta %#x; root port cmd/status %#x, secondary status %#x\n", ec(0x04), pcap,
           pcap ? ec(pcap + 8) : 0, rc, *reinterpret_cast<volatile uint32_t *>(gWin[kD].va + 0x1c));
    // (the HSI2 S2MPU at 0x131f0000 is secure: reading it took an SError on fullos206)
  }
  unsigned same = 0;
  for (unsigned i = 0; i < 256; ++i) same += dst[i] == src[i];
  printf("pcie: stage 9: loopback destination matches %u of 256 bytes (first %02x %02x %02x %02x)\n", same, dst[0], dst[1], dst[2], dst[3]);
  return same == 256 ? 0 : 1;
}

// Stage 20: the firmware's console (hnd_cons_t at pciedev_shared +20: log buffer address +8, size +12, write index +16), last 2 KB.
int console() {
  if (!gShared) gShared = ramRead(kRamBase + 0x220000 - 4);
  const uint32_t cons = ramRead(gShared + 20);
  if (cons <= kRamBase || cons >= kRamBase + 0x220000) { printf("pcie: console: none (%#x)\n", cons); return 1; }
  const uint32_t buf = ramRead(cons + 8), size = ramRead(cons + 12), widx = ramRead(cons + 16);
  printf("pcie: console at %#x: buffer %#x, %u bytes, write index %u\n", cons, buf, size, widx);
  if (!size || size > 0x10000) return 1;
  const uint32_t n = size < 2048 ? size : 2048;
  char line[160]; unsigned l = 0;
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t i = (widx + size - n + k) % size;
    const uint32_t w = ramRead(buf + (i & ~3u));
    const char ch = static_cast<char>((w >> ((i & 3) * 8)) & 0xff);
    if (ch == '\r' || ch == 0) continue;
    if (ch == '\n' || l == sizeof line - 1) { line[l] = 0; if (l) printf("pcie: fw: %s\n", line); l = 0; IODelay(200); continue; }
    line[l++] = ch;
  }
  return 0;
}

// ---- Stage 21 (after 8): CLM blob, events, up, escan; the networks found go to the log ----
uint16_t gTrans = 2;
bool gNoScan, gScanOnly;
char gWatchSsid[33];   // the network in network.conf: its security (RSN AKMs, PMF) is printed by the scan
volatile int gIoctlResult = -1;   // filled by whoever drains the control ring (ioctl() or the polling thread)
uint8_t *gIoctlOut; uint32_t gIoctlOutMax;
extern bool gPolling;
extern IOLock *gTxLock;
bool gLinkUp, gKeyed;
void onLink();
unsigned gScanSeen;
uint32_t gRxOff;
inline uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void repost(uint32_t id) {   // give a consumed ioctl-response or event buffer back to the firmware
  uint8_t m[40] = {};
  const bool ev = id >= 0x2000;
  m[0] = ev ? 0x0D : 0x0B; memcpy(m + 4, &id, 4);
  const uint16_t blen = ev ? 2048 : 8192; memcpy(m + 8, &blen, 2);
  putAddr(m + 16, gDma.pa + (ev ? kEvent + (id - 0x2000) * 2048 : kIoctlResp + (id - 0x1000) * 8192));
  h2dPush(0, m, 24);
}
void onEvent(const uint8_t *pkt, uint16_t len) {
  if (len < 72) return;
  const uint32_t type = be32(pkt + 28), status = be32(pkt + 32), dlen = be32(pkt + 44);
  if (type == 16) gLinkUp = (pkt[27] & 1) != 0;   // WLC_E_LINK: flags (big endian u16 at +26), bit 0 = link up
  if (type == 46 && status == 6) gKeyed = true;  // WLC_E_PSK_SUP, WLC_SUP_KEYED
  if (type != 69) {   // not WLC_E_ESCAN_RESULT: only the connection events go to the log, and not too many (fullos221: 69 MB of event 1)
    static unsigned printed;
    const bool interesting = type == 0 || type == 3 || type == 5 || type == 6 || type == 11 || type == 12 || type == 16 || type == 46;
    if (interesting && printed++ < 200) printf("pcie: event %u status %u reason %u flags %#x\n", type, status, be32(pkt + 36), (pkt[26] << 8) | pkt[27]);
    static uint64_t counts[160]; if (type < 160 && ++counts[type] == 1000) printf("pcie: event %u seen 1000 times\n", type);
    onLink(); return;
  }
  if (status != 8 || dlen < 12) { printf("pcie: scan done (status %u), %u networks seen\n", status, gScanSeen); return; }
  const uint8_t *r = pkt + 72;   // wl_escan_result: buflen, version, sync_id, bss_count, bss_info[]
  uint16_t count; memcpy(&count, r + 10, 2);
  const uint8_t *b = r + 12;
  for (unsigned i = 0; i < count && b + 90 <= pkt + len; ++i) {
    uint32_t blen; memcpy(&blen, b + 4, 4);
    char ssid[33] = {}; const unsigned sl = b[18] > 32 ? 32 : b[18]; memcpy(ssid, b + 19, sl);
    int16_t rssi; memcpy(&rssi, b + 78, 2);
    printf("pcie: wifi: \"%s\" %02x:%02x:%02x:%02x:%02x:%02x channel %u RSSI %d dBm\n", sl ? ssid : "(hidden)", b[8], b[9], b[10], b[11], b[12], b[13], b[88], rssi);
    if (gWatchSsid[0] && !strcmp(ssid, gWatchSsid)) {   // RSN IE (48): AKM suites (2 PSK, 6 PSK-SHA256, 8 SAE), capabilities (bit 6 MFPR, bit 7 MFPC)
      uint16_t ieOff; uint32_t ieLen; memcpy(&ieOff, b + 116, 2); memcpy(&ieLen, b + 120, 4);
      const uint8_t *ie = b + ieOff, *end = ie + ieLen;
      if (end > pkt + len) end = pkt + len;
      while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
        if (ie[0] == 48 && ie[1] >= 8) {
          const uint8_t *q = ie + 2 + 2 + 4;   // version, group cipher
          uint16_t np; memcpy(&np, q, 2); q += 2 + np * 4;   // pairwise suites
          uint16_t na; memcpy(&na, q, 2); q += 2;
          char akm[64] = {}; size_t al = 0;
          for (unsigned k = 0; k < na && q + 4 <= ie + 2 + ie[1]; ++k, q += 4) al += snprintf(akm + al, sizeof akm - al, "%u ", q[3]);
          uint16_t caps = 0; if (q + 2 <= ie + 2 + ie[1]) memcpy(&caps, q, 2);
          printf("pcie: wifi: the configured network: RSN AKMs [ %s] (2 PSK, 6 PSK-SHA256, 8 SAE), PMF capable %u required %u\n", akm, (caps >> 7) & 1, (caps >> 6) & 1);
        }
        ie += 2 + ie[1];
      }
    }
    ++gScanSeen;
    if (!blen) break;
    b += blen;
  }
}
// Drains the control completion ring; returns the length of an ioctl answer (copied to out) when one arrives, -1 otherwise.
int console();
// D2H mailbox (brcmfmac brcmf_pcie_handle_mb_data): the firmware asks to enter deep sleep (DS_ENTER_REQ 0x2) and waits for the host's
// DS_ACK (0x2) in the H2D mailbox word plus a config-space SBMBX (0x98) write. fullos220 trapped about a minute after joining
// with nobody answering.
uint64_t gMbSeen;
void mailbox() {
  if (!gShared) return;
  const uint32_t d2hAddr = ramRead(gShared + 44), h2dAddr = ramRead(gShared + 40);
  if (d2hAddr <= kRamBase || d2hAddr >= kRamBase + 0x220000) return;
  const uint32_t mb = ramRead(d2hAddr);
  if (!mb) return;
  ramPut(d2hAddr, 0);
  if (gMbSeen++ < 20) printf("pcie: mailbox from the firmware %#x\n", mb);
  if ((mb & 0x2) && h2dAddr > kRamBase && h2dAddr < kRamBase + 0x220000) {
    for (unsigned i = 0; i < 100 && ramRead(h2dAddr); ++i) IODelay(100);
    ramPut(h2dAddr, 0x2);   // H2D_HOST_DS_ACK
    *reinterpret_cast<volatile uint32_t *>(gCfg + 0x98) = 1;   // SBMBX
    __asm__ volatile("dsb sy" ::: "memory");
  }
  if (mb & 0x10000000) { static bool dumped; printf("pcie: the firmware halted (mailbox FWHALT)\n"); if (!dumped) { dumped = true; console(); } }
}
int drain(uint8_t *out, uint32_t outMax) {
  mailbox();
  Ring &c = gRing[2];
  int got = -1;
  const uint16_t w = static_cast<uint16_t>(ridxGet(c, true));
  while (c.r != w) {
    const uint8_t *it = ringSlot(c, c.r);
    uint32_t reqid; memcpy(&reqid, it + 4, 4);
    if (it[0] == 0x0C && reqid >= 0x1000 && reqid < 0x1008) {   // IOCTL_CMPLT
      uint16_t st, rlen; memcpy(&st, it + 8, 2); memcpy(&rlen, it + 12, 2);
      const uint8_t *buf = gDma.va + kIoctlResp + (reqid - 0x1000) * 8192 + gRxOff;
      if (out && rlen) memcpy(out, buf, rlen < outMax ? rlen : outMax);
      if (gIoctlOut && rlen) memcpy(gIoctlOut, buf, rlen < gIoctlOutMax ? rlen : gIoctlOutMax);
      got = st ? -2 - st : rlen;
      gIoctlResult = got;
      repost(reqid);
    } else if (it[0] == 0x0E && reqid >= 0x2000 && reqid < 0x2008) {   // WL_EVENT
      uint16_t elen; memcpy(&elen, it + 12, 2);
      onEvent(gDma.va + kEvent + (reqid - 0x2000) * 2048 + gRxOff, elen);
      repost(reqid);
    } else if (it[0] != 0x0A) {
      printf("pcie: d2h type %#x req %#x status %u\n", it[0], reqid, it[8] | (it[9] << 8));
    }
    c.r = static_cast<uint16_t>((c.r + 1) % c.items);
    ridxPut(c, false, c.r);
  }
  return got;
}
// One ioctl: data (len bytes) goes in the request buffer; the answer comes back in it (outMax bytes at most). 0 or the error.
int ioctl(uint32_t cmd, uint8_t *data, uint32_t len, uint32_t outMax) {
  uint8_t *req = gDma.va + kIoctlReq;
  memcpy(req, data, len);
  __asm__ volatile("dsb sy" ::: "memory");
  uint8_t m[40] = {};
  m[0] = 0x09; const uint32_t rid = 0x7777; memcpy(m + 4, &rid, 4);
  memcpy(m + 8, &cmd, 4); const uint16_t tid = gTrans++, inlen = static_cast<uint16_t>(len), olen = static_cast<uint16_t>(outMax > len ? outMax : len);
  memcpy(m + 12, &tid, 2); memcpy(m + 14, &inlen, 2); memcpy(m + 16, &olen, 2);
  putAddr(m + 24, gDma.pa + kIoctlReq);
  if (gTxLock) IOLockLock(gTxLock);   // the polling thread (stage 23) also drains the control ring: the answer lands in gIoctlResult
  gIoctlResult = -1; gIoctlOut = data; gIoctlOutMax = outMax;
  const bool pushed = h2dPush(0, m, 40);
  if (gTxLock) IOLockUnlock(gTxLock);
  if (!pushed) { gIoctlOut = nullptr; return -100; }
  int r = -1;
  for (unsigned ms = 0; ms < 3000 && r == -1; ms += 2) {
    IOSleep(2);
    if (gTxLock) IOLockLock(gTxLock);
    if (!gPolling) drain(nullptr, 0);
    r = gIoctlResult;
    if (r != -1) gIoctlOut = nullptr;
    if (gTxLock) IOLockUnlock(gTxLock);
  }
  if (r == -1) { gIoctlOut = nullptr; return -101; }
  return r >= 0 ? 0 : r;
}
int iovarSet(const char *name, const void *val, uint32_t vlen) {
  static uint8_t buf[2048];
  const uint32_t n = static_cast<uint32_t>(strlen(name)) + 1;
  if (n + vlen > sizeof buf) return -102;
  memcpy(buf, name, n); memcpy(buf + n, val, vlen);
  return ioctl(263, buf, n + vlen, 0);
}
// akita (Generated-by: LLM): WLC_GET_VAR of a 32-bit iovar; -1000 - error when the firmware refuses it
int iovarGet(const char *name) {
  uint8_t buf[64] = {};
  const uint32_t n = static_cast<uint32_t>(strlen(name)) + 1;
  if (n > sizeof buf - 4) return -999;
  memcpy(buf, name, n);
  const int r = ioctl(262, buf, n < 8 ? 8 : n, 8);
  if (r) return -1000 - (r < 0 ? -r : r);
  int v; memcpy(&v, buf, 4); return v;
}
void filterDump(const char *when) {
  printf("pcie: filters %s: arpoe %d arp_ol %d ndoe %d pkt_filter_mode %d grat_arp %d proxy_arp %d block_ping %d dhcp_unicast %d wnm %d allmulti %d\n", when,
         iovarGet("arpoe"), iovarGet("arp_ol"), iovarGet("ndoe"), iovarGet("pkt_filter_mode"), iovarGet("grat_arp"), iovarGet("proxy_arp"),
         iovarGet("block_ping"), iovarGet("dhcp_unicast"), iovarGet("wnm"), iovarGet("allmulti"));
}
int wifiScan() {
  if (!gDma.va || !gShared) { printf("pcie: stage 21: run stage 8 first\n"); return 1; }
  gRxOff = ramRead(gShared + 36);
  if (gScanOnly) goto scan;
  {
  // CLM blob in 1400-byte chunks (brcmf_c_download_blob)
  uint8_t *clm = static_cast<uint8_t *>(IOMalloc(65536));
  const int clmLen = clm ? readFile("/Users/akita/.akita/wifi/clm.blob", clm, 65536) : -1;
  int r = 0;
  for (int off = 0; clmLen > 0 && off < clmLen && r == 0; off += 1400) {
    const uint32_t chunk = static_cast<uint32_t>(clmLen - off > 1400 ? 1400 : clmLen - off);
    uint8_t d[12 + 1400];
    const uint16_t flag = static_cast<uint16_t>((1u << 12) | (off == 0 ? 0x2u : 0u) | (off + static_cast<int>(chunk) >= clmLen ? 0x4u : 0u)), type = 2;
    const uint32_t crc = 0;
    memcpy(d, &flag, 2); memcpy(d + 2, &type, 2); memcpy(d + 4, &chunk, 4); memcpy(d + 8, &crc, 4); memcpy(d + 12, clm + off, chunk);
    r = iovarSet("clmload", d, 12 + chunk);
  }
  if (clm) IOFree(clm, 65536);
  printf("pcie: stage 21: CLM blob %d bytes loaded: %d (rx data offset %u)\n", clmLen, r, gRxOff);
  // only the events this driver uses (fullos221/222: with every event on, the firmware ran out of packet buffers in lb_alloc and
  // killed itself, _hnd_die): SET_SSID 0, AUTH 3, DEAUTH 5/6, DISASSOC 11/12, LINK 16, PSK_SUP 46, ESCAN_RESULT 69
  uint8_t mask[16] = {};
  static const unsigned kEvents[] = {0, 3, 5, 6, 11, 12, 16, 46, 69};
  for (unsigned e : kEvents) mask[e / 8] |= static_cast<uint8_t>(1u << (e % 8));
  printf("pcie: stage 21: event_msgs %d\n", iovarSet("event_msgs", mask, sizeof mask));
  // akita (Generated-by: LLM): no ARP/ND offload. The firmware's agent answers or swallows ARP for a host IP table we never
  // fill (fullos224/225: requests left en1, no reply ever came back); macOS does ARP itself.
  { uint32_t zero = 0; const int a = iovarSet("arp_ol", &zero, 4); const int b = iovarSet("arpoe", &zero, 4); const int c = iovarSet("ndoe", &zero, 4);
    printf("pcie: stage 21: arp_ol %d arpoe %d ndoe %d (0 = off)\n", a, b, c); }
  filterDump("after init");
  // a stable MAC address: the firmware makes a random one each boot; derive one from the platform UUID (locally administered)
  IOService *pe = IOService::getPlatform();
  OSObject *uo = pe ? pe->getProperty("IOPlatformUUID") : nullptr;
  if (!uo && pe && pe->getProvider()) uo = pe->getProvider()->getProperty("IOPlatformUUID");   // IOPlatformExpertDevice
  if (OSString *uuid = OSDynamicCast(OSString, uo)) {
    uint32_t h = 2166136261u;
    for (const char *c = uuid->getCStringNoCopy(); *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    uint8_t mac[6] = {0x02, 0x8a, static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16), static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h)};
    printf("pcie: stage 21: MAC from the platform UUID: %d\n", iovarSet("cur_etheraddr", mac, 6));
  }
  uint8_t up[4] = {};
  printf("pcie: stage 21: up %d\n", ioctl(2, up, 4, 0));
  uint8_t pm[4] = {};   // WLC_SET_PM 0: no power save (a desktop machine on mains)
  printf("pcie: stage 21: power save off %d\n", ioctl(86, pm, 4, 0));
  if (gNoScan) return 0;
  }
scan:
  // escan: the firmware refused version 1 (BCME_VERSION); try the params versions 4, 3, 2 (wlioctl.h wl_scan_params_v2..v4)
  gScanSeen = 0;
  int er = -1;
  for (uint32_t ver = 4; ver >= 2 && er != 0; --ver) {
    uint8_t e[8 + 80] = {};
    const uint16_t act = 1, sync = 0x1234; memcpy(e, &ver, 4); memcpy(e + 4, &act, 2); memcpy(e + 6, &sync, 2);
    uint8_t *pp = e + 8;
    const uint16_t pv = static_cast<uint16_t>(ver), plen = ver == 4 ? 76 : 72;
    memcpy(pp, &pv, 2); memcpy(pp + 2, &plen, 2);
    memset(pp + 40, 0xff, 6);   // bssid broadcast
    pp[46] = 2;                 // bss type any
    const unsigned first = ver == 4 ? 56 : 52;   // nprobes (v4 has scan_type_ext before it)
    const int32_t def = -1; for (unsigned k = 0; k < 4; ++k) memcpy(pp + first + k * 4, &def, 4);
    er = iovarSet("escan", e, 8u + plen);
    printf("pcie: stage 21: escan v%u: %d\n", ver, er);
  }
  for (unsigned ms = 0; ms < 8000; ms += 10) { IOSleep(10); if (!gPolling) drain(nullptr, 0); }
  printf("pcie: stage 21: %u networks in 8 s\n", gScanSeen);
  return gScanSeen ? 0 : 1;
}

// ---- Stage 22: join a WPA2-PSK network with the firmware's own supplicant (sup_wpa). The network comes from
// /Users/akita/.akita/wifi/network.conf, written by the user: "ssid=<name>" and "psk=<passphrase>" lines. ----
extern bool gPolling;
void onLink();
int wifiJoin() {
  if (!gDma.va) { printf("pcie: stage 22: run stages 8 and 21 first\n"); return 1; }
  char conf[512] = {};
  const int n = readFile("/Users/akita/.akita/wifi/network.conf", reinterpret_cast<uint8_t *>(conf), sizeof conf - 1);
  if (n <= 0) { printf("pcie: stage 22: no /Users/akita/.akita/wifi/network.conf\n"); return 1; }
  char ssid[33] = {}, psk[65] = {};
  for (char *line = conf; line && *line;) {
    char *end = strchr(line, '\n'); if (end) *end = 0;
    size_t l = strlen(line); while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
    if (!strncmp(line, "ssid=", 5)) strlcpy(ssid, line + 5, sizeof ssid);
    if (!strncmp(line, "psk=", 4)) strlcpy(psk, line + 4, sizeof psk);
    line = end ? end + 1 : nullptr;
  }
  const size_t slen = strlen(ssid), plen = strlen(psk);
  if (!slen || plen < 8) { printf("pcie: stage 22: network.conf needs ssid= and a psk= of 8..63 characters\n"); return 1; }
  strlcpy(gWatchSsid, ssid, sizeof gWatchSsid);
  printf("pcie: stage 22: joining a network (%zu-character name)\n", slen);   // the name and passphrase never go to the log
  gLinkUp = gKeyed = false;
  uint8_t v[4];
  auto set32 = [&](uint32_t cmd, uint32_t val) { memcpy(v, &val, 4); return ioctl(cmd, v, 4, 0); };
  int r1 = set32(20, 1);            // WLC_SET_INFRA
  int r2 = set32(22, 0);            // WLC_SET_AUTH open
  int r3 = set32(134, 4);           // WLC_SET_WSEC AES
  int r4 = set32(165, 0x80);        // WLC_SET_WPA_AUTH WPA2-PSK
  uint32_t one = 1;
  int r5 = iovarSet("sup_wpa", &one, 4);
  uint8_t pmk[262] = {};   // wsec_pmk_t: key_len, flags, key[WSEC_MAX_PASSPHRASE_LEN = 256], opt_len (68 and 70 bytes gave BCME_BADLEN)
  const uint16_t kl = static_cast<uint16_t>(plen), fl = 1;   // WSEC_PASSPHRASE
  memcpy(pmk, &kl, 2); memcpy(pmk + 2, &fl, 2); memcpy(pmk + 4, psk, plen);
  int r6 = ioctl(268, pmk, sizeof pmk, 0);   // WLC_SET_WSEC_PMK
  memset(psk, 0, sizeof psk); memset(pmk, 0, sizeof pmk); memset(conf, 0, sizeof conf);
  uint8_t ss[36] = {};
  const uint32_t sl = static_cast<uint32_t>(slen); memcpy(ss, &sl, 4); memcpy(ss + 4, ssid, slen);
  int r7 = ioctl(26, ss, sizeof ss, 0);      // WLC_SET_SSID: start the join
  printf("pcie: stage 22: infra %d auth %d wsec %d wpa_auth %d sup_wpa %d pmk %d join %d\n", r1, r2, r3, r4, r5, r6, r7);
  for (unsigned ms = 0; ms < 60000 && !(gLinkUp && gKeyed); ms += 10) {
    IOSleep(10);
    if (gPolling) continue;   // the polling thread sees the events
    drain(nullptr, 0);
  }
  onLink();
  printf("pcie: stage 22: link %s, keys %s\n", gLinkUp ? "up" : "down", gKeyed ? "installed" : "not installed");
  return gLinkUp && gKeyed ? 0 : 1;
}

// ---- Stage 23: the data path and a macOS network interface (en1). RX buffers on the rxpost ring, one TX flow ring (all traffic
// to the AP), a polling thread for the completion rings, ifnet with ether framing (as AkitaUsbGadget.h). ----
Ring gFlow;
uint16_t gFlowId = 2;
uint8_t gMac[6];
ifnet_t gIf;
IOLock *gTxLock = nullptr;
uint32_t gTxNext;
uint64_t gRxPkts, gTxPkts, gTxDrops;
// akita (Generated-by: LLM): traffic by kind, printed every 20 s while it changes (works without a shell on the phone)
uint64_t gRxBc, gRxMc, gRxUc, gRxOther, gRxArp, gRxArpReply, gTxArp, gTxBad; uint32_t gTxLastBad;
bool gFlowUp; bool gPolling;
void postRx(uint32_t i) {
  uint8_t m[32] = {};
  m[0] = 0x11; const uint32_t id = 0x3000 + i; memcpy(m + 4, &id, 4);
  const uint16_t dl = kBufSz; memcpy(m + 10, &dl, 2);
  putAddr(m + 24, gDma.pa + kRxBuf + i * kBufSz);
  h2dPush(1, m, 32);
}
uint64_t gTxPosted, gTxDone;
uint32_t gToPeer, gToPeerMacOk, gPeerPings; uint8_t gToPeerLastMac[6];
// Post one frame whose payload (after the 14-byte Ethernet header) is already in TX slot `slot`. Called with gTxLock held.
bool txPost(uint32_t slot, const uint8_t hdr[14], uint32_t dlen32) {
  uint8_t *buf = gDma.va + kTxBuf + slot * kBufSz;
  {
    // pad short frames to the Ethernet minimum (60 bytes without FCS): ARP requests are 42
    if (dlen32 < 46) { memset(buf + dlen32, 0, 46 - dlen32); dlen32 = 46; }
    __asm__ volatile("dsb sy" ::: "memory");
    uint8_t m[48] = {};
    m[0] = 0x0F; const uint32_t id = 0x4000 + slot; memcpy(m + 4, &id, 4);
    memcpy(m + 8, hdr, 14);
    m[22] = 0x01; m[23] = 1;   // 802.3 frame, one segment
    putAddr(m + 32, gDma.pa + kTxBuf + slot * kBufSz);
    const uint16_t dl = static_cast<uint16_t>(dlen32); memcpy(m + 42, &dl, 2);
    if (hdr[12] == 0x08 && hdr[13] == 0x06) ++gTxArp;
    Ring &r = gFlow;
    r.r = tcm16r(r.ridx);
    if (static_cast<uint16_t>((r.w + 1) % r.items) != r.r) {
      uint8_t *it = ringSlot(r, r.w);
      memcpy(it, m, 48);
      it[3] = static_cast<uint8_t>(r.seq++ % 253);
      __asm__ volatile("dsb sy" ::: "memory");
      r.w = static_cast<uint16_t>((r.w + 1) % r.items);
      tcm16(r.widx, r.w);
      bell();
      ++gTxPosted;
      return true;
    }
  }
  return false;
}
void arpAsked(const uint8_t *f, uint32_t len);   // below, with the ARP gleaning
errno_t wifiOutput(ifnet_t ifp, mbuf_t mb) {
  const size_t len = mbuf_pkthdr_len(mb);
  bpf_tap_out(ifp, DLT_EN10MB, mb, nullptr, 0);   // DHCP (IPConfiguration) and tcpdump work through BPF
  IOLockLock(gTxLock);
  bool sent = false;
  if (gFlowUp && len >= 14 && len <= kBufSz) {
    const uint32_t slot = gTxNext++ % kNumTx;
    uint8_t hdr[14];
    mbuf_copydata(mb, 0, 14, hdr);
    mbuf_copydata(mb, 14, len - 14, gDma.va + kTxBuf + slot * kBufSz);
    if (hdr[12] == 0x08 && hdr[13] == 0x00 && len >= 34) {   // IPv4 to the control channel's peer: which MAC does the stack use?
      extern uint8_t gPeerIpForTx[4];
      const uint8_t *ip = gDma.va + kTxBuf + slot * kBufSz;
      if (!memcmp(ip + 16, gPeerIpForTx, 4)) { ++gToPeer; memcpy(gToPeerLastMac, hdr, 6); }
    }
    if (hdr[12] == 0x08 && hdr[13] == 0x06 && len >= 42) {   // an ARP request of ours: answer it from the gleaned table
      uint8_t f[42]; memcpy(f, hdr, 14); memcpy(f + 14, gDma.va + kTxBuf + slot * kBufSz, 28); arpAsked(f, 42);
    }
    sent = txPost(slot, hdr, static_cast<uint32_t>(len - 14));
  }
  IOLockUnlock(gTxLock);
  if (sent) { ++gTxPkts; ifnet_stat_increment_out(gIf, 1, static_cast<uint32_t>(len), 0); } else { ++gTxDrops; ifnet_stat_increment_out(gIf, 0, 0, 1); }
  mbuf_freem(mb);
  return 0;
}
errno_t wifiIoctl(ifnet_t ifp, unsigned long cmd, void *data) {
  if (cmd == SIOCSIFADDR) { ifnet_set_flags(ifp, IFF_UP, IFF_UP); return 0; }
  if (cmd == SIOCSIFFLAGS && !(ifnet_flags(ifp) & IFF_UP)) {   // akita (Generated-by: LLM): who takes en1 down (fullos227/228)
    char who[32] = {}; proc_selfname(who, sizeof who);
    printf("pcie: en1 taken down by %s[%d]\n", who, proc_selfpid());
  }
  if (cmd == SIOCSIFFLAGS) { ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING); return 0; }
  if (cmd == SIOCADDMULTI || cmd == SIOCDELMULTI) return 0;
  // akita (Generated-by: LLM): the media state SystemConfiguration's KernelEventMonitor asks for (SIOCGIFMEDIA and
  // SIOCGIFXMEDIA, in their 32/64-bit forms: same leading fields, only the trailing list pointer differs). Without it
  // System Settings showed en1 "Not connected" (fullos268).
  const unsigned long num = cmd & 0xffff;
  if ((num == (('i' << 8) | 56) || num == (('i' << 8) | 72)) && data) {
    struct MediaHead { char name[IFNAMSIZ]; int current, mask, status, active, count; };
    MediaHead *m = static_cast<MediaHead *>(data);
    // the media list (one entry) goes to the caller's ifm_ulist, a user pointer after the five ints: 4 bytes in the
    // 32-bit form, 8 (aligned at 40) in the 64-bit one; the ioctl's size field tells them apart (ifconfig: "no media types?")
    const int asked = m->count;
    const unsigned len = static_cast<unsigned>((cmd >> 16) & 0x1fff);
    const int media = IFM_ETHER | IFM_AUTO;
    if (asked > 0) {
      user_addr_t list = 0;
      if (len >= 48) list = *reinterpret_cast<const uint64_t *>(static_cast<uint8_t *>(data) + 40);
      else if (len >= 40) list = *reinterpret_cast<const uint32_t *>(static_cast<uint8_t *>(data) + 36);
      if (list && copyout(&media, list, sizeof media)) return EFAULT;
    }
    m->current = m->active = media;
    m->mask = 0;
    m->status = IFM_AVALID | (gLinkUp ? IFM_ACTIVE : 0);
    m->count = 1;
    // the boot-time link event went out before configd listened: say it again to the first one who asks
    static bool announced;
    if (!announced && gLinkUp) {
      announced = true;
      dlil_post_msg(reinterpret_cast<struct ifnet *>(ifp), KEV_DL_SUBCLASS, KEV_DL_LINK_ON, nullptr, 0, FALSE);
    }
    return 0;
  }
  return EOPNOTSUPP;
}
void wifiDetach(ifnet_t) {}
void onLink() {
  if (gIf) ifnet_set_flags(gIf, gLinkUp ? (IFF_UP | IFF_RUNNING) : IFF_UP, IFF_UP | IFF_RUNNING);
  // akita (Generated-by: LLM): tell SystemConfiguration when the link comes and goes (KEV_DL_LINK_ON/OFF)
  static int reported = -1;
  if (gIf && reported != (gLinkUp ? 1 : 0)) {
    reported = gLinkUp ? 1 : 0;
    dlil_post_msg(reinterpret_cast<struct ifnet *>(gIf), KEV_DL_SUBCLASS, gLinkUp ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF, nullptr, 0, FALSE);
  }
}
// ---- akita (Generated-by: LLM): control channel over Wi-Fi, the same idea as AkitaUsbGadget.h's (5140/5141) for when the
// USB-C port holds a dongle. Built and parsed here, beside the BSD stack, so it works while userland is wedged.
//   the Mac sends "H" (hello) to UDP 5141, broadcast or unicast: it becomes the peer and gets the kernel log ("AKLG" + 32-bit
//   offset + text) by unicast to the port the hello came from, from the start; "L" replays the log; "FASTBOOT" resets into fastboot.
// Only the peer may send L and FASTBOOT, and only the peer reaches the root shell ports 2323-2325 over Wi-Fi.
extern "C" uint8_t sheng_log_buffer[];
extern "C" void akita_reset_to_fastboot(void);
struct WifiPeer { bool on; uint8_t mac[6], ip[4]; uint16_t port; uint64_t sent; uint32_t packets; };
WifiPeer gPeer;
uint8_t gPeerIpForTx[4];
uint32_t gCtlCmds, gShellDrops;
bool ownIp(uint8_t ip[4], uint8_t mask[4] = nullptr) {
  ifaddr_t *list = nullptr;
  bool ok = false;
  if (gIf && ifnet_get_address_list_family(gIf, &list, AF_INET) == 0 && list) {
    for (unsigned i = 0; list[i] && !ok; ++i) {
      struct sockaddr_in sin = {};
      if (ifaddr_address(list[i], reinterpret_cast<struct sockaddr *>(&sin), sizeof sin) == 0 && sin.sin_family == AF_INET &&
          (ntohl(sin.sin_addr.s_addr) >> 16) != 0xA9FE) {   // not 169.254
        memcpy(ip, &sin.sin_addr.s_addr, 4); ok = true;
        if (mask) {
          struct sockaddr_in m = {};
          if (ifaddr_netmask(list[i], reinterpret_cast<struct sockaddr *>(&m), sizeof m) == 0 && m.sin_family == AF_INET) memcpy(mask, &m.sin_addr.s_addr, 4);
          else { static const uint8_t c[4] = {255, 255, 255, 0}; memcpy(mask, c, 4); }
        }
      }
    }
    ifnet_free_address_list(list);
  }
  return ok;
}
uint16_t csum16(const uint8_t *d, uint32_t n) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i + 1 < n; i += 2) sum += static_cast<uint32_t>((d[i] << 8) | d[i + 1]);
  if (n & 1) sum += static_cast<uint32_t>(d[n - 1] << 8);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}
// One UDP datagram to the peer. Called with gTxLock held.
bool ctlSendLocked(const uint8_t src[4], uint16_t sport, uint16_t dport, const void *h, uint32_t hl, const void *p, uint32_t pl) {
  const uint32_t udpLen = 8 + hl + pl, ipLen = 20 + udpLen;
  if (!gFlowUp || 14 + ipLen > kBufSz) return false;
  const uint32_t slot = gTxNext++ % kNumTx;
  uint8_t *ip = gDma.va + kTxBuf + slot * kBufSz;
  static uint16_t ident;
  ++ident;
  const uint8_t ih[20] = {0x45, 0, static_cast<uint8_t>(ipLen >> 8), static_cast<uint8_t>(ipLen), static_cast<uint8_t>(ident >> 8),
                          static_cast<uint8_t>(ident), 0x40, 0, 64, 17, 0, 0, src[0], src[1], src[2], src[3],
                          gPeer.ip[0], gPeer.ip[1], gPeer.ip[2], gPeer.ip[3]};
  memcpy(ip, ih, 20);
  const uint16_t sum = csum16(ip, 20); ip[10] = static_cast<uint8_t>(sum >> 8); ip[11] = static_cast<uint8_t>(sum);
  uint8_t *u = ip + 20;
  u[0] = static_cast<uint8_t>(sport >> 8); u[1] = static_cast<uint8_t>(sport); u[2] = static_cast<uint8_t>(dport >> 8); u[3] = static_cast<uint8_t>(dport);
  u[4] = static_cast<uint8_t>(udpLen >> 8); u[5] = static_cast<uint8_t>(udpLen); u[6] = u[7] = 0;   // no UDP checksum (IPv4 allows it)
  memcpy(u + 8, h, hl); memcpy(u + 8 + hl, p, pl);
  uint8_t eh[14];
  memcpy(eh, gPeer.mac, 6); memcpy(eh + 6, gMac, 6); eh[12] = 0x08; eh[13] = 0x00;
  return txPost(slot, eh, ipLen);
}
// akita (Generated-by: LLM): a screenshot over Wi-Fi ("S<n>", the gadget's format: "AKFB" + id, offset, total, width, height +
// RGB), the framebuffer the panel scans out, downscaled n times, to the port the request came from.
struct WifiShot { bool active; uint8_t *img; uint32_t bytes, w, h, total, off, id; uint16_t port; IOMemoryMap *fb; PE_Video video; };
WifiShot gShot;
void shotStart(uint16_t port, unsigned scale) {
  if (gShot.active) return;
  if (!gShot.fb) {
    if (IOService::getPlatform()->getConsoleInfo(&gShot.video) != kIOReturnSuccess || gShot.video.v_depth != 32 || !gShot.video.v_width) {
      printf("pcie: control channel: no framebuffer for a screenshot\n"); return;
    }
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(gShot.video.v_baseAddr & ~3UL, gShot.video.v_rowBytes * gShot.video.v_height, kIODirectionIn);
    if (md) { gShot.fb = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache); md->release(); }
    if (!gShot.fb) return;
  }
  if (scale < 1) scale = 1;
  if (scale > 4) scale = 4;
  const uint32_t w = static_cast<uint32_t>(gShot.video.v_width / scale), h = static_cast<uint32_t>(gShot.video.v_height / scale);
  const uint32_t need = w * h * 3;
  if (gShot.bytes < need) { if (gShot.img) IOFree(gShot.img, gShot.bytes); gShot.img = static_cast<uint8_t *>(IOMalloc(need)); gShot.bytes = gShot.img ? need : 0; }
  if (!gShot.img) return;
  const uint8_t *fb = reinterpret_cast<const uint8_t *>(gShot.fb->getVirtualAddress());
  uint8_t *out = gShot.img;
  for (uint32_t y = 0; y < h; ++y) {
    const uint32_t *row = reinterpret_cast<const uint32_t *>(fb + static_cast<uint64_t>(y) * scale * gShot.video.v_rowBytes);
    for (uint32_t x = 0; x < w; ++x) { const uint32_t px = row[x * scale]; *out++ = static_cast<uint8_t>(px >> 16); *out++ = static_cast<uint8_t>(px >> 8); *out++ = static_cast<uint8_t>(px); }
  }
  gShot.w = w; gShot.h = h; gShot.total = need; gShot.off = 0; gShot.port = port; ++gShot.id;
  __atomic_store_n(&gShot.active, true, __ATOMIC_RELEASE);
}

// The retained kernel log to the peer, a few datagrams per poll tick, never more than half the TX slots in flight.
void ctlPump() {
  if (!gPeer.on) return;
  uint8_t src[4] = {};
  ownIp(src);
  if (__atomic_load_n(&gShot.active, __ATOMIC_ACQUIRE)) {   // a screenshot goes first
    IOLockLock(gTxLock);
    for (unsigned n = 0; n < 8 && gShot.off < gShot.total && gTxPosted - gTxDone < kNumTx / 2; ++n) {
      uint32_t len = gShot.total - gShot.off;
      if (len > 1400) len = 1400;
      uint8_t h[20] = {'A', 'K', 'F', 'B'};
      const uint32_t f[3] = {gShot.id, gShot.off, gShot.total};
      memcpy(h + 4, f, 12);
      const uint16_t wh[2] = {static_cast<uint16_t>(gShot.w), static_cast<uint16_t>(gShot.h)};
      memcpy(h + 16, wh, 4);
      if (!ctlSendLocked(src, 5142, gShot.port, h, 20, gShot.img + gShot.off, len)) break;
      gShot.off += len;
    }
    if (gShot.off >= gShot.total) gShot.active = false;
    IOLockUnlock(gTxLock);
    return;
  }
  enum : uint32_t { kTotal = 24, kPrefix = 128, kPrefixBytes = 16384, kTail = 128 + 16384, kTailBytes = 65536, kChunk = 1400 };
  IOLockLock(gTxLock);
  for (unsigned n = 0; n < 4 && gTxPosted - gTxDone < kNumTx / 2; ++n) {
    const uint64_t total = __atomic_load_n(reinterpret_cast<uint64_t *>(sheng_log_buffer + kTotal), __ATOMIC_ACQUIRE);
    if (total == UINT64_MAX || gPeer.sent >= total) break;
    const uint8_t *from; uint64_t len;
    if (gPeer.sent < kPrefixBytes) {
      from = sheng_log_buffer + kPrefix + gPeer.sent;
      len = (total < kPrefixBytes ? total : kPrefixBytes) - gPeer.sent;
    } else {
      if (total - gPeer.sent > kTailBytes - 4096) gPeer.sent = total - (kTailBytes - 4096);   // overwritten meanwhile
      const uint64_t at = gPeer.sent % kTailBytes;
      from = sheng_log_buffer + kTail + at;
      len = total - gPeer.sent;
      if (len > kTailBytes - at) len = kTailBytes - at;
    }
    if (len > kChunk) len = kChunk;
    uint8_t h[8] = {'A', 'K', 'L', 'G'};
    const uint32_t off = static_cast<uint32_t>(gPeer.sent); memcpy(h + 4, &off, 4);
    if (!ctlSendLocked(src, 5140, gPeer.port, h, 8, from, static_cast<uint32_t>(len))) break;
    gPeer.sent += len; ++gPeer.packets;
  }
  IOLockUnlock(gTxLock);
}
void ctlRun(void *arg, wait_result_t) {
  const unsigned what = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
  printf("pcie: control channel: running command %u\n", what);
  printf("pcie: control channel: command %u -> %d\n", what, akita_acpm_command(what));
  thread_terminate(current_thread());
}
// true: the frame was for the control channel (or a shell port from someone other than the peer) and is consumed here.
bool ctlFrame(const uint8_t *f, uint32_t len) {
  if (len < 14 + 20 + 8 || f[12] != 0x08 || f[13] != 0x00) return false;
  const uint8_t *ip = f + 14;
  const uint32_t ihl = (ip[0] & 0xf) * 4u;
  if (ihl < 20 || 14 + ihl + 8 > len) return false;
  const uint8_t *l4 = ip + ihl;
  const uint16_t dport = static_cast<uint16_t>((l4[2] << 8) | l4[3]);
  const bool fromPeer = gPeer.on && !memcmp(f + 6, gPeer.mac, 6);
  if (fromPeer && ip[9] == 1 && 14 + ihl < len && l4[0] == 8) ++gPeerPings;   // ICMP echo requests from the peer
  if (ip[9] == 6 && dport >= 2323 && dport <= 2325) {   // TCP to the root shells: the peer only
    if (fromPeer) return false;
    ++gShellDrops;
    return true;
  }
  if (ip[9] != 17 || dport != 5141) return false;
  const uint32_t udpLen = static_cast<uint32_t>((l4[4] << 8) | l4[5]);
  if (udpLen < 9 || 14 + ihl + udpLen > len) return true;
  const char *t = reinterpret_cast<const char *>(l4 + 8);
  uint32_t tl = udpLen - 8;
  while (tl && (t[tl - 1] == '\n' || t[tl - 1] == '\r')) --tl;
  if (!tl) return true;
  ++gCtlCmds;
  if (t[0] == 'H') {
    if (!fromPeer) {   // a new peer: the log from the start
      IOLockLock(gTxLock);
      memcpy(gPeerIpForTx, ip + 12, 4);
      memcpy(gPeer.mac, f + 6, 6); memcpy(gPeer.ip, ip + 12, 4); gPeer.port = static_cast<uint16_t>((l4[0] << 8) | l4[1]); gPeer.sent = 0; gPeer.on = true;
      IOLockUnlock(gTxLock);
      printf("pcie: control channel: peer registered (hello over %s)\n", f[0] == 0xff ? "broadcast" : "unicast");
    } else {
      IOLockLock(gTxLock); memcpy(gPeer.ip, ip + 12, 4); gPeer.port = static_cast<uint16_t>((l4[0] << 8) | l4[1]); IOLockUnlock(gTxLock);
    }
  } else if (!fromPeer) {
    // commands other than hello only from the peer
  } else if (t[0] == 'L') {
    IOLockLock(gTxLock); gPeer.sent = 0; IOLockUnlock(gTxLock);
  } else if (t[0] == 'S') {
    shotStart(static_cast<uint16_t>((l4[0] << 8) | l4[1]), tl > 1 && t[1] >= '1' && t[1] <= '4' ? static_cast<unsigned>(t[1] - '0') : 2);
  } else if (t[0] == 'K' && tl > 1 && tl < 12) {   // K<number>: an akita_acpm command (sysctl kern.akita_acpm) without userland
    uint32_t v = 0;
    for (uint32_t i = 1; i < tl && t[i] >= '0' && t[i] <= '9'; ++i) v = v * 10 + static_cast<uint32_t>(t[i] - '0');
    thread_t th = nullptr;
    if (v && kernel_thread_start(ctlRun, reinterpret_cast<void *>(static_cast<uintptr_t>(v)), &th) == KERN_SUCCESS) thread_deallocate(th);
  } else if (tl == 8 && !memcmp(t, "FASTBOOT", 8)) {
    printf("pcie: control channel: FASTBOOT requested over Wi-Fi\n");
    akita_reset_to_fastboot();
  }
  return true;
}
void rxDeliver(const uint8_t *frame, uint32_t len);
// akita (Generated-by: LLM): ARP gleaning. This firmware hands the host no ARP frames at all (fullos226: 69 requests out, the
// Mac learned our MAC from them, 0 ARP frames in, with arpoe/arp_ol/ndoe off), so the stack never resolves a neighbour. An IPv4
// frame from a neighbour on our subnet (the router's DHCP answers, the Mac's hello) tells its MAC: hand the stack an ARP reply
// with it, at most once a minute per address.
struct Glean { uint8_t ip[4]; uint8_t mac[6]; uint64_t at; };
Glean gGlean[16];
uint32_t gGleaned;
void gleanReply(const uint8_t ip[4], const uint8_t mac[6], const uint8_t me[4]) {
  uint8_t a[60] = {};
  memcpy(a, gMac, 6); memcpy(a + 6, mac, 6); a[12] = 0x08; a[13] = 0x06;
  const uint8_t h[8] = {0, 1, 8, 0, 6, 4, 0, 2};   // Ethernet/IPv4, reply
  memcpy(a + 14, h, 8); memcpy(a + 22, mac, 6); memcpy(a + 28, ip, 4); memcpy(a + 32, gMac, 6); memcpy(a + 38, me, 4);
  ++gGleaned;
  rxDeliver(a, 60);
}
// Our own ARP requests (the stack refreshing an entry, which never gets a real reply): answered from the table by the poll
// thread (wifiOutput may run with network locks held).
uint8_t gArpAsk[8][4]; volatile uint32_t gArpAskW, gArpAskR;
void arpAsked(const uint8_t *f, uint32_t len) {   // f: a whole Ethernet frame going out
  if (len < 42 || f[12] != 0x08 || f[13] != 0x06 || f[21] != 1) return;
  const uint32_t w = gArpAskW;
  if (w - gArpAskR >= 8) return;
  memcpy(gArpAsk[w % 8], f + 38, 4);
  __atomic_store_n(&gArpAskW, w + 1, __ATOMIC_RELEASE);
}
void arpAnswer() {
  while (gArpAskR != __atomic_load_n(&gArpAskW, __ATOMIC_ACQUIRE)) {
    const uint8_t *ip = gArpAsk[gArpAskR % 8];
    for (unsigned i = 0; i < 16; ++i)
      if (gGlean[i].at && !memcmp(gGlean[i].ip, ip, 4)) {
        uint8_t me[4];
        if (ownIp(me)) gleanReply(gGlean[i].ip, gGlean[i].mac, me);
        break;
      }
    ++gArpAskR;
  }
}
// The other half: neighbours ask for us by ARP and the firmware swallows the question too, so their entry for us expires
// (fullos229/230: the router stopped answering a few minutes after the lease, while our frames still reached the Mac). Every
// 20 s ask each neighbour heard in the last 10 minutes for its own address, by unicast: the target of a request always learns
// the sender's MAC.
uint32_t gAnnounced;
void announce() {
  static uint64_t last;
  const uint64_t now = mach_absolute_time();
  if (now - last < 20ull * 24000000ull) return;
  last = now;
  uint8_t me[4];
  if (!ownIp(me)) return;
  IOLockLock(gTxLock);
  for (unsigned i = 0; i < 16; ++i) {
    if (!gGlean[i].at || now - gGlean[i].at > 600ull * 24000000ull || !gFlowUp) continue;
    const uint32_t slot = gTxNext++ % kNumTx;
    uint8_t *a = gDma.va + kTxBuf + slot * kBufSz;
    const uint8_t h[8] = {0, 1, 8, 0, 6, 4, 0, 1};   // Ethernet/IPv4, request
    memcpy(a, h, 8); memcpy(a + 8, gMac, 6); memcpy(a + 14, me, 4); memset(a + 18, 0, 6); memcpy(a + 24, gGlean[i].ip, 4);
    uint8_t eh[14];
    memcpy(eh, gGlean[i].mac, 6); memcpy(eh + 6, gMac, 6); eh[12] = 0x08; eh[13] = 0x06;
    if (txPost(slot, eh, 28)) ++gAnnounced;
  }
  IOLockUnlock(gTxLock);
}
void glean(const uint8_t *f, uint32_t len) {
  if (len < 14 + 20 || f[12] != 0x08 || f[13] != 0x00 || (f[6] & 1)) return;
  const uint8_t *sip = f + 14 + 12;
  if (!sip[0] && !sip[1] && !sip[2] && !sip[3]) return;
  const uint64_t now = mach_absolute_time();
  unsigned slot = 0; uint64_t oldest = UINT64_MAX;
  for (unsigned i = 0; i < 16; ++i) {
    if (!memcmp(gGlean[i].ip, sip, 4)) {
      if (!memcmp(gGlean[i].mac, f + 6, 6) && now - gGlean[i].at < 60ull * 24000000ull) return;
      slot = i; oldest = 0; break;
    }
    if (gGlean[i].at < oldest) { oldest = gGlean[i].at; slot = i; }
  }
  uint8_t me[4], mask[4];
  if (!ownIp(me, mask)) return;
  bool onLink = true;
  for (unsigned i = 0; i < 4; ++i) if ((sip[i] ^ me[i]) & mask[i]) onLink = false;
  if (!onLink) {
    // from beyond the router (internet traffic): refresh the neighbour that owns this MAC, the router, once a minute
    for (unsigned i = 0; i < 16; ++i)
      if (gGlean[i].at && !memcmp(gGlean[i].mac, f + 6, 6)) {
        if (now - gGlean[i].at >= 60ull * 24000000ull) { gGlean[i].at = now; gleanReply(gGlean[i].ip, gGlean[i].mac, me); }
        break;
      }
    return;
  }
  if (!memcmp(sip, me, 4)) return;
  memcpy(gGlean[slot].ip, sip, 4); memcpy(gGlean[slot].mac, f + 6, 6); gGlean[slot].at = now;
  gleanReply(sip, f + 6, me);
}
void rxDeliver(const uint8_t *frame, uint32_t len) {
  if (!gIf || len < 14 || len > kBufSz) return;
  if (ctlFrame(frame, len)) { glean(frame, len); return; }
  glean(frame, len);
  mbuf_t mb = nullptr;
  if (mbuf_gethdr(MBUF_DONTWAIT, MBUF_TYPE_DATA, &mb) != 0) return;
  if (len > mbuf_maxlen(mb) && mbuf_mclget(MBUF_DONTWAIT, MBUF_TYPE_DATA, &mb) != 0) { mbuf_freem(mb); return; }
  memcpy(mbuf_data(mb), frame, len);
  mbuf_setlen(mb, len); mbuf_pkthdr_setlen(mb, len); mbuf_pkthdr_setrcvif(mb, gIf);
  bpf_tap_in(gIf, DLT_EN10MB, mb, nullptr, 0);
  char *h = static_cast<char *>(mbuf_data(mb));
  mbuf_pkthdr_setheader(mb, h);
  mbuf_setdata(mb, h + 14, len - 14); mbuf_pkthdr_adjustlen(mb, -14);
  struct ifnet_stat_increment_param st = {}; st.packets_in = 1; st.bytes_in = len;
  ifnet_input(gIf, mb, &st);
  ++gRxPkts;
  if (frame[0] == 0xff && frame[1] == 0xff) ++gRxBc; else if (frame[0] & 1) ++gRxMc; else if (!memcmp(frame, gMac, 6)) ++gRxUc; else ++gRxOther;
  if (frame[12] == 0x08 && frame[13] == 0x06) { ++gRxArp; if (len >= 22 && frame[21] == 2) ++gRxArpReply; }
}
void pollThread(void *, wait_result_t) {
  for (;;) {
    IOSleep(2);
    IOLockLock(gTxLock);
    drain(nullptr, 0);   // control: events (link), flow ring create answers
    Ring &rc = gRing[4];   // RX completions
    const uint16_t w = static_cast<uint16_t>(ridxGet(rc, true));
    while (rc.r != w) {
      const uint8_t *it = ringSlot(rc, rc.r);
      uint32_t id; uint16_t dlen, doff; memcpy(&id, it + 4, 4); memcpy(&dlen, it + 14, 2); memcpy(&doff, it + 16, 2);
      if (id >= 0x3000 && id < 0x3000 + kNumRx) {
        const uint8_t *b = gDma.va + kRxBuf + (id - 0x3000) * kBufSz + (doff ? doff : gRxOff);
        IOLockUnlock(gTxLock); rxDeliver(b, dlen); IOLockLock(gTxLock);
        postRx(id - 0x3000);
      }
      rc.r = static_cast<uint16_t>((rc.r + 1) % rc.items);
      ridxPut(rc, false, rc.r);
    }
    Ring &tc = gRing[3];   // TX completions: nothing to free (fixed slots), just consume
    const uint16_t tw = static_cast<uint16_t>(ridxGet(tc, true));
    while (tc.r != tw) {
      const uint8_t *it = ringSlot(tc, tc.r);
      uint16_t st, ts; memcpy(&st, it + 8, 2); memcpy(&ts, it + 14, 2);
      if (st || ts) { ++gTxBad; gTxLastBad = (uint32_t(st) << 16) | ts; }
      ++gTxDone;
      tc.r = static_cast<uint16_t>((tc.r + 1) % tc.items);
    }
    ridxPut(tc, false, tc.r);
    IOLockUnlock(gTxLock);
    ctlPump();
    arpAnswer();
    announce();
    static uint64_t lastTick, lastSum;
    const uint64_t now = mach_absolute_time();
    if (now - lastTick > 20ull * 24000000ull) {   // 24 MHz timebase
      lastTick = now;
      const uint64_t sum = gRxPkts + gTxPkts + gTxBad;
      if (sum != lastSum) {
        lastSum = sum;
        printf("pcie: traffic rx %llu (bc %llu mc %llu to us %llu other %llu; arp %llu replies %llu) tx %llu (arp %llu, drops %llu, bad completions %llu last 0x%x; posted %llu done %llu) control %u cmds, log %u pkts, shell drops %u, gleaned %u, announced %u; peer pings in %u, frames to the peer's IP %u (last to %s MAC)\n",
               gRxPkts, gRxBc, gRxMc, gRxUc, gRxOther, gRxArp, gRxArpReply, gTxPkts, gTxArp, gTxDrops, gTxBad, gTxLastBad, gTxPosted, gTxDone,
               gCtlCmds, gPeer.packets, gShellDrops, gGleaned, gAnnounced, gPeerPings, gToPeer,
               !gToPeer ? "no" : !memcmp(gToPeerLastMac, gPeer.mac, 6) ? "the peer's" : (gToPeerLastMac[0] & 1) ? "a group" : "another");
      }
    }
  }
}
int dataPath() {
  if (!gDma.va || !gShared) { printf("pcie: stage 23: run stages 8 and 21 first\n"); return 1; }
  if (!gTxLock) gTxLock = IOLockAlloc();
  uint8_t mac[32] = {};
  memcpy(mac, "cur_etheraddr", 14);
  if (ioctl(262, mac, 32, 32) == 0) memcpy(gMac, mac, 6);
  // RX buffers
  for (uint32_t i = 0; i < 128; ++i) postRx(i);
  // TX flow ring: id 2 (after the two common H2D rings), 512 items of 48 bytes, all traffic to the AP (destination 0)
  const uint32_t ringInfo = ramRead(gShared + 48);
  gFlow.off = kFlowRing; gFlow.items = 512; gFlow.itemsz = 48; gFlow.w = gFlow.r = 0; gFlow.seq = 254;
  gFlow.widx = ramRead(ringInfo + 4) + gFlowId * 4; gFlow.ridx = ramRead(ringInfo + 8) + gFlowId * 4;
  tcm16(gFlow.widx, 0); tcm16(gFlow.ridx, 0);
  uint8_t m[40] = {};
  m[0] = 0x03; const uint32_t rid = 0x6000; memcpy(m + 4, &rid, 4);
  memcpy(m + 14, gMac, 6);    // sa (da stays 0: indirect, everything goes through the AP)
  m[20] = 0;                  // tid
  memcpy(m + 22, &gFlowId, 2);
  const uint16_t mi = 512, li = 48; memcpy(m + 28, &mi, 2); memcpy(m + 30, &li, 2);
  putAddr(m + 32, gDma.pa + kFlowRing);
  h2dPush(0, m, 40);
  for (unsigned ms = 0; ms < 2000; ms += 5) {   // FLOW_RING_CREATE_CMPLT (type 4) arrives on the control completion ring
    IOSleep(5);
    Ring &c = gRing[2];
    const uint16_t w = static_cast<uint16_t>(ridxGet(c, true));
    bool done = false;
    while (c.r != w) {
      const uint8_t *it = ringSlot(c, c.r);
      if (it[0] == 0x04) { uint16_t st; memcpy(&st, it + 8, 2); printf("pcie: stage 23: flow ring %u create status %u\n", gFlowId, st); gFlowUp = st == 0; done = true; }
      else if (it[0] == 0x0E) { uint32_t reqid; memcpy(&reqid, it + 4, 4); uint16_t el; memcpy(&el, it + 12, 2); onEvent(gDma.va + kEvent + (reqid - 0x2000) * 2048 + gRxOff, el); repost(reqid); }
      c.r = static_cast<uint16_t>((c.r + 1) % c.items);
      ridxPut(c, false, c.r);
    }
    if (done) break;
  }
  // the interface
  if (!gIf) {
    struct ifnet_init_eparams init = {};
    init.ver = IFNET_INIT_CURRENT_VERSION; init.len = sizeof(init);
    init.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
    init.output = wifiOutput; init.uniqueid = "akita-wifi-bcm4383"; init.uniqueid_len = 18;
    init.name = "en"; init.unit = 1; init.family = IFNET_FAMILY_ETHERNET; init.type = IFT_ETHER;
    init.demux = ether_demux; init.add_proto = ether_add_proto; init.del_proto = ether_del_proto;
    init.check_multi = ether_check_multi; init.framer_extended = ether_frameout_extended;
    init.ioctl = wifiIoctl; init.detach = wifiDetach;
    static const uint8_t bc[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    init.broadcast_addr = bc; init.broadcast_len = 6;
    if (ifnet_allocate_extended(&init, &gIf) == 0) {
      ifnet_set_addrlen(gIf, 6); ifnet_set_baudrate(gIf, 600000000); ifnet_set_mtu(gIf, ETHERMTU);
      ifnet_set_flags(gIf, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
      ifnet_set_hdrlen(gIf, 14); ifnet_set_offload(gIf, 0);
      if (ifnet_attach(gIf, nullptr) == 0) { ifnet_set_lladdr(gIf, gMac, 6); bpfattach(gIf, DLT_EN10MB, 14); netif::publish(gMac, 1); } else { ifnet_release(gIf); gIf = nullptr; }
    }
  }
  onLink();
  if (!gPolling) { thread_t t = nullptr; if (kernel_thread_start(pollThread, nullptr, &t) == KERN_SUCCESS) { thread_deallocate(t); gPolling = true; } }
  printf("pcie: stage 23: interface %s%u (MAC %02x:%02x:%02x:%02x:%02x:%02x), flow ring %s, link %s, polling %d\n", gIf ? ifnet_name(gIf) : "-", gIf ? ifnet_unit(gIf) : 0,
         gMac[0], gMac[1], gMac[2], gMac[3], gMac[4], gMac[5], gFlowUp ? "up" : "refused", gLinkUp ? "up" : "down", gPolling);
  return gIf ? 0 : 1;
}

int console();
// ---- Boot: boot-arg akita-wifi=1 runs every stage by itself once the home volume (the firmware files) is mounted. ----
void wifiBootThread(void *, wait_result_t) {
  IOSleep(60000);
  uint8_t probe[16];
  for (unsigned i = 0; i < 150 && readFile("/Users/akita/.akita/wifi/nvram.txt", probe, sizeof probe) <= 0; ++i) IOSleep(2000);
  int r = bringUp();
  if (r == 0) r = backplane();
  if (r == 0) r = enumerate();
  if (r == 0) r = download();
  if (r == 0) r = msgbuf();
  if (r == 0) { gNoScan = true; r = wifiScan(); }
  if (r == 0) r = dataPath();   // RX buffers and the flow ring first: the join's EAPOL frames may come through them
  if (r == 0) { const int j = wifiJoin(); printf("pcie: boot: join %d\n", j); if (j) console(); }
  printf("pcie: boot: Wi-Fi %s\n", r == 0 ? "up" : "failed");
}
}  // namespace pcie
extern "C" void akita_wifi_boot(void) {
  unsigned on = 0;
  PE_parse_boot_argn("akita-wifi", &on, sizeof on);
  if (on != 1) return;
  thread_t t = nullptr;
  if (kernel_thread_start(pcie::wifiBootThread, nullptr, &t) == KERN_SUCCESS) { thread_deallocate(t); printf("pcie: Wi-Fi starts in 60 s (akita-wifi=1)\n"); }
}
namespace pcie {

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
  if (stage == 5) return backplane();
  if (stage == 6) return enumerate();
  if (stage == 7) return download();
  if (stage == 8) return msgbuf();
  if (stage == 9) return loopback();
  if (stage == 20) return console();
  if (stage == 21) return wifiScan();
  if (stage == 22) return wifiJoin();
  if (stage == 26) {   // firmware capabilities and the supplicant state
    static uint8_t b[1024];
    memset(b, 0, sizeof b); memcpy(b, "cap", 4);
    const int r1 = ioctl(262, b, sizeof b, sizeof b);
    b[sizeof b - 1] = 0;
    for (char *p = reinterpret_cast<char *>(b); *p; p += 160) { char line[161] = {}; strlcpy(line, p, sizeof line); printf("pcie: cap: %s\n", line); if (strlen(p) <= 160) break; }
    uint8_t v[16] = {}; memcpy(v, "sup_wpa", 8);
    const int r2 = ioctl(262, v, sizeof v, sizeof v);
    uint32_t sup; memcpy(&sup, v, 4);
    printf("pcie: cap %d, sup_wpa %d = %u\n", r1, r2, sup);
    return 0;
  }
  if (stage == 25) { gScanOnly = true; const int r = wifiScan(); gScanOnly = false; return r; }
  if (stage == 23) return dataPath();
  if (stage == 27) { filterDump("now"); return 0; }
  if (stage == 28 || stage == 29) {   // akita (Generated-by: LLM): ARP offload the Android way (28: on with our IP), or off (29)
    uint8_t me[4] = {};
    const bool on = stage == 28 && ownIp(me);
    uint32_t mode = on ? 0x9u : 0u;   // ARP_OL_AGENT | ARP_OL_PEER_AUTO_REPLY (dhd_arp_mode)
    uint32_t en = on ? 1u : 0u;
    const int a = iovarSet("arp_ol", &mode, 4), b = iovarSet("arpoe", &en, 4);
    uint32_t none = 0;
    const int c = iovarSet("arp_hostip_clear", &none, 0);
    const int d = on ? iovarSet("arp_hostip", me, 4) : 0;
    printf("pcie: arp offload %s: arp_ol %d arpoe %d clear %d hostip %d\n", on ? "on" : "off", a, b, c, d);
    filterDump("after arp offload change");
    return 0;
  }
  if (stage == 24) { printf("pcie: rx %llu tx %llu drops %llu link %d keyed %d flow %d\n", gRxPkts, gTxPkts, gTxDrops, gLinkUp, gKeyed, gFlowUp); return 0; }
  if (stage == 14 || stage == 15) { gLowMem = stage - 14; gDma.va = nullptr; printf("pcie: host rings in %s\n", gLowMem ? "the low bank" : "macOS memory"); return 0; }
  if (stage >= 10 && stage <= 13) { if (stage < 12) gHostCap = stage - 10; else gDmaIdx = stage - 12; printf("pcie: host_cap %u, DMA indices %u\n", gHostCap, gDmaIdx); return 0; }
  printf("pcie: unknown stage %u\n", stage);
  return -1;
}
}  // namespace pcie
