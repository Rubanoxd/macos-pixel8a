// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeRecoveryInput for akita (Pixel 8a): the three side buttons as a boot keyboard, so macOS Recovery can be driven
// without a touch screen driver. The buttons are GPIO inputs of the ALIVE pin controller at 0x154D0000 (the zuma board DT's
// gpio-keys; Mu-Silicium's KeypadDeviceLib reads the same pins), active low:
//   volume up   gpa6-2 (DAT 0x154D00A4 bit 2) -> Up Arrow
//   volume down gpa4-1 (DAT 0x154D0084 bit 1) -> Down Arrow
//   power       gpa4-0 (DAT 0x154D0084 bit 0) -> Return (if the pin really carries the key; on zuma it may be
//                                                the PMIC's)
//   both volume buttons together              -> Return, latched until both are released
//   power with volume down                    -> Tab
//   power with volume up                      -> Space (presses the focused button)
// Holding both volume buttons for a second switches to pointer mode (and back). Pointer mode, for windows that only take
// clicks:
//   volume up   moves the cursor in the current direction, faster the longer it is held
//   volume down short press turns the direction clockwise (right, down, left, up); held, it moves the other way
//   power       left button (held: drag)
// The pointer is a second HID device, a boot mouse, under the same vendor/product identity.
// A third device, an absolute pointer (QEMU usb-tablet layout), serves a boot-arg input script, so a run can be driven
// without anyone at the phone: akita-input-script=STEP_STEP_... with steps
//   W<ms>        wait            A<x>,<y>  move the tablet pointer to screen point (x, y), portrait
//   C            click           D / U     button down / up (drag)
//   K<hex>       tap a key usage (28 Return, 2b Tab, 2c Space, 52/51/50/4f arrows, 29 Escape)
//   P<x>,<y>     position the mouse cursor at (x, y) using relative moves (corners to 0,0 first; avoids tablet issues)
//   M            click the mouse button where the mouse cursor is
// Points are those of the display mode: 1080x2400, or smaller with native-gop-mode-width (540 -> 540x1200).
// The script starts when the driver starts; each step is logged.
// The device is an IOHIDDevice with QEMU's boot keyboard report descriptor and the VirtIO vendor/product identity the
// macOS-ForSheng input companion uses, so the same IOHIDEventDriver personality turns its reports into key events.
// The ADT node is /arm-io/native-recovery-input (compatible "google,akita-buttons", reg = the pin controller page).
// No interrupts: a kernel thread polls every 20 ms. Off unless native-recovery-input=1.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <stdarg.h>
#include <kern/clock.h>
#include <net/kpi_interface.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <netinet/in.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/sockio.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

static_assert(sizeof(IOHIDDevice) == 208, "paired 25F71 IOHIDDevice size");

namespace {

constexpr unsigned pollMs = 20;
constexpr uint32_t gpa4Data = 0x84, gpa6Data = 0xa4;
enum : uint8_t { usageReturn = 0x28, usageTab = 0x2b, usageSpace = 0x2c, usageDown = 0x51, usageUp = 0x52 };

// QEMU hw/input/hid.c boot keyboard: modifiers, reserved, six key usages; five LED outputs.
const uint8_t keyboardDescriptor[] = {
  0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
  0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
  0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
  0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
  0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00, 0x81, 0x00,
  0xc0,
};

// Boot mouse: three buttons, relative X and Y, and (akita, for USB mice) a relative wheel.
const uint8_t mouseDescriptor[] = {
  0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
  0xc0, 0xc0,
};

// QEMU hw/input/hid.c usb-tablet: three buttons, absolute X and Y (0..32767), relative wheel.
const uint8_t tabletDescriptor[] = {
  0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f,
  0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
  0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
  0xc0, 0xc0,
};
constexpr unsigned panelWidth = 1080, panelHeight = 2400;
enum : uint8_t { kindKeyboard, kindMouse, kindTablet };

constexpr unsigned modeHoldTicks = 50;     // 1 s of both volume buttons toggles pointer mode
constexpr unsigned tapTicks = 20;          // volume down released within 400 ms turns the direction

// USB device-mode bring-up probe (towards a USB Ethernet gadget, then ssh into Recovery). Boot-args:
//   akita-usb-probe=<rounds>   how many times the final summary is repeated, 20 s apart (the log viewer only shows
//                              the last pages of the retained log, so the summary must be at the end)
//   akita-usb-connect=<level>  1: event buffer + Run/Stop; 2: also run the eUSB2 PHY power-up first;
//                              3: also, if no event arrives in 30 s, soft-reset the DWC3 core and try again
// Addresses are from the zuma vendor DT (zuma-usb.dtsi) and Google's zuma kernel modules (cal-if/zuma/cmucal-sfr.c,
// phy/samsung/exynos-usb-blkcon*.{c,h}, phy-exynos-eusb.c, eusb-con-reg.h).
// Findings so far (5 Nov 2026):
//  - The PMU is protected: reading it directly hung the phone. Linux reads it with SMC 0x82000504 (x1 = physical
//    address, x2 = 0); so does this probe. pd_hsi0 is on (cfg 1, status 1), PMU USB PHY control 3, DP control 1.
//  - The DWC3 (0x11210000) is a DWC_usb31 1.10a in device mode (GCTL 00012004, GSTS 7e800000); the bootloader's
//    event buffer (f8d16000, 0x200) is still programmed. Run/Stop alone gave DSTS link state 4 (disconnected), no
//    SOF and no event: the PHY side is off.
//  - USB sub-control (0x11100000) only has registers at 0x00-0x1c (VERSION 04004442, LINKCTRL 0a453ff0, LINKPORT
//    00011070, LINK_CLKRST 0, UTMI_CTRL 3 = force BVALID and VBUSVALID already set); reading +0x20 hung the phone.
//  - So the eUSB2 PHY (control block at 0x11110000, registers 0x00-0x20) is what the bootloader shut down
//    (phy_exynos_eusb_terminate: phy_enable 0, phy_reset 1, test_iddq 1). Level 2 replays phy_exynos_eusb_initiate
//    for zuma (version 0x700, 19.2 MHz reference: ref_freq_sel 0, pll_fb_div 368, pll_ref_div 0).
// Only defined registers are touched.
struct UsbProbeWindow {
  uint64_t phys;
  uint32_t length;
  const char *name;
  IOMemoryMap *map;
};

UsbProbeWindow usbProbeWindows[] = {
  {0x11210000, 0x10000, "dwc3", nullptr},        // DWC3 core: globals at 0xc100, device at 0xc700
  {0x11100000, 0x1000, "subctl", nullptr},       // USB sub-control (blkcon): 0x00-0x1c only
  {0x11110000, 0x1000, "eusb", nullptr},         // eUSB2 PHY control: 0x00-0x20 only
  {0x11000000, 0x4000, "cmu", nullptr},          // CMU_HSI0
};
enum { winDwc3, winSubctl, winEusb, winCmu, winCount };

uint64_t tensorPmuRead(uint64_t phys)
{
  register uint64_t x0 asm("x0") = 0x82000504;
  register uint64_t x1 asm("x1") = phys;
  register uint64_t x2 asm("x2") = 0;
  register uint64_t x3 asm("x3") = 0;
  asm volatile("smc #0"
               : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
               :
               : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
                 "memory");
  return x0;
}

uint32_t usbProbeRead(unsigned window, uint32_t offset)
{
  IOMemoryMap *map = usbProbeWindows[window].map;
  if (!map || offset + 4 > map->getLength())
    return 0xdeadbeef;
  return *reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress() + offset);
}

