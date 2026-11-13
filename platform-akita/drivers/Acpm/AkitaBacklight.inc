// akita (Generated-by: LLM, local only): display brightness. The OLED panel (google,ak3a or ak3b) takes MIPI DCS
// SET_DISPLAY_BRIGHTNESS (0x51) with a 12-bit level, big-endian, as a DCS long write; we send it through DSIM0 (0x19440000),
// the DSI host the bootloader left running. One packet, only when both command FIFOs are empty; nothing else is touched.
//   kern.akita_acpm = 9000000 prints DSIM0's state; 9100000 + level sets the brightness (clamped to 210..2047, the normal
//   range both panels accept; ak3a 4..2047, ak3b 209..3175).
// Sources: Google's exynos_drm_dsim.c (__dsim_cmd_write_locked, dsim_write_payload) and cal_9865/regs-dsim.h, the akita
// panel drivers panel-google-ak3a.c / ak3b.c (set_brightness), the zuma DT (drmdsim@19440000).

namespace backlight {

constexpr uint64_t kDsim0 = 0x19440000;
enum : uint32_t { VERSION = 0x00, LINK0 = 0x08, CONFIG = 0x4c, INTSRC = 0x50, PKTHDR = 0x58, PAYLOAD = 0x5c,
                  FIFOCTRL = 0x68, CMD_CONFIG = 0x80, RESOL = 0x3c };
constexpr uint32_t kPhEmpty = 1u << 10, kPlEmpty = 1u << 8, kIntPhEmpty = 1u << 28, kIntPlEmpty = 1u << 29;
constexpr unsigned kMin = 210, kMax = 2047;

volatile uint8_t *gD = nullptr;
IOLock *gLockB = nullptr;
unsigned gLevel = 0;   // 0: never set by us (the bootloader's level)

inline uint32_t r(uint32_t o) { return *reinterpret_cast<volatile uint32_t *>(gD + o); }
inline void w(uint32_t o, uint32_t v) {
  *reinterpret_cast<volatile uint32_t *>(gD + o) = v;
  __asm__ volatile("dsb sy" ::: "memory");
}

bool map() {
  if (!gD) gD = reinterpret_cast<volatile uint8_t *>(ml_io_map(kDsim0, 0x1000));
  if (!gLockB) gLockB = IOLockAlloc();
  return gD && gLockB;
}

void dump() {
  if (!map()) { printf("backlight: DSIM0 not mapped\n"); return; }
  const uint32_t cfg = r(CONFIG), res = r(RESOL);
  printf("backlight: DSIM0 version %#x link0 %#x config %#x (%s mode, lanes %u) resol %ux%u fifoctrl %#x cmd_config %#x "
         "intsrc %#x, level %u\n", r(VERSION), r(LINK0), cfg, (cfg >> 18) & 1 ? "video" : "command", ((cfg >> 5) & 3) + 1,
         res & 0x1fff, (res >> 16) & 0x1fff, r(FIFOCTRL), r(CMD_CONFIG), r(INTSRC), gLevel);
}

int set(unsigned level) {
  if (!map()) return -1;
  if (level < kMin) level = kMin;
  if (level > kMax) level = kMax;
  IOLockLock(gLockB);
  int rc = 0;
  const uint32_t fifo = r(FIFOCTRL);
  if (!(fifo & kPhEmpty) || !(fifo & kPlEmpty)) {
    rc = -2;   // somebody else's packets are in flight: leave them alone
  } else {
    w(INTSRC, kIntPhEmpty | kIntPlEmpty);   // write 1 to clear
    w(PAYLOAD, 0x51u | ((level >> 8) & 0xffu) << 8 | (level & 0xffu) << 16);
    w(PKTHDR, 0x39u | (3u << 8));           // DCS long write, word count 3, no BTA
    rc = -3;
    for (unsigned t = 0; t < 2000; ++t) {   // up to 20 ms
      if (r(FIFOCTRL) & kPhEmpty) { rc = 0; break; }
      IODelay(10);
    }
    if (!rc) gLevel = level;
  }
  IOLockUnlock(gLockB);
  if (rc) printf("backlight: level %u not sent (%d, fifoctrl %#x)\n", level, rc, r(FIFOCTRL));
  return rc;
}

unsigned level() { return gLevel; }

}  // namespace backlight
