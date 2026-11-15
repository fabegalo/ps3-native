#include "spu_recomp.h"
#include "spu_helpers.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
/* Full loader supplies the PPU ownership hooks, even in this SPU-only fixture. */
static spu_context ctx;
int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "unsupported")) {
        spu_unsupported(0x100, "own-negative-control");
        return 1;
    }
    if (vm_init() != 0) return 1;
    const uint8_t input[16] = {0,0,0,41, 0,0,0,9, 0,0,0,99, 0,0,0,0};
    const uint8_t expected[16] = {0,0,0,42, 0,0,0,10, 0,0,0,100, 0,0,0,1};
    memcpy(vm_base + 0x20000, input, 16);
    if (argc > 1) vm_base[0x20003] = 40; /* Deliberate input fault for the negative control. */
    spu_context_init(&ctx, 0);
    spu_recomp_register();
    spu_func_00000000(&ctx);
    int ok = !memcmp(ctx.ls + 0x100, expected, 16) &&
             !memcmp(vm_base + 0x20000, expected, 16) &&
             ctx.status == SPU_STATUS_STOPPED_BY_STOP;
    printf("SPU AOT: real MFC GET + arithmetic + PUT, all 16 bytes %s\n", ok ? "verified" : "FAILED");
    vm_shutdown();
    return !ok;
}
