// SPDX-License-Identifier: BSD-2-Clause-Patent
// akita (Generated-by: LLM, local only): staged bring-up of the Goodix "nottingham" touch controller on SPI 20 (USI2 of HSI0).
// Included by AkitaAcpm.cpp; asked through sysctl kern.akita_acpm = 2000000 + stage (the answers go to the kernel log):
//   1  read-only: USI2's clock mux/divider/gates (CMU_HSI0), the USI's mode (SYSREG_HSI0), the reset and IRQ pins
//   2  USI2's gates on (manual), its clock from OSCCLK / 2, the USI in SPI mode, the SPI controller reset into master mode 0,
//      chip select by hand; then its registers
//   3  the Goodix check (brl_dev_confirm): write eight 0xAA at 0x10000 and read them back, with no reset
//   4  a reset pulse on gpp21-2 (low 2 ms, high, 150 ms), then 3 again
//   5  the IC info block at 0x10070 (length, then the block): touch data, command and point record addresses
//   6  20 s of touch events: poll the IRQ pin (gpn0-0, low when an event waits), read the head and the points, ack with one 0x00
// Nothing here writes the controller's flash: never 0x12400 / 0x23800, never 0x55 to 0x10000 (that starts its ISP).
// Sources: Google's goodix_touch (goodix_brl_spi.c, goodix_brl_hw.c, akita branch), Linux spi-s3c64xx.c and exynos-usi.c,
// ref/zuma-linux/clk-zuma.c (USI2 clock path), the akita device tree (zuma-common-touch.dtsi). See investigacion-tactil.md.

namespace touch {

constexpr uint64_t kCmuHsi0 = 0x11000000, kSysregHsi0 = 0x11020000, kSpi20 = 0x111D0000;
constexpr uint64_t kPinAlive = 0x15060000, kPinP = 0x10C40000;
constexpr uint32_t kMuxUsi2 = 0x101c, kDivUsi2 = 0x181c, kGateSrc = 0x2120, kGateIp = 0x20fc, kGatePclk = 0x2100;
constexpr uint32_t kUsiSwConf = 0x101c;                        // SYSREG_HSI0: USI2's protocol (USI_V2: 1 UART, 2 SPI, 4 I2C)
constexpr uint32_t kGpn0 = 0x00, kGpp21 = 0x40;                // bank offsets (gpn0 first of its block, gpp21 third of 0x20 banks)
constexpr uint32_t kResetPin = 2, kIrqPin = 0;
// SPI (spi-s3c64xx): registers and bits
enum : uint32_t { CH_CFG = 0x00, MODE_CFG = 0x08, CS_REG = 0x0c, INT_EN = 0x10, STATUS = 0x14, TX_DATA = 0x18, RX_DATA = 0x1c,
                  PACKET_CNT = 0x20, PENDING_CLR = 0x24, SWAP_CFG = 0x28, FB_CLK = 0x2c, USI_CON = 0xc4, USI_OPTION = 0xc8 };
constexpr uint32_t CH_SW_RST = 1u << 5, CH_RXON = 1u << 1, CH_TXON = 1u << 0;
constexpr uint32_t CS_NSSOUT = 1u << 0;                        // 1: chip select inactive (high)

volatile uint8_t *gCmu, *gSys, *gSpi, *gPinA, *gPinP;
bool gSpiReady;

inline uint32_t rd(volatile uint8_t *b, uint32_t o) { return *reinterpret_cast<volatile uint32_t *>(b + o); }
inline void wr(volatile uint8_t *b, uint32_t o, uint32_t v) {
  *reinterpret_cast<volatile uint32_t *>(b + o) = v;
  __asm__ volatile("dsb sy" ::: "memory");
}

bool mapAll() {
  if (!gCmu) gCmu = reinterpret_cast<volatile uint8_t *>(ml_io_map(kCmuHsi0, 0x3000));
  if (!gSys) gSys = reinterpret_cast<volatile uint8_t *>(ml_io_map(kSysregHsi0, 0x2000));
  if (!gPinA) gPinA = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPinAlive, 0x1000));
  if (!gPinP) gPinP = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPinP, 0x1000));
  return gCmu && gSys && gPinA && gPinP;
}

