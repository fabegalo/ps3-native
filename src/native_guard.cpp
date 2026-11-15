#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "cellSysmodule.h"
#include "ps3emu/guest_call.h"
#include "native_lle.h"
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;
extern "C" int ps3_native_context_ready(uint32_t);
extern "C" int ps3_hle_has(uint32_t);
extern "C" [[noreturn]] void ps3_native_stop(void);
extern "C" int sys_memory_range_backed(uint32_t, uint32_t);
extern "C" int ps3_sysprx_heap_range_backed(uint32_t, uint32_t);
extern "C" int ppu_thread_stack_range_backed(uint32_t, uint32_t);
extern "C" uint32_t g_ppu_main_stack_addr, g_ppu_main_stack_size;
static bool guest_backed(uint32_t address, uint32_t size) {
    const uint64_t end = (uint64_t)address + size;
    return end <= 0x100000000ull &&
        (sys_memory_range_backed(address, size) || ps3_sysprx_heap_range_backed(address, size) ||
         ppu_thread_stack_range_backed(address, size) ||
         (address >= g_ppu_main_stack_addr && end <= (uint64_t)g_ppu_main_stack_addr + g_ppu_main_stack_size));
}
extern "C" int ps3_native_guest_range_backed(uint32_t address, uint32_t size) {
    return guest_backed(address, size);
}
static uint32_t guest_string_size(uint32_t address) {
    // shortcut: scans stop at 1 MiB, extend when a verified caller needs longer strings.
    for (uint32_t size = 0; size < 0x100000 && (uint64_t)address + size <= UINT32_MAX &&
         guest_backed(address + size, 1); ++size)
        if (!vm_read8(address + size)) return size + 1;
    return 0;
}
static int64_t guest_string_prefix(uint32_t address, uint64_t limit) {
    for (uint32_t size = 0; size < limit && size < 0x100000; ++size) {
        if ((uint64_t)address + size > UINT32_MAX || !guest_backed(address + size, 1)) return -1;
        if (!vm_read8(address + size)) return size;
    }
    return limit <= 0x100000 ? (int64_t)limit : -1;
}
static void filesystem_path(ppu_context* ctx) {
    if (ctx->gpr[3] > UINT32_MAX || !guest_string_size((u32)ctx->gpr[3])) {
        std::fprintf(stderr, "[native-blocked] invalid filesystem path pointer\n");
        std::fflush(stderr); ps3_native_stop();
    }
    const auto address = (uint32_t)ctx->gpr[3];
    std::string path;
    if (address && address <= UINT32_MAX - 1024) {
        for (unsigned i = 0; i < 1024; ++i) {
            const char value = (char)vm_read8(address + i);
            if (!value) break;
            path += value;
        }
    }
    bool mount = false;
    for (const char* name : {"/dev_bdvd", "/app_home", "/dev_hdd0", "/dev_hdd1"}) {
        std::string prefix(name);
        mount |= path == prefix || path.starts_with(prefix + '/');
    }
    bool safe = mount && path.size() < 1024 && path.find_first_of("\\:") == std::string::npos;
    for (size_t start = 0; start < path.size();) {
        auto end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        if (path.substr(start, end - start) == "..") safe = false;
        start = end + 1;
    }
    if (!safe) {
        std::fprintf(stderr, "[native-blocked] unsupported or escaping filesystem path\n");
        std::fflush(stderr); ps3_native_stop();
    }
}
extern "C" void ps3_native_hle_gate(uint32_t nid, ppu_context* ctx) {
    if (nid == ps3_compute_nid("cellGcmGetTimeStamp") && ctx->gpr[3] < 2048) return;
    if (nid == ps3_compute_nid("_sys_strlen") && guest_string_size((uint32_t)ctx->gpr[3])) return;
    if (nid == ps3_compute_nid("sys_spu_image_import") && ctx->gpr[5] == 1) return;
    if (nid == ps3_compute_nid("_sys_strncat")) {
        const auto dst = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4];
        const auto prefix = guest_string_size(dst);
        const auto length = guest_string_prefix(src, ctx->gpr[5]);
        if (prefix && length >= 0 && guest_backed(dst, prefix + (uint32_t)length) && (!length ||
            (uint64_t)dst >= (uint64_t)src + length || (uint64_t)src >= (uint64_t)dst + prefix + length)) return;
        std::fprintf(stderr, "[native-blocked] invalid or overlapping bounded string append\n");
        std::fflush(stderr); ps3_native_stop();
    }
    if (nid == ps3_compute_nid("_sys_strcpy") || nid == ps3_compute_nid("_sys_strcat")) {
        const auto dst = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4];
        const auto size = guest_string_size(src);
        const bool append = nid == ps3_compute_nid("_sys_strcat");
        const auto prefix = append ? guest_string_size(dst) : 1u;
        const auto total = prefix ? prefix - 1 + size : 0;
        if (size && prefix && guest_backed(dst, total) && ((!append && dst == src) ||
            (uint64_t)dst >= (uint64_t)src + size || (uint64_t)src >= (uint64_t)dst + total)) return;
        std::fprintf(stderr, "[native-blocked] invalid, overlapping or unbounded string copy\n");
        std::fflush(stderr); ps3_native_stop();
    }
    if (nid == ps3_compute_nid("_sys_memset") || nid == ps3_compute_nid("_sys_memcpy")) {
        const uint64_t size = ctx->gpr[5];
        const auto dst = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4];
        const bool copy = nid == ps3_compute_nid("_sys_memcpy");
        const bool overlap = copy && dst != src && dst < (uint64_t)src + size && src < (uint64_t)dst + size;
        if (size > UINT32_MAX || (size && (!guest_backed(dst, (uint32_t)size) ||
            (copy && !guest_backed(src, (uint32_t)size)))) || overlap) {
            std::fprintf(stderr, "[native-blocked] invalid or overlapping bulk-memory operation\n");
            std::fflush(stderr); ps3_native_stop();
        }
    }
    if (ps3_native_lle_library("cellSpurs")) {
        for (const char* name : {"_cellSpursAttributeInitialize", "cellSpursAttributeSetNamePrefix",
             "cellSpursAttributeEnableSpuPrintfIfAvailable", "cellSpursAttributeSetSpuThreadGroupType"}) {
            if (nid == ps3_compute_nid(name)) {
                std::fprintf(stderr, "[native-blocked] original cellSpurs owner has no prepared export nid=0x%08X\n", nid);
                std::fflush(stderr); ps3_native_stop();
            }
        }
    }
    // Expand only after inspecting and testing the upstream implementation.
    static const char* audited[] = {"sys_prx_get_module_id_by_name", "sys_initialize_tls", "sys_lwmutex_create", "sys_lwmutex_lock", "sys_lwmutex_unlock",
                                    "sys_lwmutex_destroy",
                                    "sys_time_get_system_time", "sys_ppu_thread_get_id",
                                    "sys_process_is_stack",
                                    "sys_lwcond_create", "sys_lwcond_destroy", "sys_lwcond_signal", "sys_lwcond_signal_all", "sys_lwcond_wait",
                                    "_sys_memset", "_sys_memcpy",
                                    "_sys_spu_printf_attach_group", "_sys_spu_printf_detach_group",
                                    "_sys_spu_printf_attach_thread", "_sys_spu_printf_detach_thread",
                                    "_sys_spu_printf_initialize", "_sys_spu_printf_finalize",
                                    "_sys_process_atexitspawn", "_sys_process_at_Exitspawn",
                                    "cellSysmoduleLoadModule", "cellSysmoduleIsLoaded", "cellSysmoduleInitialize",
                                    "cellSysmoduleUnloadModule",
                                    "_cellSpursAttributeInitialize", "cellSpursAttributeSetNamePrefix",
                                    "cellSpursAttributeEnableSpuPrintfIfAvailable",
                                    "cellSpursAttributeSetSpuThreadGroupType",
                                    "cellSysutilRegisterCallback", "cellSysutilUnregisterCallback", "cellSysutilCheckCallback",
                                    "cellSysutilGetSystemParamInt",
                                    "cellRtcGetCurrentTick", "cellRtcGetCurrentClockLocalTime", "cellRtcGetDayOfWeek",
                                    "cellPadInit", "cellPadEnd",
                                    "sceNpInit", "sceNpTerm", "sceNpManagerGetStatus",
                                    "sceNpTrophyInit", "sceNpTrophyTerm",
                                    "cellGameBootCheck", "cellGameContentPermit", "cellGameDataCheck",
                                    "cellGameCreateGameData",
                                    "cellGameSetParamString",
                                    "cellGameGetParamString",
                                    "sys_mmapper_allocate_memory", "sys_mmapper_map_memory",
                                    "sys_ppu_thread_create", "sys_ppu_thread_exit", "sys_ppu_thread_once",
                                    "sys_ppu_thread_register_atexit", "sys_ppu_thread_unregister_atexit",
                                    "_cellGcmInitBody",
                                    "cellGcmGetConfiguration", "cellGcmGetControlRegister", "cellGcmGetLabelAddress", "cellGcmAddressToOffset",
                                    "cellGcmMapMainMemory",
                                    "cellGcmSetTileInfo", "cellGcmBindTile", "cellGcmUnbindTile", "cellGcmSetDisplayBuffer", "cellGcmSetPrepareFlip", "cellGcmSetFlipImmediate",
                                    "cellVideoOutGetNumberOfDevice", "cellVideoOutGetState", "cellVideoOutGetResolution",
                                    "cellVideoOutGetDeviceInfo",
                                    "cellAudioOutGetSoundAvailability",
                                    "cellAudioOutConfigure",
                                    "cellAudioInit",
                                    "cellGcmGetTiledPitchSize", "cellGcmSetFlipMode", "cellGcmSetVBlankHandler",
                                    "cellGcmSetFlipHandler", "cellGcmSetUserHandler", "cellGcmSetGraphicsHandler",
                                    "cellVideoOutGetResolutionAvailability", "cellVideoOutConfigure", "cellVideoOutGetConfiguration",
                                    "cellFsOpen", "cellFsRead", "cellFsWrite", "cellFsClose", "cellFsStat", "cellFsFstat", "cellFsFsync", "cellFsLseek"};
    if (nid == ps3_compute_nid("cellSysmoduleLoadModule") || nid == ps3_compute_nid("cellSysmoduleIsLoaded")) {
        bool ready = false;
        if ((uint16_t)ctx->gpr[3] == CELL_SYSMODULE_FS) {
            ready = true;
            for (const char* name : {"cellFsOpen", "cellFsRead", "cellFsWrite", "cellFsClose", "cellFsStat", "cellFsFstat", "cellFsLseek"})
                ready &= ps3_native_context_ready(ps3_compute_nid(name)) != 0;
        } else if ((uint16_t)ctx->gpr[3] == CELL_SYSMODULE_SPURS) {
            // Activation exposes audited attribute handlers; execution APIs remain gated separately.
            ready = ps3_native_lle_library("cellSpurs") ?
                    ps3_native_lle_owns(ps3_compute_nid("cellSpursInitializeWithAttribute")) :
                    ps3_native_context_ready(ps3_compute_nid("_cellSpursAttributeInitialize")) &&
                    ps3_native_context_ready(ps3_compute_nid("cellSpursAttributeSetNamePrefix"));
        } else if ((uint16_t)ctx->gpr[3] == CELL_SYSMODULE_SYSUTIL) {
            ready = ps3_native_context_ready(ps3_compute_nid("cellSysutilRegisterCallback")) &&
                    ps3_native_context_ready(ps3_compute_nid("cellSysutilCheckCallback"));
        } else if ((uint16_t)ctx->gpr[3] == CELL_SYSMODULE_RTC) {
            ready = ps3_native_context_ready(ps3_compute_nid("cellRtcGetCurrentTick"));
        } else if ((uint16_t)ctx->gpr[3] == CELL_SYSMODULE_NET && ps3_native_lle_library("sys_net")) {
            ready = ps3_native_lle_owns(ps3_compute_nid("sys_net_initialize_network_ex"));
        } else {
            // Loading a statically linked provider records availability only. Every API call
            // still passes the separate execution allowlist below, including unsupported stubs.
            static const struct { uint16_t id; const char* symbol; } providers[] = {
                {CELL_SYSMODULE_GCM_SYS, "cellGcmGetConfiguration"},
                {CELL_SYSMODULE_AUDIO, "cellAudioInit"},
                {CELL_SYSMODULE_NET, "sys_net_initialize_network_ex"},
                {CELL_SYSMODULE_NETCTL, "cellNetCtlInit"},
                {CELL_SYSMODULE_IO, "cellPadInit"},
                {CELL_SYSMODULE_PNGDEC, "cellPngDecCreate"},
                {CELL_SYSMODULE_SYSUTIL_GAME, "cellGameBootCheck"},
                {CELL_SYSMODULE_SYSUTIL_NP, "sceNpInit"},
                {CELL_SYSMODULE_SYSUTIL_NP_TROPHY, "sceNpTrophyInit"},
                {CELL_SYSMODULE_MIC, "cellMicInit"},
                {CELL_SYSMODULE_AVCONF_EXT, "cellAudioOutGetDeviceInfo"},
                {CELL_SYSMODULE_OVIS, "cellOvisGetOverlayTableSize"},
                {CELL_SYSMODULE_SYNC, "cellSyncMutexInitialize"},
                {CELL_SYSMODULE_SYSUTIL_SAVEDATA, "cellSaveDataAutoLoad2"},
            };
            for (const auto& provider : providers)
                if ((uint16_t)ctx->gpr[3] == provider.id) ready = ps3_hle_has(ps3_compute_nid(provider.symbol)) != 0;
        }
        if (!ready) {
            std::fprintf(stderr, "[native-blocked] HLE module provider not ready id=0x%llX\n", (unsigned long long)ctx->gpr[3]);
            std::fflush(stderr); ps3_native_stop();
        }
    }
    if (nid == ps3_compute_nid("sys_ppu_thread_create") && (ctx->gpr[8] & ~1ull)) {
        std::fprintf(stderr, "[native-blocked] unsupported PPU thread creation flags\n");
        std::fflush(stderr); ps3_native_stop();
    }
    if (nid == ps3_compute_nid("cellGcmSetTileInfo") && (uint8_t)ctx->gpr[8] != 0) {
        std::fprintf(stderr, "[native-blocked] compressed RSX tiles need validated compression semantics\n");
        std::fflush(stderr); ps3_native_stop();
    }
    if (nid == ps3_compute_nid("cellVideoOutGetResolution") && ctx->gpr[3] != 2) {
        std::fprintf(stderr, "[native-blocked] unaudited video resolution id=%llu\n", (unsigned long long)ctx->gpr[3]);
        std::fflush(stderr); ps3_native_stop();
    }
    if (nid == ps3_compute_nid("cellGameGetParamString") &&
        ((u32)ctx->gpr[3] != 100 || ctx->gpr[4] > UINT32_MAX - (u32)ctx->gpr[5])) {
        std::fprintf(stderr, "[native-blocked] unaudited game string id or overflowing guest output\n");
        std::fflush(stderr); ps3_native_stop();
    }
    if (nid == ps3_compute_nid("cellRtcGetCurrentTick") && ctx->gpr[3] > UINT32_MAX - 7u) {
        std::fprintf(stderr, "[native-blocked] RTC tick output crosses guest address space\n");
        std::fflush(stderr); ps3_native_stop();
    }
    if (nid == ps3_compute_nid("cellSysutilCheckCallback") && !g_ps3_guest_caller) {
        std::fprintf(stderr, "[native-blocked] sysutil callback dispatcher is not installed\n");
        std::fflush(stderr); ps3_native_stop();
    }
    if (nid == ps3_compute_nid("cellFsOpen")) {
        filesystem_path(ctx);
    }
    if (nid == ps3_compute_nid("cellFsStat")) filesystem_path(ctx);
    static std::atomic<unsigned> calls{0};
    for (const char* name : audited) {
        if (ps3_compute_nid(name) == nid) {
            if (calls.fetch_add(1) < 100) std::fprintf(stderr, "[native-hle] %s lr=0x%08X\n", name, (uint32_t)ctx->lr);
            return;
        }
    }
    std::fprintf(stderr, "[native-blocked] HLE requires audit nid=0x%08X cia=0x%08X lr=0x%08X r3=0x%llX\n",
                 nid, (uint32_t)ctx->cia, (uint32_t)ctx->lr, (unsigned long long)ctx->gpr[3]);
    std::fflush(stderr);
    ps3_native_stop();
}
extern "C" void ps3_native_syscall_gate(ppu_context* ctx) {
    const auto number = ctx->gpr[11];
    if (number == 1) return;
    if (number == 25) return;
    // Reservation capability is classified by the upstream LV2 address/rights contract.
    if (number == 14) return;
    if (number == 90 && guest_backed((uint32_t)ctx->gpr[3], 4) &&
        guest_backed((uint32_t)ctx->gpr[4], 32) &&
        vm_read32((uint32_t)ctx->gpr[4] + 4) == 0x200) return;
    if (number == 91) return;
    if (number >= 100 && number <= 104) return;
    if (number == 105 || number == 106) return;
    if ((number >= 120 && number <= 125) || number == 127 || number == 148) return;
    if (number == 403) return;
    if (number == 801 || number == 802 || number == 804 || number == 808 || number == 809 || number == 818) return;
    if (number == 130) return;
    if (number == 138) return;
    if (number == 251) return;
    if (number == 185) return;
    if (number == 173) return;
    if (number == 178 && guest_backed((u32)ctx->gpr[4], 4) && guest_backed((u32)ctx->gpr[5], 4)) return;
    if (number == 186 && (ctx->gpr[4] == 1 || ctx->gpr[4] == 2 || ctx->gpr[4] == 4)) return;
    if (number == 170 && guest_backed((uint32_t)ctx->gpr[6], 12) &&
        (vm_read32((uint32_t)ctx->gpr[6] + 8) == 0x18 || vm_read32((uint32_t)ctx->gpr[6] + 8) == 0)) return;
    if (number == 172 && guest_backed((uint32_t)ctx->gpr[7], 12) &&
        (vm_read32((uint32_t)ctx->gpr[7] + 8) & ~2u) == 0 && guest_backed((uint32_t)ctx->gpr[6], 16) &&
        vm_read32((uint32_t)ctx->gpr[6]) == 0) return;
    if (number == 128 && guest_backed((uint32_t)ctx->gpr[3], 4) &&
        guest_backed((uint32_t)ctx->gpr[4], 16)) return;
    if (number == 134 && guest_backed((uint32_t)ctx->gpr[3], 4) &&
        ctx->gpr[4] == 1) return;
    if (number == 136) return;
    if (number == 330) {
        const auto flags = ctx->gpr[4];
        const auto page = flags & ~0xFull;
        if ((flags & 0xF) == 0 || (flags & 0xF) == 0xF)
            if (page == 0 || page == 0x200 || page == 0x400) return;
    }
    // Host clocks and allocator operations have state/result regressions.
    if (number == 42 || number == 44 || number == 48 || number == 49 || number == 141 || number == 144 || number == 145 || number == 147 || number == 169 || number == 348 || number == 349 || number == 352) return;
    if (number == 170 && guest_backed((u32)ctx->gpr[6], 12))
        std::fprintf(stderr, "[native-blocked] SPU group type=0x%X name_bytes=%u\n",
                     vm_read32((u32)ctx->gpr[6] + 8), vm_read32((u32)ctx->gpr[6]));
    std::fprintf(stderr, "[native-blocked] LV2 requires audit syscall=%llu lr=0x%08X args=%llX,%llX,%llX,%llX\n",
                 (unsigned long long)number, (uint32_t)ctx->lr, (unsigned long long)ctx->gpr[3],
                 (unsigned long long)ctx->gpr[4], (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->gpr[6]);
    std::fflush(stderr);
    ps3_native_stop();
}
