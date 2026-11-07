// Scans WindowServer's writable memory for C++ objects whose (PAC-stripped) vtable pointer is one of the given unslid
// addresses, and dumps the first words of each. Usage: wsfind <pid> <unslid vtable+0x10> ... (25F71 cache)
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
    task_t t;
    if (task_read_for_pid(mach_task_self(), atoi(argv[1]), &t)) { printf("no task\n"); return 1; }
    int nv = argc - 2; uint64_t want[16];
    for (int i = 0; i < nv && i < 16; i++) want[i] = strtoull(argv[i + 2], 0, 16) + slide;
    mach_vm_address_t a = 0; mach_vm_size_t sz; natural_t depth = 0;
    static uint64_t buf[1 << 18];
    int hits = 0;
    for (;;) {
        vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(t, &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt)) break;
        if (info.is_submap) { depth++; continue; }
        if ((info.protection & VM_PROT_WRITE) && !(a >= 0x180000000ull && a < 0x300000000ull)) {
            for (mach_vm_address_t p = a; p < a + sz; p += sizeof buf) {
                mach_vm_size_t n = a + sz - p < sizeof buf ? a + sz - p : sizeof buf, got = 0;
                if (mach_vm_read_overwrite(t, p, n, (mach_vm_address_t)buf, &got)) continue;
                for (size_t k = 0; k < got / 8; k++) {
                    uint64_t v = (uint64_t)ptrauth_strip((void *)buf[k], ptrauth_key_asda);
                    for (int i = 0; i < nv; i++) if (v == want[i] && hits < 20) {
                        uint64_t obj = p + k * 8, w[8] = {0}; mach_vm_size_t g;
                        printf("obj %llx vt#%d\n", obj, i);
                        uint64_t offs[] = {0x58, 0x5e0, 0x5e8};
                        for (int o = 0; o < 3; o++) { mach_vm_read_overwrite(t, obj + offs[o], 8, (mach_vm_address_t)&w[o], &g);
                            printf("  +%llx = %llx\n", offs[o], w[o]); }
                        hits++;
                    }
                }
            }
        }
        a += sz;
    }
    printf("hits %d\n", hits);
    return 0;
}
