// SPDX-License-Identifier: MIT
// NativeMaliMetal for macOS on the Pixel 8a: Metal's discovery (IOServiceMatching("IOAccelerator") -> MetalPluginName -> this bundle ->
// NSPrincipalClass -> initWithAcceleratorPort:) gets the native Metal frontend's device (UniXNU's NativeBridgeMetalFrontend over metal2vulkan)
// on Mesa PanVK, which reaches the Mali-G715 through darwin-drm and the kernel's MaliService.
// Only the processes named in /System/Volumes/Data/ak/metal-clients (one per line, "*" for all) get a device; every other one gets nil and
// keeps the software path it has without a GPU. Each process logs to /tmp/nativemali-<name>.log.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <ptrauth.h>
#include <mach-o/dyld.h>
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

// The Pixel build may launch the signed WindowServer image from an alternate basename so libmalloc uses its legacy allocator: xzone's
// PAC-protected free list traps on this non-Apple arm64e port. Keep every WindowServer-specific Metal path keyed by its launchd service,
// not only by argv[0]. The service name is supplied by launchd before this bundle loads.
static bool window_server_process(void)
{
   if (!strcmp(getprogname(), "WindowServer"))
      return true;
   const char *service = getenv("XPC_SERVICE_NAME");
   return service && !strcmp(service, "com.apple.WindowServer");
}

static const char *metal_client_name(void)
{
   return window_server_process() ? "WindowServer" : getprogname();
}

static void plog(const char *fmt, ...)
{
   char path[256];
   snprintf(path, sizeof path, "/tmp/nativemali-%s.log", getprogname());
   va_list ap;
   // WindowServer's stderr reaches the console log (sheng-stderr-name=WindowServer on the boot line)
   if (getenv("NATIVEMALI_STDERR") || window_server_process()) {
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
      ok = !strcmp(line, "*") || !strcmp(line, metal_client_name());
   }
   fclose(f);
   return ok;
}

extern "C" const char *nmb_last_error(void);


