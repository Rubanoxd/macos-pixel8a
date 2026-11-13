// akita (Generated-by: LLM): asks BrightnessControl.framework what it makes of our backlight service, and sets nits.
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdlib.h>
@interface NSObject (BC)
+ (NSArray *)copyAvailableControls;
- (BOOL)isValid;
- (NSDictionary *)capabilities;
- (BOOL)setNits:(double)nits error:(NSError **)error;
- (double)getNitsWithError:(NSError **)error;
@end
int main(int argc, char **argv) {
  @autoreleasepool {
    if (!dlopen("/System/Library/PrivateFrameworks/BrightnessControl.framework/BrightnessControl", RTLD_NOW)) { printf("dlopen: %s\n", dlerror()); return 1; }
    Class c = NSClassFromString(@"BCAppleBacklightBrtControl");
    printf("class %p\n", (__bridge void *)c);
    NSArray *a = [c copyAvailableControls];
    printf("controls: %lu\n", (unsigned long)a.count);
    for (id ctl in a) {
      printf("valid %d\ncaps %s\n", [ctl isValid], [[[ctl capabilities] description] UTF8String]);
      NSError *e = nil;
      printf("nits now %f\n", [ctl getNitsWithError:&e]);
      if (argc > 1) {
        BOOL ok = [ctl setNits:atof(argv[1]) error:&e];
        printf("setNits %s -> %d %s\n", argv[1], ok, e ? [[e description] UTF8String] : "");
      }
    }
  }
  return 0;
}
