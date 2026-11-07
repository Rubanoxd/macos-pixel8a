// Stack scan of WindowServer's main thread: every word on its stack that points into SkyLight/QuartzCore/CoreGraphics text,
// printed unslid, for samples where the thread is inside the CA software rasterizer. Usage: wsstack <pid> <samples>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <ptrauth.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
extern kern_return_t task_read_for_pid(mach_port_t, int, mach_port_t *);
int main(int argc, char **argv) {
    uint64_t slide = (uint64_t)ptrauth_strip(dlsym(RTLD_DEFAULT, "SLSGetDebugOptions"), ptrauth_key_function_pointer) - 0x186d4b2d4;
    task_t t; if (task_read_for_pid(mach_task_self(), atoi(argv[1]), &t)) return 1;
    static uint64_t stk[32768];
    for (int k = 0, shown = 0; k < atoi(argv[2]) && shown < 3; k++) {
        thread_act_array_t th; mach_msg_type_number_t nth;
        if (task_threads(t, &th, &nth)) return 1;
        arm_thread_state64_t s; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
        thread_get_state(th[0], ARM_THREAD_STATE64, (thread_state_t)&s, &c);
        for (unsigned i = 0; i < nth; i++) mach_port_deallocate(mach_task_self(), th[i]);
        uint64_t pc = (uint64_t)ptrauth_strip((void *)s.__opaque_pc, ptrauth_key_asia) - slide;
        if (!(pc >= 0x18a5dc000 && pc < 0x18aa00000)) { usleep(3000); continue; }   // only while in QuartzCore
        uint64_t sp = (uint64_t)ptrauth_strip((void *)s.__opaque_sp, ptrauth_key_asda); mach_vm_size_t g = 0;
        for (uint64_t off = 0; off < sizeof stk; off += 4096) {   // up to the top of the stack, page by page
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(t, sp + off, 4096, (mach_vm_address_t)((char *)stk + off), &got) || got != 4096) break;
            g = off + 4096;
        }
        printf("sample pc %llx\n", pc);
        uint64_t last = 0;
        for (size_t i = 0; i < g / 8; i++) {
            uint64_t v = (uint64_t)ptrauth_strip((void *)stk[i], ptrauth_key_asia) - slide;
            if (v >= 0x186c00000 && v < 0x18b000000 && v != last) { printf("  %llx\n", v); last = v; }
        }
        shown++;
    }
    return 0;
}
