#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
int main(void) {
    @autoreleasepool {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        printf("timebase %u/%u\n", tb.numer, tb.denom);
        uint64_t t0 = mach_absolute_time();
        id<NSObject> obs = nil;
        NSArray<id<MTLDevice>> *d = MTLCopyAllDevicesWithObserver(&obs, ^(id<MTLDevice> dev, MTLDeviceNotificationName n) {
            printf("observer: %s %s\n", n.UTF8String, dev.name.UTF8String);
        });
        printf("count %lu after %.1f ms\n", (unsigned long)d.count, (mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6);
        for (id<MTLDevice> x in d) printf("  %s headless %d lowPower %d removable %d\n", x.name.UTF8String, x.isHeadless, x.isLowPower, x.isRemovable);
        printf("MTLCopyAllDevices %lu\n", (unsigned long)MTLCopyAllDevices().count);
        dispatch_semaphore_t s = dispatch_semaphore_create(0);
        t0 = mach_absolute_time();
        long r = dispatch_semaphore_wait(s, dispatch_time(0, 2000000000ull));
        printf("2 s wait returned %ld after %.1f ms\n", r, (mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6);
    }
    return 0;
}
