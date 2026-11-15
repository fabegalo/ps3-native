#include "ppu_recomp.h"
#include "native_lle.h"
#include "vm.h"
#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" int ppu_register_image_region(uint32_t, uint32_t);
extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*));
extern "C" void ps3_indirect_call(ppu_context*);
extern "C" [[noreturn]] void ps3_native_stop(void);
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

static std::atomic<const NativeLleModule*> loaded;
static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static const NativeLleExport* find_export(const NativeLleModule* m, uint32_t nid) {
    if (!m) return nullptr;
    size_t lo = 0, hi = m->export_count;
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (m->exports[mid].nid < nid) lo = mid + 1;
        else hi = mid;
    }
    return lo < m->export_count && m->exports[lo].nid == nid ? m->exports + lo : nullptr;
}
extern "C" int ps3_native_lle_load(const NativeLleModule* m) {
    if (const auto* current = loaded.load(std::memory_order_acquire)) return current == m;
    if (!m || !m->name || !m->library || !m->image || !m->functions || !m->exports ||
        !m->base || m->size < 8 || (uint64_t)m->base + m->size > 0x100000000ull ||
        !m->function_count || m->function_count > 65536 || !m->export_count || m->export_count > 4096) return 0;
    const uint64_t end = (uint64_t)m->base + m->size;
    for (size_t i = 0; i < m->function_count; ++i) {
        const auto& f = m->functions[i];
        if (!f.entry || (f.address & 3) || f.address < m->base || f.address >= end ||
            (i && m->functions[i - 1].address >= f.address)) return 0;
    }
    for (size_t i = 0; i < m->export_count; ++i) {
        const auto& e = m->exports[i];
        if (e.opd < m->base || (uint64_t)e.opd + 8 > end || (e.opd & 3) ||
            (i && m->exports[i - 1].nid >= e.nid)) return 0;
        const auto code = be32(m->image + e.opd - m->base);
        bool prepared = false;
        for (size_t j = 0; j < m->function_count; ++j) prepared |= m->functions[j].address == code;
        if (!prepared) return 0;
    }
    if (!ppu_register_image_region(m->base, m->size)) return 0;
    std::memcpy(vm_base + m->base, m->image, m->size);
    for (size_t i = 0; i < m->function_count; ++i)
        ppu_register_function(m->functions[i].address, m->functions[i].entry);
    loaded.store(m, std::memory_order_release);
    std::fprintf(stderr, "[native-lle] loaded %s library=%s base=0x%08X bytes=%u exports=%zu\n",
        m->name, m->library, m->base, m->size, m->export_count);
    return 1;
}
extern "C" int ps3_native_lle_owns(uint32_t nid) {
    return find_export(loaded.load(std::memory_order_acquire), nid) != nullptr;
}
extern "C" int ps3_native_lle_library(const char* library) {
    const auto* m = loaded.load(std::memory_order_acquire);
    return m && library && !std::strcmp(m->library, library);
}
extern "C" int ps3_native_lle_dispatch(uint32_t nid, ppu_context* ctx) {
    const auto* m = loaded.load(std::memory_order_acquire);
    const auto* e = find_export(m, nid);
    if (!e) return 0;
    const auto code = vm_read32(e->opd), toc = vm_read32(e->opd + 4);
    const auto* original = m->image + e->opd - m->base;
    if (code != be32(original) || toc != be32(original + 4)) {
        std::fprintf(stderr, "[native-blocked] original PRX export descriptor changed nid=0x%08X\n", nid);
        std::fflush(stderr); ps3_native_stop();
    }
    const auto saved_toc = ctx->gpr[2];
    auto* previous = g_active_ctx;
    auto previous_trampoline = g_trampoline_fn;
    g_trampoline_fn = nullptr;
    ctx->gpr[2] = toc;
    ctx->ctr = code;
    ps3_indirect_call(ctx);
    ctx->gpr[2] = saved_toc;
    g_active_ctx = previous;
    g_trampoline_fn = previous_trampoline;
    return 1;
}
