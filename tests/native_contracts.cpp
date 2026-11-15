#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include "vm.h"
#include "spu_interp.h"
#include "spu_workload.h"
#include "cellSysutil.h"
#include "cellGcmSys.h"
#include "rsx_tiling.h"
#include "ps3emu/guest_call.h"
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
void ps3_load_prx_modules(void);
extern ps3_guest_caller_fn g_ps3_guest_caller;
void cellGcmTickVBlank(void);
void ppu_gcm_pump(void);
int cellGcmOffsetIsDisplay(u32);
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_register_function(uint64_t, void (*)(ppu_context*));
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t ppu_guest_call_ct(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
}
static unsigned callbacks;
static unsigned gcm_callbacks;
static uint64_t callback_tls, callback_tid, callback_toc;
static void native_guest_callback(ppu_context* ctx) {
    callback_tls = ctx->gpr[13]; callback_tid = ctx->thread_id; callback_toc = ctx->gpr[2];
    vm_write64(ctx->gpr[1] - 16, ctx->gpr[3]);
    ctx->gpr[3] += ctx->gpr[4];
}
static int synthetic_video_ready;
extern "C" int ppu_host_video_state(void) { return synthetic_video_ready; }
static void callback(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (opd == 0x12340 && a0 == 0xABCD && a1 == 0x4321 && a2 == 0x24000) callbacks++;
    if (opd == 0x37000 && a0) gcm_callbacks++;
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
        if (!strcmp(argv[1], "fs_boot") || !strcmp(argv[1], "fs_boot_broken")) {
            ps3_load_prx_modules();
            ctx.gpr[4] = 0x30010; ctx.gpr[5] = 0x30020; ctx.gpr[6] = 0x30040;
            call(ctx, "cellGameBootCheck", 0x30000);
            if (!strcmp(argv[1], "fs_boot_broken")) { vm_shutdown(); return (int64_t)ctx.gpr[3] < 0 ? 0 : 94; }
            if (ctx.gpr[3] || vm_read32(0x30000) != 1 || vm_read32(0x30010) != 0 ||
                std::strcmp((char*)vm_base + 0x30040, "TEST00001") ||
                vm_read32(0x30024) != UINT32_MAX) return 52;
            std::printf("[boot-space-kb] %u\n", vm_read32(0x30020));
            ctx.gpr[4] = 0x30300; ctx.gpr[5] = 32;
            call(ctx, "cellGameGetParamString", 100);
            if (ctx.gpr[3] || std::strcmp((char*)vm_base + 0x30300, "TEST00001")) return 54;
            ctx.gpr[4] = 0x30200;
            call(ctx, "cellGameContentPermit", 0x30100);
            if (ctx.gpr[3] || std::strcmp((char*)vm_base + 0x30100, "/dev_bdvd/PS3_GAME") ||
                std::strcmp((char*)vm_base + 0x30200, "/dev_bdvd/PS3_GAME/USRDIR")) return 53;
            vm_shutdown(); return 0;
        }
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
        if (!strcmp(argv[1], "callback_missing")) {
            if (vm_init()) return 1;
            vm_write32(0x38000, 0x12380); vm_write32(0x38004, 0);
            ppu_guest_call(0x38000, 0,0,0,0,0,0,0,0);
        }
        if (!strcmp(argv[1], "callback_ct_missing")) ppu_guest_call_ct(0x12380, 0, 0,0,0,0,0,0,0,0);
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
        if (!strcmp(argv[1], "sysutil_no_dispatcher")) call(ctx, "cellSysutilCheckCallback", 0);
        if (!strcmp(argv[1], "provider_not_registered")) call(ctx, "cellSysmoduleLoadModule", 0x18);
        if (!strcmp(argv[1], "gcm_unaudited")) call(ctx, "cellGcmSetZcull", 0);
        if (!strcmp(argv[1], "tile_compressed")) { ctx.gpr[8] = 10; call(ctx, "cellGcmSetTileInfo", 0); }
        if (!strcmp(argv[1], "io_unbacked")) {
            ctx.gpr[4] = 0x100000; ctx.gpr[5] = 0x20000;
            call(ctx, "cellGcmMapMainMemory", 0x40000000);
        }
        return 1; // Every unsupported path must terminate before reaching here.
    }
    if (vm_init() != 0) return 1;
    ctx.gpr[1] = 0xD00FFFF0;
    ppu_sysprx_register();
    lv2_init_syscalls();
    call(ctx, "sceNpTerm", 0);
    if ((int64_t)ctx.gpr[3] != (int32_t)0x8002aa01u) return 44;
    ctx.gpr[4] = 0x40000;
    call(ctx, "sceNpInit", 0x1000);
    if ((int64_t)ctx.gpr[3] != (int32_t)0x8002aa0Fu) return 45;
    ctx.gpr[4] = 0;
    call(ctx, "sceNpInit", 0x20000);
    if ((int64_t)ctx.gpr[3] != (int32_t)0x8002aa03u) return 46;
    ctx.gpr[4] = 0x40000;
    call(ctx, "sceNpInit", 0x20000);
    if (ctx.gpr[3]) return 47;
    call(ctx, "sceNpInit", 0x20000);
    if ((int64_t)ctx.gpr[3] != (int32_t)0x8002aa02u) return 48;
    call(ctx, "sceNpManagerGetStatus", 0x25000);
    if (ctx.gpr[3] || (int32_t)vm_read32(0x25000) != -1) return 49;
    call(ctx, "sceNpTerm", 0);
    if (ctx.gpr[3]) return 50;
    call(ctx, "sceNpManagerGetStatus", 0x25000);
    if ((int64_t)ctx.gpr[3] != (int32_t)0x8002aa01u) return 51;
    call(ctx, "cellSysmoduleLoadModule", 0x15);
    if (ctx.gpr[3]) return 36;
    ctx.gpr[4] = 0x12340; ctx.gpr[5] = 0x24000;
    call(ctx, "cellSysutilRegisterCallback", 0);
    if (ctx.gpr[3]) return 37;
    g_ps3_guest_caller = callback;
    cellSysutilQueueEvent(0, 0xABCD, 0x4321);
    call(ctx, "cellSysutilCheckCallback", 0);
    if (ctx.gpr[3] || callbacks != 1) return 38;
    call(ctx, "cellSysutilUnregisterCallback", 0);
    cellSysutilQueueEvent(0, 0xABCD, 0x4321);
    call(ctx, "cellSysutilCheckCallback", 0);
    if (ctx.gpr[3] || callbacks != 1) return 39;
    call(ctx, "cellSysutilRegisterCallback", 4);
    if ((int64_t)ctx.gpr[3] >= 0) return 40;
    // Original guest attribute layout, exact size, BE fields, signed errors and non-NUL prefix.
    call(ctx, "cellSysmoduleLoadModule", 0xA);
    if (ctx.gpr[3]) return 30;
    std::memset(vm_base + 0x33000, 0xA5, 520);
    ctx.gpr[4] = 2; ctx.gpr[5] = 0x450001; ctx.gpr[6] = 5;
    ctx.gpr[7] = 100; ctx.gpr[8] = 1001; ctx.gpr[9] = 1;
    call(ctx, "_cellSpursAttributeInitialize", 0x33000);
    if (ctx.gpr[3] || vm_read32(0x33000) != 2 || vm_read32(0x33004) != 0x450001 ||
        vm_read32(0x33008) != 5 || vm_read32(0x3300C) != 100 || vm_read32(0x33010) != 1001 ||
        vm_read8(0x33014) != 1 || vm_read8(0x331FF) || vm_read32(0x33200) != 0xA5A5A5A5) return 31;
    std::memcpy(vm_base + 0x34000, "123456789012345", 15);
    ctx.gpr[4] = 0x34000; ctx.gpr[5] = 15;
    call(ctx, "cellSpursAttributeSetNamePrefix", 0x33000);
    if (ctx.gpr[3] || std::memcmp(vm_base + 0x33015, vm_base + 0x34000, 15) || vm_read32(0x33024) != 15) return 32;
    ctx.gpr[5] = 16;
    call(ctx, "cellSpursAttributeSetNamePrefix", 0x33000);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x33024) != 15) return 33;
    call(ctx, "_cellSpursAttributeInitialize", 0x33001);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x33000) != 2) return 34;
    call(ctx, "_cellSpursAttributeInitialize", 0);
    if ((int64_t)ctx.gpr[3] >= 0) return 35;
    ctx.gpr[11] = 147; lv2_syscall(&ctx);
    if (ctx.gpr[3] != 79800000) return 11;
    auto wall_before = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    ctx.gpr[11] = 145; ctx.gpr[3] = 0x26000; ctx.gpr[4] = 0x26008; lv2_syscall(&ctx);
    auto wall_after = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (ctx.gpr[3] != 0 || vm_read64(0x26000) < (uint64_t)wall_before ||
        vm_read64(0x26000) > (uint64_t)wall_after || vm_read64(0x26008) >= 1000000000) return 12;
    call(ctx, "cellSysmoduleLoadModule", 9);
    if (ctx.gpr[3]) return 41;
    call(ctx, "cellRtcGetCurrentTick", 0x26010);
    auto rtc_seconds = vm_read64(0x26010) / 1000000 - 62135596800ull;
    if (ctx.gpr[3] || rtc_seconds < (uint64_t)wall_before || rtc_seconds > (uint64_t)wall_after + 1) return 42;
    call(ctx, "cellRtcGetCurrentTick", 0);
    if ((int64_t)ctx.gpr[3] >= 0) return 43;
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
    ctx.gpr[11] = 330; ctx.gpr[3] = 0x10000000; ctx.gpr[4] = 0x40F;
    ctx.gpr[5] = 0; ctx.gpr[6] = 0x35000; lv2_syscall(&ctx);
    auto reserved_a = vm_read32(0x35000);
    if (ctx.gpr[3] || !reserved_a || (reserved_a & 0xFFFFFFFu)) return 55;
    ctx.gpr[3] = 0x10000000; ctx.gpr[5] = 0x20000000; ctx.gpr[6] = 0x35004; lv2_syscall(&ctx);
    auto reserved_b = vm_read32(0x35004);
    if (ctx.gpr[3] || !reserved_b || (reserved_b & 0x1FFFFFFFu) ||
        (reserved_a < reserved_b + 0x10000000u && reserved_b < reserved_a + 0x10000000u)) return 56;
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    auto mapper_before = vm_read32(0x25004);
    ctx.gpr[4] = 0x400; ctx.gpr[5] = 0x35100;
    call(ctx, "sys_mmapper_allocate_memory", 0x100000);
    auto shared_id = vm_read32(0x35100);
    if (ctx.gpr[3] || !shared_id) return 81;
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    if (vm_read32(0x25004) != mapper_before - 0x100000) return 82;
    ctx.gpr[4] = shared_id; ctx.gpr[5] = 0xF;
    call(ctx, "sys_mmapper_map_memory", reserved_a);
    if (ctx.gpr[3] || vm_read32(reserved_a) || vm_read32(reserved_a + 0xFFFFC)) return 83;
    vm_write32(reserved_a, 0xAABBCCDD);
    if (vm_read32(reserved_a) != 0xAABBCCDD) return 84;
    ctx.gpr[4] = 0x400; ctx.gpr[5] = 0x35104;
    vm_write32(0x35104, 0xA5A5A5A5);
    call(ctx, "sys_mmapper_allocate_memory", 0x123);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x35104) != 0xA5A5A5A5) return 85;
    call(ctx, "sys_mmapper_allocate_memory", 0x40000000);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x35104) != 0xA5A5A5A5) return 86;
    ctx.gpr[11] = 330; ctx.gpr[4] = 0;
    vm_write32(0x35008, 0xA5A5A5A5); ctx.gpr[3] = 0x1000; ctx.gpr[6] = 0x35008; lv2_syscall(&ctx);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x35008) != 0xA5A5A5A5) return 57;
    ctx.thread_id = 17;
    ctx.gpr[4] = 0x20000; ctx.gpr[5] = 0x100000; ctx.gpr[6] = 0x200000;
    call(ctx, "_cellGcmInitBody", 0x36000);
    auto gcm_context = vm_read32(0x36000);
    if (ctx.gpr[3] || !gcm_context || vm_read32(gcm_context) != 0x200000 ||
        vm_read32(gcm_context + 4) != 0x21FFFC || vm_read32(gcm_context + 8) != 0x200000 ||
        vm_read32(vm_read32(gcm_context + 12)) != VM_HLE_INJECT_BASE + 0x2F00u) return 58;
    vm_write32(0x36004, 0xA5A5A5A5);
    ctx.gpr[4] = 0x20000; ctx.gpr[5] = 0x100000; ctx.gpr[6] = 0x200000;
    call(ctx, "_cellGcmInitBody", 0x36004);
    if ((int64_t)ctx.gpr[3] != (int32_t)CELL_GCM_ERROR_FAILURE || vm_read32(0x36004) != 0xA5A5A5A5) return 59;
    call(ctx, "cellGcmGetConfiguration", 0x36100);
    if (ctx.gpr[3] || vm_read32(0x36100) != 0xC0000000 || vm_read32(0x36104) != 0x200000 ||
        vm_read32(0x3610C) != 0x100000) return 60;
    ctx.gpr[4] = 0x36120; call(ctx, "cellGcmAddressToOffset", 0x200040);
    if (ctx.gpr[3] || vm_read32(0x36120) != 0x40) return 61;
    ctx.gpr[4] = 0x36120; call(ctx, "cellGcmAddressToOffset", 0xC0000100);
    if (ctx.gpr[3] || vm_read32(0x36120) != 0x100) return 62;
    ctx.gpr[4] = 0x36120; call(ctx, "cellGcmAddressToOffset", 0x12000000);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x36120) != 0x100) return 63;
    call(ctx, "cellGcmGetControlRegister", 0);
    if (ctx.gpr[3] != VM_HLE_INJECT_BASE + 0x2000u) return 64;
    call(ctx, "cellGcmGetLabelAddress", 3);
    vm_write32(ctx.gpr[3], 0x12345678);
    if (vm_read32(VM_HLE_INJECT_BASE + 0x30u) != 0x12345678) return 65;
    call(ctx, "cellVideoOutGetNumberOfDevice", 0); if (ctx.gpr[3] != 1) return 66;
    call(ctx, "cellVideoOutGetNumberOfDevice", 1); if (ctx.gpr[3] != 0) return 67;
    ctx.gpr[4] = 0; ctx.gpr[5] = 0x36200; call(ctx, "cellVideoOutGetState", 0);
    if (ctx.gpr[3] || vm_read8(0x36200) || vm_read8(0x36208) != 2 || vm_read8(0x3620B) != 2) return 68;
    ctx.gpr[4] = 0x36300; call(ctx, "cellVideoOutGetResolution", 2);
    if (ctx.gpr[3] || vm_read16(0x36300) != 1280 || vm_read16(0x36302) != 720) return 69;
    ctx.gpr[4] = 1; ctx.gpr[5] = 0x36200; call(ctx, "cellVideoOutGetState", 0);
    if ((int64_t)ctx.gpr[3] >= 0) return 70;
    ctx.gpr[4] = 0; ctx.gpr[5] = 0x36400;
    call(ctx, "cellVideoOutGetDeviceInfo", 0);
    if ((int64_t)ctx.gpr[3] >= 0) return 71;
    synthetic_video_ready = 1; // ABI fixture only; real GPU validation is a separate test.
    std::memset(vm_base + 0x36400, 0xA5, 464);
    call(ctx, "cellVideoOutGetDeviceInfo", 0);
    if (ctx.gpr[3] || vm_read8(0x36404) != 1 || vm_read8(0x36405) != 1 ||
        vm_read8(0x36420) != 2 || vm_read32(0x365C8) != 0xA5A5A5A5) return 72;
    ctx.gpr[4] = 1; ctx.gpr[5] = 0; ctx.gpr[6] = 0;
    call(ctx, "cellVideoOutGetResolutionAvailability", 0); if (ctx.gpr[3]) return 73;
    ctx.gpr[4] = 2; call(ctx, "cellVideoOutGetResolutionAvailability", 0); if (ctx.gpr[3] != 1) return 74;
    std::memset(vm_base + 0x36600, 0, 16);
    vm_write8(0x36600, 2); vm_write8(0x36602, 2); vm_write32(0x3660C, 5120);
    ctx.gpr[4] = 0x36600; call(ctx, "cellVideoOutConfigure", 0); if (ctx.gpr[3]) return 75;
    ctx.gpr[4] = 0x36620; call(ctx, "cellVideoOutGetConfiguration", 0);
    if (ctx.gpr[3] || std::memcmp(vm_base + 0x36600, vm_base + 0x36620, 16)) return 76;
    call(ctx, "cellGcmGetTiledPitchSize", 0x1400); if (ctx.gpr[3] != 0x1400) return 77;
    call(ctx, "cellGcmGetTiledPitchSize", 0x10001); if (ctx.gpr[3]) return 78;
    vm_write32(0x37000, 0x12340); vm_write32(0x37004, 0);
    call(ctx, "cellGcmSetVBlankHandler", 0x37000);
    cellGcmTickVBlank(); ppu_gcm_pump();
    if (gcm_callbacks != 1) return 79;
    call(ctx, "cellGcmSetVBlankHandler", 0);
    cellGcmTickVBlank(); ppu_gcm_pump();
    if (gcm_callbacks != 1) return 80;
    ctx.gpr[4] = 0x100000; ctx.gpr[5] = 0x36700;
    call(ctx, "cellGcmMapMainMemory", reserved_a);
    if (ctx.gpr[3] || !vm_read32(0x36700)) return 87;
    auto mapped_io = vm_read32(0x36700);
    ctx.gpr[4] = 0x36704; call(ctx, "cellGcmAddressToOffset", reserved_a + 16);
    if (ctx.gpr[3] || vm_read32(0x36704) != mapped_io + 16) return 88;
    ctx.gpr[4] = 1; ctx.gpr[5] = 0x100000; ctx.gpr[6] = 0x20000;
    ctx.gpr[7] = 0x200; ctx.gpr[8] = 0; ctx.gpr[9] = 0; ctx.gpr[10] = 1;
    call(ctx, "cellGcmSetTileInfo", 1);
    rsx_tile tile{};
    if (ctx.gpr[3] || rsx_tiling_find(1, 0x100000, 16, &tile)) return 89;
    call(ctx, "cellGcmBindTile", 1);
    if (ctx.gpr[3] || !rsx_tiling_find(1, 0x100000, 16, &tile) || tile.pitch != 0x200 || tile.bank != 1) return 90;
    call(ctx, "cellGcmUnbindTile", 1);
    if (ctx.gpr[3] || rsx_tiling_find(1, 0x100000, 16, &tile)) return 91;
    ctx.gpr[4] = 0x10000; ctx.gpr[5] = 5120; ctx.gpr[6] = 1280; ctx.gpr[7] = 720;
    call(ctx, "cellGcmSetDisplayBuffer", 0);
    if (ctx.gpr[3] || !cellGcmOffsetIsDisplay(0x10000)) return 92;
    call(ctx, "cellGcmSetVBlankHandler", 0x37000);
    cellGcmTickVBlank();
    auto sleep_start = std::chrono::steady_clock::now();
    ctx.gpr[11] = 141; ctx.gpr[3] = 2000; lv2_syscall(&ctx);
    auto slept = std::chrono::steady_clock::now() - sleep_start;
    if (ctx.gpr[3] || gcm_callbacks != 2 || slept < std::chrono::microseconds(2000)) return 93;
    call(ctx, "cellGcmSetVBlankHandler", 0);
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
    ctx.gpr[13] = 0xE001234;
    g_active_ctx = &ctx;
    vm_write32(0x38000, 0x12380); vm_write32(0x38004, 0x12340000);
    ppu_register_function(0x12380, native_guest_callback);
    auto callback_result = ppu_guest_call(0x38000, 10,2,0,0,0,0,0,0);
    if (callback_result != 12 || g_active_ctx != &ctx || callback_tls != ctx.gpr[13] ||
        callback_tid != 17 || callback_toc != 0x12340000) return 95;
    callback_result = ppu_guest_call_ct(0x12380, 0x12340000, 10,3,0,0,0,0,0,0);
    if (callback_result != 13 || g_active_ctx != &ctx || callback_tls != ctx.gpr[13] || callback_tid != 17) return 96;
    g_active_ctx = nullptr;
    vm_shutdown();
    std::puts("Verified first-callback retention, real mutual exclusion and monotonic clock");
    return 0;
}
