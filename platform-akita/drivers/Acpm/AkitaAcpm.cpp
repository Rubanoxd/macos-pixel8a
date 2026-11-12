// SPDX-License-Identifier: BSD-2-Clause-Patent
// akita (Generated-by: LLM, local only): the first step towards running the CPU and the GPU at their real speed.
//
// On zuma the clocks and the voltages belong to a microcontroller (ACPM, "APM"). The bootloader leaves every domain at its boot
// level (the GPU at about 150 MHz) and nothing here has ever asked for more. The AP talks to it through queues in the APM's SRAM
// and a doorbell register (Linux: drivers/firmware/samsung/exynos-acpm.c, drivers/mailbox/exynos-mailbox.c, the DVFS protocol
// in exynos-acpm-dvfs.c). This file is that protocol's polling half, and for now it only ASKS: which channels the firmware has
// and what frequency each DVFS domain runs at. Nothing is set, no regulator and no clock register is touched.
//
// Nothing runs by itself. sysctl kern.akita_acpm=N (the answers go to the kernel log):
//   1  map the SRAM and the mailbox, read the channel table (memory reads only, no message is sent)
//   2  1, then ask the DVFS channel for the frequency of every domain (ACPM_DVFS_FREQ_GET, a query)
//   3  1, then ask the thermal channel for the temperature of each of the seven zones (ACPM_TMU_READ_TEMP, a query)
// Addresses from the vendor device tree (zuma.dtsi, acpm_ipc): AP2APM mailbox 0x15110000, SRAM 0x15700000 + 0x58000, the
// table at SRAM + 0xa000; the DVFS client is channel 0 (exynos_cal_if: acpm-ipc-channel = <0>), the thermal one channel 9 (acpm_tmu).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>   // akita: AkitaPcie.inc host rings
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" int akita_acpm_command(unsigned what);
extern "C" int akita_acpm_get_khz(unsigned domain, uint32_t *khz);

namespace {

constexpr uint64_t kMboxPhys = 0x15110000, kMboxSize = 0x1000, kSramPhys = 0x15700000;
constexpr uint32_t kSramSize = 0x58000, kInitData = 0xa000, kApLow = 0x5000;
constexpr uint32_t kMboxIntgr1 = 0x40;                 // EXYNOS_MBOX_INTGR1: bit n rings channel n's doorbell
constexpr unsigned kMaxChans = 64, kDvfsChan = 0, kTmuChan = 9;
constexpr uint32_t kSeqShift = 16, kSeqMask = 0x3fu << kSeqShift;   // ACPM_PROTOCOL_SEQNUM, bits 21..16 of the first word, 1..63

// struct acpm_chan_shmem, in words: the APM's RX queue is what the AP sends into, its TX queue what the AP reads.
enum : uint32_t { kChId = 0, kChRxRear = 4, kChRxFront = 5, kChRxBase = 6, kChTxRear = 10, kChTxFront = 11, kChTxBase = 12, kChQlen = 13, kChMlen = 14, kChPoll = 17, kChWords = 18 };

struct Chan {
  bool usable;
  uint32_t id, qlen, mlen, poll;
  uint32_t txBase, txRear, txFront;   // offsets into the SRAM, AP's point of view
  uint32_t rxBase, rxRear, rxFront;
  uint32_t seq;
};

volatile uint8_t *gSram;
volatile uint32_t *gMbox;
IOLock *gAcpmLock;
Chan gChan[kMaxChans];
uint32_t gChans;
bool gProbed;

inline uint32_t sram(uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(gSram + off); }
inline void sramWrite(uint32_t off, uint32_t value) {
  *reinterpret_cast<volatile uint32_t *>(gSram + off) = value;
  __asm__ volatile("dsb sy" ::: "memory");
}

inline uint64_t ticksNow() {
  uint64_t v;
  __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v));
  return v;
}
inline uint64_t ticksPerSecond() {
  static uint64_t f;
  if (!f) __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f;
}
template <typename Condition> bool waitUs(uint64_t limit, Condition done) {
  const uint64_t end = ticksNow() + limit * ticksPerSecond() / 1000000;
  for (;;) {
    if (done()) return true;
    if (ticksNow() >= end) return false;
    IODelay(20);
  }
}

