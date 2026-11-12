// akita (Generated-by: LLM, local only): HSI2C 11 (USI11 of PERIC1), the MAX77759 bus: USB-C controller (TCPC, 0x25), PMIC
// and charger (0x66), fuel gauge (0x36). Phase 1 of video and USB host over USB-C (docs/research-hdmi-usbc.md).
// READ-ONLY towards the chips: this file never writes a MAX77759 register (the charger lives there). It only drives the I2C
// controller itself.
//   kern.akita_acpm = 5000000 + stage
//   1  state: CMU_PERIC1 mux/divider/gates for USI11, SYSREG_PERIC1 USI mode, the pins, the controller's registers
//   2  chip IDs: TCPC vendor/product/device (0x00-0x05), PMIC_ID and revision (0x66: 0x00, 0x01)
//   3  TCPC state: ALERT, ROLE_CONTROL, CC_STATUS, POWER_STATUS, FAULT_STATUS (plug a dongle and run it again)
// Sources: Linux i2c-exynos5.c (the exynos7/autov9 "USI v2" variant), the akita/zuma device tree (hsi2c_11, sysreg_peric1
// +0xc, gpp21-0/1 function 2), the Pixel 8 Pro Linux port's clk-zuma.c (derekross/linux-omarchy-mobile PR 163: CMU_PERIC1 at
// 0x10c00000, USI11 user mux 0x640, divider 0x1810, gates 0x2048 ipclk / 0x204c pclk / 0x2094 rstnsync, bit 21;
// "firmware leaves them running"), the TCPCI specification (register map).
namespace i2c {

constexpr uint64_t kCmuPeric1 = 0x10C00000, kSysPeric1 = 0x10C21000, kHsi2c = 0x10CB0000, kPinP = 0x10C40000;
constexpr uint32_t kMuxUsi11 = 0x640, kDivUsi11 = 0x1810, kGateIp = 0x2048, kGatePclk = 0x204c, kGateRst = 0x2094,
                   kGateGpio = 0x2080, kGateSys = 0x20a4, kUsiSwConf = 0x0c, kGpp21 = 0x40;
enum : uint32_t { CTL = 0x00, FIFO_CTL = 0x04, TRAILIG_CTL = 0x08, INT_EN = 0x20, INT_ST = 0x24, ERR_ST = 0x2C, FIFO_ST = 0x30,
                  TXD = 0x34, RXD = 0x38, CONF = 0x40, AUTO_CONF = 0x44, TIMEOUT = 0x48, MANUAL = 0x4C, TRANS_ST = 0x50,
                  TIM_HS1 = 0x54, TIM_HS2 = 0x58, TIM_HS3 = 0x5C, TIM_FS1 = 0x60, TIM_FS2 = 0x64, TIM_FS3 = 0x68, TIM_SLA = 0x6C,
                  ADDR = 0x70, USI_CONFIG = 0xC0, USI_CON = 0xC4, USI_OPTION = 0xC8 };
constexpr uint32_t CTL_I2C = 1u << 0, CTL_MASTER = 1u << 3, CTL_RXCHON = 1u << 6, CTL_TXCHON = 1u << 7;
constexpr uint32_t FIFO_RX_EN = 1u << 0, FIFO_TX_EN = 1u << 1;
constexpr uint32_t INT_DONE = 1u << 7, INT_ABORT = 1u << 8, INT_NOACK = 1u << 9, INT_NODEV = 1u << 10, INT_TMOUT = 1u << 11,
                   INT_ERRORS = INT_ABORT | INT_NOACK | INT_NODEV | INT_TMOUT;
constexpr uint32_t CONF_AUTO = 1u << 31, AUTO_READ = 1u << 16, AUTO_STOP = 1u << 17, AUTO_RUN = 1u << 31;
constexpr uint32_t TRANS_MASTER_BUSY = 1u << 17;

volatile uint8_t *gCmu, *gSys, *gI2c, *gPin;
IOLock *gLock;

inline uint32_t rd(volatile uint8_t *b, uint32_t o) { return *reinterpret_cast<volatile uint32_t *>(b + o); }
inline void wr(volatile uint8_t *b, uint32_t o, uint32_t v) {
  *reinterpret_cast<volatile uint32_t *>(b + o) = v;
  __asm__ volatile("dsb sy" ::: "memory");
}

bool mapAll() {
  if (!gCmu) gCmu = reinterpret_cast<volatile uint8_t *>(ml_io_map(kCmuPeric1, 0x3000));
  if (!gSys) gSys = reinterpret_cast<volatile uint8_t *>(ml_io_map(kSysPeric1, 0x1000));
  if (!gPin) gPin = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPinP, 0x1000));
  if (!gI2c) gI2c = reinterpret_cast<volatile uint8_t *>(ml_io_map(kHsi2c, 0x1000));
  if (!gLock) gLock = IOLockAlloc();
  return gCmu && gSys && gPin && gI2c && gLock;
}