void usbProbeWrite(unsigned window, uint32_t offset, uint32_t value)
{
  IOMemoryMap *map = usbProbeWindows[window].map;
  if (map && offset + 4 <= map->getLength())
    *reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress() + offset) = value;
}

uint32_t dwc3Read(uint32_t offset) { return usbProbeRead(winDwc3, offset); }
void dwc3Write(uint32_t offset, uint32_t value) { usbProbeWrite(winDwc3, offset, value); }

enum : uint32_t {
  GCTL = 0xc110, GSTS = 0xc118, GEVNTADRLO = 0xc400, GEVNTADRHI = 0xc404, GEVNTSIZ = 0xc408, GEVNTCOUNT = 0xc40c,
  DCFG = 0xc700, DCTL = 0xc704, DEVTEN = 0xc708, DSTS = 0xc70c,
  EUSB_RST_CTRL = 0x00, EUSB_CMN_CTRL = 0x04, EUSB_PLLCFG0 = 0x08, EUSB_PLLCFG1 = 0x0c, EUSB_TXTUNE = 0x14,
  EUSB_UTMI = 0x1c, EUSB_TESTSE = 0x20,
};

// The summary lines, kept so they can be repeated at the end of the log.
char usbLines[12][200];
unsigned usbLineCount;

void usbKeep(const char *format, ...) __attribute__((format(printf, 1, 2)));
void usbKeep(const char *format, ...)
{
  char line[200];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  IOLog("usbprobe %s\n", line);
  if (usbLineCount < 12)
    strlcpy(usbLines[usbLineCount++], line, sizeof(line));
}

void usbDwc3Status(const char *tag)
{
  usbKeep("%s dcfg=%08x dctl=%08x devten=%08x dsts=%08x gctl=%08x gsts=%08x evnt=%08x%08x/%08x cnt=%x", tag,
          dwc3Read(DCFG), dwc3Read(DCTL), dwc3Read(DEVTEN), dwc3Read(DSTS), dwc3Read(GCTL), dwc3Read(GSTS),
          dwc3Read(GEVNTADRHI), dwc3Read(GEVNTADRLO), dwc3Read(GEVNTSIZ), dwc3Read(GEVNTCOUNT));
}

void usbEusbStatus(const char *tag)
{
  uint32_t v[9];
  for (unsigned i = 0; i < 9; ++i)
    v[i] = usbProbeRead(winEusb, 4 * i);
  usbKeep("eusb %s %08x %08x %08x %08x %08x %08x %08x %08x %08x", tag, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
          v[8]);
}

// Announce a risky step and give the log window (copied every 3 s) time to record the announcement.
void usbAnnounce(const char *step)
{
  IOLog("usbprobe next %s\n", step);
  IOSleep(8000);
}

// phy_exynos_eusb_initiate for zuma (eUSB PHY version 0x700, minor 0, 19.2 MHz reference).
void usbEusbInitiate()
{
  uint32_t r = usbProbeRead(winEusb, EUSB_RST_CTRL);
  r |= (1U << 0) | (1U << 1) | (1U << 4) | (1U << 5);   // phy_reset, its override, utmi_port_reset, its override
  usbProbeWrite(winEusb, EUSB_RST_CTRL, r);
  r = usbProbeRead(winEusb, EUSB_CMN_CTRL);
  r |= 1U << 10;                                          // phy_cfg_rptr_mode
  r &= ~(7U << 4);                                        // ref_freq_sel 0: 19.2 MHz
  usbProbeWrite(winEusb, EUSB_CMN_CTRL, r);
  r = usbProbeRead(winEusb, EUSB_PLLCFG0);
  r = (r & ~(0xfffU << 8)) | (368U << 8);                 // pll_fb_div
  usbProbeWrite(winEusb, EUSB_PLLCFG0, r);
  r = usbProbeRead(winEusb, EUSB_PLLCFG1);
  r &= ~(0xfU << 8);                                      // pll_ref_div 0
  usbProbeWrite(winEusb, EUSB_PLLCFG1, r);
  r = usbProbeRead(winEusb, EUSB_TXTUNE);
  r &= ~(3U << 1);                                        // fsls_vref_tune 0
  usbProbeWrite(winEusb, EUSB_TXTUNE, r);
  r = usbProbeRead(winEusb, EUSB_TESTSE);
  r &= ~(1U << 6);                                        // test_iddq 0
  usbProbeWrite(winEusb, EUSB_TESTSE, r);
  r = usbProbeRead(winEusb, EUSB_CMN_CTRL);
  r &= ~1U;                                               // phy_enable 0
  usbProbeWrite(winEusb, EUSB_CMN_CTRL, r);
  IODelay(10);
  r = usbProbeRead(winEusb, EUSB_RST_CTRL);
  r &= ~1U;                                               // release phy_reset
  usbProbeWrite(winEusb, EUSB_RST_CTRL, r);
  IODelay(10);
  r = usbProbeRead(winEusb, EUSB_CMN_CTRL);
  r |= 1U;                                                // phy_enable
  usbProbeWrite(winEusb, EUSB_CMN_CTRL, r);
  IODelay(1000 + 28 + 2500);
  r = usbProbeRead(winEusb, EUSB_RST_CTRL);
  r &= ~((1U << 4) | (1U << 5));                          // release utmi_port_reset and its override
  usbProbeWrite(winEusb, EUSB_RST_CTRL, r);
}

