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

// macOS's own brightness (Control Center slider, brightness keys): BrightnessControl.framework's BCAppleBacklightBrtControl
// takes any service with backlight-control = true, reads IODisplayParameters (brightness and BrightnessMilliNits, each
// {min, max, value}) and sets the brightness with setProperties {"brightness-nits": nits in 16.16 fixed point}.
// We map nits linearly onto the panel's DCS levels (an OLED's level is close to linear in nits).
constexpr unsigned kMinMilliNits = 2000, kMaxMilliNits = 500000;

unsigned levelForMilliNits(unsigned mn) {
  if (mn < kMinMilliNits) mn = kMinMilliNits;
  if (mn > kMaxMilliNits) mn = kMaxMilliNits;
  return kMin + static_cast<unsigned>((static_cast<uint64_t>(mn - kMinMilliNits) * (kMax - kMin)) / (kMaxMilliNits - kMinMilliNits));
}
unsigned milliNitsForLevel(unsigned lv) {
  if (lv < kMin) lv = kMin;
  if (lv > kMax) lv = kMax;
  return kMinMilliNits + static_cast<unsigned>((static_cast<uint64_t>(lv - kMin) * (kMaxMilliNits - kMinMilliNits)) / (kMax - kMin));
}

}  // namespace backlight

class AkitaBacklightService : public IOService {
  OSDeclareDefaultStructors(AkitaBacklightService);
public:
  IOReturn setProperties(OSObject *properties) override;
  void publish(unsigned milliNits);
};
OSDefineMetaClassAndStructors(AkitaBacklightService, IOService)

static OSDictionary *rangeDict(unsigned mn, unsigned mx, unsigned value) {
  OSDictionary *d = OSDictionary::withCapacity(3);
  if (!d) return nullptr;
  const struct { const char *k; unsigned v; } e[] = {{"min", mn}, {"max", mx}, {"value", value}};
  for (auto &x : e) {
    OSNumber *n = OSNumber::withNumber(x.v, 32);
    if (n) { d->setObject(x.k, n); n->release(); }
  }
  return d;
}

void AkitaBacklightService::publish(unsigned milliNits) {
  OSDictionary *params = OSDictionary::withCapacity(2);
  if (!params) return;
  const unsigned user = static_cast<unsigned>((static_cast<uint64_t>(milliNits - backlight::kMinMilliNits) * 65536) /
                                              (backlight::kMaxMilliNits - backlight::kMinMilliNits));
  OSDictionary *b = rangeDict(0, 65536, user), *n = rangeDict(backlight::kMinMilliNits, backlight::kMaxMilliNits, milliNits);
  if (b) { params->setObject("brightness", b); b->release(); }
  if (n) { params->setObject("BrightnessMilliNits", n); n->release(); }
  setProperty("IODisplayParameters", params);
  params->release();
  OSNumber *bn = OSNumber::withNumber(static_cast<unsigned long long>(milliNits) * 65536 / 1000, 64);
  if (bn) { setProperty("brightness-nits", bn); bn->release(); }
}

IOReturn AkitaBacklightService::setProperties(OSObject *properties) {
  OSDictionary *d = OSDynamicCast(OSDictionary, properties);
  if (!d) return kIOReturnBadArgument;
  static unsigned logged;
  OSNumber *nits = OSDynamicCast(OSNumber, d->getObject("brightness-nits"));
  if (nits) {
    const uint64_t fixed = nits->unsigned64BitValue();   // nits, 16.16
    const unsigned mn = static_cast<unsigned>(fixed * 1000 / 65536);
    const unsigned lv = backlight::levelForMilliNits(mn);
    const int rc = backlight::set(lv);
    if (logged++ < 60) printf("backlight: macOS asks %u.%03u nits -> level %u%s\n", mn / 1000, mn % 1000, lv, rc ? " (not sent)" : "");
    publish(backlight::milliNitsForLevel(lv));
    return kIOReturnSuccess;
  }
  if (logged++ < 60) {
    OSCollectionIterator *it = OSCollectionIterator::withCollection(d);
    while (it) {
      const OSSymbol *k = OSDynamicCast(OSSymbol, it->getNextObject());
      if (!k) break;
      printf("backlight: setProperties key %s\n", k->getCStringNoCopy());
    }
    if (it) it->release();
  }
  return kIOReturnSuccess;
}

namespace backlight {
AkitaBacklightService *gService = nullptr;
thread_call_t gServiceCall = nullptr;

// published once the framebuffer is there, as its child (where a Mac's backlight sits next to its panel)
void publishService(thread_call_param_t, thread_call_param_t) {
  OSDictionary *match = IOService::serviceMatching("NativeGOPDisplay");
  IOService *fb = match ? IOService::waitForMatchingService(match, 60ull * NSEC_PER_SEC) : nullptr;
  if (match) match->release();
  IOService *parent = fb ? fb : IOService::getPlatform();
  gService = new AkitaBacklightService;
  if (!gService || !gService->init() || !gService->attach(parent)) { printf("backlight: could not make the service\n"); return; }
  gService->setProperty("backlight-control", true);
  // BCAppleBacklightBrtControl only takes setNits from a DFR (Touch Bar) control or one with force-brightness-control
  // (its _override, read as data): a Mac's own panel goes through DCP instead, which we do not have (error 16 otherwise)
  const uint32_t one = 1;
  OSData *force = OSData::withBytes(&one, sizeof one);
  if (force) { gService->setProperty("force-brightness-control", force); force->release(); }
  gService->setName("backlight");
  gService->publish(milliNitsForLevel(gLevel ? gLevel : 1080));
  gService->registerService();
  printf("backlight: service published under %s (backlight-control)\n", fb ? "NativeGOPDisplay" : "the platform");
  if (fb) fb->release();
}

void boot() {
  unsigned on = 1;
  PE_parse_boot_argn("akita-backlight", &on, sizeof on);
  if (!on || gServiceCall) return;
  gServiceCall = thread_call_allocate(publishService, nullptr);
  if (gServiceCall) thread_call_enter(gServiceCall);
}

}  // namespace backlight