// a gate register lets the clock through when it is gated automatically (HWACG, bit 28) or on by hand (bit 21)
bool clockOn(uint32_t gate) { return (gate & (1u << 28)) || (gate & (1u << 21)); }

int state() {
  if (!mapAll()) { printf("i2c: stage 1: mapping failed\n"); return 1; }
  const uint32_t gIp = rd(gCmu, kGateIp), gP = rd(gCmu, kGatePclk);
  printf("i2c: stage 1: CMU_PERIC1 usi11 mux %#x div %#x gates ip %#x pclk %#x rst %#x gpio %#x sysreg %#x\n",
         rd(gCmu, kMuxUsi11), rd(gCmu, kDivUsi11), gIp, gP, rd(gCmu, kGateRst), rd(gCmu, kGateGpio), rd(gCmu, kGateSys));
  printf("i2c: stage 1: SYSREG_PERIC1 usi11 sw_conf %#x (USI_V2: 1 UART, 2 SPI, 4 I2C); gpp21 con %#x dat %#x pud %#x drv %#x\n",
         rd(gSys, kUsiSwConf), rd(gPin, kGpp21), rd(gPin, kGpp21 + 4), rd(gPin, kGpp21 + 8), rd(gPin, kGpp21 + 0xc));
  if (!clockOn(gIp) || !clockOn(gP)) { printf("i2c: stage 1: USI11's clock looks off: not touching the controller\n"); return 2; }
  printf("i2c: stage 1: HSI2C ctl %#x fifo_ctl %#x int_en %#x int_st %#x err %#x fifo_st %#x conf %#x auto %#x timeout %#x trans %#x\n",
         rd(gI2c, CTL), rd(gI2c, FIFO_CTL), rd(gI2c, INT_EN), rd(gI2c, INT_ST), rd(gI2c, ERR_ST), rd(gI2c, FIFO_ST), rd(gI2c, CONF),
         rd(gI2c, AUTO_CONF), rd(gI2c, TIMEOUT), rd(gI2c, TRANS_ST));
  printf("i2c: stage 1: timings fs %#x %#x %#x hs %#x %#x %#x sla %#x addr %#x; usi config %#x con %#x option %#x\n",
         rd(gI2c, TIM_FS1), rd(gI2c, TIM_FS2), rd(gI2c, TIM_FS3), rd(gI2c, TIM_HS1), rd(gI2c, TIM_HS2), rd(gI2c, TIM_HS3),
         rd(gI2c, TIM_SLA), rd(gI2c, ADDR), rd(gI2c, USI_CONFIG), rd(gI2c, USI_CON), rd(gI2c, USI_OPTION));
  return 0;
}

// Poll INT_ST for `want` or an error, up to `us` microseconds. Returns the status seen (0 on timeout).
uint32_t waitInt(uint32_t want, unsigned us) {
  for (unsigned t = 0; t < us; t += 10) {
    const uint32_t st = rd(gI2c, INT_ST);
    if (st & (want | INT_ERRORS)) return st;
    IODelay(10);
  }
  return 0;
}

