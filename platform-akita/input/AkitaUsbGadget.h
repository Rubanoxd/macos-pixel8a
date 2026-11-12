// SPDX-License-Identifier: BSD-2-Clause-Patent
// AkitaUsbGadget: the Pixel 8a's USB port in device mode, as a CDC-ECM Ethernet adapter for the Mac it is plugged
// into. Included by NativeRecoveryInput.cpp inside its anonymous namespace, after the USB probe helpers it reuses
// (usbProbeWindows/usbProbeRead/usbProbeWrite, dwc3Read/dwc3Write, usbEusbInitiate, tensorPmuRead).
//
// Hardware (see the probe's notes in NativeRecoveryInput.cpp): DWC_usb31 1.10a at 0x11210000, left by the bootloader
// in device mode with clocks on and VBUS/BVALID forced valid in the USB sub-control block, but with the eUSB2 PHY shut
// down. usbEusbInitiate() powers the PHY up again; with Run/Stop set the Mac then sees a high-speed device and resets
// it (probe of 5 Nov 2026). This file answers it: event buffer and TRBs in one physically contiguous, write-combined
// (uncached) buffer, a polled event loop (no interrupt wiring), the control endpoint state machine of Linux's
// dwc3/ep0.c in its simplest form, and the CDC-ECM function (USB CDC ECM 1.2): one Ethernet frame per bulk transfer.
//
// Stage 1 (akita-usb-gadget=1): frames from the Mac are counted, and the gadget itself answers ARP and ICMP echo for
// 169.254.7.2, so `ping 169.254.7.2` on the Mac proves the whole data path. Stage 2 will hand frames to a real
// IOEthernetController instead.
//
// Endpoints (physical numbers): 0 ep0 OUT, 1 ep0 IN, 3 ep1 IN (interrupt, notifications), 4 ep2 OUT and 5 ep2 IN
// (bulk data).

namespace gadget {

enum : uint32_t {
  DALEPENA = 0xc720,
  CMD_SETEPCONFIG = 1, CMD_SETTRANSFRESOURCE = 2, CMD_SETSTALL = 4, CMD_CLEARSTALL = 5, CMD_STARTTRANSFER = 6,
  CMD_ENDTRANSFER = 8, CMD_DEPSTARTCFG = 9,
  CMDACT = 1U << 10, CMDIOC = 1U << 8, HIPRI_FORCERM = 1U << 11,
  TRB_HWO = 1U << 0, TRB_LST = 1U << 1, TRB_ISP_IMI = 1U << 10, TRB_IOC = 1U << 11,
  TRBCTL_NORMAL = 1, TRBCTL_SETUP = 2, TRBCTL_STATUS2 = 3, TRBCTL_STATUS3 = 4, TRBCTL_DATA = 5,
};

// Layout of the DMA buffer.
enum : uint32_t {
  OFF_EVENTS = 0x0000, EVENT_BYTES = 0x1000,
  OFF_TRB_EP0 = 0x1000, OFF_TRB_INTR = 0x1010, OFF_TRB_IN = 0x1020, OFF_TRB_OUT = 0x1030,
  OFF_SETUP = 0x1100,
  OFF_EP0 = 0x2000, EP0_BYTES = 0x400,
  OFF_OUT = 0x4000, OUT_BYTES = 0x800,
  OFF_IN = 0x5000, IN_BYTES = 0x800,
  OFF_INTR = 0x6000,
  DMA_BYTES = 0x8000,
};

enum : uint8_t { EP0_OUT = 0, EP0_IN = 1, EP_NOTIFY = 3, EP_BULK_OUT = 4, EP_BULK_IN = 5 };

const uint8_t deviceDescriptor[18] = {
  18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
  0x25, 0x05, 0xa2, 0xa4,   // 0525:a4a2, the Linux CDC Ethernet gadget's identity
  0x00, 0x01, 1, 2, 3, 1,
};

const uint8_t configDescriptor[80] = {
  9, 2, 80, 0, 2, 1, 0, 0xc0, 50,
  // Communication interface: CDC ECM, one interrupt endpoint.
  9, 4, 0, 0, 1, 0x02, 0x06, 0x00, 0,
  5, 0x24, 0x00, 0x10, 0x01,                                // header, CDC 1.10
  5, 0x24, 0x06, 0, 1,                                      // union: control 0, data 1
  13, 0x24, 0x0f, 4, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0,       // Ethernet: MAC string 4, max segment 1514
  7, 5, 0x81, 0x03, 16, 0, 9,                               // ep1 IN, interrupt, 16 bytes, 32 ms
  // Data interface: alternate 0 without endpoints, alternate 1 with the bulk pair.
  9, 4, 1, 0, 0, 0x0a, 0x00, 0x00, 0,
  9, 4, 1, 1, 2, 0x0a, 0x00, 0x00, 0,
  7, 5, 0x82, 0x02, 0x00, 0x02, 0,                          // ep2 IN, bulk, 512
  7, 5, 0x02, 0x02, 0x00, 0x02, 0,                          // ep2 OUT, bulk, 512
};

const char *const strings[] = {nullptr, "UniXNU", "Pixel 8a macOS Recovery", "akita0001", "02A51A000001"};
const uint8_t phoneMac[6] = {0x02, 0xa5, 0x1a, 0x00, 0x00, 0x02};
const uint8_t phoneIp[4] = {169, 254, 7, 2};

struct Trb {
  uint32_t bpl, bph, size, ctrl;
};

// Live control over the gadget (mode 2), all as raw UDP built and parsed here, beside the BSD stack:
//   5140  the kernel's retained log, broadcast as it grows ("AKLG" + 32-bit stream offset + text), from the start
//   5141  commands from the Mac: "I<steps>" input steps in the akita-input-script syntax (K, T, A, C, P, M, W, D, U),
//         "S[n]" a screenshot downscaled by n (2 by default), "L" replay the log from the start,
//         "FASTBOOT" reset straight into fastboot
//   5142  screenshot replies to the asker ("AKFB" frame, offset, total, width, height + RGB bytes)
// The handler for input steps is installed by NativeRecoveryInput (it owns the HID devices).
void (*gRemoteInput)(const char *steps, unsigned length) = nullptr;
extern "C" uint8_t sheng_log_buffer[];   // struct sheng_log_record: total at +24, prefix at +128, tail after it
enum : uint32_t { LOG_TOTAL = 24, LOG_PREFIX = 128, LOG_PREFIX_BYTES = 16384, LOG_TAIL = 128 + 16384,
                  LOG_TAIL_BYTES = 65536, UDP_CHUNK = 1400 };

class Gadget {
public:
  bool start();
  void run();

private:
  IOBufferMemoryDescriptor *fDma = nullptr;
  uint8_t *fVirt = nullptr;
  uint64_t fPhys = 0;
  uint32_t fEventOffset = 0;

