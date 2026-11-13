// akita (Generated-by: LLM, local only): USB host on the Pixel 8a's DWC3 (xHCI), for a keyboard and a mouse behind a USB-C hub.
//
// The USB PD engine in the kernel (AkitaTypec.inc) makes the phone the host (DFP) when a hub asks for the data role; then this
// file stops the gadget (AkitaUsbGadget.h), turns the DWC3 to host (GCTL.PRTCAPDIR = host, as Google's dwc3_otg_start_host;
// the eUSB2 PHY and the repeater have no host-specific setting) and runs a minimal polled xHCI driver:
//   - root ports (USB 2 only: the SuperSpeed PHY is not brought up), port reset, Enable Slot, Address Device;
//   - hubs (two tiers): hub descriptor, the slot marked as a hub, port power, reset, transaction translator for low/full speed;
//   - HID boot keyboards and mice: SET_PROTOCOL(boot), an interrupt IN ring each, reports to NativeRecoveryInput's devices.
// Sources: xHCI 1.2 specification (rings, contexts, commands, PORTSC), USB 2.0 chapter 9 and 11 (requests, hub class), HID 1.11
// (boot protocol), Google dwc3-exynos-otg.c (host switch), Linux xhci-ring.c/xhci-hub.c for the PORTSC write rules.
namespace usbhost {

extern "C" int akita_typec_host(void);   // kernel: the PD engine made the phone the host (AkitaTypec.inc)
extern "C" void akita_usbhost_set_hook(int (*hook)(unsigned));   // kernel: kern.akita_acpm 6000000 + n reaches command()

void (*gKeys)(void *ctx, const uint8_t report[8]) = nullptr;
void (*gMouse)(void *ctx, uint8_t buttons, int dx, int dy, int wheel) = nullptr;
void *gCtx = nullptr;

// DWC3 globals
enum : uint32_t { GSBUSCFG0 = 0xc100, GCTL = 0xc110, GSTS = 0xc118, GUCTL1 = 0xc11c, GSNPSID = 0xc120, GUCTL = 0xc12c,
                  GUSB2PHYCFG = 0xc200, GFLADJ = 0xc630 };
volatile bool gHubPollPaused = false, gRestartRequest = false;
// xHCI capability registers
enum : uint32_t { CAPLENGTH = 0x00, HCSPARAMS1 = 0x04, HCSPARAMS2 = 0x08, HCCPARAMS1 = 0x10, DBOFF = 0x14, RTSOFF = 0x18 };
// operational (from op base)
enum : uint32_t { USBCMD = 0x00, USBSTS = 0x04, PAGESIZE = 0x08, CRCR = 0x18, DCBAAP = 0x30, CONFIG = 0x38, PORTSC0 = 0x400 };
constexpr uint32_t CMD_RUN = 1u << 0, CMD_HCRST = 1u << 1, STS_HCH = 1u << 0, STS_CNR = 1u << 11;
constexpr uint32_t PS_CCS = 1u << 0, PS_PED = 1u << 1, PS_PR = 1u << 4, PS_PP = 1u << 9, PS_CSC = 1u << 17, PS_PEC = 1u << 18,
                   PS_PRC = 1u << 21, PS_CHANGES = 0x7fu << 17;
// TRB types
enum : uint32_t { T_NORMAL = 1, T_SETUP = 2, T_DATA = 3, T_STATUS = 4, T_LINK = 6, T_ENABLE_SLOT = 9, T_ADDRESS = 11,
                  T_CONFIG_EP = 12, T_EVAL_CTX = 13, T_DISABLE_SLOT = 10, T_NOOP_CMD = 23, E_TRANSFER = 32, E_COMMAND = 33, E_PORT = 34 };

// DMA layout (one physically contiguous buffer, below 64 GiB: the controller does 36-bit DMA)
constexpr uint32_t kDmaBytes = 0x80000, O_DCBAA = 0x0, O_CMD = 0x1000, O_EVT = 0x2000, O_ERST = 0x3000, O_SPA = 0x3400,
                   O_SP = 0x10000, O_DEV = 0x40000, kDevStride = 0x4000, kMaxDev = 8, kRing = 64, kEvtTrbs = 256;
// per device, from its base: device context, input context, EP0 ring, three interrupt rings, control data, reports
constexpr uint32_t D_OUT = 0x0, D_IN = 0x800, D_EP0 = 0x1200, D_INT = 0x1600, D_CTRL = 0x2200, D_REP = 0x2a00;

struct Ring { uint32_t off; uint32_t enq; uint32_t cycle; };
struct Dev {
  bool used, hub, configured;
  uint8_t halted;   // bit per interrupt endpoint
  uint8_t slot, speed, rootPort, depth, ttSlot, ttPort, hubPorts;
  uint32_t route;
  Ring ep0;
  uint8_t nInt; uint8_t intDci[3]; uint8_t intKind[3]; uint16_t intMps[3]; Ring intRing[3];
  uint8_t hubPortStatus[16];
  uint8_t hubPortFails[16];
};

struct Host {
  IOBufferMemoryDescriptor *md = nullptr;
  uint8_t *va = nullptr;
  uint64_t pa = 0;
  uint32_t op = 0, rt = 0, db = 0;
  uint32_t maxSlots = 0, maxPorts = 0, ctxSize = 32;
  Ring cmd;
  uint32_t evtDeq = 0, evtCycle = 1;
  Dev dev[kMaxDev];
  bool running = false;
  uint32_t reports = 0, keyReports = 0, mouseReports = 0;
  // the last command / transfer completion seen by waitFor()
  uint32_t lastCode = 0, lastSlot = 0, lastParam = 0, lastLen = 0;
};
Host gH;
uint32_t gBadRoute[8]; unsigned gBadRoutes;   // devices that failed to address: never again (they survive a root port restart)
bool badRoute(uint32_t r) { for (unsigned i = 0; i < gBadRoutes; ++i) if (gBadRoute[i] == r) return true; return false; }

inline uint32_t r32(uint32_t o) { return dwc3Read(o); }
inline void w32(uint32_t o, uint32_t v) { dwc3Write(o, v); }
inline uint64_t pa(uint32_t off) { return gH.pa + off; }
inline uint32_t *trbAt(uint32_t off, uint32_t i) { return reinterpret_cast<uint32_t *>(gH.va + off + 16 * i); }
inline void sync() { __asm__ volatile("dsb sy" ::: "memory"); }

void ringInit(Ring &r, uint32_t off) {
  bzero(gH.va + off, 16 * kRing);
  r.off = off; r.enq = 0; r.cycle = 1;
  uint32_t *l = trbAt(off, kRing - 1);   // link TRB back to the start, toggle cycle
  l[0] = static_cast<uint32_t>(pa(off)); l[1] = static_cast<uint32_t>(pa(off) >> 32); l[2] = 0;
  l[3] = (T_LINK << 10) | (1u << 1);
}
// queue one TRB; the cycle bit is written last
void ringPush(Ring &r, uint32_t p0, uint32_t p1, uint32_t st, uint32_t ctl) {
  uint32_t *t = trbAt(r.off, r.enq);
  t[0] = p0; t[1] = p1; t[2] = st;
  sync();
  t[3] = (ctl & ~1u) | r.cycle;
  if (++r.enq == kRing - 1) {   // hand the link TRB over and wrap
    uint32_t *l = trbAt(r.off, kRing - 1);
    l[3] = (l[3] & ~1u) | r.cycle;
    r.enq = 0; r.cycle ^= 1;
  }
  sync();
}
void ring(uint32_t slot, uint32_t target) { sync(); w32(gH.db + 4 * slot, target); }

uint32_t *ctx(uint32_t devOff, bool input, unsigned index) {   // index: input 0 = control, 1 = slot, 2.. = endpoints by DCI+1
  return reinterpret_cast<uint32_t *>(gH.va + devOff + (input ? D_IN : D_OUT) + gH.ctxSize * index);
}
uint32_t devOff(unsigned i) { return O_DEV + i * kDevStride; }

void handleReport(Dev &d, unsigned k, const uint8_t *b, uint32_t len);

// One event from the ring, or false. Transfer events of interrupt endpoints are handled here (reports, then the TRB again).
bool nextEvent(uint32_t &type, uint32_t ev[4]) {
  uint32_t *t = trbAt(O_EVT, gH.evtDeq);
  if ((t[3] & 1u) != gH.evtCycle) return false;
  sync();
  ev[0] = t[0]; ev[1] = t[1]; ev[2] = t[2]; ev[3] = t[3];
  type = (ev[3] >> 10) & 0x3f;
  if (++gH.evtDeq == kEvtTrbs) { gH.evtDeq = 0; gH.evtCycle ^= 1; }
  const uint64_t erdp = pa(O_EVT + 16 * gH.evtDeq) | (1u << 3);   // EHB: write 1 to clear
  w32(gH.rt + 0x20 + 0x18, static_cast<uint32_t>(erdp)); w32(gH.rt + 0x20 + 0x1c, static_cast<uint32_t>(erdp >> 32));
  if (type == E_TRANSFER) {
    const uint32_t slot = ev[3] >> 24, dci = (ev[3] >> 16) & 0x1f;
    for (unsigned i = 0; i < kMaxDev; ++i) {
      Dev &d = gH.dev[i];
      if (!d.used || d.slot != slot || !d.configured) continue;
      for (unsigned k = 0; k < d.nInt; ++k) {
        if (d.intDci[k] != dci) continue;
        const uint32_t code = ev[2] >> 24, rest = ev[2] & 0xffffff;
        uint8_t *buf = gH.va + devOff(i) + D_REP + 64 * k;
        { static uint32_t firstSeen; if (firstSeen++ < 6) IOLog("usbhost: slot %u endpoint %u: interrupt completion %u, %u bytes\n", slot, dci, code, d.intMps[k] > rest ? d.intMps[k] - rest : 0); }
        if (code != 1 && code != 13) {
          static uint32_t logged;
          if (logged++ < 40) IOLog("usbhost: slot %u endpoint %u: completion code %u (residue %u)\n", slot, dci, code, rest);
          if (code == 6 || code == 4 || code == 3) { d.halted |= static_cast<uint8_t>(1u << k); return true; }   // stall / transaction / babble: the endpoint halted
        }
        if (code == 1 || code == 13) handleReport(d, k, buf, d.intMps[k] > rest ? d.intMps[k] - rest : 0);
        ringPush(d.intRing[k], static_cast<uint32_t>(pa(devOff(i) + D_REP + 64 * k)), static_cast<uint32_t>(pa(devOff(i) + D_REP + 64 * k) >> 32),
                 d.intMps[k], (T_NORMAL << 10) | (1u << 5) | (1u << 2));   // IOC, ISP
        ring(slot, dci);
        return true;   // consumed
      }
    }
  }
  return true;
}

// Wait for a command completion (slot == 0 means any) or a transfer event on (slot, dci). Returns the completion code.
uint32_t waitFor(uint32_t want, uint32_t slot, uint32_t dci, unsigned ms) {
  for (unsigned t = 0; t < ms * 10; ++t) {
    uint32_t type, ev[4];
    while (nextEvent(type, ev)) {
      if (type == E_PORT) IOLog("usbhost: port status change event, port %u, portsc %08x\n", ev[0] >> 24, r32(gH.op + PORTSC0 + 0x10 * ((ev[0] >> 24) - 1)));
      if (type == want && (want == E_COMMAND || ((ev[3] >> 24) == slot && ((ev[3] >> 16) & 0x1f) == dci))) {
        gH.lastCode = ev[2] >> 24; gH.lastSlot = ev[3] >> 24; gH.lastParam = ev[0]; gH.lastLen = ev[2] & 0xffffff;
        return gH.lastCode;
      }
    }
    IODelay(100);
  }
  return 0;   // timeout
}

uint32_t command(uint32_t p0, uint32_t p1, uint32_t ctl) {
  ringPush(gH.cmd, p0, p1, 0, ctl);
  ring(0, 0);
  return waitFor(E_COMMAND, 0, 0, 500);
}

// A control transfer on EP0. data: the device's control buffer (D_CTRL), len bytes. Returns the completion code (1 = success,
// 13 = short packet), and the bytes moved in *got.
uint32_t control(unsigned di, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, uint32_t *got = nullptr) {
  Dev &d = gH.dev[di];
  const uint32_t setup0 = type | (req << 8) | (static_cast<uint32_t>(value) << 16), setup1 = index | (static_cast<uint32_t>(len) << 16);
  const bool in = type & 0x80;
  const uint32_t trt = len ? (in ? 3u : 2u) : 0u;
  ringPush(d.ep0, setup0, setup1, 8, (T_SETUP << 10) | (1u << 6) | (trt << 16));   // IDT
  if (len) ringPush(d.ep0, static_cast<uint32_t>(pa(devOff(di) + D_CTRL)), static_cast<uint32_t>(pa(devOff(di) + D_CTRL) >> 32), len,
                    (T_DATA << 10) | (1u << 2) | (in ? (1u << 16) : 0u));   // ISP: a short packet reports its length
  ringPush(d.ep0, 0, 0, 0, (T_STATUS << 10) | (1u << 5) | ((len && in) ? 0u : (1u << 16)));   // IOC; status IN unless the data was IN
  ring(d.slot, 1);
  uint32_t code = waitFor(E_TRANSFER, d.slot, 1, 1000);
  // the status stage's event: the data stage, when short, reports first
  if (code == 13) { const uint32_t rest = gH.lastLen; code = waitFor(E_TRANSFER, d.slot, 1, 200); if (got) *got = len > rest ? len - rest : 0; if (code == 1) code = 13; }
  else if (got) *got = len;
  return code;
}

uint8_t *ctrlBuf(unsigned di) { return gH.va + devOff(di) + D_CTRL; }

// Address a new device: Enable Slot, then Address Device with its slot and EP0 contexts. Returns the device index or -1.
int addressDevice(uint8_t speed, uint8_t rootPort, uint32_t route, uint8_t depth, uint8_t ttSlot, uint8_t ttPort) {
  int di = -1;
  for (unsigned i = 0; i < kMaxDev; ++i) if (!gH.dev[i].used) { di = static_cast<int>(i); break; }
  if (di < 0) { IOLog("usbhost: no room for another device\n"); return -1; }
  if (command(0, 0, T_ENABLE_SLOT << 10) != 1) { IOLog("usbhost: Enable Slot failed (%u)\n", gH.lastCode); return -1; }
  Dev &d = gH.dev[di];
  bzero(&d, sizeof d);
  d.used = true; d.slot = static_cast<uint8_t>(gH.lastSlot); d.speed = speed; d.rootPort = rootPort; d.route = route; d.depth = depth;
  d.ttSlot = ttSlot; d.ttPort = ttPort;
  const uint32_t off = devOff(static_cast<unsigned>(di));
  bzero(gH.va + off, D_EP0);
  ringInit(d.ep0, off + D_EP0);
  reinterpret_cast<uint64_t *>(gH.va + O_DCBAA)[d.slot] = pa(off + D_OUT);
  uint32_t *ic = ctx(off, true, 0), *sc = ctx(off, true, 1), *e0 = ctx(off, true, 2);
  ic[1] = 3;   // A0 (slot), A1 (EP0)
  sc[0] = (route & 0xfffff) | (static_cast<uint32_t>(speed) << 20) | (1u << 27);
  sc[1] = static_cast<uint32_t>(rootPort) << 16;
  if (ttSlot) sc[2] = ttSlot | (static_cast<uint32_t>(ttPort) << 8);
  const uint32_t mps = speed == 3 ? 64 : speed == 4 ? 512 : 8;
  e0[1] = (3u << 1) | (4u << 3) | (mps << 16);   // CErr 3, control
  const uint64_t deq = pa(d.ep0.off) | 1;
  e0[2] = static_cast<uint32_t>(deq); e0[3] = static_cast<uint32_t>(deq >> 32);
  e0[4] = 8;
  sync();
  if (command(static_cast<uint32_t>(pa(off + D_IN)), static_cast<uint32_t>(pa(off + D_IN) >> 32), (T_ADDRESS << 10) | (static_cast<uint32_t>(d.slot) << 24)) != 1) {
    IOLog("usbhost: Address Device failed (%u) for slot %u\n", gH.lastCode, d.slot);
    command(0, 0, (T_DISABLE_SLOT << 10) | (static_cast<uint32_t>(d.slot) << 24));
    reinterpret_cast<uint64_t *>(gH.va + O_DCBAA)[d.slot] = 0;
    d.used = false;
    return -1;
  }
  // the real EP0 packet size, for full speed
  uint32_t got = 0;
  const uint32_t c = control(static_cast<unsigned>(di), 0x80, 6, 0x0100, 0, 8, &got);
  if ((c == 1 || c == 13) && got >= 8 && speed < 3) {
    const uint32_t real = ctrlBuf(static_cast<unsigned>(di))[7];
    if (real && real != mps) {
      bzero(ic, gH.ctxSize * 3); ic[1] = 2;   // A1
      e0[1] = (3u << 1) | (4u << 3) | (real << 16); e0[2] = static_cast<uint32_t>(deq); e0[3] = static_cast<uint32_t>(deq >> 32); e0[4] = 8;
      sync();
      command(static_cast<uint32_t>(pa(off + D_IN)), static_cast<uint32_t>(pa(off + D_IN) >> 32), (T_EVAL_CTX << 10) | (static_cast<uint32_t>(d.slot) << 24));
    }
  }
  IOLog("usbhost: slot %u addressed (speed %u, root port %u, route %05x)\n", d.slot, speed, rootPort, route);
  return di;
}

// ms ticks
inline uint32_t intervalFor(uint8_t speed, uint8_t bInterval) {
  if (speed >= 3) return bInterval ? bInterval - 1u : 0u;   // 2^(bInterval-1) microframes
  uint32_t frames = bInterval ? bInterval : 1u, e = 3;       // full/low speed: ms -> 2^e microframes
  while ((1u << (e + 1)) <= frames * 8 && e < 10) ++e;
  return e;
}

void configureHub(unsigned di);
void enumerate(unsigned di);

// Read the configuration, pick HID boot interfaces or a hub, SET_CONFIGURATION, endpoints.
void enumerate(unsigned di) {
  Dev &d = gH.dev[di];
  uint32_t got = 0;
  if (control(di, 0x80, 6, 0x0100, 0, 18, &got) > 13 || got < 18) { IOLog("usbhost: slot %u: no device descriptor\n", d.slot); return; }
  const uint8_t *dd = ctrlBuf(di);
  const uint16_t vid = static_cast<uint16_t>(dd[8] | (dd[9] << 8)), pid = static_cast<uint16_t>(dd[10] | (dd[11] << 8));
  const uint8_t devClass = dd[4];
  IOLog("usbhost: slot %u: device %04x:%04x class %02x\n", d.slot, vid, pid, devClass);
  if (control(di, 0x80, 6, 0x0200, 0, 9, &got) > 13 || got < 9) return;
  uint16_t total = static_cast<uint16_t>(ctrlBuf(di)[2] | (ctrlBuf(di)[3] << 8));
  if (total > 0x700) total = 0x700;
  if (control(di, 0x80, 6, 0x0200, 0, total, &got) > 13) return;
  const uint8_t *c = ctrlBuf(di);
  const uint8_t config = c[5];
  // walk the descriptors
  uint8_t curIface = 0xff, curClass = 0, curSub = 0, curProto = 0;
  uint8_t hidIface[3] = {}, nHid = 0;
  bool isHub = devClass == 9;
  for (uint32_t o = 0; o + 2 <= got && c[o]; o += c[o]) {
    const uint8_t len = c[o], type = c[o + 1];
    if (type == 4 && len >= 9) { curIface = c[o + 2]; curClass = c[o + 5]; curSub = c[o + 6]; curProto = c[o + 7]; if (curClass == 9) isHub = true; }
    if (type == 5 && len >= 7 && (c[o + 2] & 0x80) && (c[o + 3] & 3) == 3) {   // interrupt IN
      if (curClass == 3 && curSub == 1 && (curProto == 1 || curProto == 2) && d.nInt < 3) {
        const unsigned k = d.nInt++;
        d.intDci[k] = static_cast<uint8_t>((c[o + 2] & 0xf) * 2 + 1);
        d.intKind[k] = curProto;
        d.intMps[k] = static_cast<uint16_t>((c[o + 4] | (c[o + 5] << 8)) & 0x7ff);
        if (d.intMps[k] > 64) d.intMps[k] = 64;
        hidIface[nHid++] = curIface;
        uint32_t *ep = ctx(devOff(di), true, 1 + d.intDci[k]);
        bzero(ep, gH.ctxSize);
        ep[0] = intervalFor(d.speed, c[o + 6]) << 16;
        ep[1] = (3u << 1) | (7u << 3) | (static_cast<uint32_t>(d.intMps[k]) << 16);
        ringInit(d.intRing[k], devOff(di) + D_INT + 0x400 * k);
        const uint64_t deq = pa(d.intRing[k].off) | 1;
        ep[2] = static_cast<uint32_t>(deq); ep[3] = static_cast<uint32_t>(deq >> 32);
        ep[4] = d.intMps[k] | (static_cast<uint32_t>(d.intMps[k]) << 16);
        IOLog("usbhost: slot %u: HID boot %s on interface %u, endpoint %02x, %u bytes (descriptor %02x %02x %02x %02x %02x %02x %02x), interval %u\n", d.slot,
              curProto == 1 ? "keyboard" : "mouse", curIface, c[o + 2], d.intMps[k], c[o], c[o + 1], c[o + 2], c[o + 3], c[o + 4], c[o + 5], c[o + 6], c[o + 6]);
      }
    }
  }
  if (control(di, 0x00, 9, config, 0, 0) != 1) { IOLog("usbhost: slot %u: SET_CONFIGURATION failed\n", d.slot); return; }
  if (isHub) { d.hub = true; configureHub(di); return; }
  if (!d.nInt) { IOLog("usbhost: slot %u: nothing we drive (no HID boot interface)\n", d.slot); return; }
  for (unsigned k = 0; k < nHid; ++k) {
    control(di, 0x21, 0x0B, 0, hidIface[k], 0);   // SET_PROTOCOL boot
    control(di, 0x21, 0x0A, 0, hidIface[k], 0);   // SET_IDLE 0
  }
  // Configure Endpoint: the slot (context entries) and every interrupt endpoint
  uint32_t *ic = ctx(devOff(di), true, 0), *sc = ctx(devOff(di), true, 1);
  bzero(ic, gH.ctxSize);
  uint32_t maxDci = 1;
  ic[1] = 1;
  for (unsigned k = 0; k < d.nInt; ++k) { ic[1] |= 1u << d.intDci[k]; if (d.intDci[k] > maxDci) maxDci = d.intDci[k]; }
  const uint32_t *out = ctx(devOff(di), false, 0);
  memcpy(sc, out, gH.ctxSize);   // the slot as the controller has it
  sc[0] = (sc[0] & ~(0x1fu << 27)) | (maxDci << 27);
  sc[3] = 0;
  sync();
  if (command(static_cast<uint32_t>(pa(devOff(di) + D_IN)), static_cast<uint32_t>(pa(devOff(di) + D_IN) >> 32),
              (T_CONFIG_EP << 10) | (static_cast<uint32_t>(d.slot) << 24)) != 1) {
    IOLog("usbhost: slot %u: Configure Endpoint failed (%u)\n", d.slot, gH.lastCode);
    return;
  }
  d.configured = true;
  for (unsigned k = 0; k < d.nInt; ++k) {
    ringPush(d.intRing[k], static_cast<uint32_t>(pa(devOff(di) + D_REP + 64 * k)), static_cast<uint32_t>(pa(devOff(di) + D_REP + 64 * k) >> 32),
             d.intMps[k], (T_NORMAL << 10) | (1u << 5) | (1u << 2));
    ring(d.slot, d.intDci[k]);
  }
  IOLog("usbhost: slot %u: %u HID endpoint(s) running\n", d.slot, d.nInt);
}

void handleReport(Dev &d, unsigned k, const uint8_t *b, uint32_t len) {
  ++gH.reports;
  if (d.intKind[k] == 1 && len >= 8) {
    ++gH.keyReports;
    if (gKeys) gKeys(gCtx, b);
  } else if (d.intKind[k] == 2 && len >= 3) {
    ++gH.mouseReports;
    if (gMouse) gMouse(gCtx, b[0], static_cast<int8_t>(b[1]), static_cast<int8_t>(b[2]), len >= 4 ? static_cast<int8_t>(b[3]) : 0);
  }
}

// Hub: descriptor, the slot marked as a hub, ports powered. Its ports are polled by hubPoll().
void configureHub(unsigned di) {
  Dev &d = gH.dev[di];
  uint32_t got = 0;
  if (control(di, 0xA0, 6, 0x2900, 0, 9, &got) > 13 || got < 7) { IOLog("usbhost: slot %u: no hub descriptor\n", d.slot); return; }
  const uint8_t *h = ctrlBuf(di);
  d.hubPorts = h[2] > 15 ? 15 : h[2];
  const uint16_t chars = static_cast<uint16_t>(h[3] | (h[4] << 8));
  const uint8_t pwrGood = h[5];
  uint32_t *ic = ctx(devOff(di), true, 0), *sc = ctx(devOff(di), true, 1);
  bzero(ic, gH.ctxSize);
  ic[1] = 1;   // A0: the slot
  memcpy(sc, ctx(devOff(di), false, 0), gH.ctxSize);
  sc[0] |= 1u << 26;   // Hub
  sc[1] = (sc[1] & 0x00ffffffu) | (static_cast<uint32_t>(d.hubPorts) << 24);
  sc[2] = (sc[2] & ~(3u << 16)) | ((d.speed == 3 ? ((chars >> 5) & 3u) : 0u) << 16);   // TT think time
  sc[3] = 0;
  sync();
  const uint32_t r = command(static_cast<uint32_t>(pa(devOff(di) + D_IN)), static_cast<uint32_t>(pa(devOff(di) + D_IN) >> 32),
                             (T_CONFIG_EP << 10) | (static_cast<uint32_t>(d.slot) << 24));
  IOLog("usbhost: slot %u: hub with %u ports (characteristics %04x), slot as hub %u\n", d.slot, d.hubPorts, chars, r);
  for (unsigned p = 1; p <= d.hubPorts; ++p) control(di, 0x23, 3, 8, static_cast<uint16_t>(p), 0);   // SET_FEATURE PORT_POWER
  IOSleep(pwrGood * 2u + 100u);
  d.configured = true;
}

void hubPoll(unsigned di) {
  Dev &d = gH.dev[di];
  for (unsigned p = 1; p <= d.hubPorts; ++p) {
    uint32_t got = 0;
    const uint32_t cc = control(di, 0xA3, 0, 0, static_cast<uint16_t>(p), 4, &got);
    if (cc != 1 || got < 4) { static uint32_t n; if (n++ < 20) IOLog("usbhost: hub slot %u port %u: GET_STATUS failed (%u)\n", d.slot, p, cc); continue; }
    const uint8_t *s = ctrlBuf(di);
    const uint16_t status = static_cast<uint16_t>(s[0] | (s[1] << 8)), change = static_cast<uint16_t>(s[2] | (s[3] << 8));
    if (change & 1) control(di, 0x23, 1, 16, static_cast<uint16_t>(p), 0);   // CLEAR_FEATURE C_PORT_CONNECTION
    const bool connected = status & 1;
    if (connected && !d.hubPortStatus[p] && (status & (1u << 9))) {   // low speed: resetting it took the root port down (fullos240-242)
      d.hubPortStatus[p] = 1;
      IOLog("usbhost: hub slot %u port %u: a low-speed device: left alone for now\n", d.slot, p);
      continue;
    }
    if (connected && !d.hubPortStatus[p] && d.depth < 2 && !badRoute(d.route | (static_cast<uint32_t>(p) << (4 * d.depth)))) {
      d.hubPortStatus[p] = 1;
      control(di, 0x23, 3, 4, static_cast<uint16_t>(p), 0);   // SET_FEATURE PORT_RESET
      uint16_t st = 0;
      for (unsigned t = 0; t < 50; ++t) {
        IOSleep(10);
        if (control(di, 0xA3, 0, 0, static_cast<uint16_t>(p), 4, &got) == 1 && got >= 4) {
          st = static_cast<uint16_t>(ctrlBuf(di)[0] | (ctrlBuf(di)[1] << 8));
          if (ctrlBuf(di)[2] & 0x10) break;   // C_PORT_RESET
        }
      }
      control(di, 0x23, 1, 20, static_cast<uint16_t>(p), 0);   // CLEAR_FEATURE C_PORT_RESET
      IOSleep(20);
      const uint8_t speed = (st & (1u << 9)) ? 2 : (st & (1u << 10)) ? 3 : 1;   // xHCI speeds: 1 full, 2 low, 3 high
      const uint32_t route = d.route | (static_cast<uint32_t>(p) << (4 * d.depth));
      const bool tt = d.speed == 3 && speed < 3;
      IOLog("usbhost: hub slot %u port %u: device connected (status %04x, speed %u)\n", d.slot, p, st, speed);
      int nd = addressDevice(speed, d.rootPort, route, static_cast<uint8_t>(d.depth + 1), tt ? d.slot : d.ttSlot,
                             tt ? static_cast<uint8_t>(p) : d.ttPort);
      if (nd >= 0) enumerate(static_cast<unsigned>(nd));
      else {
        if (gBadRoutes < 8) gBadRoute[gBadRoutes++] = route;   // never again: a failing low-speed device took the root port down (fullos240/241)
        IOLog("usbhost: hub slot %u port %u: given up on this device (speed %u)\n", d.slot, p, speed);
      }
    } else if (!connected && d.hubPortStatus[p]) {
      d.hubPortStatus[p] = 0;
      IOLog("usbhost: hub slot %u port %u: device gone\n", d.slot, p);
      // the slot stays allocated: unplugging is rare in this first version
    }
  }
}

// PORTSC writes: keep the read-only and RWS bits, set what we want (Linux xhci_port_state_to_neutral)
inline uint32_t portNeutral(uint32_t v) { return (v & ((1u << 0) | (1u << 3) | (0xfu << 10) | (1u << 30))) | (v & ((0xfu << 5) | (1u << 9) | (3u << 14) | (7u << 25))); }

bool start() {
  if (gH.md) {   // a restart: same memory, everything else from scratch
    IOBufferMemoryDescriptor *md = gH.md; uint8_t *va = gH.va; const uint64_t p = gH.pa;
    const uint32_t reports = gH.reports, keys = gH.keyReports, mice = gH.mouseReports;
    bzero(&gH, sizeof gH);   // plain data: pointers, counters, flags
    gH.md = md; gH.va = va; gH.pa = p; gH.reports = reports; gH.keyReports = keys; gH.mouseReports = mice;
    bzero(gH.va, kDmaBytes);
  } else {
  // DMA: physically contiguous, below 64 GiB
  gH.md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
      static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
      static_cast<IOOptionBits>(kIOMapWriteCombineCache), kDmaBytes, 0x0000000FFFFFF000ULL);
  if (!gH.md || gH.md->prepare() != kIOReturnSuccess) { IOLog("usbhost: no DMA memory\n"); return false; }
  gH.va = static_cast<uint8_t *>(gH.md->getBytesNoCopy());
  gH.pa = gH.md->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
  bzero(gH.va, kDmaBytes);
  }
  // the controller to host mode (dwc3_otg_set_host_mode): no INCR bursts, PRTCAPDIR = 1
  w32(GSBUSCFG0, r32(GSBUSCFG0) & ~1u);
  w32(GCTL, (r32(GCTL) & ~(3u << 12)) | (1u << 12));
  w32(GUCTL, r32(GUCTL) | (1u << 14));   // USBHSTINAUTORETRYEN, as Google's dwc3_exynos_core_init in host mode
  IOSleep(20);
  const uint32_t capLen = r32(CAPLENGTH) & 0xff, hcs1 = r32(HCSPARAMS1), hcs2 = r32(HCSPARAMS2), hcc1 = r32(HCCPARAMS1);
  gH.op = capLen; gH.db = r32(DBOFF) & ~3u; gH.rt = r32(RTSOFF) & ~0x1fu;
  gH.maxSlots = hcs1 & 0xff; gH.maxPorts = hcs1 >> 24; gH.ctxSize = (hcc1 & 4) ? 64 : 32;
  const uint32_t scratch = ((hcs2 >> 27) & 0x1f) | (((hcs2 >> 21) & 0x1f) << 5);
  IOLog("usbhost: xHCI caplength %u version %x, %u slots %u ports, context %u bytes, %u scratchpad pages, gctl %08x gsts %08x\n",
        capLen, r32(0) >> 16, gH.maxSlots, gH.maxPorts, gH.ctxSize, scratch, r32(GCTL), r32(GSTS));
  if (!capLen || capLen == 0xff || gH.maxPorts == 0 || scratch > 16) { IOLog("usbhost: no xHCI here\n"); return false; }
  // halt, reset
  w32(gH.op + USBCMD, r32(gH.op + USBCMD) & ~CMD_RUN);
  for (unsigned t = 0; t < 100 && !(r32(gH.op + USBSTS) & STS_HCH); ++t) IOSleep(1);
  w32(gH.op + USBCMD, CMD_HCRST);
  for (unsigned t = 0; t < 500 && (r32(gH.op + USBCMD) & CMD_HCRST || r32(gH.op + USBSTS) & STS_CNR); ++t) IOSleep(1);
  if (r32(gH.op + USBSTS) & STS_CNR) { IOLog("usbhost: controller not ready after reset (sts %08x)\n", r32(gH.op + USBSTS)); return false; }
  const uint32_t slots = gH.maxSlots < kMaxDev ? gH.maxSlots : kMaxDev;
  w32(gH.op + CONFIG, slots);
  // scratchpad
  uint64_t *dcbaa = reinterpret_cast<uint64_t *>(gH.va + O_DCBAA);
  if (scratch) {
    uint64_t *spa = reinterpret_cast<uint64_t *>(gH.va + O_SPA);
    for (unsigned i = 0; i < scratch; ++i) spa[i] = pa(O_SP + 0x1000 * i);
    dcbaa[0] = pa(O_SPA);
  }
  sync();
  w32(gH.op + DCBAAP, static_cast<uint32_t>(pa(O_DCBAA))); w32(gH.op + DCBAAP + 4, static_cast<uint32_t>(pa(O_DCBAA) >> 32));
  ringInit(gH.cmd, O_CMD);
  w32(gH.op + CRCR, static_cast<uint32_t>(pa(O_CMD)) | 1u); w32(gH.op + CRCR + 4, static_cast<uint32_t>(pa(O_CMD) >> 32));
  // event ring: one segment
  uint32_t *erst = reinterpret_cast<uint32_t *>(gH.va + O_ERST);
  erst[0] = static_cast<uint32_t>(pa(O_EVT)); erst[1] = static_cast<uint32_t>(pa(O_EVT) >> 32); erst[2] = kEvtTrbs; erst[3] = 0;
  sync();
  const uint32_t ir = gH.rt + 0x20;
  w32(ir + 0x08, 1);   // ERSTSZ
  w32(ir + 0x18, static_cast<uint32_t>(pa(O_EVT))); w32(ir + 0x1c, static_cast<uint32_t>(pa(O_EVT) >> 32));
  w32(ir + 0x10, static_cast<uint32_t>(pa(O_ERST))); w32(ir + 0x14, static_cast<uint32_t>(pa(O_ERST) >> 32));
  gH.evtDeq = 0; gH.evtCycle = 1;
  w32(gH.op + USBCMD, CMD_RUN);
  for (unsigned t = 0; t < 100 && (r32(gH.op + USBSTS) & STS_HCH); ++t) IOSleep(1);
  const uint32_t noop = command(0, 0, T_NOOP_CMD << 10);
  IOLog("usbhost: running (sts %08x), no-op command %s\n", r32(gH.op + USBSTS), noop == 1 ? "answered" : "NOT answered");
  if (noop != 1) return false;
  // power the root ports
  for (unsigned p = 0; p < gH.maxPorts; ++p) {
    const uint32_t a = gH.op + PORTSC0 + 0x10 * p, v = r32(a);
    if (!(v & PS_PP)) w32(a, portNeutral(v) | PS_PP);
  }
  IOSleep(100);
  gH.running = true;
  return true;
}