struct UsbEvents {
  uint32_t first[24];
  uint32_t kept = 0, total = 0, dstsChanges = 0, lastDsts = 0, offset = 0;
};

void usbArm(uint64_t bufferPhys)
{
  dwc3Write(GEVNTADRLO, static_cast<uint32_t>(bufferPhys));
  dwc3Write(GEVNTADRHI, static_cast<uint32_t>(bufferPhys >> 32));
  dwc3Write(GEVNTSIZ, 0x80000000U | 4096);   // interrupt masked: polled
  dwc3Write(GEVNTCOUNT, dwc3Read(GEVNTCOUNT) & 0xffff);
  dwc3Write(DEVTEN, 0xf);                     // disconnect, USB reset, connect done, link state change
  dwc3Write(DCFG, dwc3Read(DCFG) & ~7U);      // DEVSPD 0: high speed
}

// Run/Stop on, then poll events and DSTS for the given time.
void usbWatch(UsbEvents &ev, volatile uint32_t *ring, unsigned ms, const char *tag)
{
  dwc3Write(DCTL, dwc3Read(DCTL) | 0x80000000U);
  IOSleep(5);
  IOLog("usbprobe %s run dsts=%08x\n", tag, dwc3Read(DSTS));
  for (unsigned t = 0; t < ms; t += 100) {
    uint32_t pending = dwc3Read(GEVNTCOUNT) & 0xffff;
    if (pending) {
      ev.total += pending / 4;
      for (uint32_t done = 0; done < pending; done += 4) {
        uint32_t e = ring[ev.offset / 4];
        if (ev.kept < 24)
          ev.first[ev.kept++] = e;
        ev.offset = (ev.offset + 4) % 4096;
      }
      dwc3Write(GEVNTCOUNT, pending);
    }
    uint32_t dsts = dwc3Read(DSTS);
    if (dsts != ev.lastDsts) {
      if (++ev.dstsChanges <= 10)
        IOLog("usbprobe %s dsts %08x at %u ms (speed %u link %u)\n", tag, dsts, t, dsts & 7, (dsts >> 18) & 15);
      ev.lastDsts = dsts;
    }
    IOSleep(100);
  }
}

void usbProbeThread(void *, wait_result_t)
{
  unsigned rounds = 0, connect = 0;
  PE_parse_boot_argn("akita-usb-probe", &rounds, sizeof(rounds));
  PE_parse_boot_argn("akita-usb-connect", &connect, sizeof(connect));
  IOLog("usbprobe armed: %u rounds, connect=%u, first in 60 s\n", rounds, connect);
  IOSleep(60000);
  for (unsigned i = 0; i < winCount; ++i) {
    IODeviceMemory *memory = IODeviceMemory::withRange(usbProbeWindows[i].phys, usbProbeWindows[i].length);
    if (memory) {
      usbProbeWindows[i].map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
      memory->release();
    }
  }
  usbKeep("pmu hsi0 %llx/%llx phy %llx dp %llx", tensorPmuRead(0x15462a80), tensorPmuRead(0x15462a84),
          tensorPmuRead(0x15463eb0), tensorPmuRead(0x15463eb4));
  // CMU_HSI0 gates (MANUAL bit 20, CG_VAL bit 21, auto gating bit 28) and Q-channels (ENABLE bit 0, CLOCK_REQ bit 1):
  // eUSB PHY PCLKM, sub-control APB, link ACLK, ref 40, eUSB control PCLK, eUSB APB, eUSB ref 26, suspend 26;
  // Q-channels eUSB PHY, sub-control, eUSB control, link.
  usbKeep("cmu gat %08x %08x %08x %08x %08x %08x %08x %08x qch %08x %08x %08x %08x",
          usbProbeRead(winCmu, 0x2004), usbProbeRead(winCmu, 0x20a0), usbProbeRead(winCmu, 0x20a4),
          usbProbeRead(winCmu, 0x20ac), usbProbeRead(winCmu, 0x20b8), usbProbeRead(winCmu, 0x20d8),
          usbProbeRead(winCmu, 0x20dc), usbProbeRead(winCmu, 0x20e4), usbProbeRead(winCmu, 0x30a8),
          usbProbeRead(winCmu, 0x30bc), usbProbeRead(winCmu, 0x30b8), usbProbeRead(winCmu, 0x30c0));
  {
    uint32_t v[8];
    for (unsigned i = 0; i < 8; ++i)
      v[i] = usbProbeRead(winSubctl, 4 * i);
    usbKeep("subctl %08x %08x %08x %08x %08x %08x %08x %08x", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
  }
  usbDwc3Status("before");

  UsbEvents ev;
  uint64_t bufferPhys = 0;
  IOBufferMemoryDescriptor *buffer = nullptr;
  if (connect) {
    buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        4096, 0x00000000fffff000ULL);
    if (!buffer || buffer->prepare() != kIOReturnSuccess) {
      usbKeep("no event buffer");
      connect = 0;
    }
  }
  if (connect) {
    bzero(buffer->getBytesNoCopy(), 4096);
    bufferPhys = buffer->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    volatile uint32_t *ring = static_cast<volatile uint32_t *>(buffer->getBytesNoCopy());
    dwc3Write(DCTL, dwc3Read(DCTL) & ~0x80000000U);
    for (unsigned i = 0; i < 100 && !(dwc3Read(DSTS) & (1U << 22)); ++i)
      IOSleep(10);
    if (connect >= 2) {
      usbAnnounce("eusb read");
      usbEusbStatus("before");
      usbAnnounce("eusb initiate");
      usbEusbInitiate();
      usbEusbStatus("after");
    }
    usbArm(bufferPhys);
    usbWatch(ev, ring, connect >= 3 ? 30000 : 90000, "a");
    usbKeep("a buffer=%llx dsts=%08x changes=%u events=%u", bufferPhys, dwc3Read(DSTS), ev.dstsChanges, ev.total);
    if (connect >= 3 && !ev.total) {
      // Core soft reset (DCTL.CSFTRST), then the same arming again.
      uint32_t gctl = dwc3Read(GCTL);
      dwc3Write(DCTL, dwc3Read(DCTL) & ~0x80000000U);
      dwc3Write(DCTL, 1U << 30);
      unsigned wait = 0;
      for (; wait < 100 && (dwc3Read(DCTL) & (1U << 30)); ++wait)
        IOSleep(10);
      dwc3Write(GCTL, gctl);
      usbKeep("b softreset wait=%u gctl=%08x dctl=%08x", wait, dwc3Read(GCTL), dwc3Read(DCTL));
      usbArm(bufferPhys);
      usbWatch(ev, ring, 60000, "b");
      usbKeep("b dsts=%08x changes=%u events=%u", dwc3Read(DSTS), ev.dstsChanges, ev.total);
    }
    dwc3Write(DCTL, dwc3Read(DCTL) & ~0x80000000U);
    dwc3Write(DEVTEN, 0);
    for (uint32_t i = 0; i < ev.kept; i += 8)
      usbKeep("ev%02u %08x %08x %08x %08x %08x %08x %08x %08x", i, ev.first[i], ev.first[i + 1], ev.first[i + 2],
              ev.first[i + 3], ev.first[i + 4], ev.first[i + 5], ev.first[i + 6], ev.first[i + 7]);
  }
  for (unsigned round = 1; round <= rounds; ++round) {
    for (unsigned i = 0; i < usbLineCount; ++i)
      IOLog("usbprobe s%u %s\n", round, usbLines[i]);
    IOSleep(20000);
  }
}