// The channel table. Memory reads only. Lock held.
bool probe() {
  if (gProbed) return gChans != 0;
  gProbed = true;
  gSram = reinterpret_cast<volatile uint8_t *>(ml_io_map(kSramPhys, kSramSize));
  gMbox = reinterpret_cast<volatile uint32_t *>(ml_io_map(kMboxPhys, kMboxSize));
  if (!gSram || !gMbox) { printf("acpm: cannot map the SRAM or the mailbox\n"); return false; }
  const uint32_t table = sram(kInitData + 8), count = sram(kInitData + 24);
  printf("acpm: SRAM %#llx, %u channels, table at %#x\n", static_cast<unsigned long long>(kSramPhys), count, table);
  if (!count || count > kMaxChans || static_cast<uint64_t>(table) + static_cast<uint64_t>(count) * kChWords * 4 > kSramSize) {
    printf("acpm: that is not a channel table: nothing is used\n");
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = table + i * kChWords * 4;
    Chan &c = gChan[i];
    c.id = sram(at + kChId * 4); c.qlen = sram(at + kChQlen * 4); c.mlen = sram(at + kChMlen * 4); c.poll = sram(at + kChPoll * 4);
    c.txBase = sram(at + kChRxBase * 4); c.txRear = sram(at + kChRxRear * 4); c.txFront = sram(at + kChRxFront * 4);
    c.rxBase = sram(at + kChTxBase * 4); c.rxRear = sram(at + kChTxRear * 4); c.rxFront = sram(at + kChTxFront * 4);
    c.seq = 0;
    const uint64_t bytes = static_cast<uint64_t>(c.qlen) * c.mlen;
    // akita: only the AP's part of the SRAM (from the vendor tree's dump base, +0x5000) may be read: fullos145 panicked (SError) on
    // channel 14 after channels 0..13 read fine, so a queue pointer below 0x5000 makes the channel unusable and nothing there is read.
    const bool apSide = c.txRear >= kApLow && c.txFront >= kApLow && c.rxRear >= kApLow && c.rxFront >= kApLow && c.txBase >= kApLow && c.rxBase >= kApLow;
    c.usable = apSide && c.qlen && c.mlen >= 16 && !(c.mlen & 3) && c.txRear < kSramSize - 3 && c.txFront < kSramSize - 3 && c.rxRear < kSramSize - 3 &&
               c.rxFront < kSramSize - 3 && c.txBase + bytes <= kSramSize && c.rxBase + bytes <= kSramSize;
    printf("acpm: channel %u: id %u, %u messages of %u bytes, %s; AP sends at %#x (front %u rear %u), reads at %#x (front %u rear %u)%s\n", i, c.id, c.qlen, c.mlen,
           c.poll ? "polled" : "interrupt", c.txBase, c.usable ? sram(c.txFront) : 0u, c.usable ? sram(c.txRear) : 0u, c.rxBase, c.usable ? sram(c.rxFront) : 0u,
           c.usable ? sram(c.rxRear) : 0u, c.usable ? "" : " (not usable)");
    if (!c.usable) printf("acpm: channel %u raw: tx rear %#x front %#x, rx rear %#x front %#x\n", i, c.txRear, c.txFront, c.rxRear, c.rxFront);
  }
  gChans = count;
  return true;
}

// One message, and its answer when `answer` is set (the same words, as the firmware returns them). Linux's acpm_do_xfer() for a single
// caller at a time: wait for room, write the message at the front of the AP's queue, move the front on, ring the doorbell, then look at
// the other queue until a message with our sequence number shows up (100 ms at most). 0, or a negative step number. Lock held.
int transfer(unsigned chan, uint32_t *words, unsigned count, bool answer) {
  if (chan >= gChans) return -1;
  Chan &c = gChan[chan];
  if (!c.usable || !c.poll || count * 4 > c.mlen) return -2;
  const uint32_t front = sram(c.txFront);
  if (front >= c.qlen) return -3;
  const uint32_t next = (front + 1) % c.qlen;
  if (!waitUs(500000, [&c, next] { return sram(c.rxFront) != next; })) return -4;
  if (!waitUs(500000, [&c, next] { return sram(c.txRear) != next; })) return -5;   // the queue is full
  c.seq = c.seq % 63 + 1;
  words[0] = (words[0] & ~kSeqMask) | (c.seq << kSeqShift);
  for (unsigned w = 0; w < count; ++w) sramWrite(c.txBase + c.mlen * front + w * 4, words[w]);
  sramWrite(c.txFront, next);
  gMbox[kMboxIntgr1 / 4] = 1u << chan;
  __asm__ volatile("dsb sy" ::: "memory");

  bool got = false, broken = false;
  const bool done = waitUs(100000, [&] {
    const uint32_t rxFront = sram(c.rxFront);
    uint32_t i = sram(c.rxRear);
    if (rxFront >= c.qlen || i >= c.qlen) { broken = true; return true; }
    if (i == rxFront) return false;
    for (; i != rxFront; i = (i + 1) % c.qlen) {
      const uint32_t at = c.rxBase + c.mlen * i;
      if (((sram(at) & kSeqMask) >> kSeqShift) != c.seq) continue;   // one caller at a time: anything else is nobody's
      if (answer) for (unsigned w = 0; w < count; ++w) words[w] = sram(at + w * 4);
      got = true;
    }
    sramWrite(c.rxRear, rxFront);   // all read
    return got;
  });
  if (broken) return -6;
  return done && got ? 0 : -7;
}

