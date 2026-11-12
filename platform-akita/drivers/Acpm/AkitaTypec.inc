// akita (Generated-by: LLM, local only): USB Power Delivery on the MAX77759 TCPC (I2C 0x25), phase 2 of video and USB host
// over USB-C (docs/research-hdmi-usbc.md). A polling kernel thread speaks just enough PD for a powered USB-C hub with HDMI:
//   sink: Source_Capabilities -> Request (5 V) -> Accept -> PS_RDY, then COMMAND SinkVbus (charging again);
//   data role: accept the hub's DR_Swap or ask for one, so the phone becomes the host (DFP);
//   DisplayPort alternate mode as DFP: Discover Identity / SVIDs / Modes (0xFF01), Enter Mode, DP Status, DP Configure;
//   Attention / Status carry HPD.
// The phone never sources VBUS: the only TCPC commands sent are SinkVbus, the RX/TX buffer resets and Look4Connection-free
// ones listed in i2c::writeAllowed. The PMIC/charger (0x66) and the fuel gauge (0x36) are never written.
//   kern.akita_acpm = 5000000 + stage: 10 start the PD thread, 11 stop it, 12 print the PD state, 13 ask the source for its
//   capabilities again (Get_Source_Cap), 14 hard reset (the source restarts PD; VBUS may dip)
// Sources: USB PD 3.1 (message formats, structured VDMs), TCPCI rev 2.0 (register map), Linux tcpci.c (RX/TX buffer use),
// Linux typec/altmodes/displayport.c and include/linux/usb/typec_dp.h (DP configure VDO).
namespace i2c {
bool writeAllowed(uint8_t dev, uint8_t reg, const uint8_t *d, unsigned n) {
  if (dev != 0x25) return false;
  if (reg == 0x10 && n <= 2) return true;                                // ALERT: write 1 to clear
  if (reg == 0x2E && n == 1) return true;                                // MESSAGE_HEADER_INFO
  if (reg == 0x19 && n == 1) return true;                                // TCPC_CONTROL (only the orientation bit is changed)
  if (reg == 0x2F && n == 1) return true;                                // RECEIVE_DETECT
  if (reg == 0x50 && n == 1) return true;                                // TRANSMIT
  if (reg == 0x51 && n >= 3 && n <= 31) return true;                     // TX byte count, header, data objects
  if (reg == 0x23 && n == 1)                                             // COMMAND: sink, buffer resets, VBUS detection
    return d[0] == 0x55 || d[0] == 0xDD || d[0] == 0xEE || d[0] == 0x33;
  return false;
}
}  // namespace i2c

namespace typec {

enum : uint8_t { kTcpc = 0x25, TCPC_CONTROL = 0x19, ALERT = 0x10, CC_STATUS = 0x1D, POWER_STATUS = 0x1E, COMMAND = 0x23, MSG_HDR_INFO = 0x2E,
                 RX_DETECT = 0x2F, RX_COUNT = 0x30, TRANSMIT = 0x50, TX_COUNT = 0x51 };
constexpr uint16_t A_CC = 1u << 0, A_POWER = 1u << 1, A_RX = 1u << 2, A_RX_HARD_RESET = 1u << 3, A_TX_FAILED = 1u << 4,
                   A_TX_DISCARDED = 1u << 5, A_TX_SUCCESS = 1u << 6;
// control messages
enum : uint8_t { C_GOODCRC = 1, C_ACCEPT = 3, C_REJECT = 4, C_PING = 5, C_PS_RDY = 6, C_GET_SRC_CAP = 7, C_GET_SNK_CAP = 8,
                 C_DR_SWAP = 9, C_PR_SWAP = 10, C_VCONN_SWAP = 11, C_WAIT = 12, C_SOFT_RESET = 13, C_NOT_SUPPORTED = 16 };
// data messages
enum : uint8_t { D_SRC_CAP = 1, D_REQUEST = 2, D_SNK_CAP = 4, D_VDM = 15 };
// structured VDM commands
enum : uint8_t { V_DISC_IDENT = 1, V_DISC_SVID = 2, V_DISC_MODES = 3, V_ENTER = 4, V_EXIT = 5, V_ATTENTION = 6,
                 V_DP_STATUS = 0x10, V_DP_CONFIG = 0x11 };

struct State {
  bool run, contract, dfp, sinking, dpEntered, dpConfigured, hpd;
  uint8_t msgId;            // our MessageID, 3 bits
  uint8_t specRev;          // 1 = PD 2.0, 2 = PD 3.0 (the source's, once heard)
  uint32_t srcPdo[7]; unsigned srcPdos;
  uint32_t dpModeVdo, dpStatus;
  unsigned rx, tx, txFail, hardResets;
  uint8_t vdmStep;          // DFP discovery: 0 idle, 1 ident, 2 svids, 3 modes, 4 enter, 5 status, 6 config, 7 done
  uint64_t vdmAt;           // when the last VDM request went out
  thread_t thread;
};
State gS;

uint64_t nowMs() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000ull; }

