// akita (Generated-by: LLM, local only): the battery in macOS's menu bar. An IOPMPowerSource (the class powerd matches,
// as AppleSmartBattery on a MacBook) filled every 20 s from the MAX77759's fuel gauge (I2C 0x36, ModelGauge m5 registers)
// and the TCPC's VBUS state (0x25 POWER_STATUS). Read-only: the fuel gauge, the charger and the PMIC are never written.
//   boot-arg akita-battery=0 turns it off; kern.akita_acpm = 8000000 prints what it reads now.
// Sources: MAX1720x/MAX77759 fuel gauge register map (Linux max1720x_battery.c, Google's google-battery), xnu
// IOPMPowerSource.cpp, and the keys powerd reads from AppleSmartBattery (IOPMPowerSource.h, IOPSKeys.h).
#include <IOKit/pwr_mgt/IOPMPowerSource.h>

// IOPMPowerSource's setters are protected (a battery driver is its subclass): ours only opens them up
class AkitaBattery : public IOPMPowerSource {
  OSDeclareDefaultStructors(AkitaBattery);
public:
  using IOPMPowerSource::setPSProperty;
  using IOPMPowerSource::setBatteryInstalled;
  using IOPMPowerSource::setExternalConnected;
  using IOPMPowerSource::setExternalChargeCapable;
  using IOPMPowerSource::setIsCharging;
  using IOPMPowerSource::setAtWarnLevel;
  using IOPMPowerSource::setAtCriticalLevel;
  using IOPMPowerSource::setCurrentCapacity;
  using IOPMPowerSource::setMaxCapacity;
  using IOPMPowerSource::setTimeRemaining;
  using IOPMPowerSource::setAmperage;
  using IOPMPowerSource::setVoltage;
  using IOPMPowerSource::setCycleCount;
  using IOPMPowerSource::setManufacturer;
  using IOPMPowerSource::setModel;
};
OSDefineMetaClassAndStructors(AkitaBattery, IOPMPowerSource)

namespace battery {

enum : uint8_t { kFg = 0x36, RepCap = 0x05, RepSOC = 0x06, Temp = 0x08, VCell = 0x09, Current = 0x0A, AvgCurrent = 0x0B,
                 FullCapRep = 0x10, TTE = 0x11, Cycles = 0x17, DesignCap = 0x18, TTF = 0x20 };
// akita's sense resistor is 5 mOhm: current 1.5625 uV / 5 mOhm = 0.3125 mA per LSB, capacity 5 uVh / 5 mOhm = 1 mAh per LSB
constexpr int kMicroAmpPerLsb = 312;

AkitaBattery *gPs = nullptr;
thread_call_t gCall = nullptr;
bool gLogged = false;

int rd16(uint8_t reg, uint16_t &v) {
  uint8_t b[2] = {};
  const int r = i2c::readRegs(kFg, reg, b, 2);
  if (!r) v = static_cast<uint16_t>(b[0] | (b[1] << 8));
  return r;
}

struct Reading {
  bool ok = false, vbus = false;
  unsigned soc = 0, mv = 0, capMah = 0, fullMah = 0, designMah = 0, cycles = 0;
  int ma = 0, avgMa = 0, centiC = 0, tteMin = -1, ttfMin = -1;
};

Reading read() {
  Reading r;
  uint16_t soc, vcell, cur, avg, temp, cap, full, design, cyc, tte, ttf;
  if (rd16(RepSOC, soc) || rd16(VCell, vcell) || rd16(Current, cur)) return r;
  r.ok = true;
  r.soc = (soc + 128) / 256;
  if (r.soc > 100) r.soc = 100;
  r.mv = static_cast<unsigned>(vcell * 78125ull / 1000000ull);   // 78.125 uV per LSB
  r.ma = static_cast<int16_t>(cur) * kMicroAmpPerLsb / 1000;     // + charging, - discharging
  r.avgMa = rd16(AvgCurrent, avg) ? r.ma : static_cast<int16_t>(avg) * kMicroAmpPerLsb / 1000;
  if (!rd16(Temp, temp)) r.centiC = static_cast<int16_t>(temp) * 100 / 256;
  if (!rd16(RepCap, cap)) r.capMah = cap;
  if (!rd16(FullCapRep, full)) r.fullMah = full;
  if (!rd16(DesignCap, design)) r.designMah = design;
  if (!rd16(Cycles, cyc)) r.cycles = cyc / 100;                   // 1 % of a cycle per LSB
  if (!rd16(TTE, tte) && tte != 0xffff) r.tteMin = static_cast<int>(tte * 5625ull / 1000 / 60);   // 5.625 s per LSB
  if (!rd16(TTF, ttf) && ttf != 0xffff) r.ttfMin = static_cast<int>(ttf * 5625ull / 1000 / 60);
  uint8_t pw = 0;
  if (!i2c::readRegs(0x25, 0x1E, &pw, 1)) r.vbus = pw & 4;          // TCPC POWER_STATUS bit 2: VBUS present
  return r;
}

void print(const Reading &r) {
  printf("battery: %u %%, %u mV, %d mA (avg %d), %d.%02d C, %u/%u mAh (design %u), %u cycles, to empty %d min, to full %d min, "
         "VBUS %s\n", r.soc, r.mv, r.ma, r.avgMa, r.centiC / 100, r.centiC % 100 < 0 ? -(r.centiC % 100) : r.centiC % 100,
         r.capMah, r.fullMah, r.designMah, r.cycles, r.tteMin, r.ttfMin, r.vbus ? "yes" : "no");
}

void setNumber(const char *key, long long v) {
  const OSSymbol *k = OSSymbol::withCString(key);
  OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(v), 32);
  if (k && n) gPs->setPSProperty(k, n);
  if (n) n->release();
  if (k) k->release();
}
void setBool(const char *key, bool v) {
  const OSSymbol *k = OSSymbol::withCString(key);
  if (k) gPs->setPSProperty(k, v ? kOSBooleanTrue : kOSBooleanFalse);
  if (k) k->release();
}

