#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>
#include <ptrauth.h>
int main(void) {
    void *f = dlsym(RTLD_DEFAULT, "SLSGetDebugOptions");
    uint64_t slide = (uint64_t)ptrauth_strip(f, ptrauth_key_function_pointer) - 0x186d4b2d4;
    printf("slide 0x%llx\n", slide);
    printf("metal_lovin %x once %llx default_to_metal %x active %x\n",
        *(volatile uint8_t *)(0x1e9d333b4 + slide), *(volatile uint64_t *)(0x1e9d333b8 + slide),
        *(volatile uint8_t *)(0x1e9d32d18 + slide), *(volatile uint32_t *)(0x1e9d32ccc + slide));
    printf("block insn %08x\n", *(volatile uint32_t *)(0x186f298c8 + slide));
    return 0;
}
