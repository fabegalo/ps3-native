#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "cellSysmodule.h"
extern "C" int ps3_native_context_ready(uint32_t);
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
                                    "cellFsOpen", "cellFsRead", "cellFsClose", "cellFsStat", "cellFsLseek"};
    if (nid == ps3_compute_nid("cellSysmoduleLoadModule") || nid == ps3_compute_nid("cellSysmoduleIsLoaded")) {
        bool ready = (uint16_t)ctx->gpr[3] == CELL_SYSMODULE_FS;
        for (const char* name : {"cellFsOpen", "cellFsRead", "cellFsClose", "cellFsStat", "cellFsLseek"})
            ready &= ps3_native_context_ready(ps3_compute_nid(name)) != 0;
        if (!ready) {
            std::fprintf(stderr, "[native-blocked] HLE module provider not ready id=0x%llX\n", (unsigned long long)ctx->gpr[3]);
            std::fflush(stderr); std::exit(86);
        }
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
    // Host clocks and allocator operations have state/result regressions.
    if (number == 49 || number == 144 || number == 145 || number == 147 || number == 348 || number == 349 || number == 352) return;
    std::fprintf(stderr, "[native-blocked] LV2 requires audit syscall=%llu lr=0x%08X\n",
                 (unsigned long long)number, (uint32_t)ctx->lr);
    std::fflush(stderr);
    std::exit(86);
}