void update() {
  const Reading r = read();
  if (!r.ok) { printf("battery: the fuel gauge did not answer\n"); return; }
  if (!gLogged) { print(r); gLogged = true; }
  const bool charging = r.vbus && r.ma > 0 && r.soc < 100;
  gPs->setBatteryInstalled(true);
  gPs->setExternalConnected(r.vbus);
  gPs->setExternalChargeCapable(r.vbus);
  gPs->setIsCharging(charging);
  // Apple silicon MacBooks report CurrentCapacity in percent of MaxCapacity = 100, and the mAh in AppleRaw*Capacity
  gPs->setCurrentCapacity(r.soc);
  gPs->setMaxCapacity(100);
  gPs->setVoltage(r.mv);
  gPs->setAmperage(r.ma);
  gPs->setCycleCount(r.cycles);
  gPs->setAtWarnLevel(!r.vbus && r.soc <= 10);
  gPs->setAtCriticalLevel(!r.vbus && r.soc <= 5);
  const int minutes = charging ? r.ttfMin : r.vbus ? 0 : r.tteMin;
  gPs->setTimeRemaining(minutes >= 0 ? minutes : 65535);
  setNumber("AvgTimeToEmpty", !r.vbus && r.tteMin >= 0 ? r.tteMin : 65535);
  setNumber("InstantTimeToEmpty", !r.vbus && r.tteMin >= 0 ? r.tteMin : 65535);
  setNumber("AvgTimeToFull", charging && r.ttfMin >= 0 ? r.ttfMin : 65535);
  setNumber("InstantAmperage", r.ma);
  setNumber("Temperature", r.centiC);
  setNumber("AppleRawCurrentCapacity", r.capMah);
  setNumber("AppleRawMaxCapacity", r.fullMah);
  setNumber("NominalChargeCapacity", r.fullMah);
  setNumber("DesignCapacity", r.designMah);
  setNumber("AppleRawBatteryVoltage", r.mv);
  setBool("FullyCharged", r.vbus && r.soc >= 100);
  setBool("AppleRawExternalConnected", r.vbus);
  gPs->updateStatus();
}

void tick(thread_call_param_t, thread_call_param_t) {
  if (!gPs) {
    gPs = new AkitaBattery;
    if (gPs && !gPs->init()) { gPs->release(); gPs = nullptr; }
    IOService *platform = IOService::getPlatform();
    if (!gPs || !platform || !gPs->attach(platform)) { printf("battery: could not make the power source\n"); return; }
    const OSSymbol *s = OSSymbol::withCString("Google");
    if (s) { gPs->setManufacturer(const_cast<OSSymbol *>(s)); s->release(); }
    s = OSSymbol::withCString("Pixel 8a");
    if (s) { gPs->setModel(const_cast<OSSymbol *>(s)); s->release(); }
    update();
    gPs->registerService();
    printf("battery: power source registered\n");
  } else {
    update();
  }
  uint64_t deadline;
  clock_interval_to_deadline(20, NSEC_PER_SEC, &deadline);
  thread_call_enter_delayed(gCall, deadline);
}

void boot() {
  unsigned on = 1;
  PE_parse_boot_argn("akita-battery", &on, sizeof on);
  if (!on || gCall) return;
  gCall = thread_call_allocate(tick, nullptr);
  if (!gCall) return;
  uint64_t deadline;
  clock_interval_to_deadline(30, NSEC_PER_SEC, &deadline);
  thread_call_enter_delayed(gCall, deadline);
  printf("battery: power source in 30 s (akita-battery=0 turns it off)\n");
}

}  // namespace battery
