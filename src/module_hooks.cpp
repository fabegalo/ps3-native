#include <cstdio>
#include <cstring>
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
#include "../input/cellPad.h"
#include "cellSpurs.h"
#include "native_lle.h"
#include "native_storage.h"
extern "C" s32 _cellSpursAttributeInitialize(CellSpursAttribute*, u32, u32, s32, s32, s32, u8);
extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
extern "C" [[noreturn]] void ps3_native_stop(void);
extern "C" int sys_memory_range_backed(uint32_t, uint32_t);
extern "C" int ps3_native_guest_range_backed(uint32_t, uint32_t);
extern "C" void ppu_write_coherent(u32, const void*, u32);
extern "C" int ps3_sysprx_heap_range_backed(uint32_t, uint32_t);
extern "C" __attribute__((weak)) const NativeLleModule* ps3_native_firmware_definition(void) { return nullptr; }
extern "C" __attribute__((weak)) const NativeLleModule* ps3_native_firmware_extra(size_t) { return nullptr; }
extern "C" s32 sys_ppu_thread_once(u32, u32);
extern "C" s32 sys_process_is_stack(u32);
extern "C" s32 sys_process_get_paramsfo(void*);
extern "C" int cellGameReleaseCheck(void);
extern "C" s32 _sys_spu_printf_initialize(u32, u32, u32, u32);
extern "C" s32 _sys_spu_printf_finalize(void);
extern "C" u32 ps3_spu_printf_callback(u32);
extern "C" void* _sys_memset(void*, s32, u32);
extern "C" void* _sys_memcpy(void*, const void*, u32);
extern "C" s32 _sys_strncmp(const char*, const char*, u32);
extern "C" int cellAudioOutputProbe(void);
extern "C" s32 cellAudioInit(void);
extern "C" s32 cellAudioPortOpen(const void*, u32*);
extern "C" s32 cellAudioGetPortConfig(u32, void*);
extern "C" s32 cellAudioPortStart(u32);
extern "C" s32 cellAudioPortStop(u32);
extern "C" s32 cellAudioPortClose(u32);
extern "C" s32 sceNpTrophyInit(void*, u32, u32, u64);
extern "C" s32 sceNpTrophyTerm(void);
extern "C" s32 cellAudioOutGetSoundAvailability(u32, u32, u32, u32);
extern "C" s32 cellAudioOutConfigure(u32, void*, void*, u32);
extern "C" __attribute__((weak)) int ps3_native_audio_stereo_probe(void) { return cellAudioOutputProbe(); }
// Standalone CPU harnesses have no presentation backend.
extern "C" __attribute__((weak)) int ppu_host_video_state(void) { return 0; }
namespace {
void module_by_name(ppu_context* ctx) {
    if (ctx->gpr[4] || ctx->gpr[5]) { ctx->gpr[3] = (u64)(s64)(s32)0x80010002; return; }
    char name[29]{};
    for (u32 i = 0; i < 28; ++i) {
        const u64 address = (u32)ctx->gpr[3] + (u64)i;
        if (address > UINT32_MAX || !ps3_native_guest_range_backed((u32)address, 1)) {
            ctx->gpr[3] = (u64)(s64)(s32)0x80010002; return;
        }
        name[i] = (char)vm_read8((u32)address);
        if (!name[i]) break;
    }
    if (ps3_native_lle_named(name)) {
        std::fprintf(stderr, "[native-blocked] resident PRX handle query requires audit\n");
        std::fflush(stderr); ps3_native_stop();
    }
    // HLE libraries are not resident original PRX modules and have no PRX handle.
    ctx->gpr[3] = (u64)(s64)(s32)0x8001112E;
}
void memory_set(ppu_context* ctx) {
    ctx->gpr[3] = (u32)(uintptr_t)_sys_memset((void*)(uintptr_t)(u32)ctx->gpr[3], (s32)ctx->gpr[4], (u32)ctx->gpr[5]);
}
void gcm_timestamp(ppu_context* ctx) { ctx->gpr[3] = cellGcmGetTimeStamp((u32)ctx->gpr[3]); }
void audio_availability(ppu_context* ctx) {
    ctx->gpr[3] = (u64)(s64)cellAudioOutGetSoundAvailability((u32)ctx->gpr[3], (u32)ctx->gpr[4], (u32)ctx->gpr[5], (u32)ctx->gpr[6]);
}
void audio_configure(ppu_context* ctx) {
    ctx->gpr[3] = (u64)(s64)cellAudioOutConfigure((u32)ctx->gpr[3], (void*)(uintptr_t)ctx->gpr[4],
        (void*)(uintptr_t)ctx->gpr[5], (u32)ctx->gpr[6]);
}
void audio_initialize(ppu_context* ctx) { ctx->gpr[3] = (u64)(s64)cellAudioInit(); }
void pad_info2(ppu_context* ctx) {
    if (ctx->gpr[3] > UINT32_MAX || !ps3_native_guest_range_backed((u32)ctx->gpr[3], sizeof(CellPadInfo2))) {
        ctx->gpr[3] = (u64)(s64)(s32)CELL_PAD_ERROR_INVALID_PARAMETER; return;
    }
    ctx->gpr[3] = (u64)(s64)cellPadGetInfo2((CellPadInfo2*)(uintptr_t)(u32)ctx->gpr[3]);
}
void pad_setting(ppu_context* ctx) { ctx->gpr[3] = (u64)(s64)cellPadSetPortSetting((u32)ctx->gpr[3], (u32)ctx->gpr[4]); }
void pad_data(ppu_context* ctx) { ctx->gpr[3] = (u64)(s64)cellPadGetData((u32)ctx->gpr[3], (CellPadData*)(uintptr_t)ctx->gpr[4]); }
void pad_actuator(ppu_context* ctx) { ctx->gpr[3] = (u64)(s64)cellPadSetActDirect((u32)ctx->gpr[3], (CellPadActParam*)(uintptr_t)ctx->gpr[4]); }
void audio_port_start(ppu_context* ctx) { ctx->gpr[3] = (u64)(s64)cellAudioPortStart((u32)ctx->gpr[3]); }
void audio_port_stop(ppu_context* ctx) { ctx->gpr[3] = (u64)(s64)cellAudioPortStop((u32)ctx->gpr[3]); }
void audio_port_close(ppu_context* ctx) { ctx->gpr[3] = (u64)(s64)cellAudioPortClose((u32)ctx->gpr[3]); }
void audio_port_open(ppu_context* ctx) {
    if (ctx->gpr[3] > UINT32_MAX || ctx->gpr[4] > UINT32_MAX ||
        !ps3_native_guest_range_backed((u32)ctx->gpr[3], 32) ||
        !ps3_native_guest_range_backed((u32)ctx->gpr[4], 4)) {
        ctx->gpr[3] = (u64)(s64)(s32)CELL_AUDIO_ERROR_PARAM; return;
    }
    if (vm_read64((u32)ctx->gpr[3] + 16)) {
        std::fprintf(stderr, "[native-blocked] audio port attributes require audit\n");
        std::fflush(stderr); ps3_native_stop();
    }
    ctx->gpr[3] = (u64)(s64)cellAudioPortOpen((const void*)(uintptr_t)(u32)ctx->gpr[3],
                                           (u32*)(uintptr_t)(u32)ctx->gpr[4]);
}
void audio_port_config(ppu_context* ctx) {
    if (ctx->gpr[4] > UINT32_MAX || !ps3_native_guest_range_backed((u32)ctx->gpr[4], 32)) {
        ctx->gpr[3] = (u64)(s64)(s32)CELL_AUDIO_ERROR_PARAM; return;
    }
    ctx->gpr[3] = (u64)(s64)cellAudioGetPortConfig((u32)ctx->gpr[3], (void*)(uintptr_t)(u32)ctx->gpr[4]);
}
void trophy_initialize(ppu_context* ctx) {
    ctx->gpr[3] = (u64)(s64)sceNpTrophyInit((void*)(uintptr_t)ctx->gpr[3], (u32)ctx->gpr[4], (u32)ctx->gpr[5], ctx->gpr[6]);
}
void trophy_terminate(ppu_context* ctx) { ctx->gpr[3] = (u64)(s64)sceNpTrophyTerm(); }
void memory_copy(ppu_context* ctx) {
    ctx->gpr[3] = (u32)(uintptr_t)_sys_memcpy((void*)(uintptr_t)(u32)ctx->gpr[3],
        (const void*)(uintptr_t)(u32)ctx->gpr[4], (u32)ctx->gpr[5]);
}
void string_copy(ppu_context* ctx) {
    const auto src = (u32)ctx->gpr[4];
    const auto size = (u32)std::strlen(reinterpret_cast<const char*>(vm_base + src)) + 1;
    ctx->gpr[3] = (u32)(uintptr_t)_sys_memcpy((void*)(uintptr_t)(u32)ctx->gpr[3],
        (const void*)(uintptr_t)src, size);
}
void string_length(ppu_context* ctx) {
    ctx->gpr[3] = std::strlen(reinterpret_cast<const char*>(vm_base + (u32)ctx->gpr[3]));
}
void string_append(ppu_context* ctx) {
    const auto dst = (u32)ctx->gpr[3], src = (u32)ctx->gpr[4];
    const auto prefix = (u32)std::strlen(reinterpret_cast<const char*>(vm_base + dst));
    const auto size = (u32)std::strlen(reinterpret_cast<const char*>(vm_base + src)) + 1;
    _sys_memcpy((void*)(uintptr_t)(dst + prefix), (const void*)(uintptr_t)src, size);
    ctx->gpr[3] = dst;
}
void string_append_bounded(ppu_context* ctx) {
    const auto dst = (u32)ctx->gpr[3], src = (u32)ctx->gpr[4];
    const auto prefix = (u32)std::strlen(reinterpret_cast<const char*>(vm_base + dst));
    u32 size = 0;
    while (size < ctx->gpr[5] && vm_base[src + size]) ++size;
    if (size) _sys_memcpy((void*)(uintptr_t)(dst + prefix), (const void*)(uintptr_t)src, size);
    _sys_memset((void*)(uintptr_t)(dst + prefix + size), 0, 1);
    ctx->gpr[3] = dst;
}
void spu_printf_initialize(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)_sys_spu_printf_initialize(
        (u32)ctx->gpr[3], (u32)ctx->gpr[4], (u32)ctx->gpr[5], (u32)ctx->gpr[6]);
}
void spu_printf_finalize(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)_sys_spu_printf_finalize(); }
template<unsigned Index> void spu_printf_dispatch(ppu_context* ctx) {
    const auto opd = ps3_spu_printf_callback(Index);
    if (!opd) { ctx->gpr[3] = (uint64_t)(int64_t)(s32)0x8001000Fu; return; } // CELL_ESTAT
    ps3_native_call_opd(ctx, opd);
    ctx->gpr[3] = (uint64_t)(int64_t)(s32)ctx->gpr[3];
}
void thread_once(ppu_context* ctx) {
    const auto control = (u32)ctx->gpr[3], initializer = (u32)ctx->gpr[4];
    if ((control & 3) || !sys_memory_range_backed(control, 4) ||
        !sys_memory_range_backed(initializer, 8) ||
        sys_ppu_thread_once(control, initializer) != 0) {
        std::fprintf(stderr, "[native-blocked] invalid thread-once control or missing initializer dispatcher\n");
        std::fflush(stderr); ps3_native_stop();
    }
}
void load_module(ppu_context* ctx) {
    const auto id = (uint16_t)ctx->gpr[3];
    if (id == CELL_SYSMODULE_VDEC_MPEG2 || id == CELL_SYSMODULE_VDEC_AL) {
        // The MPEG-2 module depends on the original generic VDEC client.
        if (!ps3_native_lle_start("libvdec", ctx) ||
            (id == CELL_SYSMODULE_VDEC_MPEG2 && !ps3_native_lle_start("libsmvd2", ctx))) {
            std::fprintf(stderr, "[native-blocked] original video module start unavailable or concurrent\n");
            std::fflush(stderr); ps3_native_stop();
        }
        if (id == CELL_SYSMODULE_VDEC_MPEG2 && cellSysmoduleLoadModule(CELL_SYSMODULE_VDEC_AL) != CELL_OK) {
            std::fprintf(stderr, "[native-blocked] original video dependency activation failed\n");
            std::fflush(stderr); ps3_native_stop();
        }
    }
    if ((uint16_t)ctx->gpr[3] == CELL_SYSMODULE_NET && ps3_native_lle_library("sys_net") && !ps3_native_lle_start("sys_net", ctx)) {
        std::fprintf(stderr, "[native-blocked] original network module start is unavailable or concurrent\n");
        std::fflush(stderr); ps3_native_stop();
    }
    ctx->gpr[3] = (uint64_t)(int64_t)cellSysmoduleLoadModule((uint16_t)ctx->gpr[3]);
}
void module_loaded(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellSysmoduleIsLoaded((uint16_t)ctx->gpr[3]); }
void unload_module(ppu_context* ctx) {
    const auto id = (uint16_t)ctx->gpr[3];
    if (id != CELL_SYSMODULE_SYSUTIL_GAME) {
        std::fprintf(stderr, "[native-blocked] module unload lifecycle requires audit id=0x%04X\n", id);
        std::fflush(stderr); ps3_native_stop();
    }
    if (cellSysmoduleIsLoaded(id) == CELL_OK && !cellGameReleaseCheck()) {
        std::fprintf(stderr, "[native-blocked] game module unload has an active storage transaction\n");
        std::fflush(stderr); ps3_native_stop();
    }
    ctx->gpr[3] = (u64)(s64)cellSysmoduleUnloadModule(id);
}
void sysutil_register(ppu_context* ctx) {
    auto rc = cellSysutilRegisterCallback((s32)ctx->gpr[3],
        (CellSysutilCallback)(uintptr_t)(uint32_t)ctx->gpr[4], (void*)(uintptr_t)(uint32_t)ctx->gpr[5]);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}
