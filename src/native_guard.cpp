#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "cellSysmodule.h"
#include "ps3emu/guest_call.h"
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;
extern "C" int ps3_native_context_ready(uint32_t);
extern "C" int ps3_hle_has(uint32_t);
static void filesystem_path(ppu_context* ctx) {
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
        std::fflush(stderr); std::exit(86);
    }
}
extern "C" void ps3_native_hle_gate(uint32_t nid, ppu_context* ctx) {
    // Expand only after inspecting and testing the upstream implementation.
    static const char* audited[] = {"sys_initialize_tls", "sys_lwmutex_create", "sys_lwmutex_lock", "sys_lwmutex_unlock",
                                    "sys_time_get_system_time", "sys_ppu_thread_get_id",
                                    "_sys_process_atexitspawn", "_sys_process_at_Exitspawn",
                                    "cellSysmoduleLoadModule", "cellSysmoduleIsLoaded",
                                    "_cellSpursAttributeInitialize", "cellSpursAttributeSetNamePrefix",
                                    "cellSysutilRegisterCallback", "cellSysutilUnregisterCallback", "cellSysutilCheckCallback",
                                    "cellRtcGetCurrentTick",
                                    "sceNpInit", "sceNpTerm", "sceNpManagerGetStatus",
                                    "cellGameBootCheck", "cellGameContentPermit",
                                    "cellGameGetParamString",
                                    "sys_mmapper_allocate_memory", "sys_mmapper_map_memory",
                                    "_cellGcmInitBody",
                                    "cellGcmGetConfiguration", "cellGcmGetControlRegister", "cellGcmGetLabelAddress", "cellGcmAddressToOffset",
                                    "cellGcmMapMainMemory",
                                    "cellGcmSetTileInfo", "cellGcmBindTile", "cellGcmUnbindTile", "cellGcmSetDisplayBuffer",
                                    "cellVideoOutGetNumberOfDevice", "cellVideoOutGetState", "cellVideoOutGetResolution",
                                    "cellVideoOutGetDeviceInfo",
                                    "cellGcmGetTiledPitchSize", "cellGcmSetFlipMode", "cellGcmSetVBlankHandler",
                                    "cellGcmSetFlipHandler", "cellGcmSetUserHandler", "cellGcmSetGraphicsHandler",
                                    "cellVideoOutGetResolutionAvailability", "cellVideoOutConfigure", "cellVideoOutGetConfiguration",
                                    "cellFsOpen", "cellFsRead", "cellFsClose", "cellFsStat", "cellFsLseek"};
    if (nid == ps3_compute_nid("cellSysmoduleLoadModule") || nid == ps3_compute_nid("cellSysmoduleIsLoaded")) {
        bool ready = false;
        if ((uint16_t)ctx->gpr[3] == CELL_SYSMODULE_FS) {
            ready = true;
            for (const char* name : {"cellFsOpen", "cellFsRead", "cellFsClose", "cellFsStat", "cellFsLseek"})
                ready &= ps3_native_context_ready(ps3_compute_nid(name)) != 0;
        } else if ((uint16_t)ctx->gpr[3] == CELL_SYSMODULE_SPURS) {
            // Activation exposes audited attribute handlers; execution APIs remain gated separately.
            ready = ps3_native_context_ready(ps3_compute_nid("_cellSpursAttributeInitialize")) &&
                    ps3_native_context_ready(ps3_compute_nid("cellSpursAttributeSetNamePrefix"));
        } else if ((uint16_t)ctx->gpr[3] == CELL_SYSMODULE_SYSUTIL) {
            ready = ps3_native_context_ready(ps3_compute_nid("cellSysutilRegisterCallback")) &&
                    ps3_native_context_ready(ps3_compute_nid("cellSysutilCheckCallback"));
        } else if ((uint16_t)ctx->gpr[3] == CELL_SYSMODULE_RTC) {
            ready = ps3_native_context_ready(ps3_compute_nid("cellRtcGetCurrentTick"));
        } else {
            // Loading a statically linked provider records availability only. Every API call
            // still passes the separate execution allowlist below, including unsupported stubs.
            static const struct { uint16_t id; const char* symbol; } providers[] = {
                {CELL_SYSMODULE_GCM_SYS, "cellGcmGetConfiguration"},
                {CELL_SYSMODULE_AUDIO, "cellAudioInit"},
                {CELL_SYSMODULE_NET, "sys_net_initialize_network"},
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
            std::fflush(stderr); std::exit(86);
        }
    }
    if (nid == ps3_compute_nid("cellGcmSetTileInfo") && (uint8_t)ctx->gpr[8] != 0) {
        std::fprintf(stderr, "[native-blocked] compressed RSX tiles need validated compression semantics\n");
        std::fflush(stderr); std::exit(86);
    }
    if (nid == ps3_compute_nid("cellVideoOutGetResolution") && ctx->gpr[3] != 2) {
        std::fprintf(stderr, "[native-blocked] unaudited video resolution id=%llu\n", (unsigned long long)ctx->gpr[3]);
        std::fflush(stderr); std::exit(86);
    }
    if (nid == ps3_compute_nid("cellGameGetParamString") &&
        ((u32)ctx->gpr[3] != 100 || ctx->gpr[4] > UINT32_MAX - (u32)ctx->gpr[5])) {
        std::fprintf(stderr, "[native-blocked] unaudited game string id or overflowing guest output\n");
        std::fflush(stderr); std::exit(86);
    }
    if (nid == ps3_compute_nid("cellRtcGetCurrentTick") && ctx->gpr[3] > UINT32_MAX - 7u) {
        std::fprintf(stderr, "[native-blocked] RTC tick output crosses guest address space\n");
        std::fflush(stderr); std::exit(86);
    }
    if (nid == ps3_compute_nid("cellSysutilCheckCallback") && !g_ps3_guest_caller) {
        std::fprintf(stderr, "[native-blocked] sysutil callback dispatcher is not installed\n");
        std::fflush(stderr); std::exit(86);
    }
    if (nid == ps3_compute_nid("cellFsOpen")) {
        filesystem_path(ctx);
        if ((uint32_t)ctx->gpr[4] != 0) {
            std::fprintf(stderr, "[native-blocked] filesystem startup only supports read-only opens\n");
            std::fflush(stderr); std::exit(86);
        }
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
    std::exit(86);
}
extern "C" void ps3_native_syscall_gate(ppu_context* ctx) {
    const auto number = ctx->gpr[11];
    if (number == 330) {
        const auto flags = ctx->gpr[4];
        const auto page = flags & ~0xFull;
        if ((flags & 0xF) == 0 || (flags & 0xF) == 0xF)
            if (page == 0 || page == 0x200 || page == 0x400) return;
    }
    // Host clocks and allocator operations have state/result regressions.
    if (number == 49 || number == 141 || number == 144 || number == 145 || number == 147 || number == 348 || number == 349 || number == 352) return;
    std::fprintf(stderr, "[native-blocked] LV2 requires audit syscall=%llu lr=0x%08X args=%llX,%llX,%llX,%llX\n",
                 (unsigned long long)number, (uint32_t)ctx->lr, (unsigned long long)ctx->gpr[3],
                 (unsigned long long)ctx->gpr[4], (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->gpr[6]);
    std::fflush(stderr);
    std::exit(86);
}
