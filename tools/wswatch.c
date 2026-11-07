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

static char ring[24][16384]; static int rhead, rcount;
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
        for (int d = 0; d < 24 && fp; d++) {
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
    int old = ws_pid(0), pid = 0;
    printf("slide %llx, old WindowServer %d, waiting\n", slide, old); fflush(stdout);
    while (!(pid = ws_pid(old))) usleep(500);
    kern_return_t kr = task_read_for_pid(mach_task_self(), pid, &t);
    if (kr) { printf("task port %d: %d\n", pid, kr); return 1; }
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t t0 = mach_absolute_time();
    printf("new WindowServer %d\n", pid); fflush(stdout);
    uint64_t last[4] = {~0ull, ~0ull, ~0ull, ~0ull};
    const uint64_t addr[4] = {0x1e9d333b8, 0x1e9d333b4, 0x1e9d32d18, 0x1e9d32ccc};
    const int sz[4] = {8, 1, 1, 4};
    const char *nm[4] = {"once", "metal_lovin", "default_to_metal", "active_compositor"};
    int samples = 0;
    for (;;) {
        double ms = (mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
        if (ms > (argc > 1 ? atof(argv[1]) : 20000)) break;
        for (int k = 0; k < 4; k++) {
            uint64_t v = rd(addr[k] + slide, sz[k]);
            if (v != last[k]) { if (k == 2 && last[k] == 0) { stacks(ms); dump(); } printf("%.1fms %s = %llx\n", ms, nm[k], v); last[k] = v; fflush(stdout); }
        }
        if (ms > 800 && last[2] == 0) {
            only_main = 1; stacks(ms); samples++; only_main = 0;
        } else usleep(1000);
    }
    printf("done, %d samples\n", samples);
    return 0;
}