void sysutil_unregister(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellSysutilUnregisterCallback((s32)ctx->gpr[3]); }
void sysutil_check(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellSysutilCheckCallback(); }
void sysmodule_initialize(ppu_context* ctx) {
    // The firmware 4.93 export returns success without changing module state.
    ctx->gpr[3] = (u64)(s64)cellSysmoduleInitialize();
}
void rtc_tick(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellRtcGetCurrentTick((CellRtcTick*)(uintptr_t)(uint32_t)ctx->gpr[3]);
}
void rtc_local_time(ppu_context* ctx) {
    const auto address = ctx->gpr[3];
    if (address > UINT32_MAX || !ps3_native_guest_range_backed((u32)address, 16)) {
        ctx->gpr[3] = (u64)(s64)(s32)0x80010602; return;
    }
    CellRtcDateTime date{};
    const s32 result = cellRtcGetCurrentClockLocalTime(&date);
    if (result == 0) {
        const u16 fields[] = {date.year, date.month, date.day, date.hour, date.minute, date.second};
        u8 payload[16];
        for (unsigned i = 0; i < 6; ++i) {
            payload[i * 2] = (u8)(fields[i] >> 8); payload[i * 2 + 1] = (u8)fields[i];
        }
        for (unsigned i = 0; i < 4; ++i) payload[12 + i] = (u8)(date.microsecond >> (24 - i * 8));
        ppu_write_coherent((u32)address, payload, sizeof payload);
    }
    ctx->gpr[3] = (u64)(s64)result;
}
void rtc_weekday(ppu_context* ctx) {
    if (ctx->gpr[3] < 1 || ctx->gpr[3] > 9999 || ctx->gpr[4] < 1 || ctx->gpr[4] > 12 ||
        ctx->gpr[5] < 1 || ctx->gpr[5] > 31) {
        std::fprintf(stderr, "[native-blocked] RTC weekday arguments require calendar audit\n");
        std::fflush(stderr); ps3_native_stop();
    }
    ctx->gpr[3] = (u64)cellRtcGetDayOfWeek((s32)ctx->gpr[3], (s32)ctx->gpr[4], (s32)ctx->gpr[5]);
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
void system_param(ppu_context* ctx) {
    if (ctx->gpr[4] > UINT32_MAX) { ctx->gpr[3] = (u64)(s64)CELL_SYSUTIL_ERROR_VALUE; return; }
    ctx->gpr[3] = (u64)(s64)cellSysutilGetSystemParamInt((s32)ctx->gpr[3], (u32)ctx->gpr[4]);
}
void process_is_stack(ppu_context* ctx) { ctx->gpr[3] = sys_process_is_stack((u32)ctx->gpr[3]); }
void process_paramsfo(ppu_context* ctx) { ctx->gpr[3] = (u64)(s64)sys_process_get_paramsfo((void*)(uintptr_t)ctx->gpr[3]); }
void string_compare(ppu_context* ctx) {
    ctx->gpr[3] = (u64)(s64)_sys_strncmp((const char*)(uintptr_t)ctx->gpr[3],
        (const char*)(uintptr_t)ctx->gpr[4], (u32)ctx->gpr[5]);
}
void game_permit(ppu_context* ctx) {
    if (ctx->gpr[3] > UINT32_MAX || ctx->gpr[4] > UINT32_MAX ||
        !ps3_native_guest_range_backed((u32)ctx->gpr[3], 128) ||
        !ps3_native_guest_range_backed((u32)ctx->gpr[4], 128)) {
        ctx->gpr[3] = (u64)(s64)CELL_GAME_ERROR_PARAM; return;
    }
    if (cellGame_get_hdd_title_id()) {
        std::fprintf(stderr, "[native-blocked] HDD content permission needs an audited writable mount\n");
        std::fflush(stderr); ps3_native_stop();
    }
    ctx->gpr[3] = (uint64_t)(int64_t)cellGameContentPermit((char*)(uintptr_t)(u32)ctx->gpr[3],
        (char*)(uintptr_t)(u32)ctx->gpr[4]);
}
void game_create(ppu_context* ctx) {
    if (ctx->gpr[3] > UINT32_MAX || !ps3_native_guest_range_backed((u32)ctx->gpr[3], sizeof(CellGameSetInitParams))) {
        ctx->gpr[3] = (u64)(s64)CELL_GAME_ERROR_PARAM; return;
    }
    for (unsigned i : {4u, 5u}) if (ctx->gpr[i] > UINT32_MAX ||
        (ctx->gpr[i] && !ps3_native_guest_range_backed((u32)ctx->gpr[i], 128))) {
        ctx->gpr[3] = (u64)(s64)CELL_GAME_ERROR_PARAM; return;
    }
    ctx->gpr[3] = (u64)(s64)cellGameCreateGameData((CellGameSetInitParams*)(uintptr_t)(u32)ctx->gpr[3],
        (char*)(uintptr_t)(u32)ctx->gpr[4], (char*)(uintptr_t)(u32)ctx->gpr[5]);
}
void game_data(ppu_context* ctx) {
    const auto type = (u32)ctx->gpr[3];
    auto invalid = [&] { ctx->gpr[3] = (u64)(s64)CELL_GAME_ERROR_PARAM; };
    if (type < 1 || type > 3 || ctx->gpr[4] > UINT32_MAX || ctx->gpr[5] > UINT32_MAX ||
        (ctx->gpr[5] && !ps3_native_guest_range_backed((u32)ctx->gpr[5], 12))) { invalid(); return; }
    if (type != 3) {
        std::fprintf(stderr, "[native-blocked] unreviewed content-check type\n");
        std::fflush(stderr); ps3_native_stop();
    }
    std::string name;
    for (u32 i = 0; i < CELL_GAME_DIRNAME_SIZE; ++i) {
        const u64 a = ctx->gpr[4] + i;
        if (a > UINT32_MAX || !ps3_native_guest_range_backed((u32)a, 1)) { invalid(); return; }
        const char c = (char)vm_read8(a);
        if (!c) break;
        name += c;
    }
    if (name.empty() || name.size() >= CELL_GAME_DIRNAME_SIZE || name == "." || name == ".." ||
        name.find_first_of("/\\:") != std::string::npos) { invalid(); return; }
    if (!ps3_native_install_validate(name.c_str())) { invalid(); return; }
    ctx->gpr[3] = (u64)(s64)cellGameDataCheck(type, (const char*)(uintptr_t)(u32)ctx->gpr[4],
                                         (CellGameContentSize*)(uintptr_t)(u32)ctx->gpr[5]);
}
void game_string(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellGameGetParamString((s32)ctx->gpr[3],
        (char*)(uintptr_t)(u32)ctx->gpr[4], (u32)ctx->gpr[5]);
}
void game_set_string(ppu_context* ctx) {
    if (ctx->gpr[4] > UINT32_MAX) { ctx->gpr[3] = (u64)(s64)CELL_GAME_ERROR_PARAM; return; }
    for (u32 i = 0; i < 128; ++i) {
        const u64 a = ctx->gpr[4] + i;
        if (a > UINT32_MAX || !ps3_native_guest_range_backed((u32)a, 1)) break;
        if (!vm_read8(a)) {
            ctx->gpr[3] = (u64)(s64)cellGameSetParamString((s32)ctx->gpr[3], (const char*)(uintptr_t)(u32)ctx->gpr[4]);
            return;
        }
    }
    ctx->gpr[3] = (u64)(s64)CELL_GAME_ERROR_PARAM;
}
void gcm_config(ppu_context* ctx) {
    ctx->gpr[3] = (uint64_t)(int64_t)cellGcmGetConfiguration((CellGcmConfig*)(uintptr_t)(u32)ctx->gpr[3]);
}
void gcm_control(ppu_context* ctx) { ctx->gpr[3] = (uintptr_t)cellGcmGetControlRegister(); }
void gcm_pitch(ppu_context* ctx) { ctx->gpr[3] = cellGcmGetTiledPitchSize((u32)ctx->gpr[3]); }
void gcm_flip_mode(ppu_context* ctx) { cellGcmSetFlipMode((u32)ctx->gpr[3]); }
void gcm_reset_flip(ppu_context*) { cellGcmResetFlipStatus(); }
void gcm_flip_status(ppu_context* ctx) { ctx->gpr[3] = cellGcmGetFlipStatus(); }
void gcm_debug_level(ppu_context* ctx) {
    if ((u32)ctx->gpr[3] > CELL_GCM_DEBUG_LEVEL2) {
        std::fprintf(stderr, "[native-blocked] unsupported GCM debug output level\n");
        std::fflush(stderr); ps3_native_stop();
    }
    cellGcmSetDebugOutputLevel((u32)ctx->gpr[3]);
}
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
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmGetTimeStamp"), "cellGcmGetTimeStamp", gcm_timestamp);
        ps3_hle_register_ctx(ps3_compute_nid("cellAudioOutGetSoundAvailability"), "cellAudioOutGetSoundAvailability", audio_availability);
        ps3_hle_register_ctx(ps3_compute_nid("cellAudioOutConfigure"), "cellAudioOutConfigure", audio_configure);
        ps3_hle_register_ctx(ps3_compute_nid("cellAudioInit"), "cellAudioInit", audio_initialize);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyInit"), "sceNpTrophyInit", trophy_initialize);
        ps3_hle_register_ctx(ps3_compute_nid("cellAudioPortOpen"), "cellAudioPortOpen", audio_port_open);
        ps3_hle_register_ctx(ps3_compute_nid("cellPadGetInfo2"), "cellPadGetInfo2", pad_info2);
        ps3_hle_register_ctx(ps3_compute_nid("cellPadSetPortSetting"), "cellPadSetPortSetting", pad_setting);
        ps3_hle_register_ctx(ps3_compute_nid("cellPadGetData"), "cellPadGetData", pad_data);
        ps3_hle_register_ctx(ps3_compute_nid("cellPadSetActDirect"), "cellPadSetActDirect", pad_actuator);
        ps3_hle_register_ctx(ps3_compute_nid("cellAudioGetPortConfig"), "cellAudioGetPortConfig", audio_port_config);
        ps3_hle_register_ctx(ps3_compute_nid("cellAudioPortStart"), "cellAudioPortStart", audio_port_start);
        ps3_hle_register_ctx(ps3_compute_nid("cellAudioPortStop"), "cellAudioPortStop", audio_port_stop);
        ps3_hle_register_ctx(ps3_compute_nid("cellAudioPortClose"), "cellAudioPortClose", audio_port_close);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTrophyTerm"), "sceNpTrophyTerm", trophy_terminate);
        ps3_hle_register_ctx(ps3_compute_nid("sys_prx_get_module_id_by_name"), "sys_prx_get_module_id_by_name", module_by_name);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_memset"), "_sys_memset", memory_set);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_memcpy"), "_sys_memcpy", memory_copy);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_strcpy"), "_sys_strcpy", string_copy);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_strlen"), "_sys_strlen", string_length);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_strcat"), "_sys_strcat", string_append);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_strncat"), "_sys_strncat", string_append_bounded);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_spu_printf_initialize"), "_sys_spu_printf_initialize", spu_printf_initialize);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_spu_printf_finalize"), "_sys_spu_printf_finalize", spu_printf_finalize);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_spu_printf_attach_group"), "_sys_spu_printf_attach_group", spu_printf_dispatch<0>);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_spu_printf_detach_group"), "_sys_spu_printf_detach_group", spu_printf_dispatch<1>);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_spu_printf_attach_thread"), "_sys_spu_printf_attach_thread", spu_printf_dispatch<2>);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_spu_printf_detach_thread"), "_sys_spu_printf_detach_thread", spu_printf_dispatch<3>);
        ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_once"), "sys_ppu_thread_once", thread_once);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysmoduleLoadModule"), "cellSysmoduleLoadModule", load_module);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysmoduleUnloadModule"), "cellSysmoduleUnloadModule", unload_module);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysmoduleInitialize"), "cellSysmoduleInitialize", sysmodule_initialize);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysmoduleIsLoaded"), "cellSysmoduleIsLoaded", module_loaded);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysutilRegisterCallback"), "cellSysutilRegisterCallback", sysutil_register);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysutilUnregisterCallback"), "cellSysutilUnregisterCallback", sysutil_unregister);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysutilCheckCallback"), "cellSysutilCheckCallback", sysutil_check);
        ps3_hle_register_ctx(ps3_compute_nid("cellRtcGetCurrentTick"), "cellRtcGetCurrentTick", rtc_tick);
        ps3_hle_register_ctx(ps3_compute_nid("cellRtcGetCurrentClockLocalTime"), "cellRtcGetCurrentClockLocalTime", rtc_local_time);
        ps3_hle_register_ctx(ps3_compute_nid("cellRtcGetDayOfWeek"), "cellRtcGetDayOfWeek", rtc_weekday);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpInit"), "sceNpInit", np_init);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpTerm"), "sceNpTerm", np_term);
        ps3_hle_register_ctx(ps3_compute_nid("sceNpManagerGetStatus"), "sceNpManagerGetStatus", np_status);
        ps3_hle_register_ctx(ps3_compute_nid("cellGameBootCheck"), "cellGameBootCheck", game_boot);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysutilGetSystemParamInt"), "cellSysutilGetSystemParamInt", system_param);
        ps3_hle_register_ctx(ps3_compute_nid("sys_process_is_stack"), "sys_process_is_stack", process_is_stack);
        ps3_hle_register_ctx(ps3_compute_nid("sys_process_get_paramsfo"), "sys_process_get_paramsfo", process_paramsfo);
        ps3_hle_register_ctx(ps3_compute_nid("_sys_strncmp"), "_sys_strncmp", string_compare);
        ps3_hle_register_ctx(ps3_compute_nid("cellGameContentPermit"), "cellGameContentPermit", game_permit);
        ps3_hle_register_ctx(ps3_compute_nid("cellGameDataCheck"), "cellGameDataCheck", game_data);
        ps3_hle_register_ctx(ps3_compute_nid("cellGameCreateGameData"), "cellGameCreateGameData", game_create);
        ps3_hle_register_ctx(ps3_compute_nid("cellGameGetParamString"), "cellGameGetParamString", game_string);
        ps3_hle_register_ctx(ps3_compute_nid("cellGameSetParamString"), "cellGameSetParamString", game_set_string);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmGetConfiguration"), "cellGcmGetConfiguration", gcm_config);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmGetControlRegister"), "cellGcmGetControlRegister", gcm_control);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmGetTiledPitchSize"), "cellGcmGetTiledPitchSize", gcm_pitch);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmSetFlipMode"), "cellGcmSetFlipMode", gcm_flip_mode);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmResetFlipStatus"), "cellGcmResetFlipStatus", gcm_reset_flip);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmGetFlipStatus"), "cellGcmGetFlipStatus", gcm_flip_status);
        ps3_hle_register_ctx(ps3_compute_nid("cellGcmSetDebugOutputLevel"), "cellGcmSetDebugOutputLevel", gcm_debug_level);
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
    if (const auto* module = ps3_native_firmware_definition()) {
        if (!ps3_native_lle_load(module)) {
            std::fprintf(stderr, "[native-blocked] original PRX image overlaps existing memory or has invalid AOT bindings\n");
            std::fflush(stderr); ps3_native_stop();
        }
    } else {
        std::fprintf(stderr, "[native] no original PRX modules prepared; unaudited firmware calls will stop\n");
    }
    for (size_t i = 0; const auto* module = ps3_native_firmware_extra(i); ++i)
        if (!ps3_native_lle_load(module)) {
            std::fprintf(stderr, "[native-blocked] extra original PRX ownership, image or AOT bindings invalid\n");
            std::fflush(stderr); ps3_native_stop();
        }
    for (const char* library : {"libvdec", "libsmvd2"})
        if (ps3_native_lle_library(library) && !ps3_native_lle_defer_start(library)) {
            std::fprintf(stderr, "[native-blocked] original video module cannot defer initialization\n");
            std::fflush(stderr); ps3_native_stop();
        }
}
