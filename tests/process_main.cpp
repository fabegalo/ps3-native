#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include "guest_process.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

extern "C" {
uint8_t* vm_base = nullptr;
void ppu_register_function(uint64_t, void (*)(ppu_context*));
void ps3_indirect_call(ppu_context*);
uint32_t ppu_function_count(void);
int ppu_process_set_control(const void*, uint32_t);
int32_t ppu_process_debug_cleanup(uint32_t, uint32_t);
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ps3_native_hle_gate(uint32_t, ppu_context*);
void ps3_native_syscall_gate(ppu_context*);
int ps3_native_lle_dispatch(uint32_t, ppu_context*);
}
static std::atomic<unsigned> failures{0};
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "process check failed: %s at %d\n", #expression, __LINE__); ++failures; } } while (0)
static void first(ppu_context* ctx) { ctx->gpr[3] = vm_read32(0x10000) + 1; }
static void second(ppu_context* ctx) { ctx->gpr[3] = vm_read32(0x10000) + 2; }
static void run(unsigned expected) {
    ppu_context ctx{}; ctx.ctr = 0x20000; ctx.thread_id = 1;
    ppu_context* previous = g_active_ctx;
    ps3_indirect_call(&ctx); CHECK(ctx.gpr[3] == expected);
    CHECK(g_active_ctx == previous);
}
int main(int argc, char** argv) {
    const auto a = guest_process_create(), b = guest_process_create();
    CHECK(a && b && a != b);
    if (!a || !b) return 1;
    const auto shared = guest_shared_create(0x20000);
    CHECK(shared);
    CHECK(!guest_process_bind(UINT64_MAX));
    CHECK(guest_process_bind(a));
    if (argc == 2) {
        ppu_context ctx{};
        if (!std::strcmp(argv[1], "hle")) ps3_native_hle_gate(0, &ctx);
        else if (!std::strcmp(argv[1], "lv2")) ps3_native_syscall_gate(&ctx);
        else if (!std::strcmp(argv[1], "prx")) ps3_native_lle_dispatch(0, &ctx);
        return 1;
    }
    CHECK(!guest_process_destroy(a));
    CHECK((uintptr_t(guest_process_base()) & 0xFFFFFFFFu) == 0);
    CHECK(guest_process_map_private(0x10000, 0x10000));
    CHECK(!guest_process_map_private(0x10000, 0x10000));
    CHECK(!guest_process_map_private(0x10001, 0x10000));
    CHECK(!guest_process_map_private(0xFFFF0000, 0x20000));
    vm_write32(0x10000, 41);
    ppu_register_function(0x20000, first);
    CHECK(ppu_function_count() == 1);
    uint8_t control[32] = {0x40};
    CHECK(ppu_process_set_control(control, sizeof control));
    CHECK(ppu_process_debug_cleanup(1, 0) == 0);
    run(42);
    CHECK(guest_process_map_shared(shared, 0x40000000, 0, 0x10000));
    CHECK(!guest_process_map_shared(shared, 0x40000000, 0, 0x10000));
    CHECK(!guest_process_map_shared(shared, 0x40010000, UINT64_MAX, 0x10000));
    vm_write32(0x40000000, 0x11223344);
    guest_process_execution_enter();
    CHECK(!guest_process_bind(b));
    CHECK(!guest_process_bind(0));
    guest_process_execution_leave();
    CHECK(guest_process_bind(b));
    CHECK(ppu_function_count() == 0);
    control[0] = 0;
    CHECK(ppu_process_set_control(control, sizeof control));
    CHECK(uint32_t(ppu_process_debug_cleanup(1, 0)) == 0x80010002u);
    CHECK(!guest_process_range_backed(0x10000, 4));
    CHECK(guest_process_map_private(0x10000, 0x10000));
    CHECK(vm_read32(0x10000) == 0);
    vm_write32(0x10000, 82);
    ppu_register_function(0x20000, second);
    run(84); // Same PC and registration generation; the indirect cache must distinguish owners.
    CHECK(guest_process_map_shared(shared, 0x50000000, 0, 0x10000));
    CHECK(vm_read32(0x50000000) == 0x11223344);
    vm_write32(0x50000000, 0x55667788);
    CHECK(guest_shared_close(shared));
    CHECK(!guest_process_map_shared(shared, 0x50010000, 0, 0x10000));
    CHECK(guest_process_bind(a));
    run(42);
    CHECK(ppu_process_debug_cleanup(1, 0) == 0);
    CHECK(vm_read32(0x40000000) == 0x55667788); // Closing the object handle preserves live views.
    std::thread worker_a([&] {
        CHECK(guest_process_bind(a));
        for (unsigned i = 0; i < 1000; ++i) run(42);
        CHECK(vm_read32(0x10000) == 41);
    });
    std::thread worker_b([&] {
        CHECK(guest_process_bind(b));
        for (unsigned i = 0; i < 1000; ++i) run(84);
        CHECK(vm_read32(0x10000) == 82);
    });
    worker_a.join(); worker_b.join();
    CHECK(guest_process_unmap(0x40000000, 0x10000));
    CHECK(!guest_process_range_backed(0x40000000, 4));
    CHECK(!guest_process_range_backed(0x1FFFF, 2));
    CHECK(guest_process_map_private(0x40000000, 0x20000)); // Rejoin split placeholders.
    CHECK(vm_read32(0x40000000) == 0);
    CHECK(guest_process_unmap(0x40000000, 0x20000));
    CHECK(guest_process_bind(0));
    CHECK(guest_process_destroy(a));
    CHECK(!guest_process_bind(a));
    CHECK(!guest_process_destroy(a));
    CHECK(guest_process_bind(b));
    CHECK(vm_read32(0x50000000) == 0x55667788);
    CHECK(guest_process_bind(0));
    CHECK(guest_process_destroy(b));
    CHECK(!guest_process_destroy(0));
    std::printf("Process VM/AOT isolation and shared aliases: %u failures\n", failures.load());
    return failures ? 1 : 0;
}