int rd8(uint8_t reg, uint8_t &v) { return i2c::readRegs(kTcpc, reg, &v, 1); }
int rd16(uint8_t reg, uint16_t &v) { uint8_t b[2]; const int r = i2c::readRegs(kTcpc, reg, b, 2); v = static_cast<uint16_t>(b[0] | (b[1] << 8)); return r; }
int wr8(uint8_t reg, uint8_t v) { return i2c::writeRegs(kTcpc, reg, &v, 1); }
int clearAlert(uint16_t bits) { const uint8_t b[2] = {static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8)}; return i2c::writeRegs(kTcpc, ALERT, b, 2); }

// MESSAGE_HEADER_INFO: bit0 power role (0 sink), bits 2:1 spec revision, bit3 data role (1 DFP)
void headerInfo() { wr8(MSG_HDR_INFO, static_cast<uint8_t>(((gS.specRev & 3) << 1) | (gS.dfp ? 8 : 0))); }

const char *ctrlName(uint8_t t) {
  static const char *n[] = {"?", "GoodCRC", "GotoMin", "Accept", "Reject", "Ping", "PS_RDY", "Get_Source_Cap", "Get_Sink_Cap",
                            "DR_Swap", "PR_Swap", "VCONN_Swap", "Wait", "Soft_Reset", "Data_Reset", "Data_Reset_Complete",
                            "Not_Supported", "Get_Source_Cap_Extended", "Get_Status", "FR_Swap", "Get_PPS_Status",
                            "Get_Country_Codes", "Get_Sink_Cap_Extended"};
  return t < sizeof n / sizeof n[0] ? n[t] : "control?";
}

// Send a message on SOP. type: message type; objs: data objects (0 for a control message).
int send(uint8_t type, const uint32_t *objs, unsigned n) {
  const uint16_t hdr = static_cast<uint16_t>((type & 0x1f) | (gS.dfp ? (1u << 5) : 0) | ((gS.specRev & 3) << 6) |
                                             ((gS.msgId & 7) << 9) | ((n & 7) << 12));
  uint8_t b[31];
  b[0] = static_cast<uint8_t>(2 + 4 * n);
  b[1] = static_cast<uint8_t>(hdr); b[2] = static_cast<uint8_t>(hdr >> 8);
  for (unsigned i = 0; i < n; ++i) memcpy(b + 3 + 4 * i, &objs[i], 4);
  int r = i2c::writeRegs(kTcpc, TX_COUNT, b, 3 + 4 * n);
  if (!r) r = wr8(TRANSMIT, static_cast<uint8_t>((3u << 4) | 0));   // 3 retries, SOP
  // wait for the TCPC's verdict (GoodCRC received, or failed / discarded)
  for (unsigned t = 0; !r && t < 60; ++t) {
    uint16_t a = 0;
    if (rd16(ALERT, a)) break;
    if (a & (A_TX_SUCCESS | A_TX_FAILED | A_TX_DISCARDED)) {
      clearAlert(a & (A_TX_SUCCESS | A_TX_FAILED | A_TX_DISCARDED));
      if (a & A_TX_SUCCESS) { gS.msgId = (gS.msgId + 1) & 7; ++gS.tx; return 0; }
      ++gS.txFail;
      return (a & A_TX_FAILED) ? -10 : -11;
    }
    IOSleep(1);
  }
  ++gS.txFail;
  return r ? r : -12;
}
int sendCtrl(uint8_t type) {
  const int r = send(type, nullptr, 0);
  printf("typec: -> %s%s\n", ctrlName(type), r ? " (not delivered)" : "");
  return r;
}