  enum Ep0State { SETUP, DATA, STATUS_PENDING, STATUS } fEp0 = SETUP;
  bool fDataIn = false, fThreeStage = false, fStatusReady = false;
  int fStatusEp = -1;
  uint8_t fConfig = 0, fAltData = 0;
  bool fBulkOutBusy = false, fBulkInBusy = false, fNotifyBusy = false;
  unsigned fNotifyStep = 0;   // 0 nothing, 1 send NETWORK_CONNECTION, 2 send speed change, 3 done
  uint8_t fOutRsc = 0, fInRsc = 0, fNotifyRsc = 0, fEp0Rsc[2] = {};
  bool fEp0Busy[2] = {}, fDataEnabled = false;

  // Counters for the summary.
  uint32_t fResets = 0, fConnects = 0, fSetups = 0, fStalls = 0, fFramesIn = 0, fFramesOut = 0, fArp = 0, fPing = 0;
  uint32_t fLastSetup[2] = {};
  uint32_t fEvents = 0, fCmdFails = 0;
  uint32_t fLoggedSetups = 0, fLoggedFrames = 0;

  // Stage 2: a real BSD Ethernet interface (en0) fed by the bulk endpoints.
  unsigned fMode = 1;
  ifnet_t fIfp = nullptr;
  IOLock *fTxLock = nullptr;
  struct TxSlot {
    uint16_t length;
    uint8_t data[1600];
  };
  static constexpr unsigned txSlots = 32;
  TxSlot *fTx = nullptr;
  unsigned fTxHead = 0, fTxTail = 0;
  uint32_t fTxDrops = 0, fRxDrops = 0, fIfInput = 0, fIfOutput = 0;
  int fAddrError = -1;

  // Live control.
  uint64_t fLogSent = 0;
  uint32_t fLogPackets = 0, fCommands = 0, fShots = 0;
  struct Shot {
    bool active = false;
    uint8_t mac[6] = {}, ip[4] = {};
    uint16_t port = 0, width = 0, height = 0;
    uint32_t offset = 0, total = 0, id = 0;
  } fShot;
  uint8_t *fShotImage = nullptr;
  IOMemoryMap *fFbMap = nullptr;
  PE_Video fVideo = {};

  void sendUdp(const uint8_t mac[6], const uint8_t ip[4], uint16_t srcPort, uint16_t dstPort, const void *header,
               uint32_t headerLength, const void *payload, uint32_t length);
  void pumpRaw();
  bool command(const uint8_t *frame, uint32_t length);
  void startShot(const uint8_t mac[6], const uint8_t ip[4], uint16_t port, unsigned scale);

public:
  errno_t ifOutput(mbuf_t m);
  void setMode(unsigned mode) { fMode = mode; }

private:
  bool attachInterface();
  void configureAddress();
  void pumpTx();

  Trb *trb(uint32_t offset) { return reinterpret_cast<Trb *>(fVirt + offset); }
  uint64_t phys(uint32_t offset) const { return fPhys + offset; }
  static void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }

  bool epCmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t *result = nullptr);
  bool startTransfer(unsigned ep, uint32_t trbOffset, uint32_t bufOffset, uint32_t length, uint32_t trbctl,
                     uint8_t *resource = nullptr);
  bool configureEp(unsigned ep, uint32_t type, uint32_t maxPacket, uint32_t action, uint32_t interval = 0);
  void startSetup();
  void stall();
  void handleEvent(uint32_t event);
  void handleDevice(uint32_t event);
  void handleEp0(unsigned ep, unsigned type, unsigned status);
  void handleSetup();
  void sendEp0(const void *data, uint32_t length, uint32_t wLength);
  void enableData();
  void disableData();
  void sendNotification();
  void receiveFrame(uint32_t length);
  void sendFrame(const uint8_t *frame, uint32_t length);
  void startBulkOut();
  void summary(const char *tag);
};

bool Gadget::epCmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t *result)
{
  dwc3Write(0xc800 + 0x10 * ep, p2);
  dwc3Write(0xc804 + 0x10 * ep, p1);
  dwc3Write(0xc808 + 0x10 * ep, p0);
  dwc3Write(0xc80c + 0x10 * ep, cmd | CMDACT);
  for (unsigned i = 0; i < 5000; ++i) {
    uint32_t v = dwc3Read(0xc80c + 0x10 * ep);
    if (!(v & CMDACT)) {
      if (result)
        *result = v;
      if ((v >> 12) & 0xf) {
        if (++fCmdFails <= 8)
          IOLog("usbgadget ep%u cmd %x status %x\n", ep, cmd & 0xff, (v >> 12) & 0xf);
        return false;
      }
      return true;
    }
    IODelay(1);
  }
  if (++fCmdFails <= 8)
    IOLog("usbgadget ep%u cmd %x timeout\n", ep, cmd & 0xff);
  return false;
}

bool Gadget::startTransfer(unsigned ep, uint32_t trbOffset, uint32_t bufOffset, uint32_t length, uint32_t trbctl,
                           uint8_t *resource)
{
  Trb *t = trb(trbOffset);
  uint64_t buffer = phys(bufOffset);
  t->bpl = static_cast<uint32_t>(buffer);
  t->bph = static_cast<uint32_t>(buffer >> 32);
  t->size = length & 0xffffff;
  t->ctrl = TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC | (trbctl << 4);
  barrier();
  uint64_t trbPhys = phys(trbOffset);
  uint32_t result = 0;
  bool ok = epCmd(ep, CMD_STARTTRANSFER, static_cast<uint32_t>(trbPhys >> 32), static_cast<uint32_t>(trbPhys), 0,
                  &result);
  if (ok && resource)
    *resource = static_cast<uint8_t>((result >> 16) & 0x7f);
  if (ok && ep <= 1) {
    fEp0Rsc[ep] = static_cast<uint8_t>((result >> 16) & 0x7f);
    fEp0Busy[ep] = true;
  }
  return ok;
}