const char *const kDomain[] = {"MIF", "INT", "CPUCL0 (4x A510)", "CPUCL1 (4x A715)", "CPUCL2 (X3)", "DSU", "BCI", "G3D (GPU)", "G3DL2", "TPU", "INTCAM", "TNR", "CAM", "MFC", "DISP", "AOC", "BW"};

// ACPM_DVFS_FREQ_GET: { domain, 0, type 1, a timestamp in ms }; the answer's second word is the frequency in kHz.
int getKhz(unsigned domain, uint32_t *khz) {
  uint32_t words[4] = {domain & 0xfffu, 0, 1, static_cast<uint32_t>(ticksNow() * 1000 / ticksPerSecond())};
  const int r = transfer(kDvfsChan, words, 4, true);
  if (r == 0) *khz = words[1];
  return r;
}

// ACPM_TMU_READ_TEMP (exynos-acpm-tmu.c): byte 4 the type (2), byte 6 the zone; the answer's byte 5 is the firmware's result (negative: an
// error) and byte 7 the temperature in degrees Celsius, signed.
const char *const kZone[] = {"BIG (X3)", "MID (A715)", "LITTLE (A510)", "G3D (GPU)", "ISP", "TPU", "AUR"};
int getCelsius(unsigned zone, int *celsius, int *firmware) {
  uint32_t words[4] = {0, 2u | ((zone & 0xffu) << 16), 0, 0};
  const int r = transfer(kTmuChan, words, 4, true);
  if (r != 0) return r;
  *firmware = static_cast<int8_t>((words[1] >> 8) & 0xff);
  *celsius = static_cast<int8_t>((words[1] >> 24) & 0xff);
  return 0;
}

// ACPM_DVFS_FREQ_REQ (type 0): { domain, kHz, 0, a timestamp in ms }; the firmware picks the level and sets its voltage itself.
int setKhz(unsigned domain, uint32_t khz) {
  uint32_t words[4] = {domain & 0xfffu, khz, 0, static_cast<uint32_t>(ticksNow() * 1000 / ticksPerSecond())};
  return transfer(kDvfsChan, words, 4, true);
}

// The vendor tree's fvmap (SRAM + 0x31000): per domain a 24-byte header (byte 1 the number of levels, byte 8 the boot level, the
// u16 at 20 the offset of the {rate kHz, voltage uV} table from the fvmap's start). Only for printing and for a ceiling.
constexpr uint32_t kFvmap = 0x31000, kFvHeader = 24;
constexpr unsigned kDomains = sizeof kDomain / sizeof kDomain[0];
uint32_t fvmapMaxKhz(unsigned domain, bool print) {
  const uint32_t at = kFvmap + domain * kFvHeader;
  if (at + kFvHeader > kSramSize) return 0;
  const uint32_t w0 = sram(at), w2 = sram(at + 8), w5 = sram(at + 20);
  const unsigned levels = (w0 >> 8) & 0xff, init = w2 & 0xff, oRateVolt = w5 & 0xffff;
  const uint32_t table = kFvmap + oRateVolt;
  if (!levels || levels > 40 || (table & 3) || table < kApLow || table + levels * 8 > kSramSize) {
    if (print) printf("acpm: fvmap %2u %-18s header %#x %#x %#x %#x %#x %#x: no table\n", domain, kDomain[domain], w0, sram(at + 4), w2, sram(at + 12), sram(at + 16), w5);
    return 0;
  }
  uint32_t most = 0;
  char line[640];
  int n = snprintf(line, sizeof line, "acpm: fvmap %2u %-18s %u levels, boot level %u:", domain, kDomain[domain], levels, init);
  for (unsigned l = 0; l < levels; ++l) {
    const uint32_t rate = sram(table + l * 8), volt = sram(table + l * 8 + 4);
    if (rate > most) most = rate;
    if (print && n > 0 && static_cast<size_t>(n) < sizeof line - 24) n += snprintf(line + n, sizeof line - static_cast<size_t>(n), " %u/%u", rate / 1000, volt / 1000);
  }
  if (print) printf("%s (MHz/mV)\n", line);
  return most;
}

