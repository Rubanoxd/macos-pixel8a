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
  printf("pcie: unknown stage %u\n", stage);
  return -1;
}
}  // namespace pcie
