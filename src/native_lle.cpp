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
extern "C" void ppu_write_coherent(uint32_t, const void*, uint32_t);

struct Resident {
    const NativeLleModule* module = nullptr;
    std::atomic<int> started{0};
    std::atomic<bool> deferred{false};
};
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
static bool has_library(const NativeLleModule* m, const char* name) {
    if (!std::strcmp(m->library, name)) return true;
    for (size_t i = 0; i < m->additional_library_count; ++i)
        if (!std::strcmp(m->additional_libraries[i], name)) return true;
    return false;
}
static const NativeLleDataExport* find_data(const NativeLleModule* m, const char* library, uint32_t nid) {
    for (size_t i = 0; i < m->data_export_count; ++i)
        if (m->data_exports[i].nid == nid && !std::strcmp(m->data_exports[i].library, library)) return m->data_exports + i;
    return nullptr;
}
static void bind_data(uint32_t slot, uint32_t value) {
    const uint8_t bytes[] = {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
    ppu_write_coherent(slot, bytes, sizeof bytes);
}
extern "C" int ps3_native_lle_load(const NativeLleModule* m) {
    require_root_process();
    const size_t count = loaded_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) if (loaded[i].module == m) return 1;
    if (count == loaded.size()) return 0;
    if (!m || !m->name || !m->library || !m->image || !m->functions || (m->export_count && !m->exports) ||
        !m->base || m->size < 8 || (uint64_t)m->base + m->size > 0x100000000ull ||
        !m->function_count || m->function_count > 65536 || (!m->export_count && !m->data_export_count) || m->export_count > 4096) return 0;
    if (m->additional_library_count > 32 || (m->additional_library_count && !m->additional_libraries)) return 0;
    for (size_t i = 0; i < m->additional_library_count; ++i) {
        const char* name = m->additional_libraries[i];
        if (!name || !*name || std::strlen(name) > 255 || !std::strcmp(name, m->library)) return 0;
        for (size_t j = 0; j < i; ++j) if (!std::strcmp(name, m->additional_libraries[j])) return 0;
    }
    const uint64_t end = (uint64_t)m->base + m->size;
    if (m->data_export_count > 4096 || m->data_import_count > 4096 ||
        (m->data_export_count && !m->data_exports) || (m->data_import_count && !m->data_imports)) return 0;
    for (size_t i = 0; i < m->data_export_count; ++i) {
        const auto& e = m->data_exports[i];
        if (!e.library || !has_library(m, e.library) || e.address < m->base || e.address >= end) return 0;
        for (size_t j = 0; j < i; ++j)
            if (e.nid == m->data_exports[j].nid && !std::strcmp(e.library, m->data_exports[j].library)) return 0;
        for (size_t j = 0; j < count; ++j) if (find_data(loaded[j].module, e.library, e.nid)) return 0;
    }
    for (size_t i = 0; i < m->data_import_count; ++i) {
        const auto& e = m->data_imports[i];
        if (!e.library || !*e.library || std::strlen(e.library) > 255 || (e.slot & 3) ||
            e.slot < m->base || uint64_t(e.slot) + 4 > end) return 0;
        for (size_t j = 0; j < i; ++j) if (e.slot == m->data_imports[j].slot) return 0;
    }
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
    for (size_t i = 0; i < count; ++i) {
        const auto* previous = loaded[i].module;
        if (has_library(previous, m->library) || !std::strncmp(m->name, previous->name, 28)) return 0;
        for (size_t j = 0; j < m->additional_library_count; ++j)
            if (has_library(previous, m->additional_libraries[j])) return 0;
    }
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
    for (size_t i = 0; i < m->data_import_count; ++i) {
        const auto& ref = m->data_imports[i];
        uint32_t target = 0;
        for (size_t j = 0; j <= count; ++j)
            if (const auto* e = find_data(loaded[j].module, ref.library, ref.nid)) target = e->address;
        // Preparation accepts only optional data imports (attributes 9), ADDR32,
        // zero addend and writable data slots. An absent weak export links null.
        bind_data(ref.slot, target);
    }
    for (size_t j = 0; j < count; ++j) {
        const auto* previous = loaded[j].module;
        for (size_t i = 0; i < previous->data_import_count; ++i) {
            const auto& ref = previous->data_imports[i];
            if (const auto* e = find_data(m, ref.library, ref.nid)) bind_data(ref.slot, e->address);
        }
    }
    std::fprintf(stderr, "[native-lle] loaded %s library=%s base=0x%08X bytes=%u exports=%zu data_exports=%zu data_refs=%zu\n",
        m->name, m->library, m->base, m->size, m->export_count, m->data_export_count, m->data_import_count);
    return 1;
}
extern "C" int ps3_native_lle_owns(uint32_t nid) {
    for (size_t i = 0; i < loaded_count.load(std::memory_order_acquire); ++i)
        if (find_export(loaded[i].module, nid)) return 1;
    return 0;
}
extern "C" int ps3_native_lle_library(const char* library) {
    for (size_t i = 0; library && i < loaded_count.load(std::memory_order_acquire); ++i)
        if (has_library(loaded[i].module, library)) return 1;
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
        if (!has_library(m, library)) continue;
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
extern "C" int ps3_native_lle_defer_start(const char* library) {
    require_root_process();
    for (size_t i = 0; library && i < loaded_count.load(std::memory_order_acquire); ++i)
        if (has_library(loaded[i].module, library)) {
            if (loaded[i].started.load(std::memory_order_acquire) != 0) return 0;
            loaded[i].deferred.store(true, std::memory_order_release); return 1;
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
        if (!loaded[i].deferred.load(std::memory_order_acquire) &&
            !ps3_native_lle_start(loaded[i].module->library, ctx)) {
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