// Read n bytes from register `reg` of chip `dev`: a one-byte write without STOP, a repeated START, the read, STOP.
int readRegs(uint8_t dev, uint8_t reg, uint8_t *out, unsigned n) {
  if (!mapAll() || n == 0 || n > 32) return -1;
  if (!clockOn(rd(gCmu, kGateIp)) || !clockOn(rd(gCmu, kGatePclk))) return -2;
  if (!rd(gI2c, TIM_FS1) && !rd(gI2c, TIM_FS2)) return -3;   // never configured: refuse to guess the bus timing
  IOLockLock(gLock);
  int rc = 0;
  if (rd(gI2c, TRANS_ST) & TRANS_MASTER_BUSY) rc = -4;
  if (!rc) {
    if (!(rd(gI2c, CONF) & CONF_AUTO)) wr(gI2c, CONF, rd(gI2c, CONF) | CONF_AUTO);
    wr(gI2c, INT_ST, 0xffffffffu);
    wr(gI2c, FIFO_CTL, FIFO_RX_EN | FIFO_TX_EN | (1u << 4) | (1u << 16));
    wr(gI2c, INT_EN, INT_DONE | INT_ERRORS);
    wr(gI2c, ADDR, (static_cast<uint32_t>(dev) & 0x3ff) << 10);
    wr(gI2c, CTL, (rd(gI2c, CTL) & ~CTL_RXCHON) | CTL_TXCHON | CTL_MASTER | CTL_I2C);
    wr(gI2c, TXD, reg);
    wr(gI2c, AUTO_CONF, 1u | AUTO_RUN);   // one byte, no STOP
    uint32_t st = waitInt(INT_DONE, 5000);
    if (!st) rc = -5; else if (st & INT_ERRORS) rc = -(100 + static_cast<int>((st >> 8) & 0xf));
    wr(gI2c, INT_ST, st ? st : 0xffffffffu);
    if (!rc) {
      wr(gI2c, CTL, (rd(gI2c, CTL) & ~CTL_TXCHON) | CTL_RXCHON);
      wr(gI2c, AUTO_CONF, n | AUTO_READ | AUTO_STOP | AUTO_RUN);
      unsigned got = 0;
      for (unsigned t = 0; t < 10000 && got < n; t += 10) {
        while (got < n && ((rd(gI2c, FIFO_ST) >> 16) & 0x7f)) out[got++] = static_cast<uint8_t>(rd(gI2c, RXD));
        if (got >= n) break;
        if (rd(gI2c, INT_ST) & INT_ERRORS) break;
        IODelay(10);
      }
      st = waitInt(INT_DONE, 5000);
      if (got < n) rc = st & INT_ERRORS ? -(200 + static_cast<int>((st >> 8) & 0xf)) : -6;
      wr(gI2c, INT_ST, st ? st : 0xffffffffu);
    }
  }
  wr(gI2c, CTL, rd(gI2c, CTL) & ~(CTL_RXCHON | CTL_TXCHON));
  wr(gI2c, INT_EN, 0);
  IOLockUnlock(gLock);
  return rc;
}

// Write n bytes to register `reg` of chip `dev`, one transaction with STOP. Only the TCPC, and only the registers PD needs
// (typec::allowed); the PMIC, charger and fuel gauge are never written.
bool writeAllowed(uint8_t dev, uint8_t reg, const uint8_t *d, unsigned n);
int writeRegs(uint8_t dev, uint8_t reg, const uint8_t *d, unsigned n) {
  if (!mapAll() || n > 40 || !writeAllowed(dev, reg, d, n)) return -1;
  if (!clockOn(rd(gCmu, kGateIp)) || !clockOn(rd(gCmu, kGatePclk))) return -2;
  IOLockLock(gLock);
  int rc = 0;
  if (rd(gI2c, TRANS_ST) & TRANS_MASTER_BUSY) rc = -4;
  if (!rc) {
    if (!(rd(gI2c, CONF) & CONF_AUTO)) wr(gI2c, CONF, rd(gI2c, CONF) | CONF_AUTO);
    wr(gI2c, INT_ST, 0xffffffffu);
    wr(gI2c, FIFO_CTL, FIFO_RX_EN | FIFO_TX_EN | (1u << 4) | (1u << 16));
    wr(gI2c, INT_EN, INT_DONE | INT_ERRORS);
    wr(gI2c, ADDR, (static_cast<uint32_t>(dev) & 0x3ff) << 10);
    wr(gI2c, CTL, (rd(gI2c, CTL) & ~CTL_RXCHON) | CTL_TXCHON | CTL_MASTER | CTL_I2C);
    wr(gI2c, TXD, reg);
    for (unsigned i = 0; i < n; ++i) wr(gI2c, TXD, d[i]);   // the FIFO holds 64 bytes
    wr(gI2c, AUTO_CONF, (n + 1) | AUTO_STOP | AUTO_RUN);
    const uint32_t st = waitInt(INT_DONE, 10000);
    if (!st) rc = -5; else if (st & INT_ERRORS) rc = -(100 + static_cast<int>((st >> 8) & 0xf));
    wr(gI2c, INT_ST, st ? st : 0xffffffffu);
  }
  wr(gI2c, CTL, rd(gI2c, CTL) & ~(CTL_RXCHON | CTL_TXCHON));
  wr(gI2c, INT_EN, 0);
  IOLockUnlock(gLock);
  return rc;
}

