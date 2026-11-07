// metaldiscover: Metal's own discovery (no frontend linked here): MTLCopyAllDevices / MTLCreateSystemDefaultDevice, then a cleared target.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <mach/mach.h>
#include <sys/ucontext.h>

static void where(const char *tag, uintptr_t a)
{
   Dl_info di = {};
   a &= 0x0000007fffffffffull;
   if (dladdr((void *)a, &di) && di.dli_fname)
      fprintf(stderr, "  %s %#lx %s+%#lx (%s+%#lx)\n", tag, a, strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
              a - (uintptr_t)di.dli_fbase, di.dli_sname ? di.dli_sname : "?", di.dli_saddr ? a - (uintptr_t)di.dli_saddr : 0);
   else
      fprintf(stderr, "  %s %#lx\n", tag, a);
}

static void on_crash(int sig, siginfo_t *si, void *uc)
{
   _STRUCT_ARM_THREAD_STATE64 *ss = &((ucontext_t *)uc)->uc_mcontext->__ss;
   fprintf(stderr, "CRASH sig %d addr %p thread %u\n", sig, si->si_addr, (unsigned)mach_thread_self());
   where("pc", (uintptr_t)__darwin_arm_thread_state64_get_pc(*ss));
   where("lr", (uintptr_t)__darwin_arm_thread_state64_get_lr(*ss));
   for (int i = 0; i < 29; i++)
      fprintf(stderr, "  x%d=%#llx%s", i, ss->__x[i], i % 4 == 3 ? "\n" : "");
   fprintf(stderr, "\n  fp=%#llx sp=%#llx\n", (unsigned long long)__darwin_arm_thread_state64_get_fp(*ss), (unsigned long long)__darwin_arm_thread_state64_get_sp(*ss));
   uintptr_t *fp = (uintptr_t *)(__darwin_arm_thread_state64_get_fp(*ss) & 0x0000007fffffffffull);
   for (int i = 0; i < 40 && fp && ((uintptr_t)fp & 7) == 0; i++) {
      where("frame", fp[1]);
      uintptr_t n = fp[0] & 0x0000007fffffffffull;
      if (n <= (uintptr_t)fp)
         break;
      fp = (uintptr_t *)n;
   }
   _exit(128 + sig);
}

int main(void)
{
   @autoreleasepool {
      setvbuf(stdout, NULL, _IONBF, 0);
      static char altstack[65536];
      stack_t st = {altstack, sizeof altstack, 0};
      sigaltstack(&st, NULL);
      struct sigaction sa = {};
      sa.sa_sigaction = on_crash;
      sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
      sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGTRAP, &sa, NULL);
      NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
      printf("MTLCopyAllDevices: %lu device(s)\n", (unsigned long)all.count);
      for (id<MTLDevice> d in all)
         printf("  %s (registryID %llu)\n", d.name.UTF8String, d.registryID);
      id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
      printf("MTLCreateSystemDefaultDevice: %s\n", dev ? dev.name.UTF8String : "nil");
      if (!dev)
         return 1;
      id<MTLCommandQueue> queue = [dev newCommandQueue];
      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
      td.usage = MTLTextureUsageRenderTarget;
      td.storageMode = MTLStorageModeShared;
      id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
      MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
      rp.colorAttachments[0].texture = tex;
      rp.colorAttachments[0].loadAction = MTLLoadActionClear;
      rp.colorAttachments[0].storeAction = MTLStoreActionStore;
      rp.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.5, 0.0, 1.0);
      id<MTLCommandBuffer> cb = [queue commandBuffer];
      [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
      uint8_t px[4];
      [tex getBytes:px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(10, 10, 1, 1) mipmapLevel:0];
      printf("pixel BGRA = %u,%u,%u,%u (want 0,128,255,255)\n", px[0], px[1], px[2], px[3]);
      int ok = px[2] > 250 && px[1] > 120 && px[1] < 135 && px[0] < 5;
      printf(ok ? "METAL DISCOVERY PASSED: the system's Metal found the Mali through IOAccelerator and the plugin\n" : "METAL DISCOVERY FAILED\n");
      return !ok;
   }
}
