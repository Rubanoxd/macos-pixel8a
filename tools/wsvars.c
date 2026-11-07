#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <ptrauth.h>
extern kern_return_t task_read_for_pid(mach_port_t, int, mach_port_t *);
struct v { const char *name; uint64_t addr; int size; };
static const struct v vars[] = {
    {"metal_lovin", 0x1e9d333b4, 1}, {"canComposite_once", 0x1e9d333b8, 8},
    {"default_to_metal(SW)", 0x1e9d32d18, 1}, {"activate_default_once", 0x1e9d32d10, 8},
    {"initialize_compositors_once", 0x1e9d32d00, 8}, {"active_compositor", 0x1e9d32ccc, 4},
    {"gWSDebugIgnoreAcceleration", 0x1e9d32cc8, 1}, {"caws_init_once", 0x1e9d35670, 8},
    {"blk12421.isa", 0x1ee3f8b30, 8}, {"blk12421.flags", 0x1ee3f8b38, 8}, {"blk12421.invoke", 0x1ee3f8b40, 8},
    {"metal_lovin_word", 0x1e9d333b0, 8},
};
int main(int argc, char **argv) {
    printf("self invoke %llx\n", *(volatile uint64_t *)(0x1ee3f8b40 + ((uint64_t)ptrauth_strip(dlsym(RTLD_DEFAULT, "SLSGetDebugOptions"), ptrauth_key_function_pointer) - 0x186d4b2d4)));
    if (argc < 2) return 2;
    void *f = dlsym(RTLD_DEFAULT, "SLSGetDebugOptions");
    uint64_t slide = (uint64_t)ptrauth_strip(f, ptrauth_key_function_pointer) - 0x186d4b2d4;
    printf("slide 0x%llx\n", slide);
    task_t t; kern_return_t kr = task_read_for_pid(mach_task_self(), atoi(argv[1]), &t);
    if (kr) { printf("task port: kr %d %s\n", kr, mach_error_string(kr)); return 1; }
    for (unsigned i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        uint64_t val = 0; mach_vm_size_t n = 0;
        kr = mach_vm_read_overwrite(t, vars[i].addr + slide, vars[i].size, (mach_vm_address_t)&val, &n);
        if (kr) printf("%-28s read: %s\n", vars[i].name, mach_error_string(kr));
        else printf("%-28s 0x%llx\n", vars[i].name, val);
    }
    return 0;
}