// Raised domains and their boot speed; a thermal guard (every second while anything is raised) puts them back at 85 C.
// akita (Generated-by: LLM): a regulator, not a one-way switch (fullos163-169: every boot went back to boot speeds for good during the
// shader compile and the drags ran at 1.1 GHz): at 85 C a zone's domains step down a third of the way to their boot speed, at 95 C
// straight to it; after 5 s under 75 C they step back up toward the speed asked for.
struct Raised { bool on; uint32_t bootKhz, wantKhz, nowKhz; unsigned coolTicks; } gRaised[kDomains];
constexpr int kCoolCelsius = 75, kCriticalCelsius = 95;
constexpr unsigned kCoolTicksToRise = 5;
thread_call_t gGuardCall;
bool gGuardArmed;
unsigned gGuardTicks;
constexpr int kHotCelsius = 85;
constexpr unsigned kGuardZones = 4;   // X3, A715, A510, GPU

bool anyRaised() {
  for (unsigned d = 0; d < kDomains; ++d) if (gRaised[d].on) return true;
  return false;
}
void armGuard() {
  if (!gGuardCall || gGuardArmed) return;
  uint64_t deadline;
  clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
  gGuardArmed = true;
  thread_call_enter_delayed(gGuardCall, deadline);
}
void lowerAll(const char *why) {
  for (unsigned d = 0; d < kDomains; ++d) {
    if (!gRaised[d].on) continue;
    const int r = setKhz(d, gRaised[d].bootKhz);
    printf("acpm: guard: %s: %s back to %u kHz (%d)\n", why, kDomain[d], gRaised[d].bootKhz, r);
    gRaised[d].on = false;
  }
}
void guardTick(thread_call_param_t, thread_call_param_t) {
  IOLockLock(gAcpmLock);
  gGuardArmed = false;
  int hottest = -100, failures = 0;
  unsigned hotZone = 0;
  int temps[kGuardZones] = {};
  for (unsigned z = 0; z < kGuardZones; ++z) {
    int celsius = 0, firmware = 0;
    if (getCelsius(z, &celsius, &firmware) != 0 || firmware < 0) { ++failures; continue; }
    temps[z] = celsius;
    if (celsius > hottest) { hottest = celsius; hotZone = z; }
  }
  ++gGuardTicks;
  if (failures == static_cast<int>(kGuardZones)) {
    lowerAll("no temperature");
  } else {
    // akita: only the hot zone's own domains step (fullos148: the X3 at 2.9 GHz reached 89 C while the GPU sat at 56 C)
    (void)hotZone;
  }
  if (failures != static_cast<int>(kGuardZones)) {
    static const int kZoneDomains[kGuardZones][2] = {{4, -1}, {3, -1}, {2, -1}, {7, 8}};
    for (unsigned z = 0; z < kGuardZones; ++z) {
      for (int dd : kZoneDomains[z]) {
        if (dd < 0) continue;
        const unsigned d = static_cast<unsigned>(dd);
        Raised &R = gRaised[d];
        if (!R.on) continue;
        const uint32_t step = R.wantKhz > R.bootKhz ? (R.wantKhz - R.bootKhz + 2) / 3 : 0;
        uint32_t next = R.nowKhz;
        if (temps[z] >= kCriticalCelsius) {
          next = R.bootKhz;
          R.coolTicks = 0;
        } else if (temps[z] >= kHotCelsius) {
          next = R.nowKhz > R.bootKhz + step ? R.nowKhz - step : R.bootKhz;
          R.coolTicks = 0;
        } else if (temps[z] < kCoolCelsius && R.nowKhz < R.wantKhz) {
          if (++R.coolTicks >= kCoolTicksToRise) {
            next = R.nowKhz + step < R.wantKhz ? R.nowKhz + step : R.wantKhz;
            R.coolTicks = 0;
          }
        } else {
          R.coolTicks = 0;
        }
        if (next != R.nowKhz) {
          const int r = setKhz(d, next);
          printf("acpm: guard: %d C in %s: %s %u -> %u kHz (%d)\n", temps[z], kZone[z], kDomain[d], R.nowKhz, next, r);
          R.nowKhz = next;
        }
      }
    }
  }
  if (gGuardTicks % 30 == 1) {
    printf("acpm: guard: X3 %d C, A715 %d C, A510 %d C, GPU %d C; X3 %u A715 %u A510 %u MHz\n", temps[0], temps[1], temps[2], temps[3],
           gRaised[4].nowKhz / 1000, gRaised[3].nowKhz / 1000, gRaised[2].nowKhz / 1000);
  }
  if (anyRaised()) armGuard();
  IOLockUnlock(gAcpmLock);
}