void rootPoll() {
  static uint8_t seen[16];
  static uint32_t last[16];
  for (unsigned p = 0; p < gH.maxPorts && p < 16; ++p) {
    const uint32_t a = gH.op + PORTSC0 + 0x10 * p, v = r32(a);
    if (v != last[p]) { IOLog("usbhost: root port %u: portsc %08x -> %08x\n", p + 1, last[p], v); last[p] = v; }
    if (seen[p] == 2 && (v & PS_CCS) && !(v & PS_PED)) {   // enumerated, then disabled by the controller: restart the xHCI
      IOLog("usbhost: root port %u was disabled (portsc %08x): restarting the controller\n", p + 1, v);
      for (unsigned q = 0; q < 16; ++q) { seen[q] = 0; last[q] = 0; }
      if (!start()) IOLog("usbhost: restart failed\n");
      return;
    }
    if (v & PS_CHANGES) w32(a, portNeutral(v) | (v & PS_CHANGES));   // acknowledge
    const bool conn = v & PS_CCS;
    if (conn && !seen[p]) {
      seen[p] = 1;
      const uint32_t speedNow = (v >> 10) & 0xf;
      IOLog("usbhost: root port %u: connection (portsc %08x, speed %u)\n", p + 1, v, speedNow);
      if (speedNow >= 4) { IOLog("usbhost: root port %u is SuperSpeed: not supported yet (no USB 3 PHY)\n", p + 1); continue; }
      w32(a, portNeutral(v) | PS_PR);
      uint32_t s = 0;
      for (unsigned t = 0; t < 100; ++t) { IOSleep(2); s = r32(a); if (s & PS_PRC) break; }
      w32(a, portNeutral(s) | PS_PRC);
      if (!(s & PS_PED)) { IOLog("usbhost: root port %u: not enabled after reset (portsc %08x)\n", p + 1, s); seen[p] = 0; continue; }
      IOSleep(20);
      const uint8_t speed = static_cast<uint8_t>((s >> 10) & 0xf);
      const int di = addressDevice(speed, static_cast<uint8_t>(p + 1), 0, 0, 0, 0);
      if (di >= 0) { enumerate(static_cast<unsigned>(di)); seen[p] = 2; }
    } else if (!conn && seen[p]) {
      seen[p] = 0;
      IOLog("usbhost: root port %u: disconnected\n", p + 1);
    }
  }
}