// DEPCFG: param0 = type, max packet, FIFO (IN endpoints), action; param1 = events and endpoint number.
bool Gadget::configureEp(unsigned ep, uint32_t type, uint32_t maxPacket, uint32_t action, uint32_t interval)
{
  uint32_t p0 = ((type & 3) << 1) | ((maxPacket & 0x7ff) << 3) | (action << 30);
  if (ep & 1)
    p0 |= ((ep >> 1) & 0x1f) << 17;
  uint32_t p1 = (1U << 8) | ((ep & 0x1f) << 25);   // XferComplete
  if (ep <= 1)
    p1 |= 1U << 10;                                   // XferNotReady for the control endpoint
  if (interval)
    p1 |= ((interval - 1) & 0xff) << 16;
  return epCmd(ep, CMD_SETEPCONFIG, p0, p1, 0);
}

bool Gadget::start()
{
  fDma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task,
      static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
          static_cast<IOOptionBits>(kIOMapWriteCombineCache),
      DMA_BYTES, 0xfffffffffffff000ULL);
  if (!fDma || fDma->prepare() != kIOReturnSuccess) {
    IOLog("usbgadget: no DMA buffer\n");
    return false;
  }
  fVirt = static_cast<uint8_t *>(fDma->getBytesNoCopy());
  fPhys = fDma->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
  bzero(fVirt, DMA_BYTES);
  barrier();

  // Stop, power the eUSB2 PHY up, then the controller set-up of Linux's __dwc3_gadget_start.
  dwc3Write(DCTL, dwc3Read(DCTL) & ~0x80000000U);
  for (unsigned i = 0; i < 100 && !(dwc3Read(DSTS) & (1U << 22)); ++i)
    IOSleep(10);
  usbEusbInitiate();
  dwc3Write(DEVTEN, 0);
  dwc3Write(GEVNTADRLO, static_cast<uint32_t>(phys(OFF_EVENTS)));
  dwc3Write(GEVNTADRHI, static_cast<uint32_t>(phys(OFF_EVENTS) >> 32));
  dwc3Write(GEVNTSIZ, 0x80000000U | EVENT_BYTES);   // interrupt masked: polled
  dwc3Write(GEVNTCOUNT, dwc3Read(GEVNTCOUNT) & 0xffff);
  fEventOffset = 0;
  uint32_t dcfg = dwc3Read(DCFG);
  dcfg &= ~(7U | (0x7fU << 3));                     // high speed, address 0
  dwc3Write(DCFG, dcfg);
  dwc3Write(DALEPENA, 0);
  bool ok = epCmd(0, CMD_DEPSTARTCFG, 0, 0, 0);
  for (unsigned ep = 0; ep < 6; ++ep)
    ok &= epCmd(ep, CMD_SETTRANSFRESOURCE, 1, 0, 0);
  ok &= configureEp(EP0_OUT, 0, 512, 0);
  ok &= configureEp(EP0_IN, 0, 512, 0);
  dwc3Write(DALEPENA, 3);
  startSetup();
  if (fMode >= 2 && attachInterface())
    configureAddress();
  dwc3Write(DEVTEN, (1U << 0) | (1U << 1) | (1U << 2));   // disconnect, USB reset, connect done
  dwc3Write(DCTL, dwc3Read(DCTL) | 0x80000000U);
  IOLog("usbgadget: started (dma %llx, setup %s), dsts=%08x\n", fPhys, ok ? "ok" : "FAILED", dwc3Read(DSTS));
  return true;
}

void Gadget::startSetup()
{
  fEp0 = SETUP;
  fStatusReady = false;
  fStatusEp = -1;
  startTransfer(EP0_OUT, OFF_TRB_EP0, OFF_SETUP, 8, TRBCTL_SETUP);
}

void Gadget::stall()
{
  ++fStalls;
  epCmd(EP0_OUT, CMD_SETSTALL, 0, 0, 0);
  startSetup();
}

void Gadget::run()
{
  uint64_t lastSummary = 0, now = 0;
  for (;;) {
    uint32_t pending = dwc3Read(GEVNTCOUNT) & 0xffff;
    if (pending) {
      barrier();
      volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(fVirt + OFF_EVENTS);
      for (uint32_t done = 0; done < pending; done += 4) {
        uint32_t e = ring[fEventOffset / 4];
        fEventOffset = (fEventOffset + 4) % EVENT_BYTES;
        ++fEvents;
        handleEvent(e);
      }
      dwc3Write(GEVNTCOUNT, pending);
      pumpTx();
      pumpRaw();
      continue;
    }
    pumpTx();
    pumpRaw();
    IOSleep(1);
    clock_get_uptime(&now);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(now - lastSummary, &ns);
    if (ns > 20000000000ULL) {
      lastSummary = now;
      summary("s");
    }
  }
}

void Gadget::summary(const char *tag)
{
  IOLog("usbgadget %s dsts=%08x ev=%u rst=%u conn=%u setup=%u stall=%u cfg=%u alt=%u cmdfail=%u last=%08x%08x "
        "rx=%u tx=%u arp=%u ping=%u if=%s in=%u out=%u txdrop=%u rxdrop=%u addr=%d\n",
        tag, dwc3Read(DSTS), fEvents, fResets, fConnects, fSetups, fStalls, fConfig, fAltData, fCmdFails,
        fLastSetup[0], fLastSetup[1], fFramesIn, fFramesOut, fArp, fPing, fIfp ? "en0" : "-", fIfInput, fIfOutput,
        fTxDrops, fRxDrops, fAddrError);
  IOLog("usbgadget %s log=%llu/%u pkts cmds=%u shots=%u\n", tag, fLogSent, fLogPackets, fCommands, fShots);
}

void Gadget::handleEvent(uint32_t e)
{
  if (e & 1) {
    if (((e >> 1) & 0x7f) == 0)
      handleDevice(e);
    return;
  }
  unsigned ep = (e >> 1) & 0x1f, type = (e >> 6) & 0xf, status = (e >> 12) & 0xf;
  if (ep <= 1) {
    handleEp0(ep, type, status);
    return;
  }
  if (type != 1)   // only XferComplete matters for the data endpoints
    return;
  if (ep == EP_BULK_OUT) {
    fBulkOutBusy = false;
    Trb *t = trb(OFF_TRB_OUT);
    barrier();
    receiveFrame(OUT_BYTES - (t->size & 0xffffff));
    startBulkOut();
  } else if (ep == EP_BULK_IN) {
    fBulkInBusy = false;
    pumpTx();
    pumpRaw();
  } else if (ep == EP_NOTIFY) {
    fNotifyBusy = false;
    sendNotification();
  }
}

