#include "ppu_recomp.h"
#include "native_lle.h"
#include "vm.h"
#include "ps3emu/nid.h"
#include <cstdio>
#include <cstring>
extern "C" {
uint8_t* vm_base;
uint32_t ppu_load_elf(const char*);
void ps3_hle_call(uint32_t, ppu_context*);
int ppu_register_image_region(uint32_t, uint32_t);
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
}
int main(int argc, char** argv) {
    if (argc < 2 || vm_init() || !ppu_load_elf(argv[1])) return 1;
    const auto* module = ps3_native_firmware_definition();
    if (ppu_register_image_region(0x0D000000, 4096)) return 11;
    if (!module || ps3_native_lle_owns(0xD00DFEED)) return 2;
    if (argc == 3 && !std::strcmp(argv[2], "missing_export")) {
        auto owned = *module;
        owned.library = "cellSpurs";
        if (!ps3_native_lle_load(&owned)) return 9;
        ppu_context ctx{};
        ps3_hle_call(ps3_compute_nid("cellSpursAttributeSetNamePrefix"), &ctx);
        return 10;
    }
    if (argc == 3 && !std::strcmp(argv[2], "overlap")) {
        if (!ppu_register_image_region(module->base, module->size)) return 3;
        vm_write32(module->base, 0xCAFEBABE);
        return ps3_native_lle_load(module) || vm_read32(module->base) != 0xCAFEBABE ||
            ps3_native_lle_owns(0xD00DFEED) ? 4 : 0;
    }
    auto invalid = *module;
    NativeLleExport invalid_export{0xD00DFEED, module->base + module->size - 4};
    invalid.exports = &invalid_export;
    invalid.export_count = 1;
    if (ps3_native_lle_load(&invalid) || ps3_native_lle_owns(0xD00DFEED)) return 5;
    if (!ps3_native_lle_load(module) || !ps3_native_lle_owns(0xD00DFEED) || ps3_native_lle_owns(0xDEADBEEF) ||
        !ps3_native_lle_library("ownSynthetic") || ps3_native_lle_library("cellSpurs")) return 6;
    if (!ps3_native_lle_named(module->name) || ps3_native_lle_named("own-absent-module") || ps3_native_lle_named(nullptr)) return 12;
    vm_write64(module->base + 0x180, 2);
    if (!ps3_native_lle_load(module) || vm_read64(module->base + 0x180) != 2) return 7;
    vm_write64(module->base + 0x180, 1);
    if (argc == 3 && !std::strcmp(argv[2], "changed_export")) vm_write32(module->exports[0].opd, module->base + 0x84);
    ppu_context ctx{};
    ctx.thread_id = 17; ctx.gpr[1] = 0xD00FFE00; ctx.gpr[2] = 0x12345678;
    ctx.gpr[13] = 0x87654321; ctx.lr = 0x10000;
    vm_write64(ctx.gpr[1] + 0x70, 41);
    g_active_ctx = &ctx;
    ps3_hle_call(0xD00DFEED, &ctx);
    if (ctx.gpr[3] != 42 || ctx.gpr[1] != 0xD00FFE00 || ctx.gpr[2] != 0x12345678 ||
        ctx.gpr[13] != 0x87654321 || ctx.thread_id != 17 || ctx.lr != 0x10000 || g_active_ctx != &ctx) return 8;
    std::puts("Verified original-PRX binding with own PPC AOT, shared stack, TOC, TLS and export ownership");
    vm_shutdown();
    return 0;
}