void snapshot() {
  printf("touch: CMU_HSI0 mux 0x101c=%#x div 0x181c=%#x gates src 0x2120=%#x ip 0x20fc=%#x pclk 0x2100=%#x\n",
         rd(gCmu, kMuxUsi2), rd(gCmu, kDivUsi2), rd(gCmu, kGateSrc), rd(gCmu, kGateIp), rd(gCmu, kGatePclk));
  printf("touch: SYSREG_HSI0 usi2 sw_conf 0x101c=%#x\n", rd(gSys, kUsiSwConf));
  printf("touch: gpn0 (IRQ is pin 0) con %#x dat %#x pud %#x; ", rd(gPinA, kGpn0), rd(gPinA, kGpn0 + 4), rd(gPinA, kGpn0 + 8));
  printf("gpp19 con %#x dat %#x, gpp20 con %#x dat %#x, gpp21 (reset is pin 2) con %#x dat %#x pud %#x\n",
         rd(gPinP, 0x00), rd(gPinP, 0x04), rd(gPinP, 0x20), rd(gPinP, 0x24), rd(gPinP, kGpp21), rd(gPinP, kGpp21 + 4), rd(gPinP, kGpp21 + 8));
}

uint32_t gateOn(uint32_t off) {
  const uint32_t before = rd(gCmu, off);
  wr(gCmu, off, (before & ~(1u << 28)) | (1u << 20) | (1u << 21));   // HWACG off, manual mode, clock on
  return before;
}

bool spiSetup() {
  if (!gSpi) gSpi = reinterpret_cast<volatile uint8_t *>(ml_io_map(kSpi20, 0x1000));
  if (!gSpi) return false;
  const uint32_t g1 = gateOn(kGatePclk), g2 = gateOn(kGateSrc);
  wr(gCmu, kMuxUsi2, (rd(gCmu, kMuxUsi2) & ~1u) | 1u);              // USI2 from OSCCLK (24.576 MHz)
  wr(gCmu, kDivUsi2, (rd(gCmu, kDivUsi2) & ~0xfu) | 1u);            // divide by 2: 12.288 MHz into the SPI (its SCLK lower still)
  const uint32_t g3 = gateOn(kGateIp);
  IODelay(50);
  printf("touch: gates were src %#x ip %#x pclk %#x; now %#x %#x %#x; mux %#x div %#x\n", g2, g3, g1,
         rd(gCmu, kGateSrc), rd(gCmu, kGateIp), rd(gCmu, kGatePclk), rd(gCmu, kMuxUsi2), rd(gCmu, kDivUsi2));
  wr(gSys, kUsiSwConf, (rd(gSys, kUsiSwConf) & ~7u) | 2u);           // USI_V2 SPI
  wr(gSpi, USI_CON, rd(gSpi, USI_CON) & ~1u);                         // out of USI reset
  wr(gSpi, USI_OPTION, (rd(gSpi, USI_OPTION) & ~(1u << 2)) | (1u << 1));   // CLKREQ_ON, not CLKSTOP_ON
  wr(gSpi, CS_REG, CS_NSSOUT);                                         // manual chip select, inactive
  wr(gSpi, CH_CFG, CH_SW_RST);
  IODelay(10);
  wr(gSpi, CH_CFG, 0);                                                 // master, CPOL 0, CPHA 0 (mode 0), channels off
  wr(gSpi, MODE_CFG, 0);                                               // byte transfers, no DMA, no trailing count
  wr(gSpi, INT_EN, 0);
  wr(gSpi, PENDING_CLR, 0x1f);
  wr(gSpi, PENDING_CLR, 0);
  wr(gSpi, SWAP_CFG, 0);
  wr(gSpi, PACKET_CNT, 0);
  printf("touch: SPI ch_cfg %#x mode %#x cs %#x status %#x usi_con %#x usi_opt %#x sw_conf %#x\n", rd(gSpi, CH_CFG),
         rd(gSpi, MODE_CFG), rd(gSpi, CS_REG), rd(gSpi, STATUS), rd(gSpi, USI_CON), rd(gSpi, USI_OPTION), rd(gSys, kUsiSwConf));
  gSpiReady = true;
  return true;
}