void Gadget::handleDevice(uint32_t e)
{
  unsigned type = (e >> 8) & 0x1f;
  switch (type) {
  case 0:   // disconnect
  case 1:   // USB reset
    if (type == 1)
      ++fResets;
    disableData();
    dwc3Write(DCFG, dwc3Read(DCFG) & ~(0x7fU << 3));
    fConfig = 0;
    fAltData = 0;
    if (fEp0 != SETUP) {
      for (unsigned ep = 0; ep < 2; ++ep)
        if (fEp0Busy[ep]) {
          epCmd(ep, CMD_ENDTRANSFER | CMDIOC | HIPRI_FORCERM | (static_cast<uint32_t>(fEp0Rsc[ep]) << 16), 0, 0, 0);
          fEp0Busy[ep] = false;
        }
      startSetup();
    }
    break;
  case 2: {   // connect done: the control endpoint's packet size for the negotiated speed
    ++fConnects;
    unsigned speed = dwc3Read(DSTS) & 7;
    uint32_t mps = speed == 0 ? 64 : speed >= 4 ? 512 : 64;
    configureEp(EP0_OUT, 0, mps, 2);
    configureEp(EP0_IN, 0, mps, 2);
    if (fConnects <= 4)
      IOLog("usbgadget connect done speed %u\n", speed);
    break;
  }
  default:
    break;
  }
}

void Gadget::handleEp0(unsigned ep, unsigned type, unsigned status)
{
  if (type == 3) {   // XferNotReady
    if ((status & 3) != 2)
      return;        // data phase request: our data TRB is already queued
    if (fEp0 == STATUS_PENDING) {
      fEp0 = STATUS;
      startTransfer(ep, OFF_TRB_EP0, OFF_EP0, 0, fThreeStage ? TRBCTL_STATUS3 : TRBCTL_STATUS2);
    } else {
      fStatusReady = true;
      fStatusEp = static_cast<int>(ep);
    }
    return;
  }
  if (type != 1)
    return;
  fEp0Busy[ep] = false;
  switch (fEp0) {
  case SETUP:
    handleSetup();
    break;
  case DATA:
    fEp0 = STATUS_PENDING;
    if (fStatusReady) {
      fEp0 = STATUS;
      startTransfer(static_cast<unsigned>(fStatusEp), OFF_TRB_EP0, OFF_EP0, 0, TRBCTL_STATUS3);
    }
    break;
  case STATUS:
    startSetup();
    break;
  case STATUS_PENDING:
    break;
  }
}

void Gadget::sendEp0(const void *data, uint32_t length, uint32_t wLength)
{
  if (length > wLength)
    length = wLength;
  if (length > EP0_BYTES)
    length = EP0_BYTES;
  memcpy(fVirt + OFF_EP0, data, length);
  fDataIn = true;
  fEp0 = DATA;
  startTransfer(EP0_IN, OFF_TRB_EP0, OFF_EP0, length, TRBCTL_DATA);
}

void Gadget::handleSetup()
{
  barrier();
  const uint8_t *s = fVirt + OFF_SETUP;
  uint8_t type = s[0], request = s[1];
  uint16_t value = static_cast<uint16_t>(s[2] | (s[3] << 8)), index = static_cast<uint16_t>(s[4] | (s[5] << 8));
  uint16_t length = static_cast<uint16_t>(s[6] | (s[7] << 8));
  ++fSetups;
  memcpy(fLastSetup, s, 8);
  if (fLoggedSetups < 24) {
    ++fLoggedSetups;
    IOLog("usbgadget setup %02x %02x %04x %04x %04x\n", type, request, value, index, length);
  }
  fThreeStage = length != 0;
  fStatusReady = false;
  fStatusEp = -1;
  uint8_t reply[256];

  if (type == 0x80 && request == 6) {   // GET_DESCRIPTOR
    uint8_t kind = static_cast<uint8_t>(value >> 8), which = static_cast<uint8_t>(value);
    if (kind == 1) {
      sendEp0(deviceDescriptor, sizeof(deviceDescriptor), length);
    } else if (kind == 2) {
      sendEp0(configDescriptor, sizeof(configDescriptor), length);
    } else if (kind == 3 && which == 0) {
      const uint8_t languages[4] = {4, 3, 0x09, 0x04};
      sendEp0(languages, 4, length);
    } else if (kind == 3 && which < sizeof(strings) / sizeof(strings[0])) {
      const char *text = strings[which];
      uint32_t n = static_cast<uint32_t>(strlen(text));
      reply[0] = static_cast<uint8_t>(2 + 2 * n);
      reply[1] = 3;
      for (uint32_t i = 0; i < n; ++i) {
        reply[2 + 2 * i] = static_cast<uint8_t>(text[i]);
        reply[3 + 2 * i] = 0;
      }
      sendEp0(reply, reply[0], length);
    } else {
      stall();
    }
    return;
  }
  if ((type & 0x9f) == 0x80 && request == 0) {   // GET_STATUS (device, interface, endpoint)
    reply[0] = reply[1] = 0;
    sendEp0(reply, 2, length);
    return;
  }
  if (type == 0x80 && request == 8) {            // GET_CONFIGURATION
    reply[0] = fConfig;
    sendEp0(reply, 1, length);
    return;
  }
  if (type == 0x81 && request == 10) {           // GET_INTERFACE
    reply[0] = index == 1 ? fAltData : 0;
    sendEp0(reply, 1, length);
    return;
  }
  if (length == 0) {
    bool ok = true;
    if (type == 0x00 && request == 5) {           // SET_ADDRESS: takes effect now, before the status stage
      dwc3Write(DCFG, (dwc3Read(DCFG) & ~(0x7fU << 3)) | ((value & 0x7fU) << 3));
    } else if (type == 0x00 && request == 9) {    // SET_CONFIGURATION
      if (value == 1) {
        fConfig = 1;
        enableData();
      } else if (value == 0) {
        fConfig = 0;
        disableData();
      } else {
        ok = false;
      }
    } else if (type == 0x01 && request == 11) {   // SET_INTERFACE
      if (index == 1) {
        fAltData = static_cast<uint8_t>(value);
        if (value == 1 && fNotifyStep == 0) {
          fNotifyStep = 1;
          sendNotification();
        }
      }
    } else if ((type == 0x02 || type == 0x00) && (request == 1 || request == 3)) {   // CLEAR/SET_FEATURE
    } else if (type == 0x21) {                    // CDC class requests: packet filter and friends
    } else {
      ok = false;
    }
    if (!ok) {
      stall();
      return;
    }
    fEp0 = STATUS_PENDING;
    return;
  }
  if (type == 0x21) {                             // class request with data from the host: take it and ignore it
    fDataIn = false;
    fEp0 = DATA;
    uint32_t size = (length + 63U) & ~63U;
    if (size > EP0_BYTES)
      size = EP0_BYTES;
    startTransfer(EP0_OUT, OFF_TRB_EP0, OFF_EP0, size, TRBCTL_DATA);
    return;
  }
  stall();
}

