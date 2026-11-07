// SPDX-License-Identifier: MIT
// NativeMaliMetal for macOS on the Pixel 8a: Metal's discovery (IOServiceMatching("IOAccelerator") -> MetalPluginName -> this bundle ->
// NSPrincipalClass -> initWithAcceleratorPort:) gets the native Metal frontend's device (UniXNU's NativeBridgeMetalFrontend over metal2vulkan)
// on Mesa PanVK, which reaches the Mali-G715 through darwin-drm and the kernel's MaliService.
// Only the processes named in /System/Volumes/Data/ak/metal-clients (one per line, "*" for all) get a device; every other one gets nil and
// keeps the software path it has without a GPU. Each process logs to /tmp/nativemali-<name>.log.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"

@interface _MTLDevice : NSObject
@end
@interface NativeMaliMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end

static void plog(const char *fmt, ...)
{
   char path[256];
   snprintf(path, sizeof path, "/tmp/nativemali-%s.log", getprogname());
   va_list ap;
   // WindowServer's stderr reaches the console log (sheng-stderr-name=WindowServer on the boot line)
   if (getenv("NATIVEMALI_STDERR") || !strcmp(getprogname(), "WindowServer")) {
      va_start(ap, fmt);
      fprintf(stderr, "nativemali: ");
      vfprintf(stderr, fmt, ap);
      fputc('\n', stderr);
      va_end(ap);
   }
   FILE *f = fopen(path, "a");
   if (!f)
      return;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

static bool allowed(void)
{
   FILE *f = fopen("/System/Volumes/Data/ak/metal-clients", "r");
   if (!f)
      return false;
   char line[128];
   bool ok = false;
   while (!ok && fgets(line, sizeof line, f)) {
      line[strcspn(line, "\r\n")] = 0;
      ok = !strcmp(line, "*") || !strcmp(line, getprogname());
   }
   fclose(f);
   return ok;
}

extern "C" const char *nmb_last_error(void);

static void missing(const char *cls, const char *sel) { plog("missing: [%s %s]", cls, sel); }

@implementation NativeMaliMetalDevice

// WindowServer on 25F71: SkyLight's compositor statics (unslid addresses from the 25F71 shared cache), logged around the plugin's
// own steps; with /System/Volumes/Data/ak/ws-force-metal present, +load marks WSSystemCanCompositeWithMetal as done with a device.
#include <dlfcn.h>
#include <ptrauth.h>
#include <unistd.h>
static volatile uint8_t *ws_byte(uint64_t unslid)
{
   static int64_t slide = -1;
   if (slide == -1) {
      void *f = dlsym(RTLD_DEFAULT, "SLSGetDebugOptions");
      int64_t s = f ? (int64_t)(uint64_t)ptrauth_strip(f, ptrauth_key_function_pointer) - 0x186d4b2d4 : 0;
      // only on the build these addresses come from: the block's prologue and its strb of metal_lovin
      bool match = f && *(volatile uint32_t *)(0x186f298c8 + s) == 0xd503237f && *(volatile uint32_t *)(0x186f299e0 + s) == 0x390ed128;
      slide = match ? s : -2;
   }
   return slide == -2 ? NULL : (volatile uint8_t *)(unslid + slide);
}
static void ws_state(const char *tag)
{
   if (strcmp(getprogname(), "WindowServer") || !ws_byte(0x1e9d333b4))
      return;
   plog("ws %s: once %llx metal_lovin %u default_to_metal %u active %u", tag, *(volatile uint64_t *)ws_byte(0x1e9d333b8),
        *ws_byte(0x1e9d333b4), *ws_byte(0x1e9d32d18), *(volatile uint32_t *)ws_byte(0x1e9d32ccc));
}

// WindowServer: log what CoreAnimation asks of the device (which of new_metal_context's steps happen at all).
#include <objc/runtime.h>
#include <objc/message.h>
static void ws_hook_device(void)
{
   Class c = objc_getClass("NBDeviceMethods");
   if (!c)
      return;
#define HOOK0(selname) do { SEL sel = sel_registerName(selname); Method m = class_getInstanceMethod(c, sel); if (!m) break; \
      IMP orig = method_getImplementation(m); method_setImplementation(m, imp_implementationWithBlock(^id(id self) { \
         id r = ((id (*)(id, SEL))orig)(self, sel); plog("dev %s -> %p", selname, (__bridge void *)r); return r; })); } while (0)
#define HOOK1(selname, T) do { SEL sel = sel_registerName(selname); Method m = class_getInstanceMethod(c, sel); if (!m) break; \
      IMP orig = method_getImplementation(m); method_setImplementation(m, imp_implementationWithBlock(^id(id self, T a) { \
         id r = ((id (*)(id, SEL, T))orig)(self, sel, a); plog("dev %s -> %p", selname, (__bridge void *)r); return r; })); } while (0)
