#include <stdio.h>
#include <stdint.h>
extern int SLSGetDebugOptions(uint32_t *opts);
extern int SLSMainConnectionID(void);
int main(void) {
    printf("cid %d\n", SLSMainConnectionID());
    uint32_t o = 0xdeadbeef;
    int e = SLSGetDebugOptions(&o);
    printf("SLSGetDebugOptions err %d opts 0x%08x\n", e, o);
    return 0;
}