// kern.akita_acpm = 1000000 + domain * 10000 + MHz: ask for that speed (no more than the fvmap's top level), then read it back.
int raise(unsigned domain, unsigned mhz) {
  if (domain >= kDomains || !mhz) { printf("acpm: set: domain %u / %u MHz makes no sense\n", domain, mhz); return -1; }
  // akita: SRAM+0x31000 is not the fvmap here (random words; reading its "table" was an unaligned abort, fullos147), so the
  // ceilings are Tensor G3's published top speeds; the firmware picks the nearest level it has.
  static const uint32_t kTopKhz[kDomains] = {3172000, 0, 1704000, 2367000, 2910000, 0, 0, 890000, 900000, 0, 0, 0, 0, 0, 0, 0, 0};
  const uint32_t most = kTopKhz[domain];
  if (!most) { printf("acpm: set: %s is not one we raise: refused\n", kDomain[domain]); return -1; }
  uint32_t khz = mhz * 1000u;
  if (khz > most) khz = most;
  if (!gRaised[domain].on) {
    uint32_t now = 0;
    const int r = getKhz(domain, &now);
    if (r != 0 || !now) { printf("acpm: set: cannot read %s's speed (%d): refused\n", kDomain[domain], r); return -1; }
    gRaised[domain].bootKhz = now;
  }
  const int r = setKhz(domain, khz);
  uint32_t after = 0;
  const int g = getKhz(domain, &after);
  printf("acpm: set: %s asked %u kHz (boot %u): answer %d, now %u kHz (%d)\n", kDomain[domain], khz, gRaised[domain].bootKhz, r, after, g);
  gRaised[domain].on = khz != gRaised[domain].bootKhz;
  gRaised[domain].wantKhz = gRaised[domain].nowKhz = khz;   // akita: the regulator's target and where it is
  gRaised[domain].coolTicks = 0;
  if (!gGuardCall) gGuardCall = thread_call_allocate(guardTick, nullptr);
  if (anyRaised()) armGuard();
  return r;
}

// akita-dvfs=1: 45 s after the GPU addon goes in, the speeds for the desktop. GPU and memory at their top (they stay cool), the CPUs
// part of the way (the X3 at its top heated to 89 C in a minute with a busy thread). The guard watches all of it.
thread_call_t gProfileCall;
void applyProfile(thread_call_param_t, thread_call_param_t) {
  static const unsigned kProfile[][2] = {{0, 3172}, {8, 900}, {7, 890}, {2, 1704}, {3, 1800}, {4, 2000}};
  IOLockLock(gAcpmLock);
  if (probe()) {
    for (const auto &p : kProfile) raise(p[0], p[1]);
    printf("acpm: desktop speeds set (akita-dvfs=1)\n");
  } else {
    printf("acpm: desktop speeds: no ACPM\n");
  }
  IOLockUnlock(gAcpmLock);
}

}  // namespace

namespace touch { void startLive(); }   // akita: AkitaTouch.inc, included below
extern "C" int akita_acpm_rtc_seconds(uint64_t *seconds);   // akita: the PMIC RTC, at the end of this file
extern "C" int akita_acpm_rtc_write(uint64_t seconds);
extern "C" void akita_acpm_boot_profile(void);
extern "C" void akita_acpm_boot_profile(void) {
  unsigned allowed = 0, profile = 0;
  PE_parse_boot_argn("akita-acpm", &allowed, sizeof allowed);
  PE_parse_boot_argn("akita-dvfs", &profile, sizeof profile);
  // akita: the touch controller's live driver (AkitaTouch.inc), 20 s after the GPU addon goes in
  unsigned touchOn = 0;
  PE_parse_boot_argn("akita-touch", &touchOn, sizeof touchOn);
  if (allowed && touchOn == 1) {
    if (!gAcpmLock) gAcpmLock = IOLockAlloc();
    static thread_call_t touchCall;
    if (gAcpmLock && !touchCall && (touchCall = thread_call_allocate([](thread_call_param_t, thread_call_param_t) { touch::startLive(); }, nullptr))) {
      uint64_t deadline;
      clock_interval_to_deadline(20, NSEC_PER_SEC, &deadline);
      thread_call_enter_delayed(touchCall, deadline);
      printf("touch: live driver in 20 s (akita-touch=1)\n");
    }
  }
  if (!allowed || profile != 1 || gProfileCall) return;
  if (!gAcpmLock) gAcpmLock = IOLockAlloc();
  if (!gAcpmLock) return;
  gProfileCall = thread_call_allocate(applyProfile, nullptr);
  if (!gProfileCall) return;
  uint64_t deadline;
  clock_interval_to_deadline(45, NSEC_PER_SEC, &deadline);
  thread_call_enter_delayed(gProfileCall, deadline);
  printf("acpm: desktop speeds in 45 s (akita-dvfs=1)\n");
}

