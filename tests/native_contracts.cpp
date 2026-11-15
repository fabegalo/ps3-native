#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include "vm.h"
#include "spu_interp.h"
#include "spu_workload.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <string>
extern "C" {
uint8_t* vm_base = nullptr;
void ppu_unlifted_stub(uint64_t, ppu_context*);
void ps3_hle_call(uint32_t, ppu_context*);
void ps3_indirect_call(ppu_context*);
void lv2_syscall(ppu_context*);
void ppu_sysprx_register(void);
void lv2_init_syscalls(void);
uint32_t ps3_process_spawn_callback(unsigned);
extern uint32_t g_ppu_main_stack_addr, g_ppu_main_stack_size;
extern const char* ppu_vfs_root;
void ppu_fs_register(void);
}
static void call(ppu_context& ctx, const char* name, uint32_t a0) {
    ctx.gpr[3] = a0;
    ps3_hle_call(ps3_compute_nid(name), &ctx);
}
int main(int argc, char** argv) {
    ppu_context ctx{};
    if (argc >= 2 && !strncmp(argv[1], "fs", 2)) {
        if (!strcmp(argv[1], "fs_not_ready")) {
            call(ctx, "cellSysmoduleLoadModule", 0xE);
            return 1;
        }
        if (argc != 3 || vm_init() != 0) return 1;
        ctx.gpr[1] = 0xD00FFFF0;
        ppu_vfs_root = argv[2];
        ppu_fs_register();
        call(ctx, "cellSysmoduleIsLoaded", 0xE);
        if ((int64_t)ctx.gpr[3] >= 0) return 18;
        call(ctx, "cellSysmoduleLoadModule", 0xE);
        if (ctx.gpr[3]) return 19;
        call(ctx, "cellSysmoduleIsLoaded", 0xE);
        if (ctx.gpr[3]) return 20;
        std::string path = !strcmp(argv[1], "fs_traversal") ? "/dev_bdvd/../outside.txt" :
                           !strcmp(argv[1], "fs_edat") ? "/dev_bdvd/PS3_GAME/protected.edat" : "/dev_bdvd/PS3_GAME/own.txt";
        std::memcpy(vm_base + 0x30000, path.c_str(), path.size() + 1);
        ctx.gpr[4] = !strcmp(argv[1], "fs_write") ? 1 : 0;
        ctx.gpr[5] = 0x31000;
        call(ctx, "cellFsOpen", 0x30000);
        if (strcmp(argv[1], "fs") != 0) return 1;
        if (ctx.gpr[3]) return 21;
        auto fd = vm_read32(0x31000);
        ctx.gpr[4] = 0x32000; ctx.gpr[5] = 64; ctx.gpr[6] = 0x31010;
        call(ctx, "cellFsRead", fd);
        if (ctx.gpr[3] || vm_read64(0x31010) != 13 || std::memcmp(vm_base + 0x32000, "own-fixture\0!", 13)) return 22;
        call(ctx, "cellFsClose", fd);
        if (ctx.gpr[3]) return 23;
        call(ctx, "cellFsClose", fd);
        if ((int64_t)ctx.gpr[3] >= 0) return 24;
        vm_shutdown();
        std::puts("Verified typed HLE FS activation, real reads/counts and closed-handle error");
        return 0;
    }
    if (argc == 2) {
        if (!strcmp(argv[1], "hle")) ps3_hle_call(0xDEADBEEFu, &ctx);
        if (!strcmp(argv[1], "ppu")) ppu_unlifted_stub(0x12340, &ctx);
        if (!strcmp(argv[1], "syscall")) { ctx.gpr[11] = 0xFFFF; lv2_syscall(&ctx); }
        if (!strcmp(argv[1], "unaudited_syscall")) { ctx.gpr[11] = 1; lv2_syscall(&ctx); }
        if (!strcmp(argv[1], "indirect")) { ctx.ctr = 0x12340; ps3_indirect_call(&ctx); }
        if (!strcmp(argv[1], "spu")) { static spu_context spu{}; spu_interp_run(&spu, 0x100); }
        if (!strcmp(argv[1], "spu_indirect")) { static spu_context spu{}; spu.pc = 0x12340; spu_indirect_branch(&spu); }
        const uint8_t unprepared[64] = {1, 2, 3, 4};
        if (!strcmp(argv[1], "spu_missing")) spu_workload_dispatch(unprepared, sizeof(unprepared), 0);
        if (!strcmp(argv[1], "spu_missing_async")) spu_workload_dispatch_async(unprepared, sizeof(unprepared), 0);
        if (!strcmp(argv[1], "spu_missing_job")) spu_workload_dispatch_job(unprepared, sizeof(unprepared), 0, 0);
        return 1; // Every unsupported path must terminate before reaching here.
    }
    if (vm_init() != 0) return 1;
    ctx.gpr[1] = 0xD00FFFF0;
    ppu_sysprx_register();
    lv2_init_syscalls();
    ctx.gpr[11] = 147; lv2_syscall(&ctx);
    if (ctx.gpr[3] != 79800000) return 11;
    auto wall_before = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    ctx.gpr[11] = 145; ctx.gpr[3] = 0x26000; ctx.gpr[4] = 0x26008; lv2_syscall(&ctx);
    auto wall_after = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (ctx.gpr[3] != 0 || vm_read64(0x26000) < (uint64_t)wall_before ||
        vm_read64(0x26000) > (uint64_t)wall_after || vm_read64(0x26008) >= 1000000000) return 12;
    for (unsigned a = 0x27000; a < 0x27014; a += 4) vm_write32(a, 0xA5A5A5A5);
    ctx.gpr[11] = 144; ctx.gpr[3] = 0x27004; ctx.gpr[4] = 0x2700C; lv2_syscall(&ctx);
    auto zone = (int32_t)vm_read32(0x27004), dst = (int32_t)vm_read32(0x2700C);
    if (ctx.gpr[3] != 0 || zone < -1440 || zone > 1440 || dst < -180 || dst > 180 ||
        vm_read32(0x27000) != 0xA5A5A5A5 || vm_read32(0x27008) != 0xA5A5A5A5 || vm_read32(0x27010) != 0xA5A5A5A5) return 13;
    ctx.gpr[3] = 0; ctx.gpr[4] = 0x2700C; lv2_syscall(&ctx);
    if (ctx.gpr[3] == 0) return 14;
    g_ppu_main_stack_addr = 0xD0100000; g_ppu_main_stack_size = 0x20000;
    ctx.gpr[11] = 49; ctx.gpr[3] = 0x28000; lv2_syscall(&ctx);
    if (ctx.gpr[3] != 0 || vm_read32(0x28000) != 0xD0100000 || vm_read32(0x28004) != 0x20000) return 15;
    ctx.gpr[3] = 0; lv2_syscall(&ctx);
    if (ctx.gpr[3] == 0) return 16;
    ctx.gpr[3] = 0xFFFFFFFC; lv2_syscall(&ctx);
    if (ctx.gpr[3] == 0) return 17;
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    auto available = vm_read32(0x25004);
    ctx.gpr[11] = 348; ctx.gpr[3] = 0x10000; ctx.gpr[4] = 0x200; ctx.gpr[5] = 0x25010; lv2_syscall(&ctx);
    if (ctx.gpr[3] != 0) return 6;
    auto allocation = vm_read32(0x25010);
    if (!allocation || vm_read32(allocation) != 0 || vm_read32(allocation + 0xFFFC) != 0) return 7;
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    if (vm_read32(0x25004) != available - 0x10000) return 8;
    ctx.gpr[11] = 349; ctx.gpr[3] = allocation; lv2_syscall(&ctx);
    if (ctx.gpr[3] != 0) return 9;
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    if (vm_read32(0x25004) != available) return 10;
    ctx.thread_id = 17;
    call(ctx, "sys_ppu_thread_get_id", 0x24000);
    if (vm_read64(0x24000) != 17) return 5;
    call(ctx, "_sys_process_atexitspawn", 0x21000);
    call(ctx, "_sys_process_atexitspawn", 0x22000);
    call(ctx, "_sys_process_at_Exitspawn", 0x23000);
    if (ps3_process_spawn_callback(0) != 0x21000 || ps3_process_spawn_callback(1) != 0x23000) return 2;
    vm_write32(0x21000, 2);
    vm_write32(0x21004, 0x20);
    ctx.gpr[4] = 0x21000;
    call(ctx, "sys_lwmutex_create", 0x20000);
    if (vm_read32(0x20008) != 0x22 || vm_read32(0x20000) != 0) return 3;
    unsigned value = 0;
    auto worker = [&](unsigned id) {
        ppu_context local{}; local.thread_id = id; local.gpr[1] = 0xD0200000 + id * 0x10000;
        for (unsigned i = 0; i < 1000; ++i) {
            call(local, "sys_lwmutex_lock", 0x20000);
            unsigned previous = value;
            std::this_thread::yield();
            value = previous + 1;
            call(local, "sys_lwmutex_unlock", 0x20000);
        }
    };
    std::thread first(worker, 1), second(worker, 2);
    first.join(); second.join();
    call(ctx, "sys_time_get_system_time", 0);
    auto before = ctx.gpr[3];
    call(ctx, "sys_time_get_system_time", 0);
    if (ctx.gpr[3] < before || value != 2000 || vm_read32(0x20000) != 0) return 4;
    vm_shutdown();
    std::puts("Verified first-callback retention, real mutual exclusion and monotonic clock");
    return 0;
}