// One full-duplex transfer with the chip selected throughout, byte by byte through the FIFOs (64 deep: 32 ahead at most).
bool xfer(const uint8_t *tx, uint8_t *rx, uint32_t len) {
  wr(gSpi, CH_CFG, CH_SW_RST);
  wr(gSpi, CH_CFG, 0);
  wr(gSpi, CH_CFG, CH_TXON | CH_RXON);
  wr(gSpi, CS_REG, 0);                                                 // select
  uint32_t sent = 0, got = 0, spins = 0;
  while (got < len) {
    const uint32_t st = rd(gSpi, STATUS);
    const uint32_t txLvl = (st >> 6) & 0x1ff, rxLvl = (st >> 15) & 0x1ff;
    if (rxLvl) {
      const uint8_t b = static_cast<uint8_t>(rd(gSpi, RX_DATA));
      if (rx) rx[got] = b;
      ++got;
      spins = 0;
      continue;
    }
    if (sent < len && sent - got < 32 && txLvl < 32) {
      wr(gSpi, TX_DATA, tx ? tx[sent] : 0xff);
      ++sent;
      continue;
    }
    if (++spins > 200000) break;
  }
  wr(gSpi, CS_REG, CS_NSSOUT);                                         // deselect
  wr(gSpi, CH_CFG, 0);
  if (got < len) printf("touch: transfer stalled at %u of %u (status %#x)\n", got, len, rd(gSpi, STATUS));
  return got == len;
}

bool readReg(uint32_t addr, uint8_t *out, uint32_t len) {
  if (len > 512) return false;
  uint8_t tx[8 + 512], rx[8 + 512];
  memset(tx, 0xff, sizeof tx);
  tx[0] = 0xF1; tx[1] = addr >> 24; tx[2] = addr >> 16; tx[3] = addr >> 8; tx[4] = addr;
  if (!xfer(tx, rx, 8 + len)) return false;
  memcpy(out, rx + 8, len);
  return true;
}

bool writeReg(uint32_t addr, const uint8_t *data, uint32_t len) {
  if (len > 256) return false;
  uint8_t tx[5 + 256];
  tx[0] = 0xF0; tx[1] = addr >> 24; tx[2] = addr >> 16; tx[3] = addr >> 8; tx[4] = addr;
  memcpy(tx + 5, data, len);
  return xfer(tx, nullptr, 5 + len);
}

void pinOutput(volatile uint8_t *bank, uint32_t pin, bool high) {
  const uint32_t con = rd(bank, 0);
  wr(bank, 4, high ? rd(bank, 4) | (1u << pin) : rd(bank, 4) & ~(1u << pin));
  wr(bank, 0, (con & ~(0xfu << (pin * 4))) | (1u << (pin * 4)));      // output
}

bool devConfirm() {
  uint8_t aa[8], back[8];
  memset(aa, 0xAA, sizeof aa);
  for (int tries = 0; tries < 3; ++tries) {
    memset(back, 0, sizeof back);
    const bool w = writeReg(0x10000, aa, 8), r = readReg(0x10000, back, 8);
    printf("touch: confirm try %d: write %d read %d: %02x %02x %02x %02x %02x %02x %02x %02x\n", tries, w, r, back[0], back[1],
           back[2], back[3], back[4], back[5], back[6], back[7]);
    if (w && r && !memcmp(aa, back, 8)) return true;
    IOSleep(5);
  }
  return false;
}

// The IC info block's misc part: where the touch data and the commands are (convert_ic_info, packed little-endian).
struct Info { uint32_t cmdAddr, touchAddr; uint16_t touchHeadLen, pointLen; bool ok; };
Info gInfo;

uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

void readInfo() {
  uint8_t len2[2] = {};
  readReg(0x10070, len2, 2);
  const uint32_t len = le16(len2);
  uint8_t ver[16] = {};
  readReg(0x10014, ver, sizeof ver);
  printf("touch: fw version block %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; ic info length %u\n", ver[0],
         ver[1], ver[2], ver[3], ver[4], ver[5], ver[6], ver[7], ver[8], ver[9], ver[10], ver[11], len);
  if (len < 16 || len > 512) return;
  static uint8_t b[512];
  if (!readReg(0x10070, b, len)) return;
  for (uint32_t i = 0; i < len; i += 32) {
    char line[120]; int n = 0;
    for (uint32_t j = i; j < len && j < i + 32; ++j) n += snprintf(line + n, sizeof line - n, "%02x", b[j]);
    printf("touch: info+%03u %s\n", i, line);
  }
  // convert_ic_info: length(2), version(1+1+1+1+4+1+1+1+1+1+3 = 16), feature(5 x 2 = 10), param(5 bytes, then counted u16 arrays:
  // active_scan_rate[active_scan_rate_num], mutual_freq[mutual_freq_num], self_tx_freq[...], self_rx_freq[...], stylus_freq[...]),
  // then misc. Walk it as the driver does.
  uint32_t p = 2 + 16 + 10;
  const uint8_t drv = b[p], sen = b[p + 1], btn = b[p + 2], force = b[p + 3], rates = b[p + 4];
  p += 5;
  p += 2u * rates;
  for (int k = 0; k < 4 && p < len; ++k) { const uint8_t n = b[p]; p += 1 + 2u * n; }   // mutual, self tx, self rx, stylus freqs
  if (p + 40 > len) { printf("touch: info parse ran out at %u (drv %u sen %u btn %u force %u rates %u)\n", p, drv, sen, btn, force, rates); return; }
  // misc: cmd_addr u32, cmd_max_len u16, cmd_reply_addr u32, cmd_reply_len u16, fw_state_addr u32, fw_state_len u16,
  // fw_buffer_addr u32, fw_buffer_max_len u16, frame_data_addr u32, frame_data_head_len u16, fw_attr_len u16, fw_log_len u16,
  // pack_max_num u8, pack_compress_version u8, stylus_struct_len u16, mutual_struct_len u16, self_struct_len u16,
  // noise_struct_len u16, touch_data_addr u32, touch_data_head_len u16, point_struct_len u16, ...
  const uint8_t *m = b + p;
  gInfo.cmdAddr = le32(m);
  uint32_t q = 4 + 2 + 4 + 2 + 4 + 2 + 4 + 2 + 4 + 2 + 2 + 2 + 1 + 1 + 2 + 2 + 2 + 2;
  gInfo.touchAddr = le32(m + q);
  gInfo.touchHeadLen = le16(m + q + 4);
  gInfo.pointLen = le16(m + q + 6);
  gInfo.ok = gInfo.cmdAddr && gInfo.touchAddr && gInfo.pointLen >= 8 && gInfo.pointLen <= 32;
  printf("touch: drv %u sen %u; cmd_addr %#x touch_data_addr %#x head %u point_struct_len %u (%s)\n", drv, sen, gInfo.cmdAddr,
         gInfo.touchAddr, gInfo.touchHeadLen, gInfo.pointLen, gInfo.ok ? "ok" : "suspicious: check the walk against the dump");
}