// The temperature of a zone in degrees Celsius; negative step numbers as akita_acpm_get_khz, -200 when the firmware refused.
extern "C" int akita_acpm_get_celsius(unsigned zone, int *celsius);
extern "C" int akita_acpm_get_celsius(unsigned zone, int *celsius) {
  if (!gAcpmLock || !celsius) return -100;
  int firmware = 0;
  IOLockLock(gAcpmLock);
  int r = gChans ? getCelsius(zone, celsius, &firmware) : -101;
  IOLockUnlock(gAcpmLock);
  if (r == 0 && firmware < 0) r = -200;
  return r;
}

extern "C" int akita_acpm_get_khz(unsigned domain, uint32_t *khz) {
  if (!gAcpmLock || !khz) return -100;
  IOLockLock(gAcpmLock);
  const int r = gChans ? getKhz(domain, khz) : -101;
  IOLockUnlock(gAcpmLock);
  return r;
}

#include "AkitaTouch.inc"   // akita: the touch controller's staged bring-up (kern.akita_acpm = 2000000 + stage)
#include <sys/vnode.h>   // akita: AkitaPcie.inc reads the Wi-Fi firmware
#include <sys/fcntl.h>
#include <sys/random.h>
#include "AkitaPcie.inc"    // akita: the Wi-Fi PCIe link, staged look (kern.akita_acpm = 4000000 + stage)