uint32_t vdmHdr(uint16_t svid, uint8_t cmd, uint8_t objPos, uint8_t cmdType) {
  // structured, SVDM version 2.0 (bits 14:13 = 01) when PD 3.0, else 1.0
  return (static_cast<uint32_t>(svid) << 16) | (1u << 15) | ((gS.specRev >= 2 ? 1u : 0u) << 13) | ((objPos & 7u) << 8) |
         ((cmdType & 3u) << 6) | (cmd & 0x1fu);
}
int sendVdm(uint16_t svid, uint8_t cmd, uint8_t objPos, const uint32_t *vdos, unsigned n) {
  uint32_t o[7] = {vdmHdr(svid, cmd, objPos, 0)};
  for (unsigned i = 0; i < n && i < 6; ++i) o[1 + i] = vdos[i];
  const int r = send(D_VDM, o, 1 + n);
  printf("typec: -> VDM svid %04x cmd %u pos %u%s\n", svid, cmd, objPos, r ? " (not delivered)" : "");
  gS.vdmAt = nowMs();
  return r;
}

// The next step of DisplayPort discovery, as the DFP
void vdmNext() {
  switch (gS.vdmStep) {
    case 1: sendVdm(0xFF00, V_DISC_IDENT, 0, nullptr, 0); break;
    case 2: sendVdm(0xFF00, V_DISC_SVID, 0, nullptr, 0); break;
    case 3: sendVdm(0xFF01, V_DISC_MODES, 0, nullptr, 0); break;
    case 4: sendVdm(0xFF01, V_ENTER, 1, nullptr, 0); break;
    case 5: { const uint32_t st = 1u; sendVdm(0xFF01, V_DP_STATUS, 1, &st, 1); break; }   // we are a connected DFP_D
    case 6: {
      // pins the hub offers as UFP_D: bits 23:16 for a receptacle (bit 6), else 15:8. D (bit 3) keeps USB 3; then C (bit 2), E (bit 4)
      const uint32_t v = gS.dpModeVdo;
      const uint32_t pins = (v & (1u << 6)) ? ((v >> 16) & 0xff) : ((v >> 8) & 0xff);
      uint32_t pin = (pins & 8) ? 8 : (pins & 4) ? 4 : (pins & 16) ? 16 : 0;
      if (!pin) { printf("typec: DP mode offers no pin assignment we know (vdo %08x)\n", v); gS.vdmStep = 7; return; }
      const uint32_t conf = 2u | (1u << 2) | (pin << 8);   // UFP_U as UFP_D, DP signaling, pin assignment
      printf("typec: DP configure: pins offered %02x, using %c\n", pins, pin == 8 ? 'D' : pin == 4 ? 'C' : 'E');
      sendVdm(0xFF01, V_DP_CONFIG, 1, &conf, 1);
      break;
    }
    default: break;
  }
}

void onContract() {
  gS.contract = true;
  if (!gS.sinking) {
    const int r = wr8(COMMAND, 0x55);   // SinkVbus: take the hub's 5 V (charging)
    gS.sinking = r == 0;
    printf("typec: contract at 5 V, SinkVbus %d\n", r);
  }
  // become the host: the hub may ask for the swap itself; give it a moment, then ask
}