void dump(const char *what, uint8_t dev, uint8_t reg, unsigned n) {
  uint8_t b[32] = {};
  const int rc = readRegs(dev, reg, b, n);
  if (rc) { printf("i2c: %s (0x%02x reg 0x%02x): error %d (trans %#x fifo %#x)\n", what, dev, reg, rc, rd(gI2c, TRANS_ST), rd(gI2c, FIFO_ST)); return; }
  char line[3 * 32 + 1] = {};
  for (unsigned i = 0; i < n; ++i) snprintf(line + 3 * i, 4, " %02x", b[i]);
  printf("i2c: %s (0x%02x reg 0x%02x):%s\n", what, dev, reg, line);
}

int command(uint32_t stage) {
  if (stage == 1) return state();
  if (stage == 2) {
    if (state()) return 1;
    dump("TCPC vendor, product, device ID", 0x25, 0x00, 6);   // TCPCI: VENDOR_ID 0x00, PRODUCT_ID 0x02, DEVICE_ID 0x04
    dump("PMIC_ID, PMIC_REVISION", 0x66, 0x00, 2);            // MAX77759: 0x3b expected
    return 0;
  }
  if (stage == 3) {
    dump("TCPC ALERT", 0x25, 0x10, 2);
    dump("TCPC ROLE_CONTROL", 0x25, 0x1A, 1);
    dump("TCPC CC_STATUS, POWER_STATUS, FAULT_STATUS", 0x25, 0x1D, 3);
    dump("TCPC POWER_CONTROL", 0x25, 0x1C, 1);
    dump("TCPC ALERT_MASK, POWER_STATUS_MASK, FAULT_STATUS_MASK", 0x25, 0x12, 4);
    dump("TCPC TCPC_CONTROL", 0x25, 0x19, 1);
    dump("TCPC MESSAGE_HEADER_INFO, RECEIVE_DETECT", 0x25, 0x2E, 2);
    dump("TCPC TCPC_REV, PD_REV, PD_INTERFACE_REV", 0x25, 0x06, 6);
    return 0;
  }
  if (stage == 4) {   // fuel gauge (MAX1720x registers): RepSOC 0x06 (1/256 %), Temp 0x08 (1/256 C), Current 0x0A, VCell 0x1A
    uint8_t b[2];
    int soc = -1, temp = -1000, mv = -1, ma = 0;
    if (!readRegs(0x36, 0x06, b, 2)) soc = (b[0] | (b[1] << 8)) / 256;
    if (!readRegs(0x36, 0x08, b, 2)) temp = static_cast<int16_t>(b[0] | (b[1] << 8)) / 256;
    if (!readRegs(0x36, 0x1A, b, 2)) mv = static_cast<int>((b[0] | (b[1] << 8)) * 78125ull / 1000000ull);
    if (!readRegs(0x36, 0x0A, b, 2)) ma = static_cast<int16_t>(b[0] | (b[1] << 8)) * 15625 / 10000 / 5;   // 1.5625 uV / 5 mOhm
    printf("i2c: battery %d %%, %d mV, %d mA (+ charging), %d C\n", soc, mv, ma, temp);
    return 0;
  }
  printf("i2c: stage %u unknown (1 state, 2 chip IDs, 3 TCPC state, 4 battery)\n", stage);
  return -1;
}

}  // namespace i2c
