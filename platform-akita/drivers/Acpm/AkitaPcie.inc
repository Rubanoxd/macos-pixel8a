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
constexpr uint32_t kDmaSize = 0x40000;
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
  if (type != 69) { printf("pcie: event %u status %u\n", type, status); return; }   // WLC_E_ESCAN_RESULT
  if (status != 8 || dlen < 12) { printf("pcie: scan done (status %u), %u networks seen\n", status, gScanSeen); return; }
  const uint8_t *r = pkt + 72;   // wl_escan_result: buflen, version, sync_id, bss_count, bss_info[]
  uint16_t count; memcpy(&count, r + 10, 2);
  const uint8_t *b = r + 12;
  for (unsigned i = 0; i < count && b + 90 <= pkt + len; ++i) {
    uint32_t blen; memcpy(&blen, b + 4, 4);
    char ssid[33] = {}; const unsigned sl = b[18] > 32 ? 32 : b[18]; memcpy(ssid, b + 19, sl);
    int16_t rssi; memcpy(&rssi, b + 78, 2);
    printf("pcie: wifi: \"%s\" %02x:%02x:%02x:%02x:%02x:%02x channel %u RSSI %d dBm\n", sl ? ssid : "(hidden)", b[8], b[9], b[10], b[11], b[12], b[13], b[88], rssi);
    ++gScanSeen;
    if (!blen) break;
    b += blen;
  }
}
// Drains the control completion ring; returns the length of an ioctl answer (copied to out) when one arrives, -1 otherwise.
int drain(uint8_t *out, uint32_t outMax) {
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
      got = st ? -2 - st : rlen;
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
  if (!h2dPush(0, m, 40)) return -100;
  for (unsigned ms = 0; ms < 3000; ms += 2) {
    IOSleep(2);
    const int r = drain(data, outMax);
    if (r >= 0) return 0;
    if (r < -1) return r;
  }
  return -101;
}
int iovarSet(const char *name, const void *val, uint32_t vlen) {
  static uint8_t buf[2048];
  const uint32_t n = static_cast<uint32_t>(strlen(name)) + 1;
  if (n + vlen > sizeof buf) return -102;
  memcpy(buf, name, n); memcpy(buf + n, val, vlen);
  return ioctl(263, buf, n + vlen, 0);
}
int wifiScan() {
  if (!gDma.va || !gShared) { printf("pcie: stage 21: run stage 8 first\n"); return 1; }
  gRxOff = ramRead(gShared + 36);
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
  uint8_t mask[16]; memset(mask, 0xff, sizeof mask);
  printf("pcie: stage 21: event_msgs %d\n", iovarSet("event_msgs", mask, sizeof mask));
  uint8_t up[4] = {};
  printf("pcie: stage 21: up %d\n", ioctl(2, up, 4, 0));
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
  for (unsigned ms = 0; ms < 8000; ms += 10) { IOSleep(10); drain(nullptr, 0); }
  printf("pcie: stage 21: %u networks in 8 s\n", gScanSeen);
  return gScanSeen ? 0 : 1;
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
  if (stage == 5) return backplane();
  if (stage == 6) return enumerate();
  if (stage == 7) return download();
  if (stage == 8) return msgbuf();
  if (stage == 9) return loopback();
  if (stage == 20) return console();
  if (stage == 21) return wifiScan();
  if (stage == 14 || stage == 15) { gLowMem = stage - 14; gDma.va = nullptr; printf("pcie: host rings in %s\n", gLowMem ? "the low bank" : "macOS memory"); return 0; }
  if (stage >= 10 && stage <= 13) { if (stage < 12) gHostCap = stage - 10; else gDmaIdx = stage - 12; printf("pcie: host_cap %u, DMA indices %u\n", gHostCap, gDmaIdx); return 0; }
  printf("pcie: unknown stage %u\n", stage);
  return -1;
}
}  // namespace pcie
