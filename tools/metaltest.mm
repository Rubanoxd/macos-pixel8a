// metaltest: a Metal device from the native frontend (metal2vulkan over PanVK on the Mali-G715), one cleared render target read back.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "NativeBridgeMetalFrontend.h"

static void missing(const char *cls, const char *sel) { printf("missing: [%s %s]\n", cls, sel); }

int main(int argc, char **argv)
{
   @autoreleasepool {
      setvbuf(stdout, NULL, _IONBF, 0);
      setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
      const char *icd = argc > 1 ? argv[1] : "/System/Volumes/Data/ak/gpu/libvulkan_panfrost.dylib";
      const char *cache = argc > 2 ? argv[2] : "/System/Volumes/Data/ak/gpu/m2v-cache";
      mkdir(cache, 0755);
      id<MTLDevice> dev = native_bridge_metal_open(icd, cache, 0, missing);
      if (!dev) { printf("native_bridge_metal_open failed\n"); return 1; }
      printf("MTLDevice: %s (registryID %llu, unified memory %d)\n", dev.name.UTF8String, dev.registryID, (int)dev.hasUnifiedMemory);
      id<MTLCommandQueue> queue = [dev newCommandQueue];
      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:256 mipmapped:NO];
      td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      td.storageMode = MTLStorageModeShared;
      id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
      printf("texture: %p\n", (__bridge void *)tex);
      MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
      rp.colorAttachments[0].texture = tex;
      rp.colorAttachments[0].loadAction = MTLLoadActionClear;
      rp.colorAttachments[0].storeAction = MTLStoreActionStore;
      rp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.6, 0.9, 1.0);
      id<MTLCommandBuffer> cb = [queue commandBuffer];
      id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
      [enc endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
      printf("command buffer status %ld error %s\n", (long)cb.status, cb.error ? cb.error.description.UTF8String : "none");
      uint8_t px[4 * 256 * 256];
      [tex getBytes:px bytesPerRow:256 * 4 fromRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0];
      printf("pixel (0,0) = %u,%u,%u,%u; (128,128) = %u,%u,%u,%u (want 51,153,230,255)\n", px[0], px[1], px[2], px[3],
             px[(128 * 256 + 128) * 4], px[(128 * 256 + 128) * 4 + 1], px[(128 * 256 + 128) * 4 + 2], px[(128 * 256 + 128) * 4 + 3]);
      int ok = abs(px[0] - 51) < 3 && abs(px[1] - 153) < 3 && abs(px[2] - 230) < 3;
      printf(ok ? "METAL CLEAR PASSED: Metal -> metal2vulkan -> PanVK -> Mali-G715 under macOS\n" : "METAL CLEAR FAILED\n");
      return !ok;
   }
}
