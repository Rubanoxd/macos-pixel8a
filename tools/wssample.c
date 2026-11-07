// Waits for a new WindowServer, then watches SkyLight's compositor statics while it starts up and samples the backtraces of
// its threads whenever the WSSystemCanCompositeWithMetal once is running. Addresses are printed unslid (25F71 cache).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <ptrauth.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_time.h>

extern kern_return_t task_read_for_pid(mach_port_t, int, mach_port_t *);
static uint64_t slide;
static task_t t;

static uint64_t rd(uint64_t a, int n) {
    uint64_t v = 0; mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(t, a, n, (mach_vm_address_t)&v, &got)) return 0xbad;
    return v;
}
static uint64_t strip(uint64_t p) { return (uint64_t)ptrauth_strip((void *)p, ptrauth_key_asia); }
static uint64_t unslid(uint64_t a) { return a >= slide + 0x180000000ull && a < slide + 0x300000000ull ? a - slide : a; }

static int ws_pid(int skip) {
    int pids[4096]; int n = proc_listallpids(pids, sizeof pids) ;
    for (int i = 0; i < n; i++) {
        char name[64] = {0};
        proc_name(pids[i], name, sizeof name);
        if (!strcmp(name, "WindowServer") && pids[i] != skip) return pids[i];
    }
    return 0;
}

static char ring[24][32768]; static int rhead, rcount;
static char *rp; static size_t rleft;
#define OUT(...) do { int _n = snprintf(rp, rleft, __VA_ARGS__); if (_n > 0 && (size_t)_n < rleft) { rp += _n; rleft -= _n; } } while (0)
static int only_main;
static void stacks(double ms) {
    rp = ring[rhead]; rleft = sizeof ring[0]; rp[0] = 0;
    thread_act_array_t th; mach_msg_type_number_t nth;
    if (task_threads(t, &th, &nth)) return;
    for (unsigned i = 0; i < nth; i++) {
        if (only_main && i) { mach_port_deallocate(mach_task_self(), th[i]); continue; }
        arm_thread_state64_t s; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&s, &c)) continue;
        uint64_t pc = strip((uint64_t)s.__opaque_pc), lr = strip((uint64_t)s.__opaque_lr);
        uint64_t fp = strip((uint64_t)s.__opaque_fp);
        OUT("  %.1fms t%u pc %llx lr %llx", ms, i, unslid(pc), unslid(lr));
        for (int d = 0; d < 80 && fp; d++) {
            uint64_t nfp = rd(fp, 8), ret = strip(rd(fp + 8, 8));
            if (ret == 0 || ret == 0xbad) break;
            OUT(" %llx", unslid(ret));
            fp = nfp;
        }
        OUT("\n");
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    rhead = (rhead + 1) % 24; if (rcount < 24) rcount++;
}
static void dump(void) { for (int k = rcount; k > 0; k--) fputs(ring[(rhead - k + 24) % 24], stdout); rcount = 0; }

int main(int argc, char **argv) {
    slide = (uint64_t)ptrauth_strip(dlsym(RTLD_DEFAULT, "SLSGetDebugOptions"), ptrauth_key_function_pointer) - 0x186d4b2d4;
    int pid = ws_pid(0), n = argc > 1 ? atoi(argv[1]) : 20;
    if (task_read_for_pid(mach_task_self(), pid, &t)) return 1;
    for (int k = 0; k < n; k++) { stacks(k); fputs(ring[(rhead + 23) % 24], stdout); usleep(argc > 2 ? atoi(argv[2]) : 50000); }
    return 0;
}