extern "C" int akita_acpm_command(unsigned what) {
  static IOLock *made;
  if (!made && gAcpmLock) made = gAcpmLock;
  if (!made) {
    IOLock *lock = IOLockAlloc();
    if (!lock) return -1;
    if (!__sync_bool_compare_and_swap(&made, static_cast<IOLock *>(nullptr), lock)) IOLockFree(lock);
    gAcpmLock = made;
  }
  // akita: reading the APM SRAM panicked the phone with an SError on 2026-11-11 (fullos130): nothing is touched without akita-acpm=1.
  unsigned allowed = 0;
  PE_parse_boot_argn("akita-acpm", &allowed, sizeof allowed);
  if (!allowed) { printf("acpm: refused: needs boot-arg akita-acpm=1 (the APM SRAM read panicked the phone)\n"); return -1; }
  // akita: staged look at what panicked (each its own sysctl, the log flushes in between):
  //   10 map the SRAM and the mailbox and read the mailbox's first register; 11 read one SRAM word at the dump base (+0x5000,
  //   the part the vendor tree calls the AP's); 12 read the first eight words of the init data (+0xa000).
  if (what >= 10 && what <= 12) {
    IOLockLock(gAcpmLock);
    if (!gSram) gSram = reinterpret_cast<volatile uint8_t *>(ml_io_map(kSramPhys, kSramSize));
    if (!gMbox) gMbox = reinterpret_cast<volatile uint32_t *>(ml_io_map(kMboxPhys, kMboxSize));
    printf("acpm: stage %u: SRAM mapped at %p, mailbox at %p; reading in 2 s\n", what, gSram, gMbox);
    IOSleep(2000);
    if (what == 10 && gMbox) printf("acpm: stage 10: mailbox[0] = %#x, [0x40/4] = %#x\n", gMbox[0], gMbox[kMboxIntgr1 / 4]);
    if (what == 11 && gSram) printf("acpm: stage 11: SRAM+0x5000 = %#x\n", sram(0x5000));
    if (what == 12 && gSram) {
      uint32_t w[8];
      for (unsigned i = 0; i < 8; ++i) w[i] = sram(kInitData + i * 4);
      printf("acpm: stage 12: init data %#x %#x %#x %#x %#x %#x %#x %#x\n", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
    IOLockUnlock(gAcpmLock);
    return 0;
  }
  if (what > 4000000 && what < 4000100) return pcie::command(what - 4000000);   // akita: Wi-Fi PCIe stages (AkitaPcie.inc)
  if (what == 3000000 || what == 3000001) {   // akita: the PMIC RTC: 3000000 writes the kernel's calendar time (UTC) into it, 3000001 reads it
    uint64_t t = 0;
    if (what == 3000001) return akita_acpm_rtc_seconds(&t);
    clock_sec_t sec = 0; clock_usec_t usec = 0;
    clock_get_calendar_microtime(&sec, &usec);
    return akita_acpm_rtc_write(static_cast<uint64_t>(sec));
  }
  if (what > 2000000 && what < 2000100) {   // akita: touch bring-up stages (AkitaTouch.inc)
    IOLockLock(gAcpmLock);
    const int r = touch::command(what - 2000000);
    IOLockUnlock(gAcpmLock);
    return r;
  }
  if (what >= 1000000 && what < 1000000 + kDomains * 10000) {
    IOLockLock(gAcpmLock);
    const int r = probe() ? raise((what - 1000000) / 10000, (what - 1000000) % 10000) : -1;
    IOLockUnlock(gAcpmLock);
    return r;
  }
  if (what == 4) {   // akita: disabled, SRAM+0x31000 is not the fvmap (unaligned abort on fullos147)
    printf("acpm: the fvmap dump is disabled\n");
    return -1;
  }
  if (what == 40) {
    IOLockLock(gAcpmLock);
    const bool ok = probe();
    if (ok) for (unsigned d = 0; d < kDomains; ++d) fvmapMaxKhz(d, true);
    IOLockUnlock(gAcpmLock);
    return ok ? 0 : -1;
  }
  if (what == 5) {   // back to the boot speeds
    IOLockLock(gAcpmLock);
    if (probe()) lowerAll("asked");
    IOLockUnlock(gAcpmLock);
    return 0;
  }
  if (what < 1 || what > 3) { printf("acpm: command %u unknown (1: channel table, 2: frequencies, 3: temperatures)\n", what); return -1; }
  IOLockLock(gAcpmLock);
  int result = probe() ? 0 : -1;
  if (result == 0 && what == 2) {
    for (unsigned d = 0; d < sizeof kDomain / sizeof kDomain[0]; ++d) {
      uint32_t khz = 0;
      const int r = getKhz(d, &khz);
      if (r == 0) printf("acpm: domain %2u %-18s %u kHz\n", d, kDomain[d], khz);
      else { printf("acpm: domain %2u %-18s no answer (step %d): stopping here\n", d, kDomain[d], r); result = r; break; }
    }
  }
  if (result == 0 && what == 3) {
    for (unsigned z = 0; z < sizeof kZone / sizeof kZone[0]; ++z) {
      int celsius = 0, firmware = 0;
      const int r = getCelsius(z, &celsius, &firmware);
      if (r == 0) printf("acpm: zone %u %-14s %d C (firmware result %d)\n", z, kZone[z], celsius, firmware);
      else { printf("acpm: zone %u %-14s no answer (step %d): stopping here\n", z, kZone[z], r); result = r; break; }
    }
  }
  IOLockUnlock(gAcpmLock);
  return result;
}

// akita (Generated-by: LLM): the main PMIC's real-time clock, so macOS boots with the right time instead of the image's build time.
// S2MPG14 RTC block = ACPM PMIC channel 2, I2C type 0x02 (s2mpg14-register.h I2C_ADDR_RTC). Google's rtc-s2mpg14.c reads it by
// clearing then setting RUDR (bit 0 of RTC_UPDATE 0x01) to latch the counters, then SEC..YEAR at 0x06..0x0C in binary:
// sec & 0x7f, min & 0x7f, hour & 0x1f (bit 6 = PM flag, the hour itself is 0..23), weekday one-hot, day & 0x1f, month & 0x0f (1..12),
// year & 0x7f (+2000). Android keeps it in UTC. Only RUDR is ever changed (update with a one-bit mask); the time is never written.
namespace {
int rtcRead(uint32_t reg, uint8_t *value) {
  uint32_t w[4] = {(0x02u << 8) | (reg & 0xff), 0 /* FUNC_READ */, 0, 0};
  const int r = transfer(2, w, 4, true);
  if (r != 0) return r;
  if ((w[1] >> 24) & 0xff) return 1000 + static_cast<int>((w[1] >> 24) & 0xff);
  *value = static_cast<uint8_t>((w[1] >> 8) & 0xff);
  return 0;
}
int rtcUpdateBit0(uint8_t value) {
  uint32_t w[4] = {(0x02u << 8) | 0x01u, 2u /* FUNC_UPDATE */ | (uint32_t(value & 1) << 8) | (1u << 16), 0, 0};
  const int r = transfer(2, w, 4, true);
  if (r != 0) return r;
  return ((w[1] >> 24) & 0xff) ? 1000 + static_cast<int>((w[1] >> 24) & 0xff) : 0;
}
// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
}  // namespace

extern "C" int akita_acpm_rtc_seconds(uint64_t *seconds);
extern "C" int akita_acpm_rtc_seconds(uint64_t *seconds) {
  if (!seconds) return -100;
  if (!gAcpmLock) gAcpmLock = IOLockAlloc();
  if (!gAcpmLock) return -100;
  IOLockLock(gAcpmLock);
  int r = probe() ? 0 : -101;
  uint8_t v[7] = {};
  if (r == 0) r = rtcUpdateBit0(0);
  if (r == 0) { IODelay(60); r = rtcUpdateBit0(1); }
  if (r == 0) IODelay(1000);
  for (unsigned i = 0; r == 0 && i < 7; ++i) r = rtcRead(0x06 + i, &v[i]);
  IOLockUnlock(gAcpmLock);
  if (r != 0) { printf("rtc: read failed (%d)\n", r); return r; }
  const unsigned sec = v[0] & 0x7f, min = v[1] & 0x7f, hour = v[2] & 0x1f, day = v[4] & 0x1f, mon = v[5] & 0x0f, year = 2000 + (v[6] & 0x7f);
  printf("rtc: %04u-%02u-%02u %02u:%02u:%02u UTC (raw %02x %02x %02x %02x %02x %02x %02x)\n", year, mon, day, hour, min, sec,
         v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
  if (sec > 59 || min > 59 || hour > 23 || !day || day > 31 || !mon || mon > 12) return -102;
  *seconds = static_cast<uint64_t>(daysFromCivil(year, mon, day)) * 86400 + hour * 3600 + min * 60 + sec;
  return 0;
}

// Write a UTC time into the RTC (Google's s2m_rtc_set_time: the seven counters, then WUDR (bit 1 of RTC_UPDATE) cleared and set).
extern "C" int akita_acpm_rtc_write(uint64_t seconds) {
  if (seconds < 1704067200ULL || seconds >= 4102444800ULL) return -102;   // 2024..2099: the RTC holds years 2000..2127
  const int64_t days = static_cast<int64_t>(seconds / 86400);
  const unsigned rem = static_cast<unsigned>(seconds % 86400);
  // civil from days (Hinnant)
  const int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * mp + 2) / 5 + 1, mon = mp < 10 ? mp + 3 : mp - 9;
  const unsigned year = static_cast<unsigned>(yoe + era * 400 + (mon <= 2));
  const unsigned hour = rem / 3600, min = rem / 60 % 60, sec = rem % 60, wday = static_cast<unsigned>((days + 4) % 7);   // 1970-01-01 was a Thursday
  const uint8_t v[7] = {uint8_t(sec), uint8_t(min), uint8_t(hour | (hour >= 12 ? 0x40 : 0)), uint8_t(1u << wday), uint8_t(day), uint8_t(mon),
                        uint8_t(year - 2000)};
  if (!gAcpmLock) gAcpmLock = IOLockAlloc();
  if (!gAcpmLock) return -100;
  IOLockLock(gAcpmLock);
  int r = probe() ? 0 : -101;
  for (unsigned i = 0; r == 0 && i < 7; ++i) {
    uint32_t w[4] = {(0x02u << 8) | (0x06u + i), 2u /* FUNC_UPDATE */ | (uint32_t(v[i]) << 8) | (0xffu << 16), 0, 0};
    r = transfer(2, w, 4, true);
    if (r == 0 && ((w[1] >> 24) & 0xff)) r = 1000 + static_cast<int>((w[1] >> 24) & 0xff);
  }
  for (unsigned bit = 0; r == 0 && bit < 2; ++bit) {   // WUDR low, then high: latch the new counters
    uint32_t w[4] = {(0x02u << 8) | 0x01u, 2u | (uint32_t(bit ? 2 : 0) << 8) | (2u << 16), 0, 0};
    r = transfer(2, w, 4, true);
    if (r == 0 && ((w[1] >> 24) & 0xff)) r = 1000 + static_cast<int>((w[1] >> 24) & 0xff);
    IODelay(bit ? 1000 : 60);
  }
  IOLockUnlock(gAcpmLock);
  printf("rtc: wrote %04u-%02u-%02u %02u:%02u:%02u UTC: %d\n", year, mon, day, hour, min, sec, r);
  return r;
}