// akita (Generated-by: LLM, local only): a sampling profiler of WindowServer's main thread, from inside (sample(1) stops the whole
// server and dtrace does not start on this kernel). `sudo -u _windowserver touch /tmp/akita-prof-request`: within 30 s, 10 s of samples
// every ~1 ms (suspend, read the registers, walk the frame pointers on its stack, resume), then the 40 hottest return addresses by
// self and by inclusive count as image+offset (symbolize on the Mac with atos against the unstripped images).
extern "C" pthread_t pthread_main_thread_np(void);   // libsystem_pthread exports it; the header is private
static const char *akita_image(uintptr_t pc, uintptr_t *off) {
   Dl_info info;
   if (!dladdr((const void *)pc, &info) || !info.dli_fname) { *off = pc; return "?"; }
   *off = pc - (uintptr_t)info.dli_fbase;
   const char *slash = strrchr(info.dli_fname, '/');
   return slash ? slash + 1 : info.dli_fname;
}
static void akita_profile_main(unsigned seconds) {
   enum { kSlots = 16384, kDepth = 48, kRaw = 12000 };
   uint64_t *keys = (uint64_t *)calloc(kSlots, sizeof(uint64_t));
   uint32_t *selfs = (uint32_t *)calloc(kSlots, sizeof(uint32_t)), *incl = (uint32_t *)calloc(kSlots, sizeof(uint32_t));
   uint32_t *waitSelf = (uint32_t *)calloc(kSlots, sizeof(uint32_t)), *waitIncl = (uint32_t *)calloc(kSlots, sizeof(uint32_t));
   // akita: every sample's whole stack too, written to /tmp/akita-prof-stacks.txt afterwards (a call tree on the Mac)
   uint64_t *raw = (uint64_t *)calloc((size_t)kRaw * kDepth, sizeof(uint64_t));
   uint8_t *rawN = (uint8_t *)calloc(kRaw, 1), *rawRun = (uint8_t *)calloc(kRaw, 1);
   unsigned rawCount = 0;
   if (!keys || !selfs || !incl || !waitSelf || !waitIncl || !raw || !rawN || !rawRun) { free(keys); free(selfs); free(incl); free(waitSelf); free(waitIncl); free(raw); free(rawN); free(rawRun); return; }
   pthread_t mainThread = pthread_main_thread_np();
   const thread_act_t target = pthread_mach_thread_np(mainThread);
   const uintptr_t stackTop = (uintptr_t)pthread_get_stackaddr_np(mainThread), stackBottom = stackTop - pthread_get_stacksize_np(mainThread);
   mach_timebase_info_data_t tb; mach_timebase_info(&tb);
   const uint64_t end = mach_absolute_time() + (uint64_t)seconds * 1000000000ull * tb.denom / tb.numer;
   unsigned samples = 0, running = 0, lost = 0;
   auto slot = [&](uint64_t pc) -> int {
      uint64_t h = (pc * 0x9e3779b97f4a7c15ull) >> 50;
      for (unsigned i = 0; i < kSlots; ++i, h = (h + 1) & (kSlots - 1)) {
         if (keys[h] == pc) return (int)h;
         if (!keys[h]) { keys[h] = pc; return (int)h; }
      }
      return -1;
   };
   while (mach_absolute_time() < end) {
      usleep(1000);
      thread_basic_info_data_t bi; mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
      if (thread_info(target, THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) != KERN_SUCCESS) { ++lost; continue; }
      ++samples;
      const bool isRunning = bi.run_state == TH_STATE_RUNNING;
      if (thread_suspend(target) != KERN_SUCCESS) { ++lost; continue; }
      arm_thread_state64_t st; mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
      uint64_t frames[kDepth]; unsigned n = 0;
      if (thread_get_state(target, ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
         frames[n++] = (uint64_t)ptrauth_strip((void *)arm_thread_state64_get_pc(st), ptrauth_key_asia);
         frames[n++] = (uint64_t)ptrauth_strip((void *)arm_thread_state64_get_lr(st), ptrauth_key_asia);
         uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(st);
         while (n < kDepth && fp >= stackBottom && fp + 16 <= stackTop && !(fp & 7)) {
            const uintptr_t next = ((const uintptr_t *)fp)[0], ret = ((const uintptr_t *)fp)[1];
            if (!ret) break;
            frames[n++] = (uint64_t)ptrauth_strip((void *)ret, ptrauth_key_asia);
            if (next <= fp) break;
            fp = next;
         }
      }
      thread_resume(target);
      if (!n) { ++lost; continue; }
      if (isRunning) ++running;
      if (rawCount < kRaw) {
         memcpy(raw + (size_t)rawCount * kDepth, frames, n * sizeof(uint64_t));
         rawN[rawCount] = (uint8_t)n; rawRun[rawCount] = isRunning; ++rawCount;
      }
      uint32_t *selfCount = isRunning ? selfs : waitSelf, *inclCount = isRunning ? incl : waitIncl;
      int s0 = slot(frames[0]); if (s0 >= 0) selfCount[s0]++;
      for (unsigned i = 0; i < n; ++i) {
         bool seen = false;
         for (unsigned j = 0; j < i; ++j) if (frames[j] == frames[i]) { seen = true; break; }
         if (seen) continue;
         int si = slot(frames[i]); if (si >= 0) inclCount[si]++;
      }
   }
   plog("profile: %u samples of the main thread, %u running, %u lost", samples, running, lost);
   {
      // the stacks: a table of images (index name), then per sample "R|W idx:off idx:off ..." from the leaf outwards
      enum { kImages = 64 };
      uintptr_t imgBase[kImages] = {}; unsigned images = 0;
      FILE *f = fopen("/tmp/akita-prof-stacks.txt.part", "w");
      int16_t *slotImg = (int16_t *)malloc(kSlots * sizeof(int16_t));
      uint32_t *slotOff = (uint32_t *)malloc(kSlots * sizeof(uint32_t));
      if (f && slotImg && slotOff) {
         for (int i = 0; i < kSlots; ++i) {
            slotImg[i] = -1;
            if (!keys[i]) continue;
            Dl_info info;
            if (!dladdr((const void *)keys[i], &info) || !info.dli_fname) continue;
            unsigned k = 0;
            while (k < images && imgBase[k] != (uintptr_t)info.dli_fbase) ++k;
            if (k == images && images < kImages) {
               imgBase[images++] = (uintptr_t)info.dli_fbase;
               const char *slash = strrchr(info.dli_fname, '/');
               fprintf(f, "image %u %s\n", k, slash ? slash + 1 : info.dli_fname);
            }
            if (k < images) { slotImg[i] = (int16_t)k; slotOff[i] = (uint32_t)(keys[i] - (uintptr_t)info.dli_fbase); }
         }
         for (unsigned s = 0; s < rawCount; ++s) {
            fputc(rawRun[s] ? 'R' : 'W', f);
            for (unsigned i = 0; i < rawN[s]; ++i) {
               int si = slot(raw[(size_t)s * kDepth + i]);
               if (si >= 0 && slotImg[si] >= 0) fprintf(f, " %d:%x", slotImg[si], slotOff[si]);
               else fputs(" ?", f);
            }
            fputc('\n', f);
         }
         fclose(f); f = nullptr;
         rename("/tmp/akita-prof-stacks.txt.part", "/tmp/akita-prof-stacks.txt");
         plog("profile: %u stacks in /tmp/akita-prof-stacks.txt, %u images", rawCount, images);
      }
      if (f) fclose(f);
      free(slotImg); free(slotOff);
   }
   free(raw); free(rawN); free(rawRun);
   static const char *const kPass[4] = {"self", "incl", "wait-self", "wait-incl"};
   for (int pass = 0; pass < 4; ++pass) {
      uint32_t *count = pass == 0 ? selfs : pass == 1 ? incl : pass == 2 ? waitSelf : waitIncl;
      const int top = pass == 1 ? 160 : pass == 3 ? 60 : 40;
      for (int k = 0; k < top; ++k) {
         int best = -1;
         for (int i = 0; i < kSlots; ++i) if (keys[i] && count[i] && (best < 0 || count[i] > count[best])) best = i;
         if (best < 0) break;
         uintptr_t off = 0; const char *img = akita_image((uintptr_t)keys[best], &off);
         plog("profile %s %3d: %5u %s+0x%lx", kPass[pass], k, count[best], img, (unsigned long)off);
         count[best] = 0;
      }
   }
   free(keys); free(selfs); free(incl); free(waitSelf); free(waitIncl);
}

// akita: every thread of the process at once (`/tmp/akita-prof-all-request`): per thread how often it ran (by name), and the hottest
// return addresses of all running samples together, by self and by inclusive count.
static void akita_profile_all(unsigned seconds) {
   enum { kSlots = 32768, kDepth = 40, kThreads = 96, kRaw = 40000 };
   uint64_t *keys = (uint64_t *)calloc(kSlots, sizeof(uint64_t));
   uint32_t *selfs = (uint32_t *)calloc(kSlots, sizeof(uint32_t)), *incl = (uint32_t *)calloc(kSlots, sizeof(uint32_t));
   // akita: the watched threads' every sample, to /tmp/akita-prof-all-stacks.txt ("T<thread> <round> R|W idx:off ...")
   uint64_t *raw = (uint64_t *)calloc((size_t)kRaw * kDepth, sizeof(uint64_t));
   uint8_t *rawN = (uint8_t *)calloc(kRaw, 1), *rawRun = (uint8_t *)calloc(kRaw, 1), *rawThread = (uint8_t *)calloc(kRaw, 1);
   uint32_t *rawRound = (uint32_t *)calloc(kRaw, sizeof(uint32_t));
   unsigned rawCount = 0;
   if (!keys || !selfs || !incl || !raw || !rawN || !rawRun || !rawThread || !rawRound) {
      free(keys); free(selfs); free(incl); free(raw); free(rawN); free(rawRun); free(rawThread); free(rawRound); return;
   }
   struct { thread_act_t port; unsigned runs; char name[48]; bool watched; } seen[kThreads] = {};
   unsigned seenCount = 0, rounds = 0;
   const thread_act_t me = mach_thread_self();
   mach_timebase_info_data_t tb; mach_timebase_info(&tb);
   const uint64_t end = mach_absolute_time() + (uint64_t)seconds * 1000000000ull * tb.denom / tb.numer;
   auto slot = [&](uint64_t pc) -> int {
      uint64_t h = (pc * 0x9e3779b97f4a7c15ull) >> 49;
      for (unsigned i = 0; i < kSlots; ++i, h = (h + 1) & (kSlots - 1)) {
         if (keys[h] == pc) return (int)h;
         if (!keys[h]) { keys[h] = pc; return (int)h; }
      }
      return -1;
   };
   while (mach_absolute_time() < end) {
      usleep(2000);
      thread_act_array_t list = nullptr; mach_msg_type_number_t count = 0;
      if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) continue;
      ++rounds;
      for (mach_msg_type_number_t t = 0; t < count; ++t) {
         const thread_act_t th = list[t];
         thread_basic_info_data_t bi; mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
         // akita: the bridge's threads and the main thread are sampled waiting too (their stacks go to the file below)
         unsigned known = 0;
         while (known < seenCount && seen[known].port != th) ++known;
         const bool watched = known < seenCount && seen[known].watched;
         if (th != me && thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) == KERN_SUCCESS &&
             (bi.run_state == TH_STATE_RUNNING || watched) && thread_suspend(th) == KERN_SUCCESS) {
            const bool isRunning = bi.run_state == TH_STATE_RUNNING;
            arm_thread_state64_t st; mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
            uint64_t frames[kDepth]; unsigned n = 0;
            if (thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
               frames[n++] = (uint64_t)ptrauth_strip((void *)arm_thread_state64_get_pc(st), ptrauth_key_asia);
               frames[n++] = (uint64_t)ptrauth_strip((void *)arm_thread_state64_get_lr(st), ptrauth_key_asia);
               uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(st), sp = (uintptr_t)arm_thread_state64_get_sp(st);
               // frame pointers within 8 MiB above the stack pointer: a thread's stack, without knowing its bounds
               while (n < kDepth && fp >= sp && fp - sp < (8u << 20) && !(fp & 7)) {
                  vm_size_t got = 0; uintptr_t pair[2] = {};
                  if (vm_read_overwrite(mach_task_self(), fp, sizeof pair, (vm_address_t)pair, &got) != KERN_SUCCESS || got != sizeof pair) break;
                  if (!pair[1]) break;
                  frames[n++] = (uint64_t)ptrauth_strip((void *)pair[1], ptrauth_key_asia);
                  if (pair[0] <= fp) break;
                  fp = pair[0];
               }
            }
            thread_resume(th);
            if (n) {
               unsigned k = 0;
               while (k < seenCount && seen[k].port != th) ++k;
               if (k == seenCount && seenCount < kThreads) {
                  seen[k].port = th; ++seenCount;
                  pthread_t pt = pthread_from_mach_thread_np(th);
                  if (!pt || pthread_getname_np(pt, seen[k].name, sizeof seen[k].name) || !seen[k].name[0])
                     snprintf(seen[k].name, sizeof seen[k].name, "thread %u%s", th, th == pthread_mach_thread_np(pthread_main_thread_np()) ? " (main)" : "");
                  seen[k].watched = !strncmp(seen[k].name, "metal2vulkan", 12) || th == pthread_mach_thread_np(pthread_main_thread_np());
               }
               if (k < seenCount && seen[k].watched && rawCount < kRaw) {
                  memcpy(raw + (size_t)rawCount * kDepth, frames, n * sizeof(uint64_t));
                  rawN[rawCount] = (uint8_t)n; rawRun[rawCount] = isRunning; rawThread[rawCount] = (uint8_t)k; rawRound[rawCount] = rounds; ++rawCount;
                  for (unsigned i = 0; i < n; ++i) (void)slot(frames[i]);
               }
               if (isRunning) {
                  if (k < seenCount) seen[k].runs++;
                  int s0 = slot(frames[0]); if (s0 >= 0) selfs[s0]++;
                  for (unsigned i = 0; i < n; ++i) {
                     bool dup = false;
                     for (unsigned j = 0; j < i; ++j) if (frames[j] == frames[i]) { dup = true; break; }
                     if (!dup) { int si = slot(frames[i]); if (si >= 0) incl[si]++; }
                  }
               }
            }
         }
         mach_port_deallocate(mach_task_self(), th);
      }
      vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(thread_act_t));
   }
   plog("profile-all: %u rounds every ~2 ms", rounds);
   if (FILE *f = fopen("/tmp/akita-prof-all-stacks.txt.part", "w")) {
      enum { kImages = 64 };
      uintptr_t imgBase[kImages] = {}; unsigned images = 0;
      int16_t *slotImg = (int16_t *)malloc(kSlots * sizeof(int16_t));
      uint32_t *slotOff = (uint32_t *)malloc(kSlots * sizeof(uint32_t));
      if (slotImg && slotOff) {
         for (unsigned k = 0; k < seenCount; ++k) if (seen[k].watched) fprintf(f, "thread %u %s\n", k, seen[k].name);
         for (int i = 0; i < kSlots; ++i) {
            slotImg[i] = -1;
            Dl_info info;
            if (!keys[i] || !dladdr((const void *)keys[i], &info) || !info.dli_fname) continue;
            unsigned k = 0;
            while (k < images && imgBase[k] != (uintptr_t)info.dli_fbase) ++k;
            if (k == images && images < kImages) {
               imgBase[images++] = (uintptr_t)info.dli_fbase;
               const char *slash = strrchr(info.dli_fname, '/');
               fprintf(f, "image %u %s\n", k, slash ? slash + 1 : info.dli_fname);
            }
            if (k < images) { slotImg[i] = (int16_t)k; slotOff[i] = (uint32_t)(keys[i] - (uintptr_t)info.dli_fbase); }
         }
         for (unsigned s = 0; s < rawCount; ++s) {
            fprintf(f, "T%u %u %c", rawThread[s], rawRound[s], rawRun[s] ? 'R' : 'W');
            for (unsigned i = 0; i < rawN[s]; ++i) {
               int si = slot(raw[(size_t)s * kDepth + i]);
               if (si >= 0 && slotImg[si] >= 0) fprintf(f, " %d:%x", slotImg[si], slotOff[si]); else fputs(" ?", f);
            }
            fputc('\n', f);
         }
      }
      fclose(f);
      free(slotImg); free(slotOff);
      rename("/tmp/akita-prof-all-stacks.txt.part", "/tmp/akita-prof-all-stacks.txt");
      plog("profile-all: %u stacks of the watched threads in /tmp/akita-prof-all-stacks.txt", rawCount);
   }
   free(raw); free(rawN); free(rawRun); free(rawThread); free(rawRound);
   for (unsigned k = 0; k < seenCount; ++k) if (seen[k].runs * 50 >= rounds) plog("profile-all thread %-40s ran in %u%% of rounds", seen[k].name, seen[k].runs * 100 / (rounds ? rounds : 1));
   for (int pass = 0; pass < 2; ++pass) {
      uint32_t *cnt = pass ? incl : selfs;
      for (int k = 0; k < (pass ? 160 : 50); ++k) {
         int best = -1;
         for (int i = 0; i < kSlots; ++i) if (keys[i] && cnt[i] && (best < 0 || cnt[i] > cnt[best])) best = i;
         if (best < 0) break;
         uintptr_t off = 0; const char *img = akita_image((uintptr_t)keys[best], &off);
         plog("profile-all %s %3d: %5u %s+0x%lx", pass ? "incl" : "self", k, cnt[best], img, (unsigned long)off);
         cnt[best] = 0;
      }
   }
   mach_port_deallocate(mach_task_self(), me);
   free(keys); free(selfs); free(incl);
}
static void missing(const char *cls, const char *sel) { plog("missing: [%s %s]", cls, sel); }