// Experiments at run time (kern.akita_acpm = 6000000 + n, also over the Wi-Fi control channel: akita-live.py wcmd):
//   1 dump; 2 eUSB UTMI host pull-downs (override); 3 blkcon UTMI_CTRL without the forced BVALID/VBUSVALID;
//   4 blkcon UTMI_CTRL forced DP/DM pull-downs; 5 the eUSB2 PHY power-up again; 6 reset root port 1 even without a connection;
//   7 GUSB2PHYCFG: PHY suspend off; 8 root port power off and on again; 9 undo 2-4
void dumpState(const char *why) {
  IOLog("usbhost: %s: gsnpsid %08x guctl %08x guctl1 %08x gfladj %08x gctl %08x\n", why, r32(GSNPSID), r32(GUCTL), r32(GUCTL1), r32(GFLADJ), r32(GCTL));
  IOLog("usbhost: %s: portsc %08x %08x usbsts %08x gctl %08x gsts %08x gusb2phycfg %08x\n", why,
        gH.op ? r32(gH.op + PORTSC0) : 0, gH.op ? r32(gH.op + PORTSC0 + 0x10) : 0, gH.op ? r32(gH.op + USBSTS) : 0,
        r32(GCTL), r32(GSTS), r32(GUSB2PHYCFG));
  IOLog("usbhost: %s: eusb rst %08x cmn %08x txtune %08x utmi %08x testse %08x; blkcon linkctrl %08x linkport %08x clkrst %08x utmi_ctrl %08x\n", why,
        usbProbeRead(winEusb, 0x00), usbProbeRead(winEusb, 0x04), usbProbeRead(winEusb, 0x14), usbProbeRead(winEusb, 0x1c), usbProbeRead(winEusb, 0x20),
        usbProbeRead(winSubctl, 0x04), usbProbeRead(winSubctl, 0x08), usbProbeRead(winSubctl, 0x0c), usbProbeRead(winSubctl, 0x10));
}
int command(unsigned n) {
  switch (n) {
    case 1: dumpState("state"); return 0;
    case 2: usbProbeWrite(winEusb, 0x1c, usbProbeRead(winEusb, 0x1c) | 0x1e); break;          // dm/dp pull-down + overrides
    case 3: usbProbeWrite(winSubctl, 0x10, usbProbeRead(winSubctl, 0x10) & ~3u); break;       // no forced BVALID/VBUSVALID
    case 4: usbProbeWrite(winSubctl, 0x10, usbProbeRead(winSubctl, 0x10) | (3u << 8)); break; // forced DM/DP pull-downs
    case 5: usbEusbInitiate(); break;
    case 6: if (gH.op) { const uint32_t a = gH.op + PORTSC0, v = r32(a); w32(a, portNeutral(v) | PS_PR); IOSleep(60); } break;
    case 7: w32(GUSB2PHYCFG, r32(GUSB2PHYCFG) & ~(1u << 6)); break;
    case 8: if (gH.op) for (unsigned p = 0; p < gH.maxPorts; ++p) { const uint32_t a = gH.op + PORTSC0 + 0x10 * p; w32(a, portNeutral(r32(a)) & ~PS_PP); IOSleep(200); w32(a, portNeutral(r32(a)) | PS_PP); } break;
    case 9: usbProbeWrite(winEusb, 0x1c, usbProbeRead(winEusb, 0x1c) & ~0x1eu); usbProbeWrite(winSubctl, 0x10, (usbProbeRead(winSubctl, 0x10) & ~(3u << 8)) | 3u); break;
    // Google's host settings for zuma (zuma-usb.dtsi, dwc3-exynos core init)
    case 10: {   // snps,quirk-frame-length-adjustment = 0x20 (GFLADJ_30MHZ_SDBND_SEL | 0x20), and the 19.2 MHz ref clock fields (Linux dwc3_ref_clk_period)
      uint32_t v = r32(GFLADJ);
      v = (v & ~0x3fu) | (1u << 7) | 0x20;
      v = (v & ~(0x3fffu << 8)) | (200u << 8);                 // REFCLK_FLADJ: 125000 * (1e9 / (19.2e6 * 52)) - 125000
      v = (v & ~(0x7fu << 24) & ~(1u << 31)) | (12u << 24) | (1u << 31);   // 240MHZDECR = 25: 12 plus PLS1
      v |= 1u << 23;                                           // REFCLK_LPM_SEL
      w32(GFLADJ, v); break;
    }
    case 11: w32(GUCTL, (r32(GUCTL) & ~(0x3ffu << 22)) | (0x34u << 22) | (1u << 17) | (1u << 21)); break;   // REFCLKPER 52 ns, sparse control, no extra delay
    case 12: w32(GCTL, r32(GCTL) & ~(1u << 10)); break;          // SOFITPSYNC off (adj-sof-accuracy)
    case 13: w32(GUCTL1, r32(GUCTL1) | (1u << 17)); break;       // park mode off for high speed
    case 14: w32(GUCTL, (r32(GUCTL) & ~0x1ffu) | 200u); break;   // device timeout (usb_host_device_timeout = 200)
    case 15: gHubPollPaused = !gHubPollPaused; IOLog("usbhost: hub polling %s\n", gHubPollPaused ? "paused" : "on"); break;
    case 16: gRestartRequest = true; break;
    case 17: { const uint32_t a = gH.op + PORTSC0; IOLog("usbhost: root port 1 portsc %08x portpmsc %08x portli %08x\n", r32(a), r32(a + 4), r32(a + 8)); return 0; }
    default: IOLog("usbhost: command %u unknown\n", n); return -1;
  }
  IOSleep(50);
  dumpState("after command");
  return 0;
}