void Gadget::enableData()
{
  if (fDataEnabled)
    return;
  fDataEnabled = true;
  epCmd(0, CMD_DEPSTARTCFG | (2U << 16), 0, 0, 0);
  configureEp(EP_NOTIFY, 3, 16, 0, 9);
  configureEp(EP_BULK_OUT, 2, 512, 0);
  configureEp(EP_BULK_IN, 2, 512, 0);
  dwc3Write(DALEPENA, dwc3Read(DALEPENA) | (1U << EP_NOTIFY) | (1U << EP_BULK_OUT) | (1U << EP_BULK_IN));
  startBulkOut();
  if (fNotifyStep == 0) {
    fNotifyStep = 1;
    sendNotification();
  }
}

void Gadget::disableData()
{
  if (fBulkOutBusy)
    epCmd(EP_BULK_OUT, CMD_ENDTRANSFER | CMDIOC | HIPRI_FORCERM | (static_cast<uint32_t>(fOutRsc) << 16), 0, 0, 0);
  if (fBulkInBusy)
    epCmd(EP_BULK_IN, CMD_ENDTRANSFER | CMDIOC | HIPRI_FORCERM | (static_cast<uint32_t>(fInRsc) << 16), 0, 0, 0);
  if (fNotifyBusy)
    epCmd(EP_NOTIFY, CMD_ENDTRANSFER | CMDIOC | HIPRI_FORCERM | (static_cast<uint32_t>(fNotifyRsc) << 16), 0, 0, 0);
  fBulkOutBusy = fBulkInBusy = fNotifyBusy = false;
  fNotifyStep = 0;
  fDataEnabled = false;
  dwc3Write(DALEPENA, dwc3Read(DALEPENA) & 3U);
}

