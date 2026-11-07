#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
extern kern_return_t task_read_for_pid(mach_port_t, int, mach_port_t *);
int main(int argc, char **argv) {
    task_t t; if (task_read_for_pid(mach_task_self(), atoi(argv[1]), &t)) return 1;
    for (int i = 2; i < argc; i++) {
        uint64_t a = strtoull(argv[i], 0, 16), v = 0; mach_vm_size_t g;
        kern_return_t kr = mach_vm_read_overwrite(t, a, 8, (mach_vm_address_t)&v, &g);
        printf("%llx: %llx%s\n", a, v, kr ? " (read failed)" : "");
    }
    return 0;
}
