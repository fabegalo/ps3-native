#include <cstdio>
#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include "cellSysmodule.h"
extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
namespace {
void load_module(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellSysmoduleLoadModule((uint16_t)ctx->gpr[3]); }
void module_loaded(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)(int64_t)cellSysmoduleIsLoaded((uint16_t)ctx->gpr[3]); }
// Typed HLE adapters only; no original PRX is loaded by this registration.
struct RegisterAdapters {
    RegisterAdapters() {
        ps3_hle_register_ctx(ps3_compute_nid("cellSysmoduleLoadModule"), "cellSysmoduleLoadModule", load_module);
        ps3_hle_register_ctx(ps3_compute_nid("cellSysmoduleIsLoaded"), "cellSysmoduleIsLoaded", module_loaded);
    }
} registration;
}
extern "C" void ps3_load_prx_modules(void) {
    std::fprintf(stderr, "[native] no original PRX modules prepared; unaudited firmware calls will stop\n");
}