// NETWORK_CONNECTION (connected), then CONNECTION_SPEED_CHANGE (480 Mbit/s both ways).
void Gadget::sendNotification()
{
  if (fNotifyBusy || fNotifyStep == 0 || fNotifyStep >= 3 || !fConfig)
    return;
  uint8_t *n = fVirt + OFF_INTR;
  uint32_t length;
  if (fNotifyStep == 1) {
    const uint8_t connected[8] = {0xa1, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(n, connected, 8);
    length = 8;
  } else {
    const uint8_t speed[16] = {0xa1, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
                               0x00, 0x18, 0x9c, 0x1c, 0x00, 0x18, 0x9c, 0x1c};
    memcpy(n, speed, 16);
    length = 16;
  }
  ++fNotifyStep;
  fNotifyBusy = startTransfer(EP_NOTIFY, OFF_TRB_INTR, OFF_INTR, length, TRBCTL_NORMAL, &fNotifyRsc);
}

void Gadget::startBulkOut()
{
  if (fBulkOutBusy || !fConfig)
    return;
  fBulkOutBusy = startTransfer(EP_BULK_OUT, OFF_TRB_OUT, OFF_OUT, OUT_BYTES, TRBCTL_NORMAL, &fOutRsc);
}

void Gadget::sendFrame(const uint8_t *frame, uint32_t length)
{
  if (fBulkInBusy || !fConfig || length > IN_BYTES - 1)
    return;
  memcpy(fVirt + OFF_IN, frame, length);
  if ((length % 512) == 0)
    fVirt[OFF_IN + length++] = 0;   // never a multiple of the packet size: no zero-length packet needed
  ++fFramesOut;
  fBulkInBusy = startTransfer(EP_BULK_IN, OFF_TRB_IN, OFF_IN, length, TRBCTL_NORMAL, &fInRsc);
}

uint16_t ipChecksum(const uint8_t *data, uint32_t length)
{
  uint32_t sum = 0;
  for (uint32_t i = 0; i + 1 < length; i += 2)
    sum += static_cast<uint32_t>((data[i] << 8) | data[i + 1]);
  if (length & 1)
    sum += static_cast<uint32_t>(data[length - 1] << 8);
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

// Stage 1: answer ARP and ICMP echo for 169.254.7.2 directly.
void Gadget::receiveFrame(uint32_t length)
{
  ++fFramesIn;
  barrier();
  const uint8_t *f = fVirt + OFF_OUT;
  if (length < 14)
    return;
  uint16_t ethertype = static_cast<uint16_t>((f[12] << 8) | f[13]);
  if (fLoggedFrames < 12) {
    ++fLoggedFrames;
    IOLog("usbgadget rx %u bytes type %04x\n", length, ethertype);
  }
  if (fMode >= 2 && command(f, length))
    return;
  if (fIfp) {
    // Stage 2: into the BSD stack (the layout of feth's receive path).
    mbuf_t m = nullptr;
    if (length > 1600 || mbuf_gethdr(MBUF_DONTWAIT, MBUF_TYPE_DATA, &m) != 0) {
      ++fRxDrops;
      return;
    }
    if (length > mbuf_maxlen(m) && mbuf_mclget(MBUF_DONTWAIT, MBUF_TYPE_DATA, &m) != 0) {
      mbuf_freem(m);
      ++fRxDrops;
      return;
    }
    memcpy(mbuf_data(m), f, length);
    mbuf_setlen(m, length);
    mbuf_pkthdr_setlen(m, length);
    mbuf_pkthdr_setrcvif(m, fIfp);
    char *header = static_cast<char *>(mbuf_data(m));
    mbuf_pkthdr_setheader(m, header);
    mbuf_setdata(m, header + 14, length - 14);
    mbuf_pkthdr_adjustlen(m, -14);
    struct ifnet_stat_increment_param stats = {};
    stats.packets_in = 1;
    stats.bytes_in = length;
    ++fIfInput;
    ifnet_input(fIfp, m, &stats);
    return;
  }
  uint8_t out[1600];
  if (ethertype == 0x0806 && length >= 42) {
    const uint8_t *a = f + 14;
    if (a[6] == 0 && a[7] == 1 && !memcmp(a + 24, phoneIp, 4)) {   // request for our address
      ++fArp;
      memcpy(out, f + 6, 6);
      memcpy(out + 6, phoneMac, 6);
      out[12] = 0x08;
      out[13] = 0x06;
      uint8_t *r = out + 14;
      memcpy(r, a, 6);                // hardware/protocol types and sizes
      r[6] = 0;
      r[7] = 2;                       // reply
      memcpy(r + 8, phoneMac, 6);
      memcpy(r + 14, phoneIp, 4);
      memcpy(r + 18, a + 8, 6);       // the asker's MAC
      memcpy(r + 24, a + 14, 4);      // and IP
      bzero(out + 42, 18);
      sendFrame(out, 60);
    }
    return;
  }
  if (ethertype == 0x0800 && length >= 34) {
    const uint8_t *ip = f + 14;
    uint32_t ihl = (ip[0] & 0xf) * 4U;
    uint32_t total = static_cast<uint32_t>((ip[2] << 8) | ip[3]);
    if (ip[9] != 1 || memcmp(ip + 16, phoneIp, 4) || total < ihl + 8 || 14 + total > length || total > 1500)
      return;
    const uint8_t *icmp = ip + ihl;
    if (icmp[0] != 8)
      return;
    ++fPing;
    memcpy(out, f + 6, 6);
    memcpy(out + 6, phoneMac, 6);
    out[12] = 0x08;
    out[13] = 0x00;
    uint8_t *rip = out + 14;
    memcpy(rip, ip, total);
    memcpy(rip + 12, ip + 16, 4);
    memcpy(rip + 16, ip + 12, 4);
    rip[8] = 64;
    rip[10] = rip[11] = 0;
    uint16_t sum = ipChecksum(rip, ihl);
    rip[10] = static_cast<uint8_t>(sum >> 8);
    rip[11] = static_cast<uint8_t>(sum);
    uint8_t *ricmp = rip + ihl;
    ricmp[0] = 0;
    ricmp[2] = ricmp[3] = 0;
    sum = ipChecksum(ricmp, total - ihl);
    ricmp[2] = static_cast<uint8_t>(sum >> 8);
    ricmp[3] = static_cast<uint8_t>(sum);
    uint32_t frame = 14 + total;
    if (frame < 60) {
      bzero(out + frame, 60 - frame);
      frame = 60;
    }
    sendFrame(out, frame);
  }
}


// ---- Live control ---------------------------------------------------------------------------------------------------
void Gadget::sendUdp(const uint8_t mac[6], const uint8_t ip[4], uint16_t srcPort, uint16_t dstPort, const void *header,
                     uint32_t headerLength, const void *payload, uint32_t length)
{
  uint8_t frame[1514];
  uint32_t udpLength = 8 + headerLength + length;
  uint32_t ipLength = 20 + udpLength;
  if (14 + ipLength > sizeof(frame))
    return;
  memcpy(frame, mac, 6);
  memcpy(frame + 6, phoneMac, 6);
  frame[12] = 0x08;
  frame[13] = 0x00;
  uint8_t *iph = frame + 14;
  static uint16_t ident;
  ++ident;
  const uint8_t ipHeader[20] = {0x45, 0, static_cast<uint8_t>(ipLength >> 8), static_cast<uint8_t>(ipLength),
                                static_cast<uint8_t>(ident >> 8), static_cast<uint8_t>(ident), 0x40, 0, 64, 17, 0, 0,
                                phoneIp[0], phoneIp[1], phoneIp[2], phoneIp[3], ip[0], ip[1], ip[2], ip[3]};
  memcpy(iph, ipHeader, 20);
  uint16_t sum = ipChecksum(iph, 20);
  iph[10] = static_cast<uint8_t>(sum >> 8);
  iph[11] = static_cast<uint8_t>(sum);
  uint8_t *udp = iph + 20;
  udp[0] = static_cast<uint8_t>(srcPort >> 8);
  udp[1] = static_cast<uint8_t>(srcPort);
  udp[2] = static_cast<uint8_t>(dstPort >> 8);
  udp[3] = static_cast<uint8_t>(dstPort);
  udp[4] = static_cast<uint8_t>(udpLength >> 8);
  udp[5] = static_cast<uint8_t>(udpLength);
  udp[6] = udp[7] = 0;   // no UDP checksum (allowed over IPv4)
  memcpy(udp + 8, header, headerLength);
  memcpy(udp + 8 + headerLength, payload, length);
  uint32_t total = 14 + ipLength;
  if (total < 60) {
    bzero(frame + total, 60 - total);
    total = 60;
  }
  sendFrame(frame, total);
}

// Raw traffic only when the BSD stack has nothing queued: a screenshot in progress, else the log.
void Gadget::pumpRaw()
{
  if (fMode < 2 || fBulkInBusy || !fConfig || fAltData != 1)
    return;
  if (fTxLock) {
    IOLockLock(fTxLock);
    bool queued = fTxTail != fTxHead;
    IOLockUnlock(fTxLock);
    if (queued)
      return;
  }
  if (fShot.active) {
    uint32_t n = fShot.total - fShot.offset;
    if (n > UDP_CHUNK)
      n = UDP_CHUNK;
    uint8_t header[20] = {'A', 'K', 'F', 'B'};
    uint32_t fields[3] = {fShot.id, fShot.offset, fShot.total};
    memcpy(header + 4, fields, 12);
    memcpy(header + 16, &fShot.width, 2);
    memcpy(header + 18, &fShot.height, 2);
    sendUdp(fShot.mac, fShot.ip, 5142, fShot.port, header, sizeof(header), fShotImage + fShot.offset, n);
    if (fBulkInBusy) {
      fShot.offset += n;
      if (fShot.offset >= fShot.total)
        fShot.active = false;
    }
    return;
  }
  uint64_t total = __atomic_load_n(reinterpret_cast<uint64_t *>(sheng_log_buffer + LOG_TOTAL), __ATOMIC_ACQUIRE);
  if (total == UINT64_MAX || fLogSent >= total)
    return;
  const uint8_t *source;
  uint64_t n;
  if (fLogSent < LOG_PREFIX_BYTES) {
    source = sheng_log_buffer + LOG_PREFIX + fLogSent;
    n = (total < LOG_PREFIX_BYTES ? total : LOG_PREFIX_BYTES) - fLogSent;
  } else {
    if (total - fLogSent > LOG_TAIL_BYTES - 4096)
      fLogSent = total - (LOG_TAIL_BYTES - 4096);   // overwritten meanwhile: skip ahead
    uint64_t index = fLogSent % LOG_TAIL_BYTES;
    source = sheng_log_buffer + LOG_TAIL + index;
    n = total - fLogSent;
    if (n > LOG_TAIL_BYTES - index)
      n = LOG_TAIL_BYTES - index;
  }
  if (n > UDP_CHUNK)
    n = UDP_CHUNK;
  uint8_t header[8] = {'A', 'K', 'L', 'G'};
  uint32_t offset = static_cast<uint32_t>(fLogSent);
  memcpy(header + 4, &offset, 4);
  const uint8_t everyone[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, broadcastIp[4] = {169, 254, 255, 255};
  sendUdp(everyone, broadcastIp, 5140, 5140, header, sizeof(header), source, static_cast<uint32_t>(n));
  if (fBulkInBusy) {
    fLogSent += n;
    ++fLogPackets;
  }
}

extern "C" void akita_reset_to_fastboot(void);   // bsd/kern/kern_exec.c

// UDP to 169.254.7.2:5141 is a command for the gadget, not for the stack.
bool Gadget::command(const uint8_t *f, uint32_t length)
{
  if (length < 42 || f[12] != 0x08 || f[13] != 0x00)
    return false;
  const uint8_t *ip = f + 14;
  uint32_t ihl = (ip[0] & 0xf) * 4U;
  if (ip[9] != 17 || memcmp(ip + 16, phoneIp, 4) || 14 + ihl + 8 > length)
    return false;
  const uint8_t *udp = ip + ihl;
  uint16_t dstPort = static_cast<uint16_t>((udp[2] << 8) | udp[3]);
  if (dstPort != 5141)
    return false;
  uint16_t srcPort = static_cast<uint16_t>((udp[0] << 8) | udp[1]);
  uint32_t udpLength = static_cast<uint32_t>((udp[4] << 8) | udp[5]);
  if (udpLength < 9 || 14 + ihl + udpLength > length)
    return true;
  const char *text = reinterpret_cast<const char *>(udp + 8);
  uint32_t textLength = udpLength - 8;
  while (textLength && (text[textLength - 1] == '\n' || text[textLength - 1] == '\r'))
    --textLength;
  ++fCommands;
  if (text[0] == 'I' && gRemoteInput) {
    gRemoteInput(text + 1, textLength - 1);
  } else if (text[0] == 'S') {
    unsigned scale = textLength > 1 && text[1] >= '2' && text[1] <= '4' ? static_cast<unsigned>(text[1] - '0') : 2;
    startShot(f + 6, ip + 12, srcPort, scale);
  } else if (text[0] == 'L') {
    fLogSent = 0;
  } else if (textLength == 8 && !memcmp(text, "FASTBOOT", 8)) {
    // back to fastboot even when userland is wedged (no shell): PMU boot mode + watchdog, in the kernel
    IOLog("usbgadget: FASTBOOT requested from the Mac\n");
    akita_reset_to_fastboot();
  }
  return true;
}

void Gadget::startShot(const uint8_t mac[6], const uint8_t ip[4], uint16_t port, unsigned scale)
{
  if (!fFbMap) {
    if (IOService::getPlatform()->getConsoleInfo(&fVideo) != kIOReturnSuccess || fVideo.v_depth != 32 ||
        !fVideo.v_width || !fVideo.v_height || fVideo.v_rowBytes < fVideo.v_width * 4) {
      IOLog("usbgadget: no framebuffer for screenshots\n");
      return;
    }
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withPhysicalAddress(fVideo.v_baseAddr & ~3UL,
                                                                         fVideo.v_rowBytes * fVideo.v_height,
                                                                         kIODirectionIn);
    if (memory) {
      fFbMap = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
      memory->release();
    }
    if (!fFbMap)
      return;
    fShotImage = static_cast<uint8_t *>(IOMalloc((fVideo.v_width / 2) * (fVideo.v_height / 2) * 3));
    if (!fShotImage)
      return;
  }
  if (scale < 2)
    scale = 2;
  uint32_t width = static_cast<uint32_t>(fVideo.v_width / scale), height = static_cast<uint32_t>(fVideo.v_height / scale);
  const uint8_t *fb = reinterpret_cast<const uint8_t *>(fFbMap->getVirtualAddress());
  uint8_t *out = fShotImage;
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t *row = reinterpret_cast<const uint32_t *>(fb + static_cast<uint64_t>(y) * scale * fVideo.v_rowBytes);
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t p = row[x * scale];   // BGRA in memory: blue in the low byte
      *out++ = static_cast<uint8_t>(p >> 16);
      *out++ = static_cast<uint8_t>(p >> 8);
      *out++ = static_cast<uint8_t>(p);
    }
  }
  memcpy(fShot.mac, mac, 6);
  memcpy(fShot.ip, ip, 4);
  fShot.port = port;
  fShot.width = static_cast<uint16_t>(width);
  fShot.height = static_cast<uint16_t>(height);
  fShot.offset = 0;
  fShot.total = width * height * 3;
  ++fShot.id;
  ++fShots;
  fShot.active = true;
}

// ---- Stage 2: the BSD interface -------------------------------------------------------------------------------------
errno_t gadgetIfOutput(ifnet_t ifp, mbuf_t m);
errno_t gadgetIfIoctl(ifnet_t ifp, unsigned long cmd, void *data);
void gadgetIfDetach(ifnet_t) {}

bool Gadget::attachInterface()
{
  fTxLock = IOLockAlloc();
  fTx = static_cast<TxSlot *>(IOMalloc(sizeof(TxSlot) * txSlots));
  if (!fTxLock || !fTx) {
    IOLog("usbgadget: no memory for the interface\n");
    return false;
  }
  struct ifnet_init_eparams init = {};
  init.ver = IFNET_INIT_CURRENT_VERSION;
  init.len = sizeof(init);
  init.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
  init.output = gadgetIfOutput;
  init.uniqueid = "akita-usb-ecm";
  init.uniqueid_len = 13;
  init.name = "en";
  init.unit = 0;
  init.family = IFNET_FAMILY_ETHERNET;
  init.type = IFT_ETHER;
  init.demux = ether_demux;
  init.add_proto = ether_add_proto;
  init.del_proto = ether_del_proto;
  init.check_multi = ether_check_multi;
  init.framer_extended = ether_frameout_extended;
  init.softc = this;
  init.ioctl = gadgetIfIoctl;
  init.detach = gadgetIfDetach;
  static const uint8_t broadcast[ETHER_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  init.broadcast_addr = broadcast;
  init.broadcast_len = ETHER_ADDR_LEN;
  ifnet_t ifp = nullptr;
  errno_t error = ifnet_allocate_extended(&init, &ifp);
  if (error) {
    IOLog("usbgadget: ifnet_allocate_extended %d\n", error);
    return false;
  }
  ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
  ifnet_set_baudrate(ifp, 480000000);
  ifnet_set_mtu(ifp, ETHERMTU);
  ifnet_set_flags(ifp, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
  ifnet_set_hdrlen(ifp, sizeof(struct ether_header));
  ifnet_set_offload(ifp, 0);
  error = ifnet_attach(ifp, nullptr);
  if (error) {
    IOLog("usbgadget: ifnet_attach %d\n", error);
    ifnet_release(ifp);
    return false;
  }
  ifnet_set_lladdr(ifp, phoneMac, ETHER_ADDR_LEN);
  fIfp = ifp;
  IOLog("usbgadget: interface %s%u attached\n", ifnet_name(ifp), ifnet_unit(ifp));
  return true;
}

// 169.254.7.2/16 and up, from the kernel (SIOCAIFADDR on a kernel socket), so nothing has to be typed for it.
void Gadget::configureAddress()
{
  ifnet_set_flags(fIfp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
  socket_t so = nullptr;
  fAddrError = sock_socket(PF_INET, SOCK_DGRAM, 0, nullptr, nullptr, &so);
  if (fAddrError)
    return;
  struct ifaliasreq request = {};
  snprintf(request.ifra_name, sizeof(request.ifra_name), "%s%u", ifnet_name(fIfp), ifnet_unit(fIfp));
  struct sockaddr_in *address = reinterpret_cast<struct sockaddr_in *>(&request.ifra_addr);
  address->sin_len = sizeof(*address);
  address->sin_family = AF_INET;
  memcpy(&address->sin_addr, phoneIp, 4);
  struct sockaddr_in *broadcast = reinterpret_cast<struct sockaddr_in *>(&request.ifra_broadaddr);
  broadcast->sin_len = sizeof(*broadcast);
  broadcast->sin_family = AF_INET;
  const uint8_t all[4] = {169, 254, 255, 255};
  memcpy(&broadcast->sin_addr, all, 4);
  struct sockaddr_in *mask = reinterpret_cast<struct sockaddr_in *>(&request.ifra_mask);
  mask->sin_len = sizeof(*mask);
  mask->sin_family = AF_INET;
  const uint8_t bits[4] = {255, 255, 0, 0};
  memcpy(&mask->sin_addr, bits, 4);
  fAddrError = sock_ioctl(so, SIOCAIFADDR, &request);
  sock_close(so);
  IOLog("usbgadget: address 169.254.7.2/16 on en0: %d\n", fAddrError);
}

errno_t Gadget::ifOutput(mbuf_t m)
{
  size_t length = mbuf_pkthdr_len(m);
  IOLockLock(fTxLock);
  unsigned next = (fTxHead + 1) % txSlots;
  if (length < 14 || length > 1600 || next == fTxTail) {
    ++fTxDrops;
  } else {
    mbuf_copydata(m, 0, length, fTx[fTxHead].data);
    fTx[fTxHead].length = static_cast<uint16_t>(length);
    fTxHead = next;
    ++fIfOutput;
  }
  IOLockUnlock(fTxLock);
  ifnet_stat_increment_out(fIfp, 1, static_cast<uint32_t>(length), 0);
  mbuf_freem(m);
  return 0;
}

void Gadget::pumpTx()
{
  if (!fTxLock || fBulkInBusy || !fConfig)
    return;
  IOLockLock(fTxLock);
  if (fTxTail != fTxHead) {
    TxSlot &slot = fTx[fTxTail];
    uint32_t length = slot.length;
    if (length < 60) {
      bzero(slot.data + length, 60 - length);
      length = 60;
    }
    sendFrame(slot.data, length);
    if (fBulkInBusy)
      fTxTail = (fTxTail + 1) % txSlots;
  }
  IOLockUnlock(fTxLock);
}

errno_t gadgetIfOutput(ifnet_t ifp, mbuf_t m)
{
  return static_cast<Gadget *>(ifnet_softc(ifp))->ifOutput(m);
}

errno_t gadgetIfIoctl(ifnet_t ifp, unsigned long cmd, void *data)
{
  switch (cmd) {
  case SIOCSIFADDR:
    ifnet_set_flags(ifp, IFF_UP, IFF_UP);
    return 0;
  case SIOCSIFFLAGS:
    ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
    return 0;
  case SIOCSIFMTU:
    return ifnet_set_mtu(ifp, static_cast<struct ifreq *>(data)->ifr_mtu);
  case SIOCADDMULTI:
  case SIOCDELMULTI:
    return 0;
  default:
    return ether_ioctl(ifp, static_cast<uint32_t>(cmd), data);
  }
}

Gadget gGadget;

void gadgetThread(void *, wait_result_t)
{
  IOLog("usbgadget armed: start in 60 s\n");
  IOSleep(60000);
  for (unsigned i = 0; i < winCount; ++i) {
    if (usbProbeWindows[i].map)
      continue;
    IODeviceMemory *memory = IODeviceMemory::withRange(usbProbeWindows[i].phys, usbProbeWindows[i].length);
    if (memory) {
      usbProbeWindows[i].map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
      memory->release();
    }
  }
  IOLog("usbgadget pmu hsi0 %llx phy %llx\n", tensorPmuRead(0x15462a84), tensorPmuRead(0x15463eb0));
  unsigned mode = 1;
  PE_parse_boot_argn("akita-usb-gadget", &mode, sizeof(mode));
  gGadget.setMode(mode);
  if (gGadget.start())
    gGadget.run();
}

}  // namespace gadget
