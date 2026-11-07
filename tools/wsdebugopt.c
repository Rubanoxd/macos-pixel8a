#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
extern int SLSSetDebugOptions(uint32_t opts);
extern int SLSGetDebugOptions(uint32_t *opts);
int main(int argc, char **argv) {
    uint32_t o = (uint32_t)strtoul(argv[1], 0, 0);
    int e = SLSSetDebugOptions(o);
    uint32_t now = 0; SLSGetDebugOptions(&now);
    printf("SLSSetDebugOptions(0x%x) -> %d, now 0x%x\n", o, e, now);
    return 0;
}