#include "AkitaUsbGadget.h"
#include "AkitaUsbHost.h"   // akita: USB host (xHCI) for a keyboard and mouse once USB PD makes the phone the host

}  // namespace

extern "C" void akita_touch_set_sink(void (*sink)(void *, int, int, int), void *ctx);   // akita: kernel, AkitaTouch.inc

class NativeRecoveryHID : public IOHIDDevice {
  OSDeclareDefaultStructors(NativeRecoveryHID)

public:
  uint8_t fKind = kindKeyboard;
  IOReturn newReportDescriptor(IOMemoryDescriptor **descriptor) const override;
  OSString *newTransportString() const override { return OSString::withCString("Virtual"); }
  OSString *newManufacturerString() const override { return OSString::withCString("Google"); }
  OSString *newProductString() const override
  {
    static const char *const names[] = {"Pixel 8a Buttons", "Pixel 8a Buttons Pointer", "Pixel 8a Script Tablet"};
    return OSString::withCString(names[fKind]);
  }
  OSNumber *newVendorIDNumber() const override { return OSNumber::withNumber(0x1af4, 32); }
  OSNumber *newProductIDNumber() const override { return OSNumber::withNumber(0x1052, 32); }
  OSNumber *newLocationIDNumber() const override { return OSNumber::withNumber(0x41420000 + fKind, 32); }
  OSNumber *newPrimaryUsagePageNumber() const override { return OSNumber::withNumber(kHIDPage_GenericDesktop, 32); }
  OSNumber *newPrimaryUsageNumber() const override
  {
    return OSNumber::withNumber(fKind == kindKeyboard ? kHIDUsage_GD_Keyboard : kHIDUsage_GD_Mouse, 32);
  }
};

OSDefineMetaClassAndStructors(NativeRecoveryHID, IOHIDDevice)

IOReturn NativeRecoveryHID::newReportDescriptor(IOMemoryDescriptor **descriptor) const
{
  if (fKind == kindTablet)
    *descriptor = IOBufferMemoryDescriptor::withBytes(tabletDescriptor, sizeof(tabletDescriptor), kIODirectionNone);
  else if (fKind == kindMouse)
    *descriptor = IOBufferMemoryDescriptor::withBytes(mouseDescriptor, sizeof(mouseDescriptor), kIODirectionNone);
  else
    *descriptor = IOBufferMemoryDescriptor::withBytes(keyboardDescriptor, sizeof(keyboardDescriptor), kIODirectionNone);
  return *descriptor ? kIOReturnSuccess : kIOReturnNoMemory;
}

class NativeRecoveryInput : public IOService {
  OSDeclareDefaultStructors(NativeRecoveryInput)

  IOMemoryMap *fPins = nullptr;
  NativeRecoveryHID *fHid = nullptr;
  NativeRecoveryHID *fMouse = nullptr;
  NativeRecoveryHID *fTablet = nullptr;
  char fScript[600] = {};
  IOLock *fSendLock = nullptr;   // the poll and script threads both report
  unsigned fScreenWidth = panelWidth, fScreenHeight = panelHeight;   // the display mode NativeGOPDisplay publishes
  unsigned fKeepAwakeSeconds = 0;   // akita-input-keepawake: nudge the mouse this often so the display never idles off
  unsigned fTabletX = panelWidth / 2, fTabletY = panelHeight / 2;
  uint8_t fLast[8] = {};
  unsigned fReports = 0;
  // Pointer mode state.
  bool fPointer = false, fToggled = false, fLastButton = false;
  unsigned fComboTicks = 0, fUpTicks = 0, fDownTicks = 0, fDirection = 0;

