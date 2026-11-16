#include "ppu_recomp.h"
#include "native_lle.h"
#include "guest_process.h"
#include "vm.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <array>
#include <exception>

extern "C" int ppu_register_image_region(uint32_t, uint32_t);
extern "C" void ppu_register_function(uint64_t, void (*)(ppu_context*));
extern "C" void ps3_indirect_call(ppu_context*);
extern "C" [[noreturn]] void ps3_native_stop(void);
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" int ps3_native_guest_range_backed(uint32_t, uint32_t);
extern "C" void ppu_aot_check_return(ppu_context*, uint64_t, uint64_t, uint64_t);

struct Resident { const NativeLleModule* module = nullptr; std::atomic<int> started{0}; };
// shortcut: 64 resident prepared modules; expand on measured firmware requirements.
static std::array<Resident, 64> loaded;
static std::atomic<size_t> loaded_count{0};
static void require_root_process() {
    if (guest_process_current()) {
        std::fprintf(stderr, "[native-blocked] isolated process PRX ownership is not integrated\n");
        std::fflush(stderr); ps3_native_stop();
    }
}
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
    require_root_process();
    const size_t count = loaded_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) if (loaded[i].module == m) return 1;
    if (count == loaded.size()) return 0;
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
        for (size_t j = 0; j < count; ++j) if (find_export(loaded[j].module, e.nid)) return 0;
    }
    for (size_t i = 0; i < count; ++i)
        if (!std::strcmp(m->library, loaded[i].module->library) || !std::strncmp(m->name, loaded[i].module->name, 28)) return 0;
    if (m->start_opd) {
        if (m->start_opd < m->base || (uint64_t)m->start_opd + 8 > end || (m->start_opd & 3)) return 0;
        const auto code = be32(m->image + m->start_opd - m->base);
        bool prepared = false;
        for (size_t i = 0; i < m->function_count; ++i) prepared |= m->functions[i].address == code;
        if (!prepared) return 0;
    }
    if (!ppu_register_image_region(m->base, m->size)) return 0;
    std::memcpy(vm_base + m->base, m->image, m->size);
    for (size_t i = 0; i < m->function_count; ++i)
        ppu_register_function(m->functions[i].address, m->functions[i].entry);
    loaded[count].module = m;
    loaded[count].started.store(m->start_opd ? 0 : 2, std::memory_order_relaxed);
    loaded_count.store(count + 1, std::memory_order_release);
    std::fprintf(stderr, "[native-lle] loaded %s library=%s base=0x%08X bytes=%u exports=%zu\n",
        m->name, m->library, m->base, m->size, m->export_count);
    return 1;
}
extern "C" int ps3_native_lle_owns(uint32_t nid) {
    for (size_t i = 0; i < loaded_count.load(std::memory_order_acquire); ++i)
        if (find_export(loaded[i].module, nid)) return 1;
    return 0;
}
extern "C" int ps3_native_lle_library(const char* library) {
    for (size_t i = 0; library && i < loaded_count.load(std::memory_order_acquire); ++i)
        if (!std::strcmp(loaded[i].module->library, library)) return 1;
    return 0;
}
extern "C" int ps3_native_lle_named(const char* name) {
    for (size_t i = 0; name && i < loaded_count.load(std::memory_order_acquire); ++i)
        if (!std::strncmp(loaded[i].module->name, name, 28)) return 1;
    return 0;
}
extern "C" int ps3_native_lle_start(const char* library, ppu_context* ctx) {
    require_root_process();
    for (size_t i = 0; library && ctx && i < loaded_count.load(std::memory_order_acquire); ++i) {
        auto& entry = loaded[i]; const auto* m = entry.module;
        if (std::strcmp(library, m->library)) continue;
        if (entry.started.load(std::memory_order_acquire) == 2) return 1;
        int idle = 0;
        if (!entry.started.compare_exchange_strong(idle, 1)) return 0;
        const auto* original = m->image + m->start_opd - m->base;
        if (vm_read32(m->start_opd) != be32(original) || vm_read32(m->start_opd + 4) != be32(original + 4)) {
            std::fprintf(stderr, "[native-blocked] original PRX start descriptor changed\n");
            std::fflush(stderr); ps3_native_stop();
        }
        const ppu_context saved = *ctx;
        ctx->gpr[3] = ctx->gpr[4] = 0;
        ps3_native_call_opd(ctx, m->start_opd);
        if (ctx->gpr[1] != saved.gpr[1] || ctx->gpr[13] != saved.gpr[13] || ctx->thread_id != saved.thread_id ||
            std::memcmp(ctx->gpr + 14, saved.gpr + 14, 18 * sizeof(uint64_t)) ||
            std::memcmp(ctx->fpr + 14, saved.fpr + 14, 18 * sizeof(double)) ||
            std::memcmp(ctx->vr + 20, saved.vr + 20, 12 * sizeof(ctx->vr[0])) || ((ctx->cr ^ saved.cr) & 0x00FFF000u)) {
            std::fprintf(stderr, "[native-blocked] original PRX start violated PPU ABI\n");
            std::fflush(stderr); ps3_native_stop();
        }
        const auto result = ctx->gpr[3], reserve_addr = ctx->reserve_addr, reserve_value = ctx->reserve_value;
        const auto reserve_valid = ctx->reserve_valid;
        const auto fpscr = ctx->fpscr, vscr = ctx->vscr;
        *ctx = saved;
        ctx->fpscr = fpscr; ctx->vscr = vscr;
        ctx->reserve_addr = reserve_addr; ctx->reserve_value = reserve_value; ctx->reserve_valid = reserve_valid;
        if (result != 0) {
            std::fprintf(stderr, "[native-blocked] PRX start requires lifecycle handling result=0x%llX\n", (unsigned long long)result);
            std::fflush(stderr); ps3_native_stop();
        }
        entry.started.store(2, std::memory_order_release);
        std::fprintf(stderr, "[native-lle] started library=%s opd=0x%08X\n", m->library, m->start_opd);
        return 1;
    }
    return 0;
}
extern "C" int ps3_native_lle_dispatch(uint32_t nid, ppu_context* ctx) {
    require_root_process();
    const NativeLleModule* m = nullptr; const NativeLleExport* e = nullptr;
    for (size_t i = 0; i < loaded_count.load(std::memory_order_acquire); ++i) {
        if (const auto* found = find_export(loaded[i].module, nid)) {
            if (loaded[i].started.load(std::memory_order_acquire) != 2) {
                std::fprintf(stderr, "[native-blocked] PRX export called before module start completed\n");
                std::fflush(stderr); ps3_native_stop();
            }
            m = loaded[i].module; e = found; break;
        }
    }
    if (!e) return 0;
    const auto code = vm_read32(e->opd), toc = vm_read32(e->opd + 4);
    const auto* original = m->image + e->opd - m->base;
    if (code != be32(original) || toc != be32(original + 4)) {
        std::fprintf(stderr, "[native-blocked] original PRX export descriptor changed nid=0x%08X\n", nid);
        std::fflush(stderr); ps3_native_stop();
    }
    ps3_native_call_opd(ctx, e->opd);
    return 1;
}
extern "C" void ps3_native_lle_start_all(ppu_context* ctx) {
    for (size_t i = 0; i < loaded_count.load(std::memory_order_acquire); ++i)
        if (!ps3_native_lle_start(loaded[i].module->library, ctx)) {
            std::fprintf(stderr, "[native-blocked] prepared PRX startup failed\n");
            std::fflush(stderr); ps3_native_stop();
        }
}
extern "C" void ps3_native_call_opd(ppu_context* ctx, uint32_t opd) {
    if (!ctx || (opd & 3) || !ps3_native_guest_range_backed(opd, 8)) {
        std::fprintf(stderr, "[native-blocked] invalid synchronous guest OPD=0x%08X\n", opd);
        std::fflush(stderr); ps3_native_stop();
    }
    struct Restore {
        ppu_context* ctx;
        uint64_t toc;
        ppu_context* active;
        void (*trampoline)(void*);
        int exceptions;
        ~Restore() {
            const bool unwinding = std::uncaught_exceptions() > exceptions;
            if (!unwinding) ctx->gpr[2] = toc;
            g_active_ctx = active;
            g_trampoline_fn = unwinding ? nullptr : trampoline;
        }
    } restore{ctx, ctx->gpr[2], g_active_ctx, g_trampoline_fn, std::uncaught_exceptions()};
    const auto return_pc = ctx->lr, return_stack = ctx->gpr[1], return_thread = ctx->thread_id;
    g_trampoline_fn = nullptr;
    ctx->gpr[2] = vm_read32(opd + 4);
    ctx->ctr = vm_read32(opd);
    ps3_indirect_call(ctx);
    ppu_aot_check_return(ctx, return_pc, return_stack, return_thread);
}
