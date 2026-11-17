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
void ps3_hle_register(uint32_t, const char*, void*);
int ppu_register_image_region(uint32_t, uint32_t);
void lv2_init_syscalls(void);
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
}
static void extra_export(ppu_context* ctx) { ctx->gpr[3] += 1; }
static void broken_start(ppu_context* ctx) { ++ctx->gpr[13]; ctx->gpr[3] = 0; }
static void shared_start(ppu_context* ctx) { vm_write32(0x09000030, vm_read32(0x09000030) + 1); ctx->gpr[3] = 0; }
static unsigned fs_bridge_calls = 0;
static void fs_bridge(ppu_context* ctx) { ++fs_bridge_calls; ctx->gpr[11] = 802; lv2_syscall(ctx); }
int main(int argc, char** argv) {
    if (argc < 2 || vm_init() || !ppu_load_elf(argv[1])) return 1;
    const auto* module = ps3_native_firmware_definition();
    if (ppu_register_image_region(0x0D000000, 4096)) return 11;
    if (!module || ps3_native_lle_owns(0xD00DFEED)) return 2;
    if (argc == 3 && (!std::strcmp(argv[2], "atrac_missing_export") || !std::strcmp(argv[2], "adec_missing_export"))) {
        const bool atrac = !std::strcmp(argv[2], "atrac_missing_export");
        auto owned = *module; owned.library = atrac ? "cellAtrac" : "cellAdec";
        if (!ps3_native_lle_load(&owned)) return 36;
        const char* name = atrac ? "cellAtracDecode" : "cellAdecOpen";
        const auto nid = ps3_compute_nid(name);
        ps3_hle_register(nid, name, (void*)extra_export);
        ppu_context ctx{}; ps3_hle_call(nid, &ctx); return 37;
    }
    if (argc == 3 && !std::strcmp(argv[2], "data")) {
        uint8_t consumer_image[64] = {9,0,0,32,9,0,128,0}; consumer_image[51] = 4; consumer_image[55] = 0x7B;
        const NativeLleFunction consumer_function[] = {{0x09000020, extra_export}};
        const NativeLleExport consumer_export[] = {{0xD00DFC00, 0x09000000}};
        const NativeLleDataImport ref[] = {{"ownData", 0xFACEBEEF, 0x09000030}};
        NativeLleModule consumer{"ownConsumer", "ownConsumer", 0x09000000, 64, consumer_image,
                                consumer_function, 1, consumer_export, 1};
        consumer.data_imports = ref; consumer.data_import_count = 1;
        auto invalid = consumer; invalid.data_imports = nullptr;
        if (ps3_native_lle_load(&invalid) || !ps3_native_lle_load(&consumer) ||
            vm_read32(0x09000030) || vm_read32(0x09000034) != 0x7B) return 32;
        uint8_t provider_image[64] = {9,0x20,0,32,9,0x20,128,0}; provider_image[51] = 7;
        const NativeLleFunction provider_function[] = {{0x09200020, extra_export}};
        const NativeLleDataExport variable[] = {{"ownData", 0xFACEBEEF, 0x09200030}};
        NativeLleModule provider{"ownProvider", "ownData", 0x09200000, 64, provider_image,
                                provider_function, 1, nullptr, 0};
        provider.data_exports = variable; provider.data_export_count = 1;
        const NativeLleDataExport outside[] = {{"ownData", 0xFACEBEEF, 0x09200040}};
        invalid = provider; invalid.data_exports = outside;
        if (ps3_native_lle_load(&invalid) || !ps3_native_lle_load(&provider) ||
            vm_read32(0x09000030) != 0x09200030 || vm_read32(vm_read32(0x09000030)) != 7) return 33;
        vm_write32(0x09200030, 11);
        if (!ps3_native_lle_load(&provider) || vm_read32(0x09200030) != 11) return 34;
        vm_write32(0x09000030, 0x09200031);
        const uint8_t second_image[64] = {9,0x60,0,32,9,0x60,128,0};
        const NativeLleFunction second_function[] = {{0x09600020, extra_export}};
        const NativeLleExport second_export[] = {{0xD00DFC01, 0x09600000}};
        const NativeLleDataImport second_ref[] = {{"ownData", 0xFACEBEEF, 0x09600030}};
        NativeLleModule second{"ownSecondConsumer", "ownSecondConsumer", 0x09600000, 64, second_image,
                              second_function, 1, second_export, 1};
        second.data_imports = second_ref; second.data_import_count = 1;
        if (!ps3_native_lle_load(&second) || vm_read32(0x09600030) != 0x09200030 ||
            vm_read32(0x09000030) != 0x09200031 || vm_read32(0x09200030) != 11) return 35;
        puts("Verified optional PRX data references, late binding, opaque addresses and preserved resident state"); return 0;
    }
    if (argc == 3 && (!std::strcmp(argv[2], "http_missing_export") || !std::strcmp(argv[2], "ssl_missing_export"))) {
        const bool http = !std::strcmp(argv[2], "http_missing_export");
        auto owned = *module; owned.library = http ? "cellHttp" : "cellSsl";
        if (!ps3_native_lle_load(&owned)) return 30;
        const char* name = http ? "cellHttpInit" : "cellSslInit";
        const auto nid = ps3_compute_nid(name);
        ps3_hle_register(nid, name, (void*)extra_export);
        ppu_context ctx{}; ps3_hle_call(nid, &ctx); return 31;
    }
    if (argc == 3 && (!std::strcmp(argv[2], "gcm_missing_export") || !std::strcmp(argv[2], "gcm_missing_private"))) {
        auto owned = *module; owned.library = "cellGcmSys";
        if (!ps3_native_lle_load(&owned)) return 19;
        const char* name = !std::strcmp(argv[2], "gcm_missing_export") ? "cellGcmGetTimeStamp" : "_cellGcmInitBody";
        const auto nid = ps3_compute_nid(name);
        ps3_hle_register(nid, name, (void*)extra_export);
        ppu_context ctx{};
        ps3_hle_call(nid, &ctx);
        return 20;
    }
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
    static const uint8_t extra_image[64] = {9,0,0,32,9,0,128,0,9,0,0,36,9,0,128,0};
    const bool fs_test = argc == 3 && !std::strcmp(argv[2], "fs_bridge");
    const bool libraries_test = argc == 3 && !std::strcmp(argv[2], "libraries");
    const u32 extra_nid = fs_test ? ps3_compute_nid("cellFsRead") : 0xD00DFA00;
    const NativeLleFunction extra_functions[] = {{0x09000020, fs_test ? fs_bridge : extra_export}, {0x09000024, libraries_test ? shared_start : broken_start}};
    const NativeLleExport extra_exports[] = {{extra_nid, 0x09000000}};
    const char* const extra_libraries[] = {"ownExtraAlias"};
    const NativeLleModule extra{"ownExtra", "ownExtra", 0x09000000, 64, extra_image,
                              extra_functions, 2, extra_exports, 1,
                              libraries_test || (argc == 3 && !std::strcmp(argv[2], "bad_start_abi")) ? 0x09000008u : 0, extra_libraries, 1};
    auto invalid_extra = extra;
    invalid_extra.additional_libraries = nullptr;
    if (ps3_native_lle_load(&invalid_extra)) return 23;
    const char* const duplicate_library[] = {"ownExtra"};
    invalid_extra.additional_libraries = duplicate_library;
    if (ps3_native_lle_load(&invalid_extra)) return 24;
    auto collision = extra;
    const NativeLleExport duplicate[] = {{0xD00DFEED, 0x09000000}};
    collision.exports = duplicate;
    if (ps3_native_lle_load(&collision)) return 15;
    collision = extra; collision.library = "ownSynthetic";
    if (ps3_native_lle_load(&collision) || !ps3_native_lle_load(&extra) ||
        !ps3_native_lle_owns(extra_nid) || !ps3_native_lle_library("ownExtra")) return 16;
    if (!ps3_native_lle_library("ownExtraAlias")) return 25;
    collision = extra; collision.name = "ownAliasCollision"; collision.library = "ownExtraAlias";
    const uint8_t other_image[64] = {9,0x20,0,32,9,0x20,0x80,0};
    const NativeLleFunction other_function[] = {{0x09200020, extra_export}};
    const NativeLleExport other_export[] = {{0xD00DFA01, 0x09200000}};
    collision.base = 0x09200000; collision.image = other_image;
    collision.functions = other_function; collision.function_count = 1;
    collision.exports = other_export; collision.export_count = 1; collision.start_opd = 0;
    collision.additional_libraries = nullptr; collision.additional_library_count = 0;
    if (ps3_native_lle_load(&collision)) return 26;
    collision.library = "ownDifferent"; collision.additional_libraries = extra_libraries; collision.additional_library_count = 1;
    if (ps3_native_lle_load(&collision)) return 27;
    vm_write64(module->base + 0x180, 2);
    if (!ps3_native_lle_load(module) || vm_read64(module->base + 0x180) != 2) return 7;
    vm_write64(module->base + 0x180, 1);
    if (argc == 3 && !std::strcmp(argv[2], "changed_export")) vm_write32(module->exports[0].opd, module->base + 0x84);
    ppu_context ctx{};
    ctx.thread_id = 17; ctx.gpr[1] = 0xD00FFE00; ctx.gpr[2] = 0x12345678;
    ctx.gpr[13] = 0x87654321; ctx.lr = 0x10000;
    vm_write64(ctx.gpr[1] + 0x70, 41);
    g_active_ctx = &ctx;
    if (libraries_test) {
        if (!ps3_native_lle_defer_start("ownExtraAlias")) return 28;
        ps3_native_lle_start_all(&ctx);
        if (vm_read32(0x09000030) || !ps3_native_lle_start("ownExtraAlias", &ctx) ||
            vm_read32(0x09000030) != 1 || !ps3_native_lle_start("ownExtra", &ctx) ||
            vm_read32(0x09000030) != 1 || ps3_native_lle_defer_start("ownExtra")) return 29;
    }
    if (fs_test) {
        lv2_init_syscalls();
        ctx.gpr[3] = UINT32_MAX;
        ps3_hle_call(extra_nid, &ctx);
        return fs_bridge_calls != 1 || (u32)ctx.gpr[3] != 0x8001002Au;
    }
    if (argc == 3 && !std::strcmp(argv[2], "bad_start_abi")) { ps3_native_lle_start("ownExtra", &ctx); return 18; }
    if (argc == 3 && !std::strcmp(argv[2], "changed_start")) vm_write32(module->start_opd, module->base + 0x80);
    ctx.gpr[3] = 41; ps3_hle_call(0xD00DFA00, &ctx);
    if (ctx.gpr[3] != 42 || ctx.gpr[2] != 0x12345678 || g_active_ctx != &ctx) return 17;
    if (argc == 3 && !std::strcmp(argv[2], "not_started")) { ps3_hle_call(0xD00DFEED, &ctx); return 14; }
    if (argc == 3 && !std::strcmp(argv[2], "deferred")) {
        if (!ps3_native_lle_defer_start("ownSynthetic") || ps3_native_lle_defer_start("absent")) return 20;
        ps3_native_lle_start_all(&ctx);
        if (vm_read32(module->base + 0x18C)) return 21;
    }
    if (!ps3_native_lle_start("ownSynthetic", &ctx) || vm_read32(module->base + 0x18C) != 1 ||
        !ps3_native_lle_start("ownSynthetic", &ctx) || vm_read32(module->base + 0x18C) != 1 ||
        ps3_native_lle_start("absent", &ctx)) return 13;
    if (ps3_native_lle_defer_start("ownSynthetic")) return 22;
    ps3_hle_call(0xD00DFEED, &ctx);
    if (ctx.gpr[3] != 42 || ctx.gpr[1] != 0xD00FFE00 || ctx.gpr[2] != 0x12345678 ||
        ctx.gpr[13] != 0x87654321 || ctx.thread_id != 17 || ctx.lr != 0x10000 || g_active_ctx != &ctx) return 8;
    std::puts("Verified original-PRX binding with own PPC AOT, shared stack, TOC, TLS and export ownership");
    vm_shutdown();
    return 0;
}
