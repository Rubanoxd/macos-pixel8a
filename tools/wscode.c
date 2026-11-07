#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <ptrauth.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
extern kern_return_t task_read_for_pid(mach_port_t, int, mach_port_t *);
int main(int argc, char **argv) {
    uint64_t slide = (uint64_t)ptrauth_strip(dlsym(RTLD_DEFAULT, "SLSGetDebugOptions"), ptrauth_key_function_pointer) - 0x186d4b2d4;
    task_t t; if (task_read_for_pid(mach_task_self(), atoi(argv[1]), &t)) return 1;
    uint64_t a0 = strtoull(argv[2], 0, 16), n = strtoull(argv[3], 0, 0);
    int diffs = 0;
    for (uint64_t a = a0; a < a0 + n; a += 4) {
        uint32_t v = 0; mach_vm_size_t got;
        mach_vm_read_overwrite(t, a + slide, 4, (mach_vm_address_t)&v, &got);
        uint32_t mine = *(volatile uint32_t *)(a + slide);
        if (v != mine) { printf("DIFF %llx ws %08x mine %08x\n", a, v, mine); diffs++; }
    }
    printf("%llu words, %d diffs\n", n / 4, diffs);
    return 0;
}
