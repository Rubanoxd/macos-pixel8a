#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>
#include <ptrauth.h>
int main(void) {
    void *f = dlsym(RTLD_DEFAULT, "SLSGetDebugOptions");
    uint64_t slide = (uint64_t)ptrauth_strip(f, ptrauth_key_function_pointer) - 0x186d4b2d4;
    for (uint64_t a = 0x186f29940; a < 0x186f299f0; a += 4)
        printf("%llx: %08x\n", a, *(volatile uint32_t *)(a + slide));
    return 0;
}
