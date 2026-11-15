#include <cstdio>
#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include "cellSysmodule.h"
#include "cellSysutil.h"
#include "cellRtc.h"
#include "cellGame.h"
#include "cellGcmSys.h"
#include "cellVideoOut.h"
#include <filesystem>
#include <cstdlib>
#include <thread>
#include <chrono>
#include "../network/sceNp.h"
#include "cellSpurs.h"
extern "C" s32 _cellSpursAttributeInitialize(CellSpursAttribute*, u32, u32, s32, s32, s32, u8);
extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
extern "C" [[noreturn]] void ps3_native_stop(void);
extern "C" int sys_memory_range_backed(uint32_t, uint32_t);
extern "C" int ps3_sysprx_heap_range_backed(uint32_t, uint32_t);
extern "C" s32 sys_ppu_thread_once(u32, u32);
extern "C" s32 _sys_spu_printf_initialize(u32, u32, u32, u32);
extern "C" s32 _sys_spu_printf_finalize(void);
// Standalone CPU harnesses have no presentation backend.
extern "C" __attribute__((weak)) int ppu_host_video_state(void) { return 0; }
namespace {
void spu_printf_initialize(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)_sys_spu_printf_initialize(
        (u32)ctx->gpr[3], (u32)ctx->gpr[4], (u32)ctx->gpr[5], (u32)ctx->gpr[6]);
}
void spu_printf_finalize(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)_sys_spu_printf_finalize(); }
void thread_once(ppu_context* ctx) {
    const auto control = (u32)ctx->gpr[3], initializer = (u32)ctx->gpr[4];
    if ((control & 3) || !sys_memory_range_backed(control, 4) ||
        !sys_memory_range_backed(initializer, 8) ||
        sys_ppu_thread_once(control, initializer) != 0) {
        std::fprintf(stderr, "[native-blocked] invalid thread-once control or missing initializer dispatcher\n");
        std::fflush(stderr); ps3_native_stop();
    }
}
void load_module(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellSysmoduleLoadModule((uint16_t)ctx->gpr[3]); }
void module_loaded(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellSysmoduleIsLoaded((uint16_t)ctx->gpr[3]); }
void sysutil_register(ppu_context* ctx) {
    auto rc = cellSysutilRegisterCallback((s32)ctx->gpr[3],
        (CellSysutilCallback)(uintptr_t)(uint32_t)ctx->gpr[4], (void*)(uintptr_t)(uint32_t)ctx->gpr[5]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}
void sysutil_unregister(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellSysutilUnregisterCallback((s32)ctx->gpr[3]); }
void sysutil_check(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellSysutilCheckCallback(); }
void rtc_tick(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellRtcGetCurrentTick((CellRtcTick*)(uintptr_t)(uint32_t)ctx->gpr[3]);
}
void np_init(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)sceNpInit((u32)ctx->gpr[3], (void*)(uintptr_t)(uint32_t)ctx->gpr[4]);
}
void np_term(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)sceNpTerm(); }
void np_status(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)sceNpManagerGetStatus((s32*)(uintptr_t)(uint32_t)ctx->gpr[3]);
}
void game_boot(ppu_context* ctx) {
    auto rc = cellGameBootCheck((u32*)(uintptr_t)(u32)ctx->gpr[3], (u32*)(uintptr_t)(u32)ctx->gpr[4],
        (CellGameContentSize*)(uintptr_t)(u32)ctx->gpr[5], (char*)(uintptr_t)(u32)ctx->gpr[6]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}
void game_permit(ppu_context* ctx) {
    if (cellGame_get_hdd_title_id()) {
        std::fprintf(stderr, "[native-blocked] HDD content permission needs an audited writable mount\n");
        std::fflush(stderr); ps3_native_stop();
    }
    ctx->gpr[3] = (uint64_t)(int64_t)cellGameContentPermit((char*)(uintptr_t)(u32)ctx->gpr[3],
        (char*)(uintptr_t)(u32)ctx->gpr[4]);
}
void game_string(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellGameGetParamString((s32)ctx->gpr[3],
        (char*)(uintptr_t)(u32)ctx->gpr[4], (u32)ctx->gpr[5]);
}
void gcm_config(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellGcmGetConfiguration((CellGcmConfig*)(uintptr_t)(u32)ctx->gpr[3]);
}
void gcm_control(ppu_context* ctx) { ctx->gpr[3] = (uintptr_t)cellGcmGetControlRegister(); }
void gcm_pitch(ppu_context* ctx) { ctx->gpr[3] = cellGcmGetTiledPitchSize((u32)ctx->gpr[3]); }
void gcm_flip_mode(ppu_context* ctx) { cellGcmSetFlipMode((u32)ctx->gpr[3]); }
void gcm_vblank_handler(ppu_context* ctx) { cellGcmSetVBlankHandler((CellGcmVBlankHandler)(uintptr_t)(u32)ctx->gpr[3]); }
void gcm_flip_handler(ppu_context* ctx) { cellGcmSetFlipHandler((CellGcmFlipHandler)(uintptr_t)(u32)ctx->gpr[3]); }
void gcm_user_handler(ppu_context* ctx) { cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)(u32)ctx->gpr[3]); }
void gcm_prepare_flip(ppu_context* ctx) {
    const auto context = (u32)ctx->gpr[3];
    if (!sys_memory_range_backed(context, 16) && !ps3_sysprx_heap_range_backed(context, 16)) {
        std::fprintf(stderr, "[native-blocked] unbacked GCM flip context\n");
        std::fflush(stderr); ps3_native_stop();
    }
    ctx->gpr[3] = (uint64_t)(int64_t)cellGcmSetPrepareFlip((void*)(uintptr_t)context, (u32)ctx->gpr[4]);
}
void gcm_graphics_handler(ppu_context* ctx) { cellGcmSetGraphicsHandler((CellGcmGraphicsHandler)(uintptr_t)(u32)ctx->gpr[3]); }
void gcm_label(ppu_context* ctx) { ctx->gpr[3] = (uintptr_t)cellGcmGetLabelAddress((u8)ctx->gpr[3]); }
void gcm_offset(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellGcmAddressToOffset((u32)ctx->gpr[3], (u32*)(uintptr_t)(u32)ctx->gpr[4]);
}
void gcm_map_main(ppu_context* ctx) {
    if (!sys_memory_range_backed((u32)ctx->gpr[3], (u32)ctx->gpr[4])) {
        std::fprintf(stderr, "[native-blocked] RSX IO mapping requires allocated and mapped guest backing\n");
        std::fflush(stderr); ps3_native_stop();
    }
    ctx->gpr[3] = (uint64_t)(int64_t)cellGcmMapMainMemory((u32)ctx->gpr[3], (u32)ctx->gpr[4],
        (u32*)(uintptr_t)(u32)ctx->gpr[5]);
}
void gcm_tile(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellGcmSetTileInfo((u8)ctx->gpr[3], (u8)ctx->gpr[4], (u32)ctx->gpr[5],
        (u32)ctx->gpr[6], (u32)ctx->gpr[7], (u8)ctx->gpr[8], (u16)ctx->gpr[9], (u8)ctx->gpr[10]);
}
void gcm_bind_tile(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellGcmBindTile((u8)ctx->gpr[3]); }
void gcm_unbind_tile(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellGcmUnbindTile((u8)ctx->gpr[3]); }
void gcm_display(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellGcmSetDisplayBuffer((u32)ctx->gpr[3], (u32)ctx->gpr[4], (u32)ctx->gpr[5],
        (u32)ctx->gpr[6], (u32)ctx->gpr[7]);
}
void video_count(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellVideoOutGetNumberOfDevice((u32)ctx->gpr[3]); }
void video_state(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellVideoOutGetState((u32)ctx->gpr[3], (u32)ctx->gpr[4],
        (CellVideoOutState*)(uintptr_t)(u32)ctx->gpr[5]);
}
void video_resolution(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellVideoOutGetResolution((u32)ctx->gpr[3],
        (CellVideoOutResolution*)(uintptr_t)(u32)ctx->gpr[4]);
}
void video_device(ppu_context* ctx) {
    for (unsigned i = 0; i < 100 && ppu_host_video_state() < 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto rc = ppu_host_video_state() == 1
        ? cellVideoOutGetDeviceInfo((u32)ctx->gpr[3], (u32)ctx->gpr[4], (CellVideoOutDeviceInfo*)(uintptr_t)(u32)ctx->gpr[5])
        : CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND;
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}
void video_available(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellVideoOutGetResolutionAvailability((u32)ctx->gpr[3], (u32)ctx->gpr[4],
        (u32)ctx->gpr[5], (u32)ctx->gpr[6]);
}
void video_configure(ppu_context* ctx) {
    if (ctx->gpr[4] && (vm_read8(ctx->gpr[4]) != 2 || vm_read8(ctx->gpr[4] + 1) != 0)) {
        std::fprintf(stderr, "[native-blocked] video mode differs from the native 720p RGB output\n");
        std::fflush(stderr); ps3_native_stop();
    }
    ctx->gpr[3] = (uint64_t)(int64_t)cellVideoOutConfigure((u32)ctx->gpr[3], (CellVideoOutConfiguration*)(uintptr_t)(u32)ctx->gpr[4],
        (void*)(uintptr_t)(u32)ctx->gpr[5], (u32)ctx->gpr[6]);
}
void video_configuration(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellVideoOutGetConfiguration((u32)ctx->gpr[3], (CellVideoOutConfiguration*)(uintptr_t)(u32)ctx->gpr[4],
        (void*)(uintptr_t)(u32)ctx->gpr[5]);
}
void spurs_attribute(ppu_context* ctx) {
    auto rc = _cellSpursAttributeInitialize((CellSpursAttribute*)(uintptr_t)(uint32_t)ctx->gpr[3],
        (u32)ctx->gpr[4], (u32)ctx->gpr[5], (s32)ctx->gpr[6], (s32)ctx->gpr[7], (s32)ctx->gpr[8], (u8)ctx->gpr[9]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}
void spurs_prefix(ppu_context* ctx) {
    auto rc = cellSpursAttributeSetNamePrefix((CellSpursAttribute*)(uintptr_t)(uint32_t)ctx->gpr[3],
        (const char*)(uintptr_t)(uint32_t)ctx->gpr[4], (u32)ctx->gpr[5]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}
void spurs_printf_attribute(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellSpursAttributeEnableSpuPrintfIfAvailable(
        (CellSpursAttribute*)(uintptr_t)(u32)ctx->gpr[3]);
}
void spurs_group_type(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellSpursAttributeSetSpuThreadGroupType(
        (CellSpursAttribute*)(uintptr_t)(u32)ctx->gpr[3], (s32)ctx->gpr[4]);
}
// Typed HLE adapters only; no original PRX is loaded by this registration.
struct RegisterAdapters {
    RegisterAdapters() {
        ps3_hle_register_ctx(ps3_compute_nid("_sys_spu_printf_initialize"), "_sys_spu_printf_initialize", spu_printf_initialize);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_spu_printf_finalize"), "_sys_spu_printf_finalize", spu_printf_finalize);
        ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_once"), "sys_ppu_thread_once", thread_once);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysmoduleLoadModule"), "cellSysmoduleLoadModule", load_module);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysmoduleIsLoaded"), "cellSysmoduleIsLoaded", module_loaded);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysutilRegisterCallback"), "cellSysutilRegisterCallback", sysutil_register);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysutilUnregisterCallback"), "cellSysutilUnregisterCallback", sysutil_unregister);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysutilCheckCallback"), "cellSysutilCheckCallback", sysutil_check);
        ps3_hle_register_ctx(ps3_compute_nid("cellRtcGetCurrentTick"), "cellRtcGetCurrentTick", rtc_tick);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpInit"), "sceNpInit", np_init);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTerm"), "sceNpTerm", np_term);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpManagerGetStatus"), "sceNpManagerGetStatus", np_status);
        ps3_hle_register_ctx(ps3_compute_nid("cellGameBootCheck"), "cellGameBootCheck", game_boot);
        ps3_hle_register_ctx(ps3_compute_nid("cellGameContentPermit"), "cellGameContentPermit", game_permit);
        ps3_hle_register_ctx(ps3_compute_nid("cellGameGetParamString"), "cellGameGetParamString", game_string);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmGetConfiguration"), "cellGcmGetConfiguration", gcm_config);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmGetControlRegister"), "cellGcmGetControlRegister", gcm_control);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmGetTiledPitchSize"), "cellGcmGetTiledPitchSize", gcm_pitch);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmSetFlipMode"), "cellGcmSetFlipMode", gcm_flip_mode);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmSetVBlankHandler"), "cellGcmSetVBlankHandler", gcm_vblank_handler);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmSetFlipHandler"), "cellGcmSetFlipHandler", gcm_flip_handler);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmSetUserHandler"), "cellGcmSetUserHandler", gcm_user_handler);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmSetPrepareFlip"), "cellGcmSetPrepareFlip", gcm_prepare_flip);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmSetGraphicsHandler"), "cellGcmSetGraphicsHandler", gcm_graphics_handler);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmGetLabelAddress"), "cellGcmGetLabelAddress", gcm_label);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmAddressToOffset"), "cellGcmAddressToOffset", gcm_offset);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmMapMainMemory"), "cellGcmMapMainMemory", gcm_map_main);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmSetTileInfo"), "cellGcmSetTileInfo", gcm_tile);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmBindTile"), "cellGcmBindTile", gcm_bind_tile);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmUnbindTile"), "cellGcmUnbindTile", gcm_unbind_tile);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmSetDisplayBuffer"), "cellGcmSetDisplayBuffer", gcm_display);
        ps3_hle_register_ctx(ps3_compute_nid("cellVideoOutGetNumberOfDevice"), "cellVideoOutGetNumberOfDevice", video_count);
        ps3_hle_register_ctx(ps3_compute_nid("cellVideoOutGetState"), "cellVideoOutGetState", video_state);
        ps3_hle_register_ctx(ps3_compute_nid("cellVideoOutGetResolution"), "cellVideoOutGetResolution", video_resolution);
        ps3_hle_register_ctx(ps3_compute_nid("cellVideoOutGetDeviceInfo"), "cellVideoOutGetDeviceInfo", video_device);
        ps3_hle_register_ctx(ps3_compute_nid("cellVideoOutGetResolutionAvailability"), "cellVideoOutGetResolutionAvailability", video_available);
        ps3_hle_register_ctx(ps3_compute_nid("cellVideoOutConfigure"), "cellVideoOutConfigure", video_configure);
        ps3_hle_register_ctx(ps3_compute_nid("cellVideoOutGetConfiguration"), "cellVideoOutGetConfiguration", video_configuration);
        ps3_hle_register_ctx(ps3_compute_nid("_cellSpursAttributeInitialize"), "_cellSpursAttributeInitialize", spurs_attribute);
        ps3_hle_register_ctx(ps3_compute_nid("cellSpursAttributeSetNamePrefix"), "cellSpursAttributeSetNamePrefix", spurs_prefix);
        ps3_hle_register_ctx(ps3_compute_nid("cellSpursAttributeEnableSpuPrintfIfAvailable"), "cellSpursAttributeEnableSpuPrintfIfAvailable", spurs_printf_attribute);
        ps3_hle_register_ctx(ps3_compute_nid("cellSpursAttributeSetSpuThreadGroupType"), "cellSpursAttributeSetSpuThreadGroupType", spurs_group_type);
    }
} registration;
}
extern "C" void ps3_load_prx_modules(void) {
    extern const char* ppu_vfs_root;
    std::filesystem::path root = std::filesystem::u8path(ppu_vfs_root);
#ifdef _WIN32
    if (const wchar_t* configured = _wgetenv(L"PS3_VFS_ROOT")) root = configured;
#endif
    auto path = (root / "PS3_GAME/PARAM.SFO").u8string();
    cellGame_init_from_paramsfo(reinterpret_cast<const char*>(path.c_str()));
    std::fprintf(stderr, "[native] no original PRX modules prepared; unaudited firmware calls will stop\n");
}