void handleVdm(const uint32_t *o, unsigned n) {
  const uint32_t h = o[0];
  const uint16_t svid = static_cast<uint16_t>(h >> 16);
  const uint8_t cmd = h & 0x1f, type = (h >> 6) & 3;
  printf("typec: <- VDM svid %04x cmd %u type %s, %u VDOs:", svid, cmd, type == 0 ? "REQ" : type == 1 ? "ACK" : type == 2 ? "NAK" : "BUSY", n - 1);
  for (unsigned i = 1; i < n; ++i) printf(" %08x", o[i]);
  printf("\n");
  if (type == 0) {   // a request to us
    if (cmd == V_ATTENTION && svid == 0xFF01 && n > 1) {
      gS.dpStatus = o[1]; gS.hpd = (o[1] >> 7) & 1;
      printf("typec: DP attention: status %08x, HPD %s\n", o[1], gS.hpd ? "high" : "low");
      return;
    }
    // we are not a UFP with modes: NAK requests (as a UFP, Discover Identity from the hub)
    uint32_t nak = vdmHdr(svid, cmd, (h >> 8) & 7, 2);
    send(D_VDM, &nak, 1);
    return;
  }
  if (type != 1) { printf("typec: VDM %u refused (%s)\n", cmd, type == 2 ? "NAK" : "BUSY"); gS.vdmStep = 7; return; }
  switch (cmd) {
    case V_DISC_IDENT: if (gS.vdmStep == 1) gS.vdmStep = 2; break;
    case V_DISC_SVID: {
      bool dp = false;
      for (unsigned i = 1; i < n; ++i) if ((o[i] >> 16) == 0xFF01 || (o[i] & 0xffff) == 0xFF01) dp = true;
      printf("typec: the hub %s DisplayPort (SVID ff01)\n", dp ? "offers" : "does not offer");
      gS.vdmStep = dp ? 3 : 7;
      break;
    }
    case V_DISC_MODES: if (n > 1) { gS.dpModeVdo = o[1]; printf("typec: DP mode VDO %08x\n", o[1]); gS.vdmStep = 4; } break;
    case V_ENTER: gS.dpEntered = true; printf("typec: DisplayPort mode entered\n"); gS.vdmStep = 5; break;
    case V_DP_STATUS: if (n > 1) { gS.dpStatus = o[1]; gS.hpd = (o[1] >> 7) & 1; printf("typec: DP status %08x HPD %s\n", o[1], gS.hpd ? "high" : "low"); } gS.vdmStep = 6; break;
    case V_DP_CONFIG: gS.dpConfigured = true; printf("typec: DisplayPort configured: the hub now routes DP lanes (next: the PHY and the DP link)\n"); gS.vdmStep = 7; break;
    default: break;
  }
  if (gS.vdmStep < 7) vdmNext();
}

void handleMessage(const uint8_t *b, unsigned cnt) {
  // b[0] frame type, b[1..2] header, then objects
  if (cnt < 3) return;
  const uint16_t hdr = static_cast<uint16_t>(b[1] | (b[2] << 8));
  const uint8_t type = hdr & 0x1f, nObj = (hdr >> 12) & 7, rev = (hdr >> 6) & 3;
  if (b[0] != 0) return;   // SOP only
  ++gS.rx;
  if (rev && rev < gS.specRev) { gS.specRev = rev; headerInfo(); }
  uint32_t o[7] = {};
  for (unsigned i = 0; i < nObj && 3 + 4 * i + 4 <= cnt; ++i) memcpy(&o[i], b + 3 + 4 * i, 4);
  if (!nObj) {
    printf("typec: <- %s\n", ctrlName(type));
    switch (type) {
      case C_ACCEPT: break;
      case C_PS_RDY: onContract(); break;
      case C_GET_SNK_CAP: { const uint32_t pdo = (1u << 26) | (1u << 25) | (100u << 10) | 150u; send(D_SNK_CAP, &pdo, 1); break; }   // 5 V 1.5 A, USB comm, dual-role data
      case C_DR_SWAP:
        if (sendCtrl(C_ACCEPT) == 0) {
          gS.dfp = !gS.dfp; headerInfo();
          printf("typec: data role swapped: the phone is now %s\n", gS.dfp ? "the host (DFP)" : "a device (UFP)");
          if (gS.dfp && gS.vdmStep == 0) { gS.vdmStep = 1; vdmNext(); }
        }
        break;
      case C_PR_SWAP: case C_VCONN_SWAP: sendCtrl(C_REJECT); break;   // never the power source
      case C_SOFT_RESET: gS.msgId = 0; sendCtrl(C_ACCEPT); gS.contract = false; break;
      case C_GET_SRC_CAP: sendCtrl(gS.specRev >= 2 ? C_NOT_SUPPORTED : C_REJECT); break;
      default: break;
    }
    return;
  }
  switch (type) {
    case D_SRC_CAP: {
      gS.srcPdos = nObj;
      printf("typec: <- Source_Capabilities (PD %s):", rev >= 2 ? "3.0" : "2.0");
      for (unsigned i = 0; i < nObj; ++i) {
        gS.srcPdo[i] = o[i];
        if ((o[i] >> 30) == 0) printf(" [%u] %u mV %u mA", i + 1, ((o[i] >> 10) & 0x3ff) * 50, (o[i] & 0x3ff) * 10);
        else printf(" [%u] %08x", i + 1, o[i]);
      }
      printf("\n");
      // Request PDO 1 (vSafe5V), its full current, USB communications capable, no USB suspend
      const uint32_t maxI = o[0] & 0x3ff;
      const uint32_t rdo = (1u << 28) | (1u << 25) | (1u << 24) | (maxI << 10) | maxI;
      const int r = send(D_REQUEST, &rdo, 1);
      printf("typec: -> Request 5 V %u mA%s\n", maxI * 10, r ? " (not delivered)" : "");
      break;
    }
    case D_VDM: handleVdm(o, nObj); break;
    default: printf("typec: <- data message %u with %u objects\n", type, nObj); break;
  }
}

