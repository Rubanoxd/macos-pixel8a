// akita (Generated-by: LLM, local only): macOS's own brightness (Control Center slider, brightness keys) to the Pixel 8a's
// panel. CoreBrightness keeps the user brightness (DisplayServicesGetBrightness) but cannot commit it on this port (no DCP),
// so this agent watches it and writes "brightness-nits" (16.16) to AkitaBacklightService (AkitaBacklight.inc in the kernel),
// which sends DCS 0x51 to the panel. Runs as a LaunchAgent of the console user.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

static const double kMinNits = 2.0, kMaxNits = 500.0;

static int setNits(io_service_t s, double nits) {
  int v = (int)lround(nits * 65536.0);
  CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
  const void *k[] = {CFSTR("brightness-nits")}, *o[] = {n};
  CFDictionaryRef d = CFDictionaryCreate(NULL, k, o, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  kern_return_t r = IORegistryEntrySetCFProperties(s, d);
  CFRelease(d);
  CFRelease(n);
  return r;
}

int main(void) {
  void *h = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_NOW);
  int (*get)(CGDirectDisplayID, float *) = h ? (int (*)(CGDirectDisplayID, float *))dlsym(h, "DisplayServicesGetBrightness") : NULL;
  if (!get) { fprintf(stderr, "akita-brightnessd: no DisplayServicesGetBrightness\n"); return 1; }
  io_service_t s = IO_OBJECT_NULL;
  float last = -1;
  for (;;) {
    if (!s) s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AkitaBacklightService"));
    float b = -1;
    if (s && get(CGMainDisplayID(), &b) == 0 && b >= 0 && b <= 1 && fabsf(b - last) > 0.002f) {
      // the slider is perceptual: square it onto the panel's nits (its level is close to linear in nits)
      double nits = kMinNits + (kMaxNits - kMinNits) * (double)b * (double)b;
      int r = setNits(s, nits);
      if (r) { IOObjectRelease(s); s = IO_OBJECT_NULL; }
      else last = b;
    }
    usleep(100000);
  }
}
