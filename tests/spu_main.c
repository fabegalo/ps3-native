#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_overlay_residency.h"
#include "spu_workload.h"
#include "spu_interp.h"
#include "spu_bytes.h"
#include "variants/meta.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
/* Full loader supplies the PPU ownership hooks, even in this SPU-only fixture. */
static spu_context ctx;
extern uint64_t ppu_timebase_now(void);
extern void spu_begin_image(int);
extern void spu_overlay_note_get(spu_context*, uint32_t, const uint8_t*, uint32_t);
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
static int continued_after_stop;
static void terminal_leaf(spu_context* context) {
    context->stop_code = 0x102;
    spu_stop(context);
    continued_after_stop = 1;
}
static void terminal_outer(spu_context* context) {
    terminal_leaf(context);
    continued_after_stop = 2;
}
int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "unsupported")) {
        spu_unsupported(0x100, "own-negative-control");
        return 1;
    }
    if (vm_init() != 0) return 1;
    if (argc > 1 && !strncmp(argv[1], "variant", 7)) {
        spu_context_init(&ctx, 0); ctx.image_id = OWN_VARIANT_OWNER_ID;
        memcpy(ctx.ls + 0x100, own_variant_owner, sizeof own_variant_owner);
        if (!spu_overlay_bind_initial_image(&ctx)) return 24;
        for (unsigned cycle = 0; cycle < 3; ++cycle) {
            memcpy(ctx.ls + OWN_VARIANT_ENTRY, cycle == 1 ? own_variant_second : own_variant_first, sizeof own_variant_first);
            ctx.pc = 0x100; ctx.status = SPU_STATUS_RUNNING;
            ctx.gpr[3] = spu_splat_u32(0xDEADBEEF);
            if (cycle == 2 && !strcmp(argv[1], "variant_mutation")) ctx.ls[OWN_VARIANT_ENTRY + 2] ^= 1;
            if (cycle == 2 && !strcmp(argv[1], "variant_unowned")) {
                ctx.image_id = 123; ctx.initial_code_index = 0; ctx.pc = OWN_VARIANT_ENTRY;
            }
            if (cycle == 2 && !strcmp(argv[1], "variant_unbound")) {
                ctx.initial_code_index = 0;
            }
            spu_run_with_halt(spu_indirect_branch, &ctx);
            if (ctx.gpr[3]._u32[0] != (cycle == 1 ? 9u : 7u) || ctx.status != SPU_STATUS_STOPPED_BY_STOP) return 25;
        }
        puts("SPU AOT variants: original lifted caller, warm-cache swaps and native results verified");
        ctx.pc = 0x104; ctx.status = SPU_STATUS_RUNNING;
        spu_run_with_halt(spu_indirect_branch, &ctx);
        if (ctx.status != SPU_STATUS_STOPPED_BY_STOP || ctx.gpr[3]._u32[0] != 7u) return 26;
        vm_shutdown(); return 0;
    }
    const uint8_t input[16] = {0,0,0,41, 0,0,0,9, 0,0,0,99, 0,0,0,0};
    const uint8_t expected[16] = {0,0,0,42, 0,0,0,10, 0,0,0,100, 0,0,0,1};
    memcpy(vm_base + 0x20000, input, 16);
    if (argc > 1) vm_base[0x20003] = 40; /* Deliberate input fault for the negative control. */
    spu_context_init(&ctx, 0);
    if (argc > 1 && !strcmp(argv[1], "stop_unowned")) {
        ctx.stop_code = 0x102;
        spu_stop(&ctx); return 23;
    }
    if (argc > 1 && !strcmp(argv[1], "spurs_service_unprepared")) {
        ctx.image_id = 123;
        ctx.pc = SPURS_TASKSET_PM_SYSCALL_LS;
        spu_ls_write32(&ctx, 0x27C4, SPURS_TASKSET_PM_SYSCALL_LS);
        spu_indirect_branch(&ctx); return 21;
    }
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
    spu_code_enter(&ctx, &segment, 0);
    if (argc > 1 && !strcmp(argv[1], "code_mutation")) {
        spu_ls_write32(&ctx, 0x3000, 0);
        return 5;
    }
    spu_ls_write32(&ctx, 0x3000, 0x40800383);
    spu_ls_write32(&ctx, 0x3010, 0xDEADBEEF);
    const spu_overlay_code_segment streamed = {0, sizeof own_spu_bytes, own_spu_bytes};
    if (!spu_overlay_register_initial_image(124, &streamed, 1)) return 10;
    const uint64_t streamed_fingerprint = spu_workload_fingerprint(own_spu_bytes, sizeof own_spu_bytes);
    if (!spu_overlay_register_initial_payload(124, 0, sizeof own_spu_bytes, streamed_fingerprint) ||
        spu_overlay_register_initial_payload(124, 0, sizeof own_spu_bytes, streamed_fingerprint ^ 1)) return 17;
    spu_begin_image(124); spu_recomp_register(); spu_begin_image(0);
    spu_workload_register_img(spu_workload_fingerprint(own_spu_bytes, sizeof own_spu_bytes), spu_func_00000000, 124, "own streamed image");
    static spu_context split;
    spu_context_init(&split, 1); split.image_id = 123;
    const size_t half = sizeof own_spu_bytes / 2;
    memcpy(split.ls, own_spu_bytes, half);
    spu_overlay_note_get(&split, 0x20000, split.ls, (uint32_t)half);
    if (spu_lifted_lookup(&split, 0)) return 18;
    memcpy(split.ls + half, own_spu_bytes + half, sizeof own_spu_bytes - half);
    split.ls[sizeof own_spu_bytes - 1] ^= 1;
    spu_overlay_note_get(&split, 0x20000 + (uint32_t)half, split.ls + half, (uint32_t)(sizeof own_spu_bytes - half));
    if (spu_lifted_lookup(&split, 0)) return 19;
    split.ls[sizeof own_spu_bytes - 1] ^= 1;
    if (spu_lifted_lookup(&split, 0) != spu_func_00000000) return 20;
    memcpy(ctx.ls, own_spu_bytes, sizeof own_spu_bytes);
    ctx.ls[4] ^= 1;
    spu_overlay_note_get(&ctx, 0x20000, ctx.ls, sizeof own_spu_bytes);
    if (spu_lifted_lookup(&ctx, 0)) return 11;
    ctx.ls[4] ^= 1;
    spu_overlay_note_get(&ctx, 0x20000, ctx.ls, sizeof own_spu_bytes);
    if (spu_lifted_lookup(&ctx, 0) != spu_func_00000000) return 12;
    if (argc > 1 && !strcmp(argv[1], "overlay_mutation")) {
        ctx.image_id = 124;
        spu_code_enter(&ctx, &streamed, 0);
        spu_ls_write32(&ctx, 0, 0); return 13;
    }
    if (argc > 1 && !strcmp(argv[1], "overlay_stale_return")) {
        spu_code_enter(&ctx, &streamed, 0);
        SPU_CODE_SAVE(&ctx);
        spu_code_enter(&ctx, &segment, 0);
        spu_ls_write32(&ctx, 0, 0);
        SPU_CODE_RESTORE(&ctx); return 15;
    }
    spu_ls_write32(&ctx, 0, 0);
    if (spu_lifted_lookup(&ctx, 0)) return 14;
    memcpy(ctx.ls, own_spu_bytes, sizeof own_spu_bytes);
    spu_overlay_note_get(&ctx, 0x20000, ctx.ls, sizeof own_spu_bytes);
    if (spu_lifted_lookup(&ctx, 0) != spu_func_00000000) return 16;
    spu_recomp_register();
    spu_run_with_halt(spu_func_00000000, &ctx);
    int ok = !memcmp(ctx.ls + 0x100, expected, 16) &&
             !memcmp(vm_base + 0x20000, expected, 16) &&
             ctx.status == SPU_STATUS_STOPPED_BY_STOP;
    ctx.status = SPU_STATUS_RUNNING;
    const int halted = spu_run_with_halt(terminal_outer, &ctx);
    if (!halted || continued_after_stop || ctx.status != SPU_STATUS_STOPPED_BY_STOP || ctx.stop_code != 0x102) {
        fprintf(stderr, "terminal stop resumed host callers: halted=%d continued=%d\n", halted, continued_after_stop);
        return 22;
    }
    printf("SPU AOT: real MFC GET + arithmetic + PUT, all 16 bytes %s\n", ok ? "verified" : "FAILED");
    vm_shutdown();
    return !ok;
}