void listen(unsigned seconds) {
  if (!gInfo.ok) { printf("touch: no IC info yet (stage 5)\n"); return; }
  const uint64_t end = mach_absolute_time();
  uint64_t deadline;
  clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
  (void)end;
  unsigned events = 0, shown = 0;
  while (mach_absolute_time() < deadline) {
    const bool low = !(rd(gPinA, kGpn0 + 4) & (1u << kIrqPin));
    if (!low) { IODelay(2000); continue; }
    uint8_t buf[8 + 10 * 32] = {};
    const uint32_t len = 8 + 10u * gInfo.pointLen;
    if (!readReg(gInfo.touchAddr, buf, len > sizeof buf ? sizeof buf : len)) break;
    ++events;
    const uint8_t type = buf[0] >> 4, touches = buf[2] & 0x0f;
    if (shown < 60) {
      ++shown;
      char line[200]; int n = snprintf(line, sizeof line, "touch: event type %#x touches %u:", buf[0], touches);
      for (unsigned i = 0; i < touches && i < 5; ++i) {
        const uint8_t *d = buf + 8 + i * gInfo.pointLen;
        n += snprintf(line + n, sizeof line - n, " [id %u x %u y %u w %u]", d[0] >> 4, le16(d + 2), le16(d + 4), le16(d + 6));
      }
      if (!(type & 0x8)) n += snprintf(line + n, sizeof line - n, " head %02x %02x %02x %02x %02x %02x %02x %02x", buf[0], buf[1],
                                       buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
      printf("%s\n", line);
    }
    const uint8_t zero = 0;
    writeReg(gInfo.touchAddr, &zero, 1);                                  // the ack: one 0x00 at the touch data address
  }
  printf("touch: listened %u s: %u events\n", seconds, events);
}

// The touch rails on the main PMIC (S2MPG14, PM block = I2C type 0x01) through ACPM's PMIC channel 2 (zuma-pmic.dtsi:
// s2mpg14mfd acpm-ipc-channel = <2>; acpm_mfd.c: cmd[0] = type << 8 | reg (channel field 0), cmd[1] = func | value << 8 |
// mask << 16, the answer's value in cmd[1] bits 15..8 and its status in bits 31..24; FUNC_READ 0, FUNC_UPDATE 2).
// LDO4M (TS1 AVDD) at 0x2E: 1.8 V + 25 mV * sel, 3.0 V = 0x30; LDO25M (TS1 DVDD) at 0x43: 0.7 V + 25 mV * sel, 1.8 V = 0x2C;
// bit 7 enables (s2mpg14-regulator.c).
constexpr unsigned kPmicChan = 2;
constexpr uint32_t kPmType = 0x01, kL4mCtrl = 0x2E, kL25mCtrl = 0x43;

int pmicRead(uint32_t reg, uint8_t *value) {
  uint32_t w[4] = {(kPmType << 8) | (reg & 0xff), 0 /* FUNC_READ */, 0, 0};
  const int r = transfer(kPmicChan, w, 4, true);
  if (r != 0) return r;
  if ((w[1] >> 24) & 0xff) return 1000 + static_cast<int>((w[1] >> 24) & 0xff);
  *value = static_cast<uint8_t>((w[1] >> 8) & 0xff);
  return 0;
}

int pmicUpdate(uint32_t reg, uint8_t value, uint8_t mask) {
  uint32_t w[4] = {(kPmType << 8) | (reg & 0xff), 2u /* FUNC_UPDATE */ | (uint32_t(value) << 8) | (uint32_t(mask) << 16), 0, 0};
  const int r = transfer(kPmicChan, w, 4, true);
  if (r != 0) return r;
  return ((w[1] >> 24) & 0xff) ? 1000 + static_cast<int>((w[1] >> 24) & 0xff) : 0;
}

void railsShow(const char *when) {
  uint8_t l4 = 0, l25 = 0;
  const int a = pmicRead(kL4mCtrl, &l4), b = pmicRead(kL25mCtrl, &l25);
  printf("touch: rails %s: LDO4M ctrl %#x (%s, %u mV) [%d], LDO25M ctrl %#x (%s, %u mV) [%d]\n", when, l4, (l4 & 0x80) ? "on" : "off",
         1800 + 25u * (l4 & 0x3f), a, l25, (l25 & 0x80) ? "on" : "off", 700 + 25u * (l25 & 0x3f), b);
}

// The live driver (boot-arg akita-touch=1): a kernel thread brings the controller up (rails, SPI, reset, check, IC info) and then
// polls the IRQ pin, 4 ms apart while a finger was seen in the last half second, 16 ms otherwise; each event's first finger goes
// to the input driver's sink (NativeRecoveryInput registers it: akita_touch_set_sink) in panel coordinates (1080 x 2400,
// portrait), down while a finger is on the glass, up when none is.
void (*volatile gSink)(void *, int, int, int);
void *volatile gSinkCtx;
bool gLiveStarted;

bool bringUp() {
  IOLockLock(gAcpmLock);
  const bool acpm = probe();
  int r = -1;
  if (acpm) {
    uint8_t l4 = 0, l25 = 0;
    pmicRead(kL4mCtrl, &l4); pmicRead(kL25mCtrl, &l25);
    if ((l25 & 0xbf) != 0xac) { pmicUpdate(kL25mCtrl, 0x2C, 0x3F); pmicUpdate(kL25mCtrl, 0x80, 0x80); IOSleep(3); }
    if ((l4 & 0xbf) != 0xb0) { pmicUpdate(kL4mCtrl, 0x30, 0x3F); pmicUpdate(kL4mCtrl, 0x80, 0x80); IOSleep(15); }
    r = 0;
  }
  IOLockUnlock(gAcpmLock);
  if (r != 0 || !mapAll() || !spiSetup()) { printf("touch: bring-up failed (acpm %d)\n", acpm); return false; }
  pinOutput(gPinP + kGpp21, kResetPin, false); IOSleep(2); pinOutput(gPinP + kGpp21, kResetPin, true); IOSleep(150);
  if (!devConfirm()) { printf("touch: the controller does not answer\n"); return false; }
  readInfo();
  return gInfo.ok;
}

enum GestureState { kIdle, kPending, kDrag, kHeld, kScroll };
GestureState gState = kIdle;
int gSx, gSy, gScrollRef;
uint64_t gT0, gHoldTicks;

void liveLoop(void *, wait_result_t) {
  nanoseconds_to_absolutetime(550000000ull, &gHoldTicks);
  if (!bringUp()) return;
  printf("touch: live (touch data at %#x, %u-byte points)\n", gInfo.touchAddr, gInfo.pointLen);
  uint64_t lastSeen = 0, activeTicks = 0;
  nanoseconds_to_absolutetime(500000000ull, &activeTicks);
  bool down = false;
  int lx = 0, ly = 0;
  unsigned errors = 0, delivered = 0;
  for (;;) {
    const uint64_t now = mach_absolute_time();
    IOSleep(now - lastSeen < activeTicks ? 4 : 16);
    if (rd(gPinA, kGpn0 + 4) & (1u << kIrqPin)) continue;   // high: nothing waits
    uint8_t buf[8 + 10 * 32] = {};
    const uint32_t len = 8 + 2u * gInfo.pointLen;            // the head and the first two points are enough here
    if (!readReg(gInfo.touchAddr, buf, len)) { if (++errors < 10) printf("touch: read failed\n"); continue; }
    const uint8_t zero = 0;
    writeReg(gInfo.touchAddr, &zero, 1);
    if (!(buf[0] & 0x80)) continue;                          // not a touch event (request, status, gesture): ignored for now
    lastSeen = mach_absolute_time();
    const unsigned touches = buf[2] & 0x0f;
    const uint8_t *d0 = buf + 8, *d1 = buf + 8 + gInfo.pointLen;
    const int x = le16(d0 + 2), y = le16(d0 + 4);
    auto send = [&](int a, int b, int code) { if (gSink) { gSink(gSinkCtx, a, b, code); ++delivered; } };
    // Gestures (sink codes: 0 up, 1 left button down at, 2 right click at, 3 wheel by a, 4 move to): a tap clicks, a finger
    // that moves drags, one held still for 550 ms right-clicks, two fingers scroll (one wheel line per 30 panel units of their
    // mean along the panel's short side, which is the desktop's vertical in landscape).
    if (touches >= 2) {
      const int mean = (x + le16(d1 + 2)) / 2;
      if (gState != kScroll) {
        if (gState == kDrag) send(lx, ly, 0);
        gState = kScroll;
        gScrollRef = mean;
      } else {
        while (mean - gScrollRef >= 30) { send(-1, 0, 3); gScrollRef += 30; }
        while (gScrollRef - mean >= 30) { send(1, 0, 3); gScrollRef -= 30; }
      }
    } else if (touches == 1) {
      if (gState == kIdle) {
        gState = kPending; gSx = x; gSy = y; gT0 = lastSeen;
        send(x, y, 4);
      } else if (gState == kPending) {
        const int dx = x - gSx, dy = y - gSy;
        if (dx * dx + dy * dy > 25 * 25) {
          send(gSx, gSy, 1); send(x, y, 1); gState = kDrag;
        } else if (lastSeen - gT0 > gHoldTicks) {
          send(gSx, gSy, 2); gState = kHeld;
        }
      } else if (gState == kDrag && (x != lx || y != ly)) {
        send(x, y, 1);
      }
      lx = x; ly = y;
    } else {
      if (gState == kPending) { send(gSx, gSy, 1); send(gSx, gSy, 0); }   // a tap
      else if (gState == kDrag) send(lx, ly, 0);
      gState = kIdle;
    }
    (void)down;
    if (delivered && delivered % 2000 == 0) printf("touch: %u reports delivered\n", delivered);
  }
}

void startLive() {
  if (gLiveStarted) return;
  gLiveStarted = true;
  thread_t thread;
  if (kernel_thread_start(liveLoop, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}

int command(unsigned stage) {
  if (!mapAll()) { printf("touch: could not map\n"); return -1; }
  switch (stage) {
    case 1: snapshot(); return 0;
    case 2: return spiSetup() ? 0 : -1;
    case 3: if (!gSpiReady) { printf("touch: stage 2 first\n"); return -1; }
            printf("touch: confirm %s\n", devConfirm() ? "OK: the controller answers" : "failed"); return 0;
    case 4: if (!gSpiReady) { printf("touch: stage 2 first\n"); return -1; }
            pinOutput(gPinP + kGpp21, kResetPin, false); IOSleep(2); pinOutput(gPinP + kGpp21, kResetPin, true); IOSleep(150);
            snapshot();
            printf("touch: after reset: confirm %s\n", devConfirm() ? "OK: the controller answers" : "failed"); return 0;
    case 5: if (!gSpiReady) { printf("touch: stage 2 first\n"); return -1; }
            readInfo(); return 0;
    case 6: if (!gSpiReady) { printf("touch: stage 2 first\n"); return -1; }
            listen(20); return 0;
    case 9: startLive(); return 0;
    case 7: if (!probe()) { printf("touch: no ACPM\n"); return -1; }
            railsShow("now"); return 0;
    case 8: {
      if (!probe()) { printf("touch: no ACPM\n"); return -1; }
      railsShow("before");
      // brl_power_on: iovdd (DVDD 1.8 V) first, 3 ms, then avdd (AVDD 3.0 V), 15 ms; the voltage before the enable
      int r1 = pmicUpdate(kL25mCtrl, 0x2C, 0x3F), r2 = pmicUpdate(kL25mCtrl, 0x80, 0x80);
      IOSleep(3);
      int r3 = pmicUpdate(kL4mCtrl, 0x30, 0x3F), r4 = pmicUpdate(kL4mCtrl, 0x80, 0x80);
      IOSleep(15);
      printf("touch: rails set: %d %d %d %d\n", r1, r2, r3, r4);
      railsShow("after");
      return 0;
    }
  }
  printf("touch: stage %u unknown\n", stage);
  return -1;
}

}  // namespace touch

extern "C" void akita_touch_set_sink(void (*sink)(void *, int, int, int), void *ctx);
extern "C" void akita_touch_set_sink(void (*sink)(void *, int, int, int), void *ctx) {
  touch::gSinkCtx = ctx;
  touch::gSink = sink;
  printf("touch: input sink %s\n", sink ? "registered" : "removed");
}