#define HOOK2E(selname) do { SEL sel = sel_registerName(selname); Method m = class_getInstanceMethod(c, sel); if (!m) break; \
      IMP orig = method_getImplementation(m); method_setImplementation(m, imp_implementationWithBlock(^id(id self, id a, NSError **e) { \
         id r = ((id (*)(id, SEL, id, NSError **))orig)(self, sel, a, e); \
         plog("dev %s %s -> %p %s", selname, [[a description] UTF8String] ?: "", (__bridge void *)r, e && *e ? [[*e description] UTF8String] : ""); return r; })); } while (0)
   plog("hooks on %s", class_getName(c));
   HOOK0("newCommandQueue");
   HOOK1("newCommandQueueWithMaxCommandBufferCount:", NSUInteger);
   HOOK2E("newLibraryWithURL:error:");
   HOOK2E("newLibraryWithFile:error:");
   HOOK2E("newBinaryArchiveWithDescriptor:error:");
   HOOK1("newDepthStencilStateWithDescriptor:", id);
   HOOK1("newRenderPipelineStateWithDescriptor:error:", id);
   {
      SEL sel = sel_registerName("supportsFamily:"); Method m = class_getInstanceMethod(c, sel);
      if (m) { IMP orig = method_getImplementation(m); method_setImplementation(m, imp_implementationWithBlock(^BOOL(id self, NSInteger f) {
         BOOL r = ((BOOL (*)(id, SEL, NSInteger))orig)(self, sel, f); plog("dev supportsFamily: %ld -> %d", (long)f, r); return r; })); }
   }
}
+ (void)load
{
   plog("load: %s", getprogname());
   if (!strcmp(getprogname(), "WindowServer")) {
      ws_hook_device();
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
         NSArray *all = MTLCopyAllDevices();
         id<MTLDevice> def = MTLCreateSystemDefaultDevice();
         plog("ws later: MTLCopyAllDevices %lu first %s, default %s", (unsigned long)all.count,
              [[all.firstObject name] UTF8String] ?: "-", [[def name] UTF8String] ?: "-");
         ws_state("later");
      });
   }
   ws_state("load");
   // Before SkyLight's check it decides Metal at startup; after it, a client's SLSSetDebugOptions(0x800000d9) switches over.
   if (!strcmp(getprogname(), "WindowServer") && ws_byte(0x1e9d333b4) && !access("/System/Volumes/Data/ak/ws-force-metal", F_OK)) {
      *ws_byte(0x1e9d333b4) = 1;
      uint64_t zero = 0;
      __atomic_compare_exchange_n((volatile uint64_t *)ws_byte(0x1e9d333b8), &zero, ~0ull, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
      ws_state("forced");
   }
}
- (id)initWithAcceleratorPort:(unsigned)port
{
   // Metal allocated self as a bare _MTLDevice; whatever we return instead, ARC releases self and -[_MTLDevice dealloc] would tear down
   // state that was never set up (dispatch_release(NULL)). Keep the tiny shell alive so it is never deallocated.
   CFRetain((__bridge CFTypeRef)self);
   bool ok = port && allowed();
   plog("init: port %u pid %d allowed %d", port, getpid(), ok);
   ws_state("init");
   if (!ok)
      return nil;
   static id device;
   static dispatch_once_t once;
   dispatch_once(&once, ^{
      setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
      NSString *bundle = [[NSBundle bundleForClass:[NativeMaliMetalDevice class]] bundlePath];
      NSString *icd = [bundle stringByAppendingPathComponent:@"libvulkan_panfrost.dylib"];
      uint64_t registryID = 0;
      if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL)) {
         if (auto retain = (int (*)(unsigned))dlsym(iokit, "IOObjectRetain"))
            retain(port);
         if (auto entryID = (int (*)(unsigned, uint64_t *))dlsym(iokit, "IORegistryEntryGetRegistryEntryID"))
            entryID(port, &registryID);
      }
      char cache[512] = {};
      if (!confstr(_CS_DARWIN_USER_CACHE_DIR, cache, sizeof cache - 8) || !cache[0])
         strlcpy(cache, "/tmp/", sizeof cache);
      strlcat(cache, "m2v", sizeof cache);
      // The per-user cache folder may not exist yet this early in a boot: create every level, and fall back to /tmp.
      for (char *p = cache + 1; *p; p++)
         if (*p == '/') { *p = 0; mkdir(cache, 0700); *p = '/'; }
      mkdir(cache, 0700);
      struct stat st;
      if (stat(cache, &st) || !S_ISDIR(st.st_mode) || access(cache, W_OK)) {
         snprintf(cache, sizeof cache, "/tmp/m2v-%u", getuid());
         mkdir(cache, 0700);
      }
      plog("open: icd %s cache %s registryID %llu port %u", icd.UTF8String, cache, registryID, port);
      @try {
         device = native_bridge_metal_open(icd.UTF8String, cache, registryID, missing);
      } @catch (NSException *e) {
         plog("exception: %s", e.description.UTF8String);
         device = nil;
      }
      plog("device: %s", device ? [[device name] UTF8String] : "(none)");
      // _MTLDevice keeps the IOAccelerator service in state its own init sets up, which this device never runs: answer it here.
      // CoreDisplay ties each display to the accelerator behind [device acceleratorPort]; with 0 the display has none and
      // SkyLight composites it in software.
      if (device) {
         unsigned service = port;
         class_replaceMethod(object_getClass(device), sel_registerName("acceleratorPort"),
                             imp_implementationWithBlock(^unsigned(id) { return service; }), "I16@0:8");
         plog("acceleratorPort: %u", ((unsigned (*)(id, SEL))objc_msgSend)(device, sel_registerName("acceleratorPort")));
      }
      if (!device)
         plog("error: %s", nmb_last_error());
   });
   return device;
}
@end