void orient();
void poll() {
  uint16_t a = 0;
  if (rd16(ALERT, a) || !a) return;
  if (a & A_RX_HARD_RESET) {
    ++gS.hardResets; gS.contract = false; gS.msgId = 0; gS.dfp = false; gS.sinking = false; gS.vdmStep = 0;
    gS.dpEntered = gS.dpConfigured = false;
    headerInfo();
    wr8(RX_DETECT, 0x21);   // the TCPC cleared RECEIVE_DETECT: listen again before the source's capabilities come
    printf("typec: <- Hard Reset from the hub (%u), receive on again\n", gS.hardResets);
  }
  if (a & A_RX) {
    // MAX77759: the RX buffer can only be read through READABLE_BYTE_COUNT (0x30), in one transaction of count + 1 bytes
    // (count includes the frame-type byte). fullos236 read 0x31 separately and never saw a message (Linux
    // tcpci_maxim_core.c process_rx does it this way).
    uint8_t probe[2] = {};
    const int r1 = i2c::readRegs(kTcpc, RX_COUNT, probe, 2);
    const uint8_t cnt = probe[0];
    if (r1 || cnt < 3 || cnt > 31) {
      printf("typec: RX status with byte count %u frame %u (read %d): dropped\n", cnt, probe[1], r1);
      clearAlert(static_cast<uint16_t>(A_RX | (1u << 10)));
    } else {
      uint8_t b[33] = {};
      const int r2 = i2c::readRegs(kTcpc, RX_COUNT, b, cnt + 1u);
      clearAlert(static_cast<uint16_t>(A_RX | (a & (1u << 10))));   // RX_STATUS (+ RX_BUF_OVF) after the read
      if (!r2) handleMessage(b + 1, cnt); else printf("typec: RX buffer read failed %d\n", r2);
    }
    a &= static_cast<uint16_t>(~A_RX);
  }
  if (a & (A_CC | A_POWER)) {
    uint8_t cc = 0, pw = 0;
    rd8(CC_STATUS, cc); rd8(POWER_STATUS, pw);
    printf("typec: CC_STATUS %02x POWER_STATUS %02x\n", cc, pw);
    if (pw & 4) orient();
    if (!(pw & 4)) { gS.contract = gS.sinking = gS.dfp = false; gS.vdmStep = 0; gS.msgId = 0; headerInfo(); }   // VBUS gone: detached
  }
  const uint16_t rest = a & static_cast<uint16_t>(~(A_TX_SUCCESS | A_TX_FAILED | A_TX_DISCARDED | A_RX));
  if (rest) clearAlert(rest);
}

void thread(void *, wait_result_t) {
  printf("typec: PD thread up\n");
  uint64_t contractAt = 0;
  while (gS.run) {
    poll();
    const uint64_t t = nowMs();
    // with a contract and still a device, ask for the host role once a second, a few times
    if (gS.contract && !gS.dfp) {
      if (!contractAt) contractAt = t;
      static unsigned asked;
      if (t - contractAt > 500 && asked < 3 && t - contractAt > 500 + 1000ull * asked) { ++asked; sendCtrl(C_DR_SWAP); }
    } else contractAt = 0;
    // a VDM request with no answer in 1 s: send it again (up to the step's limit of the hub's patience)
    if (gS.dfp && gS.vdmStep && gS.vdmStep < 7 && t - gS.vdmAt > 1000) vdmNext();
    IOSleep(2);
  }
  printf("typec: PD thread stopped\n");
  thread_terminate(current_thread());
}