@implementation NativeMaliMetalDevice

// WindowServer on 25F71: SkyLight's compositor statics (unslid addresses from the 25F71 shared cache), logged around the plugin's
// own steps; with /System/Volumes/Data/ak/ws-force-metal present, +load marks WSSystemCanCompositeWithMetal as done with a device.
#include <dlfcn.h>
#include <mach-o/dyld.h>
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
   if (!window_server_process() || !ws_byte(0x1e9d333b4))
      return;
   plog("ws %s: once %llx metal_lovin %u default_to_metal %u active %u", tag, *(volatile uint64_t *)ws_byte(0x1e9d333b8),
        *ws_byte(0x1e9d333b4), *ws_byte(0x1e9d32d18), *(volatile uint32_t *)ws_byte(0x1e9d32ccc));
}

// Copy the tree under src into dst (WindowServer cannot spawn cp): directories 0700, files 0600, owner the caller.
#include <dirent.h>
#include <fcntl.h>
static void copy_tree(const char *src, const char *dst, unsigned *copied, unsigned *failed)
{
   DIR *d = opendir(src);
   if (!d) { (*failed)++; return; }
   mkdir(dst, 0700);
   struct dirent *e;
   while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
         continue;
      char s[1024], t[1024];
      snprintf(s, sizeof s, "%s/%s", src, e->d_name);
      snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
      struct stat st;
      if (stat(s, &st)) { (*failed)++; continue; }
      if (S_ISDIR(st.st_mode)) { copy_tree(s, t, copied, failed); continue; }
      int in = open(s, O_RDONLY), out = in < 0 ? -1 : open(t, O_WRONLY | O_CREAT | O_EXCL, 0600);
      bool ok = in >= 0 && out >= 0;
      char buf[65536];
      ssize_t n;
      while (ok && (n = read(in, buf, sizeof buf)) > 0)
         ok = write(out, buf, n) == n;
      if (in >= 0) close(in);
      if (out >= 0) close(out);
      ok ? (*copied)++ : (*failed)++;
   }
   closedir(d);
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
   // Diagnostic: with /System/Volumes/Data/ak/m2v-log-pipelines present, metal2vulkan logs every render VkPipeline it makes
   // (wall time, largest module, the key part that forced it) on stderr, which goes to /tmp/m2v-<process>.log.
   if (!access("/System/Volumes/Data/ak/m2v-log-pipelines", F_OK)) {
      char path[256];
      snprintf(path, sizeof path, "/tmp/m2v-%s.log", getprogname());
      FILE *f = freopen(path, "a", stderr);
      if (f)
         setvbuf(stderr, NULL, _IOLBF, 0);
      setenv("METAL2VULKAN_LOG_PIPELINES", "1", 1);
      setenv("NATIVE_METAL_WARNINGS", "1", 1);   // each kind of bridge/runtime warning, once, on stderr
      // akita: no AFBC in the render server's images unless asked (/tmp/akita-afbc exists): an AFBC image is a little larger than
      // Metal's answer for a heap texture, so every heap texture (Core Animation's blur targets) was a shadow copied in and out of
      // the heap every pass (`held_to_metal`).
      if (window_server_process() && !getenv("PANVK_DEBUG") && access("/tmp/akita-afbc", F_OK) != 0)
         setenv("PANVK_DEBUG", "noafbc", 1);
      const bool windowServer = window_server_process();
      if (windowServer)
         setenv("NATIVE_METAL_DUMP_LIVE", "1", 1);
      plog("pipeline log on: %s (%s)", path, f ? "stderr redirected" : "no redirect");
      // every 30 s: the bridge's pipeline makings (count, total/longest/waited ms, failed) and shader cache hits/misses/writes
      static dispatch_source_t timer; timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
      dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), 30 * NSEC_PER_SEC, NSEC_PER_SEC);
      dispatch_source_set_event_handler(timer, ^{
         if (windowServer && unlink("/tmp/akita-prof-all-request") == 0) {
            plog("profile-all: 10 s of every WindowServer thread");
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ akita_profile_all(10); });
         }
         if (windowServer && unlink("/tmp/akita-prof-request") == 0) {
            plog("profile: 10 s of WindowServer's main thread");
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ akita_profile_main(10); });
         }
         if (windowServer && unlink("/tmp/akita-metal-dump-request") == 0) {
            uint32_t first = native_bridge_metal_dump_next_commands();
            plog("live command buffer dump starting at %u", first);
         }
         uint64_t w[10] = {0};
         if (native_bridge_metal_work_stats(w))
            plog("work: pipelines %llu total %llu ms longest %llu ms waited %llu ms failed %llu | msgs %llu/%llu | cache hit %llu miss %llu write %llu",
                 w[0], w[1] / 1000, w[2] / 1000, w[3] / 1000, w[4], w[5], w[6], w[7], w[8], w[9]);
         // what the bridge refused (problems, their last texts) and the IOSurface/texture formats it turned down
         static char lastStatus[1024];
         char status[1024] = {}, formats[512] = {}, first[512] = {};
         native_bridge_metal_status(status, sizeof status);
         native_bridge_metal_formats(formats, sizeof formats);
         uint64_t problems = native_bridge_metal_first_problem(first, sizeof first);
         if (strcmp(status, lastStatus)) {
            strlcpy(lastStatus, status, sizeof lastStatus);
            plog("status: %s", status);
            plog("formats: %s", formats);
            plog("first problem (%llu): %s", problems, first);
         }
      });
      dispatch_resume(timer);
   }
   if (window_server_process()) {
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
   if (window_server_process() && ws_byte(0x1e9d333b4) && !access("/System/Volumes/Data/ak/ws-force-metal", F_OK)) {
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
      // Akita diagnostic: isolate writes to imported host pages from GPU writes to optimal images.
      if (!access("/System/Library/Extensions/NativeMaliMetal.bundle/m2v-shadow-host-pages", F_OK)) {
         setenv("NATIVE_METAL_SHADOW_HOST_PAGES", "1", 1);
         plog("forcing shadow images for host-page textures");
      }
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
      // A shader cache shipped in the GPU addon (/System/Library/Extensions/m2v-seed, from an earlier boot of the same build): seed
      // an empty per-user cache with it so the first boot does not spend minutes in PanVK's register allocator on the big shaders.
      {
         char v1[600];
         snprintf(v1, sizeof v1, "%s/v1", cache);
         struct stat sst;
         if (stat(v1, &sst) && !stat("/System/Library/Extensions/m2v-seed/v1", &sst)) {
            unsigned copied = 0, failed = 0;
            copy_tree("/System/Library/Extensions/m2v-seed", cache, &copied, &failed);
            plog("seeded shader cache from m2v-seed: %u files, %u failed", copied, failed);
         }
      }
      plog("open: icd %s cache %s registryID %llu port %u", icd.UTF8String, cache, registryID, port);
      @try {
         device = native_bridge_metal_open(icd.UTF8String, cache, registryID, missing);
      } @catch (NSException *e) {
         plog("exception: %s", e.description.UTF8String);
         device = nil;
      }
      plog("device: %s", device ? [[device name] UTF8String] : "(none)");
      if (device && window_server_process() && getenv("NATIVE_METAL_DUMP_LIVE")) {
         for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
            const char *name = _dyld_get_image_name(i);
            if (name && strstr(name, "NativeMaliMetal.bundle/"))
               plog("image %p %s", _dyld_get_image_header(i), name);
         }
      }
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