  static void pollThread(void *self, wait_result_t);
  static NativeRecoveryHID *newDevice(IOService *owner, uint8_t kind);
  static void scriptThread(void *self, wait_result_t);
  static void remoteThread(void *self, wait_result_t);
  void runScript();
  void runSteps(const char *steps);
  // Remote input from the USB gadget (UDP port 5141, "I<steps>"): one pending command, run by remoteThread.
  IOLock *fRemoteLock = nullptr;
  char fRemote[1024] = {};
  bool fRemotePending = false;

public:
  void queueRemote(const char *steps, unsigned length);
  // akita (Generated-by: LLM): a USB keyboard and mouse behind a hub (AkitaUsbHost.h), as these HID devices
  static void usbKeys(void *self, const uint8_t report[8]);
  static void usbMouse(void *self, uint8_t buttons, int dx, int dy, int wheel);
  // akita (Generated-by: LLM): the touchscreen's first finger (AkitaTouch.inc in the kernel), panel coordinates 1080 x 2400 portrait
  static void touchSink(void *self, int panelX, int panelY, int down);

private:
  void sendTablet(bool button);
  void sendTabletQuiet(uint8_t buttons, int8_t wheel);
  void positionWithMouse(unsigned targetX, unsigned targetY);
  void describe();
  void poll();
  void sendKeys(const uint8_t bytes[8]);
  void typeKey(uint8_t usage, uint8_t mods);
  void sendMouse(bool button, int dx, int dy);
  void pointer(bool up, bool down, bool power);

public:
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(NativeRecoveryInput, IOService)

static NativeRecoveryInput *gRemoteInputOwner;
static void remoteInputHook(const char *steps, unsigned length)
{
  if (gRemoteInputOwner)
    gRemoteInputOwner->queueRemote(steps, length);
}

bool NativeRecoveryInput::start(IOService *provider)
{
  unsigned enabled = 0;
  if (!PE_parse_boot_argn("native-recovery-input", &enabled, sizeof(enabled)) || enabled != 1)
    return false;
  // Same validation as NativeGOPDisplay, so both agree on the mode.
  unsigned modeWidth = 0;
  if (PE_parse_boot_argn("native-gop-mode-width", &modeWidth, sizeof(modeWidth)) && modeWidth &&
      modeWidth < panelWidth && modeWidth * 4 >= panelWidth && (panelHeight * modeWidth) % panelWidth == 0) {
    fScreenWidth = modeWidth;
    fScreenHeight = panelHeight * modeWidth / panelWidth;
  }
  // akita (Generated-by: LLM): native-gop-landscape=1, the display's landscape mode: the same mode turned on its side.
  unsigned landscape = 0;
  if (PE_parse_boot_argn("native-gop-landscape", &landscape, sizeof(landscape)) && landscape == 1) {
    const unsigned w = fScreenWidth;
    fScreenWidth = fScreenHeight;
    fScreenHeight = w;
  }
  fTabletX = fScreenWidth / 2;
  fTabletY = fScreenHeight / 2;
  PE_parse_boot_argn("akita-input-keepawake", &fKeepAwakeSeconds, sizeof(fKeepAwakeSeconds));
  OSData *compatible = OSDynamicCast(OSData, provider->getProperty("compatible"));
  static const char expected[] = "google,akita-buttons";
  if (!compatible || compatible->getLength() != sizeof(expected) ||
      memcmp(compatible->getBytesNoCopy(), expected, sizeof(expected)))
    return false;
  if (!IOService::start(provider))
    return false;
  fPins = provider->mapDeviceMemoryWithIndex(0, kIOMapAnywhere | kIOMapInhibitCache);
  if (!fPins || fPins->getLength() < 0x100) {
    IOLog("NativeRecoveryInput: cannot map the ALIVE pin controller\n");
    OSSafeReleaseNULL(fPins);
    return false;
  }
  fSendLock = IOLockAlloc();
  if (!fSendLock)
    return false;
  fHid = newDevice(this, kindKeyboard);
  if (!fHid)
    return false;
  fMouse = newDevice(this, kindMouse);
  if (!fMouse)
    IOLog("NativeRecoveryInput: no pointer device; keyboard only\n");
  // The companion cannot unload. Retain it for the lifetime of its polling thread.
  thread_t thread;
  retain();
  if (kernel_thread_start(pollThread, this, &thread) != KERN_SUCCESS) {
    release();
    return false;
  }
  thread_deallocate(thread);
  IOLog("NativeRecoveryInput: Pixel 8a buttons as a keyboard (up, down; both volume keys or power: return; power+down: tab; "
        "power+up: space; hold both volume keys 1 s: pointer mode)\n");
  unsigned usbGadget = 0;
  if (PE_parse_boot_argn("akita-usb-gadget", &usbGadget, sizeof(usbGadget)) && usbGadget) {
    thread_t thread;
    if (kernel_thread_start(gadget::gadgetThread, nullptr, &thread) == KERN_SUCCESS)
      thread_deallocate(thread);
    else
      IOLog("usbgadget cannot start\n");
  }
  unsigned usbProbe = 0;
  if (!usbGadget && PE_parse_boot_argn("akita-usb-probe", &usbProbe, sizeof(usbProbe)) && usbProbe) {
    thread_t probe;
    if (kernel_thread_start(usbProbeThread, nullptr, &probe) == KERN_SUCCESS)
      thread_deallocate(probe);
    else
      IOLog("usbprobe cannot start\n");
  }
  if (usbGadget >= 2) {
    // Live input from the Mac over the USB gadget.
    if (!fTablet)
      fTablet = newDevice(this, kindTablet);
    fRemoteLock = IOLockAlloc();
    thread_t remote;
    retain();
    if (fTablet && fRemoteLock && kernel_thread_start(remoteThread, this, &remote) == KERN_SUCCESS) {
      thread_deallocate(remote);
      gRemoteInputOwner = this;
      gadget::gRemoteInput = remoteInputHook;
      IOLog("NativeRecoveryInput: remote input on UDP 5141\n");
      akita_touch_set_sink(touchSink, this);   // akita: the touchscreen moves and presses the same tablet
      // akita: USB host for a keyboard and mouse behind a USB-C hub (AkitaUsbHost.h), started when the PD engine says so
      if (!fMouse)
        fMouse = newDevice(this, kindMouse);
      usbhost::gCtx = this;
      usbhost::gKeys = usbKeys;
      usbhost::gMouse = usbMouse;
      thread_t host;
      if (kernel_thread_start(usbhost::hostThread, nullptr, &host) == KERN_SUCCESS)
        thread_deallocate(host);
    } else {
      release();
    }
  }
  if (PE_parse_boot_arg_str("akita-input-script", fScript, sizeof(fScript)) && fScript[0]) {
    if (!fTablet)
      fTablet = newDevice(this, kindTablet);
    thread_t scripted;
    retain();
    if (!fTablet || kernel_thread_start(scriptThread, this, &scripted) != KERN_SUCCESS) {
      IOLog("NativeRecoveryInput: cannot start the input script\n");
      release();
    } else {
      thread_deallocate(scripted);
      IOLog("NativeRecoveryInput: input script: %s\n", fScript);
    }
  }
  registerService();
  return true;
}

NativeRecoveryHID *NativeRecoveryInput::newDevice(IOService *owner, uint8_t kind)
{
  NativeRecoveryHID *device = OSTypeAlloc(NativeRecoveryHID);
  if (!device)
    return nullptr;
  device->fKind = kind;
  if (!device->init(nullptr) || !device->attach(owner)) {
    device->release();
    return nullptr;
  }
  if (!device->start(owner)) {
    device->detach(owner);
    device->release();
    return nullptr;
  }
  return device;
}

void NativeRecoveryInput::sendKeys(const uint8_t bytes[8])
{
  IOBufferMemoryDescriptor *report = IOBufferMemoryDescriptor::withBytes(bytes, 8, kIODirectionNone);
  if (report) {
    IOLockLock(fSendLock);
    fHid->handleReport(report);
    IOLockUnlock(fSendLock);
    report->release();
  }
}

void NativeRecoveryInput::sendMouse(bool button, int dx, int dy)
{
  if (!fMouse)
    return;
  uint8_t bytes[4] = {(uint8_t)(button ? 1 : 0), (uint8_t)(int8_t)dx, (uint8_t)(int8_t)dy, 0};
  IOBufferMemoryDescriptor *report = IOBufferMemoryDescriptor::withBytes(bytes, sizeof(bytes), kIODirectionNone);
  if (report) {
    fMouse->handleReport(report);
    report->release();
  }
}

void NativeRecoveryInput::usbKeys(void *self, const uint8_t report[8])
{
  if (self)
    static_cast<NativeRecoveryInput *>(self)->sendKeys(report);
}

void NativeRecoveryInput::usbMouse(void *self, uint8_t buttons, int dx, int dy, int wheel)
{
  NativeRecoveryInput *input = static_cast<NativeRecoveryInput *>(self);
  if (!input || !input->fMouse)
    return;
  auto clamp = [](int v) { return static_cast<uint8_t>(static_cast<int8_t>(v < -127 ? -127 : v > 127 ? 127 : v)); };
  uint8_t bytes[4] = {static_cast<uint8_t>(buttons & 7), clamp(dx), clamp(dy), clamp(wheel)};
  IOBufferMemoryDescriptor *report = IOBufferMemoryDescriptor::withBytes(bytes, sizeof(bytes), kIODirectionNone);
  if (report) {
    input->fMouse->handleReport(report);
    report->release();
  }
}

// One 20 ms step of pointer mode.
void NativeRecoveryInput::pointer(bool up, bool down, bool power)
{
  static const int8_t stepX[4] = {1, 0, -1, 0}, stepY[4] = {0, 1, 0, -1};
  int dx = 0, dy = 0;
  auto speed = [](unsigned ticks) { return ticks < 10 ? 2 : ticks < 40 ? 6 : ticks < 80 ? 14 : 28; };
  if (up) {
    fUpTicks++;
    dx = stepX[fDirection] * speed(fUpTicks);
    dy = stepY[fDirection] * speed(fUpTicks);
  } else {
    fUpTicks = 0;
  }
  if (down) {
    // Held past a tap: move the other way.
    if (++fDownTicks > tapTicks && !up) {
      unsigned held = fDownTicks - tapTicks;
      dx = -stepX[fDirection] * speed(held);
      dy = -stepY[fDirection] * speed(held);
    }
  } else {
    if (fDownTicks && fDownTicks <= tapTicks) {
      fDirection = (fDirection + 1) % 4;
      static const char *const names[4] = {"right", "down", "left", "up"};
      IOLog("NativeRecoveryInput: pointer direction %s\n", names[fDirection]);
    }
    fDownTicks = 0;
  }
  if (dx || dy || power != fLastButton)
    sendMouse(power, dx, dy);
  fLastButton = power;
}

void NativeRecoveryInput::sendTablet(bool button)
{
  uint32_t x = fTabletX * 32767 / (fScreenWidth - 1), y = fTabletY * 32767 / (fScreenHeight - 1);
  uint8_t bytes[6] = {(uint8_t)(button ? 1 : 0), (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8), 0};
  IOLog("NativeRecoveryInput: tablet report btn=%u hid_x=%u(0x%x) hid_y=%u(0x%x) screen=%u,%u bytes=%02x %02x %02x %02x %02x %02x\n",
        button, x, x, y, y, fTabletX, fTabletY, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  IOBufferMemoryDescriptor *report = IOBufferMemoryDescriptor::withBytes(bytes, sizeof(bytes), kIODirectionNone);
  if (report) {
    IOLockLock(fSendLock);
    fTablet->handleReport(report);
    IOLockUnlock(fSendLock);
    report->release();
  }
}

// Position cursor at (targetX, targetY) using relative mouse: corner to (0,0) then inch forward with dx=1.
// Avoids tablet absolute positioning issues and minimizes pointer acceleration effects.
void NativeRecoveryInput::positionWithMouse(unsigned targetX, unsigned targetY)
{
  if (!fMouse) {
    IOLog("NativeRecoveryInput: positionWithMouse: no mouse device\n");
    return;
  }
  // Phase 1: corner to (0,0) with 40 big negative moves
  for (int i = 0; i < 40; i++) {
    sendMouse(false, -127, -127);
    IOSleep(5);
  }
  IOSleep(50);
  // Phase 2: move to target with small deltas. At IOSleep(8), delta=1 gives ~125 counts/sec,
  // a standard USB mouse report rate where macOS acceleration is approximately 1:1.
  unsigned cx = 0, cy = 0;
  while (cx < targetX || cy < targetY) {
    unsigned dx = (cx < targetX) ? 1u : 0u;
    unsigned dy = (cy < targetY) ? 1u : 0u;
    sendMouse(false, static_cast<int>(dx), static_cast<int>(dy));
    cx += dx;
    cy += dy;
    IOSleep(8);
  }
  IOSleep(100);
  IOLog("NativeRecoveryInput: positionWithMouse at %u,%u (%u reports)\n", targetX, targetY,
        targetX > targetY ? targetX : targetY);
}

// Log which IOHIDEventDriver attached to each HID device and how many user clients exist.
void NativeRecoveryInput::describe()
{
  NativeRecoveryHID *devices[] = {fHid, fMouse, fTablet};
  const char *names[] = {"keyboard", "mouse", "tablet"};
  for (int i = 0; i < 3; i++) {
    if (!devices[i]) continue;
    OSIterator *interfaces = devices[i]->getChildIterator(gIOServicePlane);
    unsigned matched = 0;
    while (OSObject *object = interfaces ? interfaces->getNextObject() : nullptr) {
      IOService *interface = OSDynamicCast(IOService, object);
      if (!interface) continue;
      OSIterator *clients = interface->getChildIterator(gIOServicePlane);
      unsigned nclients = 0;
      while (clients && clients->getNextObject())
        nclients++;
      OSSafeReleaseNULL(clients);
      IOLog("NativeRecoveryInput: %s -> %s, %u client(s)\n", names[i],
            interface->getMetaClass()->getClassName(), nclients);
      matched++;
    }
    OSSafeReleaseNULL(interfaces);
    if (!matched)
      IOLog("NativeRecoveryInput: %s has NO driver attached!\n", names[i]);
  }
}

void NativeRecoveryInput::scriptThread(void *self, wait_result_t)
{
  static_cast<NativeRecoveryInput *>(self)->runScript();
}

static unsigned scriptNumber(const char *&at, unsigned base)
{
  unsigned value = 0;
  for (;; at++) {
    unsigned digit;
    if (*at >= '0' && *at <= '9') digit = *at - '0';
    else if (base == 16 && *at >= 'a' && *at <= 'f') digit = *at - 'a' + 10;
    else if (base == 16 && *at >= 'A' && *at <= 'F') digit = *at - 'A' + 10;
    else return value;
    value = value * base + digit;
  }
}

void NativeRecoveryInput::typeKey(uint8_t usage, uint8_t mods)
{
  uint8_t press[8] = {mods, 0, usage}, held[8] = {mods}, none[8] = {};
  if (mods) {
    sendKeys(held);
    IOSleep(30);
  }
  sendKeys(press);
  IOSleep(60);
  sendKeys(mods ? held : none);
  if (mods) {
    IOSleep(30);
    sendKeys(none);
  }
  IOSleep(40);
}

// HID usage and modifiers for an ASCII character on the macOS Spanish ISO layout.
static bool spanishKey(char c, uint8_t &usage, uint8_t &mods)
{
  constexpr uint8_t shift = 0x02, option = 0x04;
  mods = 0;
  if (c >= 'a' && c <= 'z') {
    usage = 0x04 + (c - 'a');
    return true;
  }
  if (c >= 'A' && c <= 'Z') {
    usage = 0x04 + (c - 'A');
    mods = shift;
    return true;
  }
  if (c >= '1' && c <= '9') {
    usage = 0x1e + (c - '1');
    return true;
  }
  static const struct { char c; uint8_t usage, mods; } table[] = {
      {'0', 0x27, 0},      {' ', 0x2c, 0},      {'\n', 0x28, 0},     {'\t', 0x2b, 0},
      {'-', 0x38, 0},      {'_', 0x38, shift},  {'.', 0x37, 0},      {':', 0x37, shift},
      {',', 0x36, 0},      {';', 0x36, shift},  {'/', 0x24, shift},  {'!', 0x1e, shift},
      {'"', 0x1f, shift},  {'$', 0x21, shift},  {'%', 0x22, shift},  {'&', 0x23, shift},
      {'(', 0x25, shift},  {')', 0x26, shift},  {'=', 0x27, shift},  {'\'', 0x2d, 0},
      {'?', 0x2d, shift},  {'+', 0x30, 0},      {'*', 0x30, shift},  {'|', 0x1e, option},
      {'@', 0x1f, option}, {'#', 0x20, option},
  };
  for (const auto &entry : table) {
    if (entry.c == c) {
      usage = entry.usage;
      mods = entry.mods;
      return true;
    }
  }
  return false;
}

// Plays akita-input-script once. Unknown steps are logged and skipped.
void NativeRecoveryInput::runScript()
{
  runSteps(fScript);
}

void NativeRecoveryInput::queueRemote(const char *steps, unsigned length)
{
  if (!fRemoteLock)
    return;
  IOLockLock(fRemoteLock);
  // Commands queue up behind each other ('_' joins them); only a full buffer drops one.
  size_t used = strlen(fRemote);
  if (used + (used ? 1 : 0) + length >= sizeof(fRemote)) {
    IOLockUnlock(fRemoteLock);
    IOLog("NativeRecoveryInput: remote input queue full, dropped\n");
    return;
  }
  if (used)
    fRemote[used++] = '_';
  memcpy(fRemote + used, steps, length);
  fRemote[used + length] = 0;
  fRemotePending = true;
  IOLockWakeup(fRemoteLock, &fRemotePending, true);
  IOLockUnlock(fRemoteLock);
}

void NativeRecoveryInput::remoteThread(void *self, wait_result_t)
{
  NativeRecoveryInput *input = static_cast<NativeRecoveryInput *>(self);
  char steps[sizeof(input->fRemote)];
  for (;;) {
    IOLockLock(input->fRemoteLock);
    while (!input->fRemotePending)
      IOLockSleep(input->fRemoteLock, &input->fRemotePending, THREAD_UNINT);
    memcpy(steps, input->fRemote, sizeof(steps));
    input->fRemote[0] = 0;
    input->fRemotePending = false;
    IOLockUnlock(input->fRemoteLock);
    input->runSteps(steps);
  }
}

// akita (Generated-by: LLM): a finger on the glass as the tablet: down presses its button where the finger is, up lets go. The panel
// is portrait (1080 x 2400); in the landscape mode the desktop is the panel turned on its side as NativeGOPDisplay turns it: the
// desktop's x runs down the panel's long side, its y from the panel's right edge to its left.
void NativeRecoveryInput::touchSink(void *self, int panelX, int panelY, int down)
{
  NativeRecoveryInput *input = static_cast<NativeRecoveryInput *>(self);
  if (!input || !input->fTablet) return;
  const unsigned px = panelX < 0 ? 0u : panelX > 1079 ? 1079u : static_cast<unsigned>(panelX);
  const unsigned py = panelY < 0 ? 0u : panelY > 2399 ? 2399u : static_cast<unsigned>(panelY);
  unsigned x, y;
  if (input->fScreenWidth > input->fScreenHeight) {
    x = py * input->fScreenWidth / 2400;
    y = (1079 - px) * input->fScreenHeight / 1080;
  } else {
    x = px * input->fScreenWidth / 1080;
    y = py * input->fScreenHeight / 2400;
  }
  // codes (AkitaTouch.inc): 0 up, 1 left button down at, 2 right click at, 3 wheel by panelX (no move), 4 move to
  if (down == 3) {
    input->sendTabletQuiet(0, static_cast<int8_t>(panelX));
    return;
  }
  input->fTabletX = x < input->fScreenWidth ? x : input->fScreenWidth - 1;
  input->fTabletY = y < input->fScreenHeight ? y : input->fScreenHeight - 1;
  if (down == 2) {
    input->sendTabletQuiet(2, 0);
    IOSleep(30);
    input->sendTabletQuiet(0, 0);
    return;
  }
  input->sendTabletQuiet(down == 1 ? 1 : 0, 0);
}

// akita: a tablet report without its log line (a finger sends one every ~8 ms): buttons bit 0 left, bit 1 right; a relative wheel.
void NativeRecoveryInput::sendTabletQuiet(uint8_t buttons, int8_t wheel)
{
  uint32_t x = fTabletX * 32767 / (fScreenWidth - 1), y = fTabletY * 32767 / (fScreenHeight - 1);
  uint8_t bytes[6] = {buttons, (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8), (uint8_t)wheel};
  IOBufferMemoryDescriptor *report = IOBufferMemoryDescriptor::withBytes(bytes, sizeof(bytes), kIODirectionNone);
  if (report) {
    IOLockLock(fSendLock);
    fTablet->handleReport(report);
    IOLockUnlock(fSendLock);
    report->release();
  }
}

void NativeRecoveryInput::runSteps(const char *steps)
{
  bool button = false;
  unsigned step = 0;
  for (const char *at = steps; *at;) {
    char op = *at++;
    step++;
    switch (op) {
    case 'W': {
      unsigned ms = scriptNumber(at, 10);
      IOLog("NativeRecoveryInput: script %u wait %u ms\n", step, ms);
      IOSleep(ms);
      break;
    }
    case 'A': {
      unsigned x = scriptNumber(at, 10);
      unsigned y = (*at == ',') ? (at++, scriptNumber(at, 10)) : fTabletY;
      fTabletX = x < fScreenWidth ? x : fScreenWidth - 1;
      fTabletY = y < fScreenHeight ? y : fScreenHeight - 1;
      sendTablet(button);
      IOLog("NativeRecoveryInput: script %u pointer %u,%u\n", step, fTabletX, fTabletY);
      break;
    }
    case 'C':
      sendTablet(true);
      IOSleep(80);
      sendTablet(false);
      button = false;
      IOLog("NativeRecoveryInput: script %u click at %u,%u\n", step, fTabletX, fTabletY);
      break;
    case 'D':
    case 'U':
      button = op == 'D';
      sendTablet(button);
      IOLog("NativeRecoveryInput: script %u button %s\n", step, button ? "down" : "up");
      break;
    case 'K': {
      // K<usage hex>[,<modifier hex>]: 02 = shift, 04 = option, 08 = command.
      uint8_t usage = (uint8_t)scriptNumber(at, 16);
      uint8_t mods = (*at == ',') ? (at++, (uint8_t)scriptNumber(at, 16)) : 0;
      typeKey(usage, mods);
      IOLog("NativeRecoveryInput: script %u key %02x mods %02x\n", step, usage, mods);
      break;
    }
    case 'T': {
      // T<text>: types text with the Spanish (ISO) layout. '+' is a space,
      // %XX is the ASCII character XX (for '_', '+', '%').
      unsigned typed = 0, skipped = 0;
      while (*at && *at != '_') {
        char c = *at++;
        if (c == '+') {
          c = ' ';
        } else if (c == '%' && at[0] && at[1]) {
          char pair[3] = {at[0], at[1], 0};
          const char *hex = pair;
          c = (char)scriptNumber(hex, 16);
          at += 2;
        }
        uint8_t usage, mods;
        if (spanishKey(c, usage, mods)) {
          typeKey(usage, mods);
          typed++;
        } else {
          skipped++;
        }
      }
      IOLog("NativeRecoveryInput: script %u typed %u chars (%u unsupported)\n", step, typed, skipped);
      break;
    }
    case 'P': {
      unsigned tx = scriptNumber(at, 10);
      unsigned ty = (*at == ',') ? (at++, scriptNumber(at, 10)) : 0;
      positionWithMouse(tx, ty);
      // A later C then clicks where the mouse put the cursor.
      fTabletX = tx < fScreenWidth ? tx : fScreenWidth - 1;
      fTabletY = ty < fScreenHeight ? ty : fScreenHeight - 1;
      IOLog("NativeRecoveryInput: script %u mouse-position %u,%u\n", step, tx, ty);
      break;
    }
    case 'M':
      sendMouse(true, 0, 0);
      IOSleep(80);
      sendMouse(false, 0, 0);
      IOLog("NativeRecoveryInput: script %u mouse click\n", step);
      break;
    default:
      IOLog("NativeRecoveryInput: script %u unknown step '%c'\n", step, op);
      break;
    }
    while (*at && *at != '_')
      at++;
    if (*at == '_')
      at++;
  }
  IOLog("NativeRecoveryInput: script done (%u steps)\n", step);
}

void NativeRecoveryInput::pollThread(void *self, wait_result_t)
{
  NativeRecoveryInput *input = static_cast<NativeRecoveryInput *>(self);
  for (unsigned tick = 0;; ++tick) {
    if (tick == 30000 / pollMs || tick == 240000 / pollMs)
      input->describe();
    if (input->fKeepAwakeSeconds && tick && tick % (input->fKeepAwakeSeconds * 1000 / pollMs) == 0 && !input->fPointer) {
      input->sendMouse(false, 1, 0);
      input->sendMouse(false, -1, 0);
    }
    input->poll();
    IOSleep(pollMs);
  }
}

void NativeRecoveryInput::poll()
{
  const volatile uint32_t *pins = reinterpret_cast<const volatile uint32_t *>(fPins->getVirtualAddress());
  uint32_t gpa4 = pins[gpa4Data / 4], gpa6 = pins[gpa6Data / 4];
  bool up = !(gpa6 & (1u << 2)), down = !(gpa4 & (1u << 1)), power = !(gpa4 & (1u << 0));
  uint8_t bytes[8] = {};
  unsigned n = 2;
  // Both volume keys held for a second toggle pointer mode; a shorter press is Return (keyboard mode), sent on release.
  if (up && down) {
    if (++fComboTicks == modeHoldTicks && fMouse) {
      fPointer = !fPointer;
      fToggled = true;
      IOLog("NativeRecoveryInput: %s mode\n", fPointer ? "pointer" : "keyboard");
    }
  } else if (!up && !down) {
    if (fComboTicks && !fToggled && !fPointer) {
      // Short both-volume press: Return down now, up on the next poll.
      uint8_t press[8] = {0, 0, usageReturn};
      sendKeys(press);
      memcpy(fLast, press, sizeof(press));  // the next poll's empty report releases it
      IOLog("NativeRecoveryInput: return (both volume keys)\n");
    }
    fComboTicks = 0;
    fToggled = false;
  }
  if (fComboTicks) {
    // While the pair is held, nothing else.
  } else if (fPointer) {
    pointer(up, down, power);
    up = down = false;
    power = false;
  } else if (power && down) {
    bytes[n++] = usageTab;
  } else if (power && up) {
    bytes[n++] = usageSpace;
  }
  if (!fComboTicks && !fPointer && !(power && (up || down))) {
    if (up) bytes[n++] = usageUp;
    if (down) bytes[n++] = usageDown;
    if (power) bytes[n++] = usageReturn;
  }
  if (!memcmp(bytes, fLast, sizeof(bytes)))
    return;
  memcpy(fLast, bytes, sizeof(bytes));
  if (++fReports <= 40 || fReports % 200 == 0)
    IOLog("NativeRecoveryInput: buttons up=%u down=%u power=%u key=%02x (report %u)\n", up, down, power, bytes[2], fReports);
  sendKeys(bytes);
}
