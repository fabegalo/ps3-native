#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_overlay_residency.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
/* Full loader supplies the PPU ownership hooks, even in this SPU-only fixture. */
static spu_context ctx;
extern uint64_t ppu_timebase_now(void);
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
    const uint32_t before = (uint32_t)ppu_timebase_now();
    spu_thread_decrementer_init(&ctx, 2);
    const uint32_t after = (uint32_t)ppu_timebase_now();
    if (!ctx.dec_base_ns || (uint32_t)(~ctx.decrementer - before) > (uint32_t)(after - before)) return 6;
    spu_thread_decrementer_init(&ctx, 0);
    if (ctx.decrementer || !ctx.dec_base_ns) return 7;
    const uint8_t own_code[4] = {0x40, 0x80, 0x03, 0x83};
    const spu_overlay_code_segment segment = {0x3000, 4, own_code};
    if (!spu_overlay_register_initial_image(123, &segment, 1)) return 2;
    if (!spu_overlay_register_initial_image(123, &segment, 1)) return 8;
    const uint8_t changed_code[4] = {0x40, 0x80, 0x03, 0x84};
    const spu_overlay_code_segment changed_segment = {0x3000, 4, changed_code};
    if (spu_overlay_register_initial_image(123, &changed_segment, 1)) return 9;
    ctx.image_id = 123;
    if (spu_overlay_bind_initial_image(&ctx)) return 3;
    memcpy(ctx.ls + 0x3000, own_code, 4);
    if (!spu_overlay_bind_initial_image(&ctx)) return 4;
    if (argc > 1 && !strcmp(argv[1], "code_mutation")) {
        spu_ls_write32(&ctx, 0x3000, 0);
        return 5;
    }
    spu_ls_write32(&ctx, 0x3000, 0x40800383);
    spu_ls_write32(&ctx, 0x3010, 0xDEADBEEF);
    spu_recomp_register();
    spu_func_00000000(&ctx);
    int ok = !memcmp(ctx.ls + 0x100, expected, 16) &&
             !memcmp(vm_base + 0x20000, expected, 16) &&
             ctx.status == SPU_STATUS_STOPPED_BY_STOP;
    printf("SPU AOT: real MFC GET + arithmetic + PUT, all 16 bytes %s\n", ok ? "verified" : "FAILED");
    vm_shutdown();
    return !ok;
}
