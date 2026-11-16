#include "nonlocal_recomp.h"
#include "vm.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
extern "C" {
uint8_t* vm_base = nullptr;
extern uint32_t g_ppu_main_stack_addr, g_ppu_main_stack_size;
void ppu_register_function(uint64_t, void (*)(ppu_context*));
void ps3_native_call_opd(ppu_context*, uint32_t);
}
static std::atomic<unsigned> resumed_bridge{0};
static void own_opd_jump(ppu_context* ctx) {
    ps3_native_call_opd(ctx, 0xD0103000);
    ++resumed_bridge;
}
int main(int argc, char** argv) {
    if (vm_init() || vm_commit(0xD0100000, 0x20000)) return 1;
    g_ppu_main_stack_addr = 0xD0100000; g_ppu_main_stack_size = 0x20000;
    ppu_context ctx{}; ctx.thread_id = 1; ctx.gpr[1] = 0xD011F000;
    if (argc == 2) {
        const uint32_t pcs[] = {0x1004, 0x1010}; uint64_t stacks[2]{};
        ppu_aot_frame outer(&ctx, pcs, stacks, 2); outer.note(0);
        ctx.lr = 0x1004;
        if (!strcmp(argv[1], "pc")) ctx.lr = 0x1234;
        if (!strcmp(argv[1], "stack")) ctx.gpr[1] -= 16;
        if (!strcmp(argv[1], "unbacked")) ctx.gpr[1] = 0x30000;
        if (!strcmp(argv[1], "thread")) ctx.thread_id = 2;
        if (!strcmp(argv[1], "unvisited")) ctx.lr = pcs[1];
        if (!strcmp(argv[1], "expired")) {
            const uint32_t old_pc = 0x1234; uint64_t old_sp{};
            { ppu_aot_frame old(&ctx, &old_pc, &old_sp, 1); old.note(0); }
            ctx.lr = old_pc;
        }
        if (!strcmp(argv[1], "context")) {
            ppu_context other = ctx; const uint32_t other_pc = 0x2004; uint64_t other_sp{};
            ppu_aot_frame inner(&other, &other_pc, &other_sp, 1); inner.note(0); inner.after(0);
        } else outer.after(0);
        return 2;
    }
    ppu_register_function(0x30300, own_opd_jump);
    ppu_register_function(0x30700, own_nl_func_00030300);
    vm_write32(0xD0103000, 0x30700); vm_write32(0xD0103004, 0xDEAD0000);
    ppu_register_function(0x30100, own_nl_func_00030100);
    ctx.gpr[3] = 0xD0101000; ctx.ctr = 0xCAFEBABE; ctx.lr = 0xFFFF0000;
    own_nl_func_00030500(&ctx);
    if (ctx.gpr[3] || ctx.ctr != 0xCAFEBABE || ctx.lr != 0xFFFF0000 || vm_read64(0xD0101018) != 0x3051C) return 4;
    std::atomic<unsigned> errors{0};
    auto exercise = [&](unsigned thread) {
      ppu_context ctx{}; ctx.thread_id = thread;
      for (unsigned i = 0; i < 1000; ++i) {
        for (unsigned jump = 0; jump < 2; ++jump) {
            const uint64_t stack = thread == 1 ? 0xD011F000 : 0xD010F000;
            ctx.gpr[1] = stack; ctx.gpr[2] = 0x12340000; ctx.gpr[3] = 0xD0101000 + thread * 0x100;
            ctx.gpr[4] = jump; ctx.gpr[14] = 0x1122334455667788ull; ctx.gpr[31] = 0x88776655;
            ctx.lr = 0xFFFF0000;
            own_nl_func_00030000(&ctx);
            if (ctx.gpr[3] != jump || ctx.gpr[1] != stack || ctx.lr != 0xFFFF0000 ||
                ctx.gpr[2] != 0x12340000 || ctx.gpr[14] != 0x1122334455667788ull || ctx.gpr[31] != 0x88776655) ++errors;
        }
      }
    };
    std::thread first(exercise, 1), second(exercise, 2); first.join(); second.join();
    if (errors || resumed_bridge) return 3;
    puts("Own PPU AOT: LR calls preserve CTR; two threads with 1000 normal/nested nonlocal returns each; restored guest state; abandoned frames never resume");
    vm_shutdown(); return 0;
}