void hostThread(void *, wait_result_t) {
  akita_usbhost_set_hook(command);
  IOLog("usbhost: waiting for the USB PD engine to make the phone the host\n");
  while (!akita_typec_host()) IOSleep(500);
  IOLog("usbhost: the phone is the host (DFP): stopping the gadget\n");
  gadget::gStopRequest = true;
  for (unsigned t = 0; t < 300 && !gadget::gStopped; ++t) IOSleep(10);
  IOLog("usbhost: gadget %s\n", gadget::gStopped ? "stopped" : "did not answer (taking the controller anyway)");
  dumpState("before host");
  if (!start()) { IOLog("usbhost: could not start the xHCI\n"); return; }
  dumpState("host running");
  uint64_t lastHub = 0, lastSummary = 0;
  for (;;) {
    uint32_t type, ev[4];
    while (nextEvent(type, ev)) {
      if (type == E_PORT) { /* root ports are polled */ }
    }
    uint64_t now; clock_get_uptime(&now);
    uint64_t ns; absolutetime_to_nanoseconds(now - lastHub, &ns);
    if (ns > 250000000ULL) {
      lastHub = now;
      if (gRestartRequest) { gRestartRequest = false; IOLog("usbhost: restart on request\n"); start(); }
      rootPoll();
      if (!gHubPollPaused)
        for (unsigned i = 0; i < kMaxDev; ++i) if (gH.dev[i].used && gH.dev[i].hub && gH.dev[i].configured) hubPoll(i);
    }
    absolutetime_to_nanoseconds(now - lastSummary, &ns);
    if (ns > 20000000000ULL) {
      lastSummary = now;
      IOLog("usbhost: %u reports (%u keyboard, %u mouse), sts %08x, portsc %08x %08x\n", gH.reports, gH.keyReports, gH.mouseReports,
            r32(gH.op + USBSTS), r32(gH.op + PORTSC0), r32(gH.op + PORTSC0 + 0x10));
      for (unsigned i = 0; i < kMaxDev; ++i)
        if (gH.dev[i].used && gH.dev[i].nInt) {
          const uint32_t *oc = ctx(devOff(i), false, 0);
          IOLog("usbhost: slot %u: slot state %u, endpoint states %u %u, halted %x\n", gH.dev[i].slot, oc[3] >> 27,
                ctx(devOff(i), false, gH.dev[i].intDci[0])[0] & 7, gH.dev[i].nInt > 1 ? ctx(devOff(i), false, gH.dev[i].intDci[1])[0] & 7 : 0,
                gH.dev[i].halted);
        }
    }
    IOSleep(1);
  }
}

}  // namespace usbhost