// BMC (PD) goes over the CC line the cable uses: TCPC_CONTROL bit 0 = CC2 (fullos234: the hub was on CC2, the orientation
// was CC1, and neither our messages nor a hard reset reached it). Linux tcpci_set_polarity does the same on attach.
void orient() {
  uint8_t cc = 0, ctl = 0;
  if (rd8(CC_STATUS, cc) || rd8(TCPC_CONTROL, ctl)) return;
  const uint8_t cc1 = cc & 3, cc2 = (cc >> 2) & 3;
  if (!cc1 == !cc2) { printf("typec: orientation unclear (CC_STATUS %02x), TCPC_CONTROL %02x left\n", cc, ctl); return; }
  const uint8_t want = static_cast<uint8_t>((ctl & ~1u) | (cc2 ? 1u : 0u));
  const int r = want != ctl ? wr8(TCPC_CONTROL, want) : 0;
  printf("typec: plug on CC%u: TCPC_CONTROL %02x -> %02x (%d)\n", cc2 ? 2 : 1, ctl, want, r);
}

// Our own hard reset: the source restarts PD and sends Source_Capabilities again; RECEIVE_DETECT must be set again at once
int hardReset() {
  int r = wr8(TRANSMIT, 5);
  for (unsigned t = 0; !r && t < 20; ++t) {   // the TCPC reports TX_SUCCESS | TX_FAILED when the hard reset went out
    uint16_t a = 0;
    if (rd16(ALERT, a)) break;
    if (a & (A_TX_SUCCESS | A_TX_FAILED | A_TX_DISCARDED)) { clearAlert(a & (A_TX_SUCCESS | A_TX_FAILED | A_TX_DISCARDED)); break; }
    IOSleep(1);
  }
  gS.msgId = 0; gS.contract = gS.sinking = gS.dfp = false; gS.vdmStep = 0;
  headerInfo();
  const int r2 = wr8(RX_DETECT, 0x21);
  printf("typec: -> Hard Reset %d, receive on again %d\n", r, r2);
  return r;
}

int start() {
  if (gS.run) return 0;
  memset(&gS, 0, sizeof gS);
  gS.specRev = 2;   // PD 3.0 until the source says otherwise
  gS.run = true;
  orient();
  headerInfo();
  wr8(COMMAND, 0xEE);                 // reset the receive buffer
  clearAlert(0xffff);
  const int r = wr8(RX_DETECT, 0x21);   // SOP messages and hard resets (the TCPC answers GoodCRC itself)
  printf("typec: PD receive on: %d\n", r);
  if (kernel_thread_start(thread, nullptr, &gS.thread) != KERN_SUCCESS) { gS.run = false; return 1; }
  thread_deallocate(gS.thread);
  // the hub stopped offering its capabilities long ago (nobody answered). A Get_Source_Cap without a contract makes it hard
  // reset (fullos235), so wait a moment for capabilities, then do the hard reset ourselves: it sends them again.
  for (unsigned t = 0; t < 1500 && !gS.srcPdos; t += 50) IOSleep(50);
  if (!gS.srcPdos) hardReset();
  return 0;
}

int command(uint32_t stage) {
  if (stage == 10) return start();
  if (stage == 11) { gS.run = false; wr8(RX_DETECT, 0); return 0; }
  if (stage == 12) {
    printf("typec: run %d contract %d sinking %d role %s spec %u rx %u tx %u fail %u hard resets %u, DP entered %d configured %d HPD %d step %u mode %08x status %08x\n",
           gS.run, gS.contract, gS.sinking, gS.dfp ? "DFP" : "UFP", gS.specRev, gS.rx, gS.tx, gS.txFail, gS.hardResets,
           gS.dpEntered, gS.dpConfigured, gS.hpd, gS.vdmStep, gS.dpModeVdo, gS.dpStatus);
    return 0;
  }
  if (stage == 13) return sendCtrl(C_GET_SRC_CAP);
  if (stage == 14) return hardReset();
  return -1;
}

}  // namespace typec
