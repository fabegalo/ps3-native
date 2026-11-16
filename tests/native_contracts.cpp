#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include "ps3emu/error_codes.h"
#include "native_storage.h"
#include "vm.h"
#include "spu_interp.h"
#include "spu_workload.h"
#include "spu_coherency.h"
#include "spu_lifted_thread.h"
#include "spu_overlay_residency.h"
#define PPU_CONTEXT_H // ppu_recomp.h already supplies the generated context ABI.
#include "sys_mutex.h"
#include "sys_ppu_thread.h"
#include "sys_cond.h"
extern "C" {
#include "thread/spu_recomp.h"
}
#include "cellSysutil.h"
#include "cellGame.h"
#include "cellGcmSys.h"
#include "cellSpurs.h"
#include "cellPad.h"
#include "rsx_tiling.h"
#include "rsx_commands.h"
#include "ps3emu/guest_call.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <string>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
extern "C" {
uint8_t* vm_base = nullptr;
void ppu_unlifted_stub(uint64_t, ppu_context*);
void ps3_hle_call(uint32_t, ppu_context*);
void ps3_hle_register(uint32_t, const char*, void*);
int32_t sys_net_initialize_network_ex(void*);
int32_t cellAudioInit(void);
int32_t cellAudioQuit(void);
int32_t cellAudioPortClose(uint32_t);
void ps3_indirect_call(ppu_context*);
void lv2_syscall(ppu_context*);
void ppu_sysprx_register(void);
void lv2_init_syscalls(void);
int ppu_process_set_paramsfo(const void*, uint32_t);
void sys_memory_set_image_query(uint64_t (*)(uint64_t, uint64_t));
uint32_t ps3_process_spawn_callback(unsigned);
extern uint32_t g_ppu_main_stack_addr, g_ppu_main_stack_size;
extern const char* ppu_vfs_root;
void ppu_fs_register(void);
void ps3_load_prx_modules(void);
extern ps3_guest_caller_fn g_ps3_guest_caller;
void cellGcmTickVBlank(void);
int cellGcmVideoNext(u64, u64*);
void cellGcmVideoTick(u64);
u64 ppu_timebase_now(void);
u32 cellGcm_original_frames_presented(void);
void ppu_gcm_pump(void);
int cellGcmOffsetIsDisplay(u32);
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_register_function(uint64_t, void (*)(ppu_context*));
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t ppu_guest_call_ct(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
void ppu_install_thread_trampoline(void);
uint64_t ppu_thread_register_main(void);
uint64_t ppu_thread_register_main_priority(int32_t);
int lv2_try_syscall(ppu_context*);
s32 sys_ppu_thread_once(u32, u32);
int ps3_native_guest_range_backed(uint32_t, uint32_t);
int ppu_callback_stack_range_backed(uint32_t, uint32_t);
int sys_event_queue_push_by_id(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint32_t spu_thread_get_group_id(uint32_t);
void spu_begin_image(int);
extern int (*g_spu_user_event_hook)(spu_context*, uint32_t);
void cellGcm_rsx_process_fifo(void);
void rsx_raise_user_cmd(u32);
u32 gcm_io_offset_to_ea(u32);
int cellGcm_take_flip_pending(void);
unsigned cellGcm_flip_request_count(void);
u32 ps3_spu_printf_callback(u32);
void ppu_resv_register(ppu_context*);
int ppu_stwcx32(uint64_t, uint32_t, uint32_t);
void ppu_write_coherent(uint32_t, const void*, uint32_t);
}
static unsigned callbacks;
static int own_audio_output = 1;
extern "C" int ps3_native_audio_stereo_probe(void) { return own_audio_output; }
static unsigned gcm_callbacks;
static uint64_t callback_tls, callback_tid, callback_toc;
static std::atomic<bool> continued_after_exit{false};
static std::atomic<unsigned> thread_atexit_calls{0};
static void native_thread_atexit(ppu_context* ctx) {
    if (!ctx->thread_id || ctx->gpr[2] != 0xABCD0000) std::abort();
    ++thread_atexit_calls;
    ctx->gpr[3] = 0xBAD; // The exit status must survive callbacks.
}
static std::atomic<bool> once_entered{false}, once_release{false};
static unsigned once_calls;
static u32 synthetic_report_target;
static unsigned synthetic_report_calls;
static bool synthetic_report_early;
static unsigned synthetic_submits;
static u32 synthetic_semaphore_target;
static bool synthetic_semaphore_early;
static u32 synthetic_scanout_reports, synthetic_scanout_driver;
static unsigned synthetic_scanout_calls;
static int synthetic_scanout_result;
static bool synthetic_scanout_early;
static bool synthetic_queue_delay;
static u64 synthetic_queue_completed;
static u32 synthetic_reference_control;
static unsigned synthetic_zcull_calls;
static bool synthetic_zcull_early;
static u32 synthetic_zcull_old_offset;
static u8 synthetic_zcull_old_bound;
static int synthetic_zcull(void*, u32 index, const CellGcmZcullInfo*) {
    ++synthetic_zcull_calls;
    const auto* old = rsx_zcull_get(index);
    if (!synthetic_submits || !old || old->bound != synthetic_zcull_old_bound || old->offset != synthetic_zcull_old_offset)
        synthetic_zcull_early = true;
    return 1;
}
static unsigned synthetic_reference_checks;
static bool synthetic_reference_early;
static int synthetic_scanout(void*, u32 offset, u32 pitch, u32 width, u32 height) {
    ++synthetic_scanout_calls;
    if (offset != 0x10000 || pitch != 256 || width != 64 || height != 64 ||
        vm_read32(synthetic_scanout_reports + 0x10) != UINT32_MAX ||
        (vm_read32(synthetic_scanout_driver + 0x1100) & 0x80000000u) || cellGcm_original_frames_presented())
        synthetic_scanout_early = true;
    return synthetic_scanout_result;
}
static int synthetic_submit(void*) {
    ++synthetic_submits;
    if (synthetic_reference_control) {
        const u32 previous[2] = {0xA5A5A5A5, 0x12345678};
        if (synthetic_reference_checks >= 2 || vm_read32(synthetic_reference_control + 8) != previous[synthetic_reference_checks])
            synthetic_reference_early = true;
        ++synthetic_reference_checks;
    }
    if (synthetic_semaphore_target && vm_read32(synthetic_semaphore_target) != 0xA5A5A5A5)
        synthetic_semaphore_early = true;
    if (cellGcm_take_flip_pending()) synthetic_report_early = true;
    if (synthetic_queue_delay) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        synthetic_queue_completed = ppu_timebase_now(); synthetic_queue_delay = false;
    }
    return 1; // Explicit CPU-only completion fixture.
}
static int synthetic_report(void*, u32 method, u32, u64* timestamp, u32* value) {
    ++synthetic_report_calls;
    if (method == 0x1800) {
        if (synthetic_report_target && vm_read64(synthetic_report_target) != 0xA5A5A5A5A5A5A5A5ull)
            synthetic_report_early = true;
        *timestamp = 0x12345678ABCDEF01ull; *value = 0x89ABCDEF;
    }
    return 1; // Explicit synthetic backend for ABI/publication tests only.
}
static std::atomic<u32> once_callback_stack{0};
static std::atomic<bool> once_callback_bad{false};
static void native_once_initializer(ppu_context* ctx) {
    const u32 mutex = (u32)ctx->gpr[1] - 0x60, attr = mutex + 0x20;
    once_callback_stack.store(mutex);
    const u32 slot_end = VM_CB_STACK_BASE + ((mutex - VM_CB_STACK_BASE) / VM_CB_STACK_SIZE + 1) * VM_CB_STACK_SIZE;
    if (!ppu_callback_stack_range_backed(mutex, 24) || ppu_callback_stack_range_backed(slot_end - 16, 32) ||
        ppu_callback_stack_range_backed(mutex, 0)) once_callback_bad.store(true);
    vm_write32(attr, 2); vm_write32(attr + 4, 0x20); vm_write64(attr + 8, 0);
    ctx->gpr[3] = mutex; ctx->gpr[4] = attr;
    ps3_hle_call(ps3_compute_nid("sys_lwmutex_create"), ctx);
    if (ctx->gpr[3]) once_callback_bad.store(true);
    else {
        ctx->gpr[3] = mutex;
        ps3_hle_call(ps3_compute_nid("sys_lwmutex_destroy"), ctx);
        if (ctx->gpr[3]) once_callback_bad.store(true);
    }
    ++once_calls;
    once_entered.store(true);
    while (!once_release.load()) std::this_thread::yield();
    vm_write32(0x39600, 0xC001C0DE);
}
static void synchronous_guest(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    ppu_guest_call(opd, a0,a1,a2,a3,a4,a5,a6,a7);
}
static void native_guest_thread(ppu_context* ctx) {
    vm_write32((u32)ctx->gpr[3], 0xC0FFEE);
    vm_write64(ctx->gpr[1] - 32, 0xCAFEBABE);
    ctx->gpr[3] = 0x1122334455667788ull;
    ps3_hle_call(ps3_compute_nid("sys_ppu_thread_exit"), ctx);
    continued_after_exit.store(true);
}
static void native_guest_callback(ppu_context* ctx) {
    callback_tls = ctx->gpr[13]; callback_tid = ctx->thread_id; callback_toc = ctx->gpr[2];
    vm_write64(ctx->gpr[1] - 16, ctx->gpr[3]);
    ctx->gpr[3] += ctx->gpr[4];
}
static int synthetic_video_ready;
extern "C" int ppu_host_video_state(void) { return synthetic_video_ready; }
static void callback(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (opd == 0x12340 && a0 == 0xABCD && a1 == 0x4321 && a2 == 0x24000) callbacks++;
    if (opd == 0x37000 && a0) gcm_callbacks++;
}
static void call(ppu_context& ctx, const char* name, uint64_t a0) {
    if (!vm_base && vm_init()) std::abort();
    ctx.gpr[3] = a0;
    ps3_hle_call(ps3_compute_nid(name), &ctx);
}
int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "event_ipc")) {
        if (vm_init() || vm_commit(0x30000, 0x10000)) return 1;
        g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
        lv2_init_syscalls(); ppu_context c{};
        auto call = [&](u32 number, u64 a, u64 b = 0, u64 d = 0, u64 e = 0) {
            c.gpr[11] = number; c.gpr[3] = a; c.gpr[4] = b; c.gpr[5] = d; c.gpr[6] = e;
            lv2_syscall(&c); return (u32)c.gpr[3];
        };
        const u64 key = 0x123456789ABCDEF0ull, source = 0x1122334455667788ull;
        vm_write32(0x31010, 1); vm_write32(0x31014, 1); vm_write64(0x31018, 0);
        if (call(128, 0x31000, 0x31010, key, 2)) return 2;
        const u32 queue = vm_read32(0x31000);
        vm_write32(0x31000, 0xA5A5A5A5);
        if (call(134, 0x31000, 2, source) != (u32)CELL_EINVAL || vm_read32(0x31000) != 0xA5A5A5A5 ||
            call(134, 0x31000, 3, source)) return 3;
        const u32 port = vm_read32(0x31000);
        if (call(140, port, 0) != (u32)CELL_EINVAL || call(140, port, key + 1) != (u32)CELL_ESRCH ||
            call(136, port, queue) != (u32)CELL_EINVAL || call(140, port, key) ||
            call(140, port, key) != (u32)CELL_EISCONN || call(135, port) != (u32)CELL_EISCONN) return 4;
        if (call(138, port, 0x1111222233334444ull, 0x5555666677778888ull, 0x9999AAAABBBBCCCCull) ||
            call(130, queue, 0, 1000) || c.gpr[4] != source || c.gpr[5] != 0x1111222233334444ull ||
            c.gpr[6] != 0x5555666677778888ull || c.gpr[7] != 0x9999AAAABBBBCCCCull) return 5;
        if (call(129, queue, 0) || call(138, port) != (u32)CELL_ENOTCONN ||
            call(128, 0x31000, 0x31010, key, 2) || call(138, port) != (u32)CELL_ENOTCONN ||
            call(140, port, key)) return 6;
        const u32 replacement = vm_read32(0x31000);
        if (call(137, port) || call(137, port) != (u32)CELL_ENOTCONN || call(135, port) ||
            call(135, port) != (u32)CELL_ESRCH || call(129, replacement, 0)) return 7;
        vm_shutdown(); puts("IPC ports: keyed connection, payload, type errors and no stale routing after queue replacement verified");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "process_paramsfo")) {
        if (vm_init() || vm_commit(0x30000, 0x10000)) return 1;
        g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
        lv2_init_syscalls();
        auto query = [](u64 address) {
            ppu_context c{}; c.gpr[11] = 30; c.gpr[3] = address;
            lv2_syscall(&c); return (u32)c.gpr[3];
        };
        memset(vm_base + 0x31000, 0xA5, 80);
        if (query(0x31008) != (u32)CELL_ENOENT || query(UINT64_MAX) != (u32)CELL_ENOENT) return 2;
        for (unsigned i = 0; i < 80; ++i) if (vm_base[0x31000 + i] != 0xA5) return 3;
        u8 data[64]; for (unsigned i = 0; i < 64; ++i) data[i] = (u8)(i ^ 0x5A);
        if (ppu_process_set_paramsfo(data, 63) || ppu_process_set_paramsfo(nullptr, 64) ||
            !ppu_process_set_paramsfo(data, 64)) return 4;
        if (query(UINT64_MAX) != (u32)CELL_EFAULT || query(0x40000) != (u32)CELL_EFAULT || query(0x31008)) return 5;
        if (memcmp(vm_base + 0x31008, data, 64)) return 6;
        for (unsigned i = 0; i < 8; ++i)
            if (vm_base[0x31000 + i] != 0xA5 || vm_base[0x31048 + i] != 0xA5) return 7;
        data[0] = 0xFF;
        if (!ppu_process_set_paramsfo(data, 64) || query(UINT64_MAX) != (u32)CELL_ENOENT ||
            !ppu_process_set_paramsfo(nullptr, 0) || query(0x31008) != (u32)CELL_ENOENT) return 8;
        vm_shutdown(); puts("Opaque process PARAMSFO: absent data, original bytes, bounds and absence-before-pointer validation verified");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "kernel_lwmutex")) {
        if (vm_init() || vm_commit(0x30000, 0x10000)) return 1;
        g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
        lv2_init_syscalls();
        auto call = [](ppu_context& c, u32 number, u32 id, u64 value = 0) {
            c.gpr[11] = number; c.gpr[3] = id; c.gpr[4] = value;
            lv2_syscall(&c); return (u32)c.gpr[3];
        };
        ppu_context owner{}; owner.thread_id = 1;
        for (unsigned protocol : {1u,2u}) {
            memset(vm_base + 0x31100, 0xA5, 24);
            owner.gpr[5] = 0x31100; owner.gpr[6] = 0x80000001; owner.gpr[7] = 0x6C776D7574657800ull;
            if (call(owner, 95, 0x31000, protocol)) return 2;
            const u32 id = vm_read32(0x31000);
            for (unsigned byte = 0; byte < 24; ++byte)
                if (vm_base[0x31100 + byte] != 0xA5) return 3;
            if (!id || call(owner, 103, id) != (u32)CELL_ESRCH || call(owner, 99, id) != (u32)CELL_EBUSY) return 4;
            const auto before = std::chrono::steady_clock::now();
            if (call(owner, 97, id, 30) != (u32)CELL_ETIMEDOUT ||
                std::chrono::steady_clock::now() - before < std::chrono::microseconds(30)) return 5;
            if (call(owner, 98, id) || call(owner, 98, id) || call(owner, 99, id) ||
                call(owner, 99, id) != (u32)CELL_EBUSY) return 6;
            if (call(owner, 117, id) || call(owner, 99, id) != (u32)CELL_EBUSY ||
                call(owner, 97, id) != (u32)CELL_EBUSY) return 7;
            if (call(owner, 117, id) || call(owner, 98, id) || call(owner, 97, id)) return 8;
            for (unsigned i = 0; i < 4; ++i) {
                g_ppu_threads[i].state = PPU_THREAD_STATE_RUNNING;
                g_ppu_threads[i].ctx.thread_id = i + 1;
                g_ppu_threads[i].priority = i == 2 ? 100 : 1000;
            }
            std::atomic<unsigned> count{0}; unsigned order[3]{}; std::thread workers[3];
            for (unsigned i = 0; i < 3; ++i) {
                workers[i] = std::thread([&, i] {
                    ppu_context c{}; c.thread_id = i + 2;
                    if (call(c, 97, id)) std::abort();
                    order[count.fetch_add(1)] = i;
                    if (call(c, 98, id)) std::abort();
                });
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (__atomic_load_n(&g_sys_mutexes[id - 1].waiters, __ATOMIC_SEQ_CST) != int(i + 1)) {
                    if (std::chrono::steady_clock::now() > deadline) std::abort();
                    std::this_thread::yield();
                }
            }
            if (call(owner, 96, id) != (u32)CELL_EBUSY || call(owner, 98, id)) std::abort();
            for (auto& worker : workers) worker.join();
            if (count != 3 || order[0] != (protocol == 1 ? 0u : 1u) ||
                order[1] != (protocol == 1 ? 1u : 0u) || order[2] != 2) return 9;
            if (call(owner, 99, id) || call(owner, 96, id) || call(owner, 99, id) != (u32)CELL_ESRCH) return 10;
        }
        vm_write32(0x31000, 0xCAFEBABE);
        if (call(owner, 95, 0x31000, 3) != (u32)CELL_EINVAL || vm_read32(0x31000) != 0xCAFEBABE) return 11;
        vm_shutdown(); puts("Kernel lwmutex: original control untouched, signal tokens, timeouts, FIFO/priority and lifetime verified");
        return 0;
    }
    if (argc == 2 && !strncmp(argv[1], "mutex_queue", 11)) {
        if (vm_init() || vm_commit(0x30000, 0x10000)) return 1;
        g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
        lv2_init_syscalls();
        ppu_context owner{}; owner.thread_id = 1;
        auto mutex_call = [](ppu_context& c, u32 number, u32 id, u64 value = 0) {
            c.gpr[11] = number; c.gpr[3] = id; c.gpr[4] = value;
            lv2_syscall(&c); return (u32)c.gpr[3];
        };
        const bool priority = !strcmp(argv[1], "mutex_queue_priority");
        const bool inherit = !strcmp(argv[1], "mutex_queue_inherit");
        vm_write32(0x31010, inherit ? 3 : priority ? 2 : 1);
        vm_write32(0x31014, 0x10); vm_write32(0x31018, 0x200); vm_write32(0x3101C, 0x2000);
        if (mutex_call(owner, 100, 0x31000, 0x31010)) return 2;
        const u32 id = vm_read32(0x31000);
        if (mutex_call(owner, 102, id) || mutex_call(owner, 102, id)) return 3;
        // These are fixture-owned guest identities; no host scheduling determines their order.
        for (unsigned i = 0; i < 4; ++i) {
            g_ppu_threads[i].state = PPU_THREAD_STATE_RUNNING;
            g_ppu_threads[i].ctx.thread_id = i + 1;
            g_ppu_threads[i].priority = i == 2 ? 100 : 1000;
        }
        ppu_context timed{}; timed.thread_id = 4;
        const auto before = std::chrono::steady_clock::now();
        if (!inherit && (mutex_call(timed, 102, id, 30) != (u32)CELL_ETIMEDOUT ||
            std::chrono::steady_clock::now() - before < std::chrono::microseconds(30) ||
            __atomic_load_n(&g_sys_mutexes[id - 1].waiters, __ATOMIC_SEQ_CST))) return 4;
        std::atomic<unsigned> order{0}; unsigned observed[3]{};
        std::thread waiters[3];
        for (unsigned i = 0; i < 3; ++i) {
            waiters[i] = std::thread([&, i] {
                ppu_context c{}; c.thread_id = i + 2;
                if (mutex_call(c, 102, id)) std::abort();
                observed[order.fetch_add(1)] = i;
                if (mutex_call(c, 104, id)) std::abort();
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (__atomic_load_n(&g_sys_mutexes[id - 1].waiters, __ATOMIC_SEQ_CST) != (int)i + 1) {
                if (std::chrono::steady_clock::now() > deadline) std::abort();
                std::this_thread::yield();
            }
        }
        if (mutex_call(owner, 101, id) != (u32)CELL_EBUSY || mutex_call(owner, 104, id) || order.load()) std::abort();
        if (mutex_call(owner, 104, id)) std::abort();
        for (auto& w : waiters) w.join();
        if (order != 3 || observed[0] != (priority ? 1u : 0u) ||
            observed[1] != (priority ? 0u : 1u) || observed[2] != 2) return 5;
        vm_write32(0x31060, 0x200);
        owner.gpr[11] = 105; owner.gpr[3] = 0x31050; owner.gpr[4] = id; owner.gpr[5] = 0x31060;
        lv2_syscall(&owner);
        if (owner.gpr[3] || mutex_call(owner, 102, id) || mutex_call(owner, 102, id)) return 6;
        const u32 condition = vm_read32(0x31050);
        std::thread signal([&] {
            ppu_context c{}; c.thread_id = 2;
            if (mutex_call(c, 102, id)) std::abort();
            c.gpr[3] = condition;
            if (sys_cond_signal(&c) || mutex_call(c, 104, id)) std::abort();
        });
        owner.gpr[3] = condition; owner.gpr[4] = 1000000;
        const auto signalled_result = sys_cond_wait(&owner);
        signal.join();
        if (signalled_result || g_sys_mutexes[id - 1].lock_count != 2 || g_sys_mutexes[id - 1].owner_tid != 1) return 7;
        owner.gpr[3] = condition; owner.gpr[4] = 30;
        if ((u32)sys_cond_wait(&owner) != (u32)CELL_ETIMEDOUT || g_sys_mutexes[id - 1].lock_count != 2) return 8;
        owner.gpr[11] = 106; owner.gpr[3] = condition; lv2_syscall(&owner);
        if (owner.gpr[3] || mutex_call(owner, 104, id) || mutex_call(owner, 104, id) || mutex_call(owner, 101, id)) return 9;
        puts("Verified guest mutex FIFO/priority ordering, recursive release, timed cancellation and retained lifetime");
        vm_shutdown(); return 0;
    }
    if (argc == 2 && !strncmp(argv[1], "video_clock", 11)) {
        u64 edge = UINT64_MAX;
        const u64 epoch = 50000000, period = 1331330, end = epoch + period * 60000u;
        if (!strcmp(argv[1], "video_clock_edge")) { cellGcmVideoTick(epoch); return 5; }
        if (cellGcmVideoNext(epoch, &edge) || edge != UINT64_MAX ||
            cellGcmVideoNext(epoch + period - 1, &edge) || !cellGcmVideoNext(epoch + period, &edge) ||
            edge != epoch + period || cellGcmVideoNext(edge, &edge)) return 1;
        if (!strcmp(argv[1], "video_clock_reverse")) { cellGcmVideoNext(edge - 1, &edge); return 2; }
        for (u64 i = 2; i <= 60000; ++i)
            if (!cellGcmVideoNext(end, &edge) || edge != epoch + i * period) return 3;
        if (edge != end || cellGcmVideoNext(edge, &edge)) return 4;
        puts("Verified RSX 60000/1001 video phase, sub-millisecond edges and lossless catch-up"); return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "process_sfo") || !strcmp(argv[1], "strncmp_invalid"))) {
        if (vm_init() || vm_commit(0x30000, 0x10000)) return 1;
        g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
        ppu_context info{}; info.gpr[1] = 0x3FF00;
        auto compare = [&](u64 a, u64 b, u64 n) {
            info.gpr[4] = b; info.gpr[5] = n; call(info, "_sys_strncmp", a); return (s32)info.gpr[3];
        };
        vm_write8(0x3FFFF, 'A'); vm_write8(0x3FFFE, 'B');
        if (!strcmp(argv[1], "strncmp_invalid")) { compare(0x3FFFF, 0x3FFFF, 2); return 2; }
        if (compare(0, 0, 0) || compare(0x3FFFF, 0x3FFFE, 2) >= 0 ||
            compare(0x3FFFE, 0x3FFFF, 2) <= 0 || compare(0x3FFFF, 0x3FFFF, 1)) return 6;
        vm_write32(0x31000, 0xA5A5A5A5);
        cellGame_set_title_id(""); call(info, "sys_process_get_paramsfo", 0x31000);
        if ((u32)info.gpr[3] != (u32)CELL_ENOENT || vm_read32(0x31000) != 0xA5A5A5A5) return 2;
        cellGame_set_title_id("TEST00001");
        for (u64 invalid : {0ull, 0x3FFE0ull, 0x100031000ull}) {
            call(info, "sys_process_get_paramsfo", invalid);
            if ((u32)info.gpr[3] != (u32)CELL_EFAULT) return 3;
        }
        vm_write32(0x30FFC, 0xA5A5A5A5); vm_write32(0x31040, 0xA5A5A5A5);
        call(info, "sys_process_get_paramsfo", 0x31000);
        if (info.gpr[3] || vm_read8(0x31000) || memcmp(vm_base + 0x31001, "TEST00001", 9) ||
            vm_read32(0x30FFC) != 0xA5A5A5A5 || vm_read32(0x31040) != 0xA5A5A5A5) return 4;
        for (u32 i = 10; i < 64; ++i) if (vm_read8(0x31000 + i)) return 5;
        vm_shutdown(); std::puts("Verified process SFO identity, 64-byte ABI and failed-call preservation"); return 0;
    }
    if (argc == 2 && !strncmp(argv[1], "mapper_named", 12)) {
        if (vm_init() || vm_commit(0x30000, 0x10000)) return 1;
        g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
        lv2_init_syscalls(); ppu_context memory{}; memory.gpr[1] = 0x3FF00;
        auto named = [&](u64 key, u64 size, u64 flags, u64 output = 0x31000) {
            memory.gpr[11] = 332; memory.gpr[3] = key; memory.gpr[4] = size;
            memory.gpr[5] = flags; memory.gpr[6] = output; lv2_syscall(&memory); return (u32)memory.gpr[3];
        };
        const u64 key = 0x123456789ABCDEF0ull;
        vm_write32(0x31000, 0xA5A5A5A5);
        if (!strcmp(argv[1], "mapper_named_pointer")) { named(key, 0x10000, 0xC200, 0x40000); return 2; }
        if (named(key, 0x10000, 0x200) != (u32)CELL_ESRCH || vm_read32(0x31000) != 0xA5A5A5A5) return 3;
        memory.gpr[11] = 352; memory.gpr[3] = 0x31010; lv2_syscall(&memory);
        if (memory.gpr[3]) return 4;
        const u32 available = vm_read32(0x31014);
        if (named(key, 0x10000, 0xC200) || !vm_read32(0x31000)) return 5;
        const u32 id = vm_read32(0x31000);
        if (named(key, 0x10000, 0xC200) != (u32)CELL_EEXIST || vm_read32(0x31000) != id ||
            named(key, 0x10000, 0x200) != (u32)CELL_EEXIST ||
            named(key, 0x20000, 0x200) != (u32)CELL_EPERM ||
            named(key, 0x100000, 0x400) != (u32)CELL_EPERM ||
            named(key, 0x20000, 0xC200) != (u32)CELL_EEXIST ||
            named(key + 1, 0x10000, 0x200) != (u32)CELL_ESRCH) return 6;
        memory.gpr[11] = 352; memory.gpr[3] = 0x31010; lv2_syscall(&memory);
        if (memory.gpr[3] || vm_read32(0x31014) != available - 0x10000) return 7;
        if (named(key + 1, 0x10000, 0x8200) || vm_read32(0x31000) == id) return 8;
        if (named(0xFFFF000000000000ull, 0x10000, 0x200)) return 9;
        const u32 first_anonymous = vm_read32(0x31000);
        if (named(0xFFFF000000000000ull, 0x10000, 0x200) || vm_read32(0x31000) == first_anonymous) return 10;
        if (named(key + 2, 0, 0x8200) != (u32)CELL_EALIGN ||
            named(key + 2, 1, 0x8200) != (u32)CELL_EALIGN ||
            named(key + 2, 0x10000, 0xA200) != (u32)CELL_EINVAL) return 11;
        vm_shutdown(); puts("Verified named mapper creation modes, missing names, geometry errors, quotas and independent anonymous objects"); return 0;
    }
    if (argc == 2 && (!strncmp(argv[1], "rsx_memory", 10) || !strncmp(argv[1], "rsx_context", 11))) {
        if (vm_init() || vm_commit(0x30000, 0x10000)) return 1;
        g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
        lv2_init_syscalls(); ppu_context memory{}; memory.gpr[1] = 0x3FF00;
        auto allocate = [&](u64 handle, u64 address, u64 size) {
            memory.gpr[11] = 668; memory.gpr[3] = handle; memory.gpr[4] = address; memory.gpr[5] = size;
            memory.gpr[6] = !strcmp(argv[1], "rsx_memory_attributes") ? 1 : 0x80000;
            memory.gpr[7] = 0x300000; memory.gpr[8] = 15; memory.gpr[9] = 8;
            lv2_syscall(&memory); return (u32)memory.gpr[3];
        };
        vm_write32(0x31000, 0xA5A5A5A5); vm_write64(0x31010, 0xA5A5A5A5A5A5A5A5ull);
        if (!strcmp(argv[1], "rsx_memory_query")) { allocate(0x31000, 0x31010, 0); return 2; }
        if (!strcmp(argv[1], "rsx_memory_attributes")) { allocate(0x31000, 0x31010, 0x100000); return 2; }
        for (u64 invalid : {0ull, 0xFFFFFFFEull, 0x100031000ull})
            if (allocate(invalid, 0x31010, 0x100000) != (u32)CELL_EFAULT) return 3;
        if (allocate(0x31000, 0x100031010ull, 0x100000) != (u32)CELL_EFAULT ||
            allocate(0x31000, 0x31000, 0x100000) != (u32)CELL_EFAULT) return 4;
        for (u64 invalid : {1ull, 0x100001ull, 0x10100000ull, 0x100000000ull})
            if (allocate(0x31000, 0x31010, invalid) != (u32)-17 || vm_read32(0x31000) != 0xA5A5A5A5 ||
                vm_read64(0x31010) != 0xA5A5A5A5A5A5A5A5ull) return 5;
        if (allocate(0x31000, 0x31010, 0x200000) || !vm_read32(0x31000)) return 6;
        const u64 local = vm_read64(0x31010);
        if (local > UINT32_MAX || !ps3_native_guest_range_backed((u32)local, 0x200000) ||
            ps3_native_guest_range_backed((u32)local, 0x200001) || vm_read32((u32)local) ||
            vm_read32((u32)local + 0x1FFFFC)) return 7;
        vm_write32((u32)local + 0x1FFFFC, 0xCAFEBABE);
        if (!strncmp(argv[1], "rsx_context", 11)) {
            const u32 handle = vm_read32(0x31000);
            g_ps3_guest_caller = callback;
            cellGcmSetVBlankHandler((CellGcmVBlankHandler)(uintptr_t)0x37000);
            memory.gpr[11] = 675; memory.gpr[3] = 0x31140; memory.gpr[4] = 0; memory.gpr[5] = 8;
            lv2_syscall(&memory);
            if (memory.gpr[3] || vm_read64(0x31140) != VM_RSX_DEVICE_BASE || vm_read32(VM_RSX_DEVICE_BASE + 0x30)) return 32;
            auto context = [&](u64 output, u64 object, u64 flags) {
                memory.gpr[11] = 670; memory.gpr[3] = output;
                memory.gpr[4] = 0x31110; memory.gpr[5] = 0x31120; memory.gpr[6] = 0x31130;
                memory.gpr[7] = object; memory.gpr[8] = flags;
                lv2_syscall(&memory); return (u32)memory.gpr[3];
            };
            vm_write32(0x31100, 0xA5A5A5A5);
            if (ps3_native_guest_range_backed(VM_RSX_CONTEXT_BASE, 4) ||
                context(0x100031100ull, handle, 0) != (u32)CELL_EFAULT ||
                context(0x31110, handle, 0) != (u32)CELL_EFAULT ||
                context(0x31100, handle + 1, 0) != (u32)CELL_EINVAL ||
                vm_read32(0x31100) != 0xA5A5A5A5) return 9;
            if (!strcmp(argv[1], "rsx_context_flags")) { context(0x31100, handle, 0x10); return 2; }
            if (context(0x31100, handle, 0x20) || !vm_read32(0x31100)) return 10;
            if (vm_read32(VM_RSX_DEVICE_BASE + 0x30) != 1) return 33;
            if (!strcmp(argv[1], "rsx_context_multiple")) { context(0x31100, handle, 0); return 2; }
            const u32 driver = VM_RSX_CONTEXT_BASE + 0x100000, reports = driver + 0x100000;
            const u32 queue = vm_read32(driver + 0x12D0);
            if (vm_read64(0x31110) != VM_RSX_CONTEXT_BASE || vm_read64(0x31120) != driver ||
                vm_read64(0x31130) != reports || !ps3_native_guest_range_backed(VM_RSX_CONTEXT_BASE, VM_RSX_CONTEXT_SIZE) ||
                ps3_native_guest_range_backed(VM_RSX_CONTEXT_BASE, VM_RSX_CONTEXT_SIZE + 1) ||
                vm_read32(driver) != 0x211 || vm_read32(driver + 8) != 0x200000 ||
                vm_read32(driver + 0x2C) != 0x1000 || vm_read32(driver + 0x34) != 0x1400 || vm_read32(driver + 0x54) != 0x20 ||
                !queue ||
                cellGcm_control_guest_addr() != VM_RSX_CONTEXT_BASE + 0x40 ||
                vm_read64(reports + 0x1400) != UINT64_MAX || vm_read32(reports + 0x1408) ||
                vm_read32(reports + 0x140C) != UINT32_MAX) return 11;
            rsx_state state{}; rsx_state_init(&state);
            rsx_backend semaphore_backend{}; semaphore_backend.submit = synthetic_submit;
            if (strcmp(argv[1], "rsx_context_semaphore_backend")) rsx_set_backend(&semaphore_backend);
            synthetic_semaphore_target = reports + 0x20;
            vm_write32(synthetic_semaphore_target, 0xA5A5A5A5);
            vm_write32(VM_HLE_INJECT_BASE + 0x20, 0xA5A5A5A5);
            rsx_process_method(&state, 0x1D6C, 0x20);
            rsx_process_method(&state, 0x1D74, 0x12345678);
            if (vm_read32(reports + 0x20) != 0x12345678 ||
                vm_read32(VM_HLE_INJECT_BASE + 0x20) != 0xA5A5A5A5 || synthetic_semaphore_early || synthetic_submits != 1) return 13;
            synthetic_semaphore_target = 0; synthetic_submits = 0; rsx_set_backend(nullptr);
            vm_write32(driver + 0x12C0, 0x80);
            rsx_raise_user_cmd(0x12345678);
            memory.gpr[11] = 130; memory.gpr[3] = queue; memory.gpr[4] = 0x31200; memory.gpr[5] = 1000;
            lv2_syscall(&memory);
            if (memory.gpr[3] || !memory.gpr[4] || memory.gpr[5] || memory.gpr[6] != 0x80 ||
                memory.gpr[7] || vm_read32(driver + 0x12CC) != 0x12345678) return 12;
            memory.gpr[11] = 348; memory.gpr[3] = 0x100000; memory.gpr[4] = 0x400; memory.gpr[5] = 0x31210;
            lv2_syscall(&memory);
            if (memory.gpr[3]) return 14;
            const u32 mapped = vm_read32(0x31210), id = vm_read32(0x31100);
            auto iomap = [&](u64 object, u64 io, u64 ea, u64 size, u64 flags) {
                memory.gpr[11] = 672; memory.gpr[3] = object; memory.gpr[4] = io;
                memory.gpr[5] = ea; memory.gpr[6] = size; memory.gpr[7] = flags;
                lv2_syscall(&memory); return (u32)memory.gpr[3];
            };
            const u64 attributes = 0xE000000000000800ull;
            if (iomap(id + 1, 0, mapped, 0x100000, attributes) != (u32)CELL_EINVAL ||
                iomap(id, 1, mapped, 0x100000, attributes) != (u32)CELL_EINVAL ||
                iomap(id, 0, mapped + 1, 0x100000, attributes) != (u32)CELL_EINVAL ||
                iomap(id, 0, 0x100000000ull + mapped, 0x100000, attributes) != (u32)CELL_EINVAL ||
                iomap(id, 0x10000000, mapped, 0x100000, attributes) != (u32)CELL_EINVAL ||
                iomap(id, 0, mapped, 0x100001, attributes) != (u32)CELL_EINVAL ||
                iomap(id, 0, 0x59900000, 0x100000, attributes) != (u32)CELL_EINVAL || gcm_io_offset_to_ea(0)) return 15;
            if (!strcmp(argv[1], "rsx_context_iomap_flags")) { iomap(id, 0, mapped, 0x100000, 1); return 2; }
            if (iomap(id, 0, mapped, 0x100000, attributes) || gcm_io_offset_to_ea(0xFFFFC) != mapped + 0xFFFFC ||
                gcm_io_offset_to_ea(0x100000) || vm_read64(reports + 0x1000) != UINT64_MAX) return 16;
            if (!strcmp(argv[1], "rsx_context_iomap_overlap")) { iomap(id, 0, mapped, 0x100000, attributes); return 2; }
            auto attribute = [&](u64 object, u64 packet, u64 get, u64 put, u64 value = 0) {
                memory.gpr[11] = 674; memory.gpr[3] = object; memory.gpr[4] = packet;
                memory.gpr[5] = get; memory.gpr[6] = put; memory.gpr[7] = value; memory.gpr[8] = 0;
                lv2_syscall(&memory); return (u32)memory.gpr[3];
            };
            if (attribute(id + 1, 1, 4, 8) != (u32)CELL_EINVAL ||
                attribute(id, 1, 1, 8) != (u32)CELL_EINVAL ||
                attribute(id, 1, 0x100000000ull, 8) != (u32)CELL_EINVAL ||
                attribute(id, 1, 0x100000, 8) != (u32)CELL_EINVAL ||
                vm_read64(VM_RSX_CONTEXT_BASE + 0x40)) return 17;
            if (!strcmp(argv[1], "rsx_context_attribute")) { attribute(id, 0x106, 0, 0); return 2; }
            auto sync_mode = [&](u64 head, u64 selector) {
                memory.gpr[11] = 677; memory.gpr[3] = 0x202; memory.gpr[4] = head;
                memory.gpr[5] = selector; memory.gpr[6] = 0; memory.gpr[7] = 0;
                lv2_syscall(&memory); return memory.gpr[3];
            };
            if (!strcmp(argv[1], "rsx_context_device_attribute")) { sync_mode(1, 2); return 2; }
            if (sync_mode(0, 1) != 2 || sync_mode(1, 1) != 2 || sync_mode(2, 1) != 255) return 20;
            memory.gpr[11] = 677; memory.gpr[3] = 2; memory.gpr[4] = 1;
            memory.gpr[5] = !strcmp(argv[1], "rsx_context_alternate_timing") ? 1 : 0;
            memory.gpr[6] = memory.gpr[7] = 0; lv2_syscall(&memory);
            if (memory.gpr[3]) return 21;
            if (!strcmp(argv[1], "rsx_context_sync_mode")) { attribute(id, 0x101, 1, 1); return 2; }
            if (attribute(id, 0x101, 1, 2) || sync_mode(1, 1) != 2 ||
                attribute(id, 0x101, 2, 2) != (u32)-17) return 22;
            const u64 tile_range = (1ull << 32) | 0xF0000, tile_format = (0x200ull << 32) | 0x4001E000;
            if (attribute(id, 0x300, 0, tile_range, tile_format) || !cellGcmGetTileInfo(0)->bound ||
                cellGcmGetTileInfo(0)->offset || cellGcmGetTileInfo(0)->size != 0x100000 ||
                cellGcmGetTileInfo(0)->pitch != 0x200 || cellGcmGetTileInfo(0)->format) return 23;
            if (attribute(id, 0x300, 15, tile_range, tile_format) != (u32)-17 ||
                attribute(id, 0x300, 0, tile_range, tile_format | 0x800) != (u32)-17 ||
                !cellGcmGetTileInfo(0)->bound || cellGcmGetTileInfo(0)->size != 0x100000) return 24;
            if (!strcmp(argv[1], "rsx_context_tile_compressed")) { attribute(id, 0x300, 0, tile_range, tile_format | (7ull << 26)); return 2; }
            if (attribute(id, 0x300, 0, 0, 0) || cellGcmGetTileInfo(0)->bound) return 25;
            const u64 main_tile_range = (0x80000002ull << 32) | 0x800F0000;
            rsx_tile main_tile{};
            if (attribute(id, 0x300, 0, main_tile_range, tile_format) ||
                !rsx_tiling_find(1, 0x40, 16, &main_tile) || main_tile.size != 0x100000 || main_tile.pitch != 0x200 ||
                attribute(id, 0x300, 0, 0, 0) || rsx_tiling_find(1, 0x40, 16, &main_tile)) return 26;
            auto zcull = [&](u64 slot, u64 range, u64 offset, u64 status) {
                memory.gpr[11] = 674; memory.gpr[3] = id; memory.gpr[4] = 0x301;
                memory.gpr[5] = slot; memory.gpr[6] = range; memory.gpr[7] = offset; memory.gpr[8] = status;
                lv2_syscall(&memory); return (u32)memory.gpr[3];
            };
            const u64 region_range = 0x2100400040ull, region_status = 0xFF80500400202000ull;
            if (zcull(8, region_range, 0x20000, region_status) != (u32)-17 ||
                zcull(0, region_range | 1, 0x20000, region_status) != (u32)-17 ||
                zcull(0, region_range, 0x20000, region_status | 1) != (u32)-17) return 60;
            semaphore_backend.set_zcull = !strcmp(argv[1], "rsx_context_zcull_backend") ? nullptr : synthetic_zcull;
            rsx_set_backend(&semaphore_backend);
            if (zcull(0, region_range, 0x20001, region_status) != (u32)-17 ||
                zcull(0, region_range, 3ull * 1024 * 1024 << 32, region_status) != (u32)-17 ||
                zcull(0, region_range, 0x20000, (region_status & ~(15ull << 44)) | (8ull << 44)) != (u32)-17 ||
                zcull(0, region_range, 0x20000, (region_status & ~(0x3FFull << 34)) | (2ull << 34)) != (u32)-17 ||
                synthetic_zcull_calls || rsx_zcull_get(0)->bound) return 61;
            if (zcull(0, region_range, 0x20000, region_status) || synthetic_zcull_early || synthetic_zcull_calls != 1 ||
                rsx_zcull_get(0) != cellGcmGetZcullInfo(0) || !rsx_zcull_get(0)->bound ||
                rsx_zcull_get(0)->offset != 0x20000 || rsx_zcull_get(0)->width != 64 ||
                rsx_zcull_get(0)->height != 64 || rsx_zcull_get(0)->sRef != 128 || rsx_zcull_get(0)->sMask != 255) return 62;
            if (!strcmp(argv[1], "rsx_context_zcull_overlap")) { zcull(1, region_range, 0x30000, region_status); return 2; }
            synthetic_zcull_old_bound = 1; synthetic_zcull_old_offset = 0x20000;
            if (zcull(0, region_range, 0x20000, region_status & 0xFFFFFFFF00000000ull) || synthetic_zcull_early ||
                rsx_zcull_get(0)->bound || synthetic_zcull_calls != 2 || rsx_zcull_get(8)) return 63;
            synthetic_zcull_old_bound = 0; synthetic_zcull_old_offset = 0;
            const u64 stencil_never = region_status & ~(15ull << 44);
            if (zcull(7, region_range, (8192ull << 32) | 0x40000, stencil_never) ||
                rsx_zcull_get(7)->sFunc || !rsx_zcull_get(7)->bound || synthetic_zcull_calls != 3) return 64;
            synthetic_zcull_old_bound = 1; synthetic_zcull_old_offset = 0x40000;
            if (zcull(7, region_range, (8192ull << 32) | 0x40000, stencil_never & 0xFFFFFFFF00000000ull) ||
                rsx_zcull_get(7)->bound || synthetic_zcull_calls != 4) return 65;
            synthetic_submits = 0; rsx_set_backend(nullptr);
            if (attribute(id, 0x104, 0, (64ull << 32) | 64, 256ull << 32)) return 27;
            auto* display = cellGcmGetDisplayBufferByFlipIndex(0);
            if (!display || display->width != 64 || display->height != 64 || display->pitch != 256 || display->offset ||
                !attribute(id, 0x104, 0, (65ull << 32) | 64, 256ull << 32) ||
                !attribute(id, 0x104, 0, (64ull << 32) | 0xFFFFFFFFu, 256ull << 32) ||
                display->width != 64 || display->height != 64) return 28;
            rsx_set_backend(&semaphore_backend);
            const u32 commands[] = {0x40064, 0x30, 0x40068, 1, 0x40064, 0x40, 0x4006C, 2};
            for (u32 i = 0; i < 8; ++i) vm_write32(mapped + 0x1000 + i * 4, commands[i]);
            vm_write32(reports + 0x30, 0); vm_write32(reports + 0x40, 0);
            if (attribute(id, 1, 0x1000, 0x1020)) return 29;
            cellGcm_rsx_process_fifo();
            if (vm_read32(VM_RSX_CONTEXT_BASE + 0x44) != 0x1008 || vm_read32(reports + 0x40)) return 30;
            vm_write32(reports + 0x30, 1); cellGcm_rsx_process_fifo();
            if (vm_read32(VM_RSX_CONTEXT_BASE + 0x44) != 0x1020 || vm_read32(reports + 0x40) != 2 ||
                attribute(id, 1, 0, 0)) return 31;
            const u32 dma_commands[] = {0x40060, 0x56616661, 0x40064, 0x30, 0x40068, 1,
                0x40060, 0x66616661, 0x40064, 0x30, 0x40068, 2, 0x40064, 0x40, 0x4006C, 3};
            for (u32 i = 0; i < 16; ++i) vm_write32(mapped + 0x1000 + i * 4, dma_commands[i]);
            if (!strcmp(argv[1], "rsx_context_semaphore_dma")) vm_write32(mapped + 0x1004, 0xDEADBEEF);
            if (attribute(id, 1, 0x1000, 0x1040)) return 34;
            cellGcm_rsx_process_fifo();
            if (vm_read32(VM_RSX_CONTEXT_BASE + 0x44) != 0x1028 || vm_read32(reports + 0x40) != 2 ||
                vm_read32(VM_RSX_DEVICE_BASE + 0x30) != 1) return 35;
            vm_write32(reports + 0x30, 2); cellGcm_rsx_process_fifo();
            if (vm_read32(VM_RSX_CONTEXT_BASE + 0x44) != 0x1040 || vm_read32(reports + 0x40) != 3 ||
                vm_read32(VM_RSX_DEVICE_BASE + 0x40) || attribute(id, 1, 0, 0)) return 36;
            vm_write32(mapped + 0x1000, 0x40050); vm_write32(mapped + 0x1004, 0x12345678);
            vm_write32(mapped + 0x1008, 0x40050); vm_write32(mapped + 0x100C, 0x87654321);
            if (attribute(id, 1, 0x1000, 0x1010)) return 51;
            synthetic_reference_control = VM_RSX_CONTEXT_BASE + 0x40;
            vm_write32(synthetic_reference_control + 8, 0xA5A5A5A5);
            if (!strcmp(argv[1], "rsx_context_reference_backend")) semaphore_backend.submit = nullptr;
            cellGcm_rsx_process_fifo(); cellGcm_rsx_process_fifo();
            if (synthetic_reference_early || synthetic_reference_checks != 2 ||
                vm_read32(synthetic_reference_control + 4) != 0x1010 || vm_read32(synthetic_reference_control + 8) != 0x87654321) return 52;
            synthetic_reference_control = 0;
            if (attribute(id, 1, 0, 0)) return 53;
            if (!strcmp(argv[1], "rsx_context_device_release")) {
                const u32 bad_release[] = {0x40060, 0x56616661, 0x40064, 0x30, 0x4006C, 0};
                for (u32 i = 0; i < 6; ++i) vm_write32(mapped + 0x1000 + i * 4, bad_release[i]);
                attribute(id, 1, 0x1000, 0x1018); cellGcm_rsx_process_fifo(); return 37;
            }
            if (!strcmp(argv[1], "rsx_context_main_surface")) {
                vm_write32(mapped + 0x1000, 0x40194); vm_write32(mapped + 0x1004, 0xFEED0001);
                attribute(id, 1, 0x1000, 0x1008); cellGcm_rsx_process_fifo(); return 50;
            }
            if (attribute(id, 0x104, 1, (64ull << 32) | 64, (256ull << 32) | 0x10000)) return 38;
            const u32 original_flip[] = {0x4E944, 1, 0x40060, 0x56616661, 0x40064, 0x30, 0x4006C, 0,
                0x40064, 0x30, 0x40068, 1, 2, 0x40064, 0x10, 0x4006C, UINT32_MAX, 0x4E924, 0x8000010F};
            for (u32 i = 0; i < 19; ++i) vm_write32(mapped + 0x1000 + i * 4, original_flip[i]);
            vm_write32(mapped, 0x40060); vm_write32(mapped + 4, 0x66616661);
            vm_write32(mapped + 8, 0x40194); vm_write32(mapped + 12, 0xFEED0000); vm_write32(mapped + 16, 0x20000);
            vm_write32(driver + 0x12C0, 0x52);
            for (u32 i = 0; i < 16; i += 4) vm_write32(reports + 0x10 + i, 0xA5A5A5A5);
            semaphore_backend.scanout = synthetic_scanout;
            synthetic_scanout_reports = reports; synthetic_scanout_driver = driver;
            u64 edge = 0;
            const u64 epoch = ppu_timebase_now(), period = 1331330;
            if (cellGcmVideoNext(epoch, &edge) || attribute(id, 1, 0x1000, 0x104C)) return 39;
            const unsigned before_queue = synthetic_submits;
            synthetic_queue_delay = true;
            if (!strcmp(argv[1], "rsx_context_queue_backend")) semaphore_backend.submit = nullptr;
            cellGcm_rsx_process_fifo();
            const u64 queue_finished = ppu_timebase_now();
            if (synthetic_submits != before_queue + 1 || vm_read32(VM_RSX_CONTEXT_BASE + 0x44) != 0x1028 ||
                vm_read32(VM_RSX_DEVICE_BASE + 0x30) || vm_read32(reports + 0x10) != 0xA5A5A5A5 ||
                vm_read32(driver + 0x1100) != 0x40000002 || synthetic_scanout_calls) return 40;
            auto receive = [&](u64 flags) {
                memory.gpr[11] = 130; memory.gpr[3] = queue; memory.gpr[4] = 0x31200; memory.gpr[5] = 1000;
                lv2_syscall(&memory); return !memory.gpr[3] && memory.gpr[6] == flags;
            };
            if (!receive(0x40) || cellGcmVideoNext(epoch + period - 1, &edge)) return 41;
            cellGcm_rsx_process_fifo();
            if (vm_read32(VM_RSX_DEVICE_BASE + 0x30)) return 42;
            if (!strcmp(argv[1], "rsx_context_queue_metadata")) attribute(id, 0x104, 1, (64ull << 32) | 64, (256ull << 32) | 0x10000);
            u64 ticks = 0;
            while (cellGcmVideoNext(ppu_timebase_now() + period, &edge)) {
                cellGcmVideoTick(edge); ++ticks;
                if (!receive(2) || (edge <= synthetic_queue_completed && vm_read32(VM_RSX_DEVICE_BASE + 0x30))) return 48;
                if (vm_read32(VM_RSX_DEVICE_BASE + 0x30)) break;
                if (ticks > 240) return 49;
            }
            if (edge <= synthetic_queue_completed || edge > queue_finished + period ||
                vm_read64(driver + 0x1118) != edge || vm_read64(driver + 0x1128) != ticks ||
                vm_read32(driver + 0x1114) != (u32)edge || vm_read32(driver + 0x1134) != (u32)(edge >> 32) ||
                vm_read32(VM_RSX_DEVICE_BASE + 0x30) != 1) return 43;
            ppu_gcm_pump(); if (gcm_callbacks) return 54;
            if (!strcmp(argv[1], "rsx_context_scanout_backend")) semaphore_backend.scanout = nullptr;
            cellGcm_rsx_process_fifo();
            if (vm_read32(VM_RSX_CONTEXT_BASE + 0x44) != 0x1044 || synthetic_scanout_calls != 1 ||
                vm_read32(reports + 0x10) != UINT32_MAX || (vm_read32(driver + 0x1100) & 0x80000000u) ||
                cellGcm_original_frames_presented()) return 44;
            synthetic_scanout_result = 1; cellGcm_rsx_process_fifo();
            if (!receive(0x10) || synthetic_scanout_calls != 2 || synthetic_scanout_early ||
                vm_read32(VM_RSX_CONTEXT_BASE + 0x44) != 0x104C || vm_read32(driver + 0x1108) != 1 ||
                vm_read64(driver + 0x10F8) != edge || vm_read32(driver + 0x1100) != 0xC0000002 ||
                cellGcm_original_frames_presented() != 1) return 45;
            for (u32 i = 0; i < 16; i += 4) if (vm_read32(reports + 0x10 + i)) return 46;
            if (attribute(id, 1, 0, 0)) return 47;
            rsx_set_backend(nullptr); synthetic_submits = 0;
            const u32 flip_status = driver + 0x10C0;
            if (attribute(id, 0x10A, 0, 0, 0x80000000) || vm_read32(flip_status) != 0x80000000 ||
                attribute(id, 0x10A, 0, 0x80000000, 0x123) || vm_read32(flip_status) != 0x80000123 ||
                attribute(id, 0x10A, 2, 0, 0) != (u32)-17 || vm_read32(flip_status) != 0x80000123 ||
                attribute(id, 0x10A, 0, 0x100000000ull, 0) != (u32)-17 || vm_read32(flip_status) != 0x80000123) return 19;
            rsx_backend fifo_backend{}; fifo_backend.submit = synthetic_submit;
            rsx_set_backend(&fifo_backend);
            if (attribute(id, 1, 4, 8) || vm_read32(VM_RSX_CONTEXT_BASE + 0x40) != 8 ||
                vm_read32(VM_RSX_CONTEXT_BASE + 0x44) != 4 || synthetic_submits != 1 || synthetic_report_early) return 18;
            if (!strcmp(argv[1], "rsx_context_fifo_pending")) { attribute(id, 1, 0, 0); return 2; }
            rsx_set_backend(nullptr);
            vm_shutdown(); std::puts("Verified RSX context backing, ABI, real interrupt delivery and shared control"); return 0;
        }
        vm_write32(0x31004, 0xA5A5A5A5);
        if (allocate(0x31004, 0x31020, 0x100000) != (u32)CELL_ENOMEM || vm_read32(0x31004) != 0xA5A5A5A5 ||
            vm_read32((u32)local + 0x1FFFFC) != 0xCAFEBABE) return 8;
        vm_shutdown(); std::puts("Verified real RSX local allocation, capacity, backing, endian outputs and failed-call preservation");
        return 0;
    }
    if (argc == 2 && !std::strcmp(argv[1], "audio_host")) {
        if (vm_init()) return 1;
        if (vm_commit(0x30000, 0x10000)) return 4;
        g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
        ps3_load_prx_modules();
        ppu_context audio{};
        audio.gpr[1] = 0x3FF00;
        const u32 param = 0x31000, output = 0x31100, config = 0x31204;
        for (unsigned cycle = 0; cycle < 2; ++cycle) {
            if (cellAudioInit() || (u32)cellAudioInit() != (u32)CELL_AUDIO_ERROR_ALREADY_INIT) return 2;
            vm_write64(param, 3); vm_write64(param + 8, 8); vm_write64(param + 16, 0);
            vm_write64(param + 24, 0); vm_write32(output, 0xA5A5A5A5);
            audio.gpr[4] = output; call(audio, "cellAudioPortOpen", param);
            if ((u32)audio.gpr[3] != (u32)CELL_AUDIO_ERROR_PARAM || vm_read32(output) != 0xA5A5A5A5) return 5;
            u32 first_buffer = 0;
            for (u32 port = 0; port < 8; ++port) {
                vm_write64(param, port & 1 ? 2 : 8);
                audio.gpr[4] = output; call(audio, "cellAudioPortOpen", param);
                if (audio.gpr[3] || vm_read32(output) != port) return 6;
                vm_write32(config - 4, 0xA5A5A5A5); vm_write32(config + 32, 0xA5A5A5A5);
                audio.gpr[4] = config; call(audio, "cellAudioGetPortConfig", port);
                const u32 buffer = vm_read32(config + 28), bytes = vm_read32(config + 24), index = vm_read32(config);
                if (audio.gpr[3] || vm_read32(config - 4) != 0xA5A5A5A5 || vm_read32(config + 32) != 0xA5A5A5A5 ||
                    vm_read32(config + 4) != 1 || vm_read64(config + 8) != (port & 1 ? 2u : 8u) ||
                    vm_read64(config + 16) != 8 || bytes != (port & 1 ? 16384u : 65536u) ||
                    !ps3_native_guest_range_backed(buffer, bytes) || ps3_native_guest_range_backed(buffer + bytes, 4) ||
                    !ps3_native_guest_range_backed(index, 8) || vm_read64(index)) return 7;
                if (!port) first_buffer = buffer;
            }
            vm_write32(output, 0xA5A5A5A5);
            audio.gpr[4] = output; call(audio, "cellAudioPortOpen", param);
            if ((u32)audio.gpr[3] != (u32)CELL_AUDIO_ERROR_PORT_FULL || vm_read32(output) != 0xA5A5A5A5) return 8;
            call(audio, "cellAudioPortStart", 0);
            if (audio.gpr[3]) return 11;
            audio.gpr[4] = config; call(audio, "cellAudioGetPortConfig", 0);
            if (audio.gpr[3] || vm_read32(config + 4) != 2) return 12;
            const u32 read_index = vm_read32(config);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (!vm_read64(read_index) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (!vm_read64(read_index) || vm_read64(read_index) >= 8) return 13;
            call(audio, "cellAudioPortStop", 0);
            if (audio.gpr[3]) return 14;
            const u64 stopped_index = vm_read64(read_index);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (vm_read64(read_index) != stopped_index) return 15;
            for (u32 port = 0; port < 8; ++port) {
                call(audio, "cellAudioPortClose", port);
                if (audio.gpr[3]) return 9;
            }
            if (ps3_native_guest_range_backed(first_buffer, 4)) return 10;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (cellAudioQuit() || (u32)cellAudioQuit() != (u32)CELL_AUDIO_ERROR_NOT_INIT) return 3;
        }
        vm_shutdown();
        std::puts("Verified real host audio consumption/start/stop and eight guest ports twice; no game PCM supplied");
        return 0;
    }
    ppu_context ctx{};
    ctx.gpr[1] = 0xD00FFFF0;
    if (argc == 2 && (!strcmp(argv[1], "language") || !strcmp(argv[1], "language_other") || !strcmp(argv[1], "enter_button"))) {
        if (vm_init()) return 1;
        g_ppu_main_stack_addr = 0xD0100000; g_ppu_main_stack_size = 0x20000;
        const u32 output = 0xD0101004;
        for (u32 a = output - 4; a <= output + 4; a += 4) vm_write32(a, 0xA5A5A5A5);
        ctx.gpr[4] = output;
        const bool button = !strcmp(argv[1], "enter_button");
        const u32 preference = button ? 0x112 : !strcmp(argv[1], "language_other") ? 0x116 : 0x111;
        call(ctx, "cellSysutilGetSystemParamInt", preference);
        const auto language = vm_read32(output);
        if (ctx.gpr[3] || language > (button ? 1u : 19u) || vm_read32(output - 4) != 0xA5A5A5A5 ||
            vm_read32(output + 4) != 0xA5A5A5A5) return 1;
        std::printf("[language-value] %u\n", language);
        for (const u64 bad : {0ull, 0xFFFFFFFEull, 0x1D0101004ull}) {
            ctx.gpr[4] = bad; call(ctx, "cellSysutilGetSystemParamInt", preference);
            if ((u32)ctx.gpr[3] != (u32)CELL_SYSUTIL_ERROR_VALUE || vm_read32(output) != language) return 2;
        }
        vm_shutdown(); return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "tty") || !strcmp(argv[1], "tty_channel"))) {
        if (vm_init()) return 1;
        lv2_init_syscalls();
        g_ppu_main_stack_addr = 0xD0100000; g_ppu_main_stack_size = 0x20000;
        const u32 data = 0xD0102000, count = 0xD0101000, size = 0x4011;
        for (u32 i = 0; i < size; ++i) vm_base[data + i] = (uint8_t)(i % 251);
        vm_write32(count - 4, 0xA5A5A5A5); vm_write32(count + 4, 0xA5A5A5A5);
        ctx.gpr[11] = 403; ctx.gpr[3] = !strcmp(argv[1], "tty_channel") ? 2 : 0;
        ctx.gpr[4] = data; ctx.gpr[5] = size; ctx.gpr[6] = count;
        lv2_syscall(&ctx);
        if (ctx.gpr[3] || vm_read32(count) != size || vm_read32(count - 4) != 0xA5A5A5A5 ||
            vm_read32(count + 4) != 0xA5A5A5A5) return 1;
        ctx.gpr[3] = 0; ctx.gpr[4] = 0x1D0102000ull; lv2_syscall(&ctx);
        if ((u32)ctx.gpr[3] != (u32)CELL_EFAULT || vm_read32(count) != size) return 2;
        ctx.gpr[3] = 0; ctx.gpr[4] = data; ctx.gpr[6] = 0xFFFFFFFE; lv2_syscall(&ctx);
        if ((u32)ctx.gpr[3] != (u32)CELL_EFAULT || vm_read32(count) != size) return 3;
        ctx.gpr[3] = 16; ctx.gpr[6] = count; lv2_syscall(&ctx);
        if ((u32)ctx.gpr[3] != (u32)CELL_EINVAL) return 4;
        ctx.gpr[3] = 0; ctx.gpr[4] = 0; ctx.gpr[5] = 0; lv2_syscall(&ctx);
        if (ctx.gpr[3] || vm_read32(count)) return 5;
        vm_shutdown();
        return 0;
    }
    if (argc >= 2 && !strncmp(argv[1], "fs", 2)) {
        if (!strcmp(argv[1], "fs_not_ready")) {
            call(ctx, "cellSysmoduleLoadModule", 0xE);
            return 1;
        }
        if (argc != 3 || vm_init() != 0) return 1;
        lv2_init_syscalls();
        g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
        ctx.gpr[1] = 0xD00FFFF0;
        ppu_vfs_root = argv[2];
        ppu_fs_register();
        if (!strncmp(argv[1], "fs_data", 7)) {
            ps3_load_prx_modules();
            g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
            const bool broken = !strcmp(argv[1], "fs_data_broken");
            const bool escape = !strcmp(argv[1], "fs_data_escape");
            const bool present = !strcmp(argv[1], "fs_data_present") || !strcmp(argv[1], "fs_data_size");
            std::strcpy((char*)vm_base + 0x30300, escape ? "../outside" : "TEST00001");
            std::memset(vm_base + 0x30400, 0xA5, 20);
            ctx.gpr[4] = 0x30300; ctx.gpr[5] = !strcmp(argv[1], "fs_data_present") ? 0 : 0x30404;
            call(ctx, "cellGameDataCheck", 3);
            const u32 expected = escape ? 0x8002CB07u : broken ? 0x8002CB05u : present ? 0 : 2;
            if ((u32)ctx.gpr[3] != expected || vm_read32(0x30400) != 0xA5A5A5A5 ||
                vm_read32(0x30410) != 0xA5A5A5A5) return 1;
            if (!broken && !escape) {
                if (ctx.gpr[5]) {
                    if (vm_read32(0x30408) != (present ? UINT32_MAX : 0) || vm_read32(0x3040C) != 3) return 2;
                    std::printf("[data-space-kb] %u\n", vm_read32(0x30404));
                }
                ctx.gpr[4] = 0x30300; ctx.gpr[5] = 0;
                call(ctx, "cellGameDataCheck", 3);
                if ((u32)ctx.gpr[3] != 0x8002CB23u) return 3;
            }
            if (!strcmp(argv[1], "fs_data_permit")) {
                ctx.gpr[4] = 0x30600; call(ctx, "cellGameContentPermit", 0x30500);
                return ctx.gpr[3] || vm_read8(0x30500) || vm_read8(0x30600) ? 4 : 0;
            }
            if (!strncmp(argv[1], "fs_data_install", 15)) {
                std::memset(vm_base + 0x30700, 0, 212);
                std::strcpy((char*)vm_base + 0x30700, "Own instalação");
                std::strcpy((char*)vm_base + 0x30780, "TEST00001");
                std::strcpy((char*)vm_base + 0x3078C, "01.23");
                ctx.gpr[4] = 0x30500; ctx.gpr[5] = 0x30600;
                call(ctx, "cellGameCreateGameData", 0x30700);
                if (ctx.gpr[3] || std::strncmp((char*)vm_base + 0x30500, "/dev_hdd0/.native-install/", 25)) return 5;
                if (!strcmp(argv[1], "fs_data_install_unload")) {
                    call(ctx, "cellSysmoduleLoadModule", 0x3E);
                    call(ctx, "cellSysmoduleUnloadModule", 0x3E);
                    return 9;
                }
                if (!strcmp(argv[1], "fs_data_install_stage")) return 0;
                call(ctx, "cellSysmoduleLoadModule", 0xE);
                const std::string filename = std::string((char*)vm_base + 0x30600) +
                    (!strcmp(argv[1], "fs_data_install_escape") ? "/escape.bin" : "/ação.bin");
                std::strcpy((char*)vm_base + 0x30A00, filename.c_str());
                ctx.gpr[4] = 0x202; ctx.gpr[5] = 0x100030900ull; ctx.gpr[6] = ctx.gpr[7] = 0;
                call(ctx, "cellFsOpen", 0x30A00);
                if ((u32)ctx.gpr[3] != (u32)CELL_EFAULT) return 10;
                ctx.gpr[4] = !strcmp(argv[1], "fs_data_install_escape") ? 0x202 : 0xC2;
                ctx.gpr[5] = 0x30900; call(ctx, "cellFsOpen", 0x30A00);
                if (ctx.gpr[3]) return 11;
                const u32 data_fd = vm_read32(0x30900);
                uint8_t payload[20]; for (unsigned i = 0; i < 20; ++i) payload[i] = (uint8_t)i;
                std::memcpy(vm_base + 0x30B00, payload, 20);
                ctx.gpr[4] = 0x30B00; ctx.gpr[5] = 20; ctx.gpr[6] = 0x30B00;
                call(ctx, "cellFsWrite", data_fd);
                if (ctx.gpr[3] || vm_read64(0x30B00) != 20) return 12;
                call(ctx, "cellFsFsync", data_fd); if (ctx.gpr[3]) return 25;
                ctx.gpr[4] = 0x30C00; call(ctx, "cellFsFstat", data_fd);
                if (ctx.gpr[3] || vm_read64(0x30C24) != 20 || vm_read64(0x30C2C) != 4096) return 13;
                ctx.gpr[4] = 0; ctx.gpr[5] = 0; ctx.gpr[6] = 0x30910; call(ctx, "cellFsLseek", data_fd);
                if (ctx.gpr[3] || vm_read64(0x30910)) return 14;
                ctx.gpr[4] = 0x30B00; ctx.gpr[5] = 20; ctx.gpr[6] = 0x30910; call(ctx, "cellFsRead", data_fd);
                if (ctx.gpr[3] || vm_read64(0x30910) != 20 || std::memcmp(vm_base + 0x30B00, payload, 20)) return 15;
                ctx.gpr[4] = 0xC2; ctx.gpr[5] = 0x30920; ctx.gpr[6] = ctx.gpr[7] = 0;
                call(ctx, "cellFsOpen", 0x30A00);
                if ((u32)ctx.gpr[3] != (u32)CELL_EEXIST) return 16;
                call(ctx, "cellFsClose", data_fd); if (ctx.gpr[3]) return 17;
                ctx.gpr[4] = 0x402; ctx.gpr[5] = 0x30900; ctx.gpr[6] = ctx.gpr[7] = 0;
                call(ctx, "cellFsOpen", 0x30A00); if (ctx.gpr[3]) return 18;
                const u32 append_fd = vm_read32(0x30900);
                vm_write8(0x30B00, 0xEE); ctx.gpr[4] = 0x30B00; ctx.gpr[5] = 1; ctx.gpr[6] = 0x30910;
                call(ctx, "cellFsWrite", append_fd); if (ctx.gpr[3] || vm_read64(0x30910) != 1) return 19;
                call(ctx, "cellFsClose", append_fd); if (ctx.gpr[3]) return 20;
                ctx.gpr[4] = 0; ctx.gpr[5] = 0x30900; ctx.gpr[6] = ctx.gpr[7] = 0;
                call(ctx, "cellFsOpen", 0x30A00); if (ctx.gpr[3]) return 21;
                const u32 read_fd = vm_read32(0x30900);
                call(ctx, "cellFsFsync", read_fd);
                if ((u32)ctx.gpr[3] != (u32)CELL_EBADF) return 26;
                ctx.gpr[4] = 0x30B00; ctx.gpr[5] = 21; ctx.gpr[6] = 0x30910;
                call(ctx, "cellFsRead", read_fd);
                if (ctx.gpr[3] || vm_read64(0x30910) != 21 || std::memcmp(vm_base + 0x30B00, payload, 20) || vm_read8(0x30B14) != 0xEE) return 22;
                call(ctx, "cellFsClose", read_fd); if (ctx.gpr[3]) return 23;
                call(ctx, "cellFsFsync", read_fd);
                if ((u32)ctx.gpr[3] != (u32)CELL_EBADF) return 27;
                ctx.gpr[4] = 0x30C00; call(ctx, "cellFsStat", 0x30A00);
                if (ctx.gpr[3] || vm_read64(0x30C24) != 21) return 24;
                std::strcpy((char*)vm_base + 0x30800, "Updated title"); ctx.gpr[4] = 0x30800;
                call(ctx, "cellGameSetParamString", 1); if (ctx.gpr[3]) return 7;
                std::strcpy((char*)vm_base + 0x30800, "Título local"); ctx.gpr[4] = 0x30800;
                call(ctx, "cellGameSetParamString", 19); if (ctx.gpr[3]) return 8;
                if (!strcmp(argv[1], "fs_data_install_race")) {
                    namespace fs = std::filesystem;
#ifdef _WIN32
                    fs::path hdd(_wgetenv(L"PS3_HDD0_ROOT"));
#else
                    fs::path hdd(std::getenv("PS3_HDD0_ROOT"));
#endif
                    fs::create_directories(hdd / "game/TEST00001");
                }
                ctx.gpr[4] = 0x30600; call(ctx, "cellGameContentPermit", 0x30500);
                if (ctx.gpr[3] || std::strcmp((char*)vm_base + 0x30500, "/dev_hdd0/game/TEST00001") ||
                    std::strcmp((char*)vm_base + 0x30600, "/dev_hdd0/game/TEST00001/USRDIR")) return 6;
            }
            vm_shutdown(); return 0;
        }
        if (!strcmp(argv[1], "fs_boot") || !strcmp(argv[1], "fs_boot_broken")) {
            ps3_load_prx_modules();
            ctx.gpr[4] = 0x30010; ctx.gpr[5] = 0x30020; ctx.gpr[6] = 0x30040;
            call(ctx, "cellGameBootCheck", 0x30000);
            if (!strcmp(argv[1], "fs_boot_broken")) { vm_shutdown(); return (int64_t)ctx.gpr[3] < 0 ? 0 : 94; }
            if (ctx.gpr[3] || vm_read32(0x30000) != 1 || vm_read32(0x30010) != 0 ||
                std::strcmp((char*)vm_base + 0x30040, "TEST00001") ||
                vm_read32(0x30024) != UINT32_MAX) return 52;
            std::printf("[boot-space-kb] %u\n", vm_read32(0x30020));
            call(ctx, "cellSysmoduleLoadModule", 0x3E);
            call(ctx, "cellSysmoduleUnloadModule", 0x3E);
            if (ctx.gpr[3]) return 285;
            call(ctx, "cellSysmoduleIsLoaded", 0x3E);
            if ((u32)ctx.gpr[3] != 0x80012003u) return 286;
            call(ctx, "cellSysmoduleLoadModule", 0x3E);
            ctx.gpr[4] = 0x30010; ctx.gpr[5] = 0x30020; ctx.gpr[6] = 0x30040;
            call(ctx, "cellGameBootCheck", 0x30000);
            if (ctx.gpr[3]) return 287;
            ctx.gpr[4] = 0x30300; ctx.gpr[5] = 32;
            call(ctx, "cellGameGetParamString", 100);
            if (ctx.gpr[3] || std::strcmp((char*)vm_base + 0x30300, "TEST00001")) return 54;
            ctx.gpr[4] = 0x30200;
            call(ctx, "cellGameContentPermit", 0x30100);
            if (ctx.gpr[3] || std::strcmp((char*)vm_base + 0x30100, "/dev_bdvd/PS3_GAME") ||
                std::strcmp((char*)vm_base + 0x30200, "/dev_bdvd/PS3_GAME/USRDIR")) return 53;
            vm_shutdown(); return 0;
        }
        call(ctx, "cellSysmoduleIsLoaded", 0xE);
        if ((int64_t)ctx.gpr[3] >= 0) return 18;
        call(ctx, "cellSysmoduleLoadModule", 0xE);
        if (ctx.gpr[3]) return 19;
        call(ctx, "cellSysmoduleIsLoaded", 0xE);
        if (ctx.gpr[3]) return 20;
        if (!strcmp(argv[1], "fs_relative")) {
            for (const char* relative : {"PS3_GAME/own.txt", "./PS3_GAME/own.txt", "../outside.txt", "", "C:\\host-file", "CON"}) {
                std::strcpy((char*)vm_base + 0x30000, relative);
                for (bool raw : {false, true}) {
                    vm_write32(0x31000, 0xA5A5A5A5);
                    ctx.gpr[4] = 0; ctx.gpr[5] = 0x31000; ctx.gpr[6] = ctx.gpr[7] = ctx.gpr[8] = 0;
                    if (raw) { ctx.gpr[11] = 801; ctx.gpr[3] = 0x30000; lv2_syscall(&ctx); }
                    else call(ctx, "cellFsOpen", 0x30000);
                    if ((u32)ctx.gpr[3] != (u32)CELL_ENOENT || vm_read32(0x31000) != 0xA5A5A5A5) return 95;
                    std::memset(vm_base + 0x32100, 0xA5, 52); ctx.gpr[4] = 0x32100;
                    if (raw) { ctx.gpr[11] = 808; ctx.gpr[3] = 0x30000; lv2_syscall(&ctx); }
                    else call(ctx, "cellFsStat", 0x30000);
                    if ((u32)ctx.gpr[3] != (u32)CELL_ENOENT) return 96;
                    for (unsigned i = 0; i < 52; ++i) if (vm_base[0x32100 + i] != 0xA5) return 97;
                }
            }
            vm_shutdown(); return 0;
        }
        std::string path = !strcmp(argv[1], "fs_traversal") ? "/dev_bdvd/../outside.txt" :
                           !strcmp(argv[1], "fs_edat") ? "/dev_bdvd/PS3_GAME/protected.edat" : "/dev_bdvd/PS3_GAME/own.txt";
        std::memcpy(vm_base + 0x30000, path.c_str(), path.size() + 1);
        const bool raw_fs = !std::strcmp(argv[1], "fs_syscall");
        auto file_call = [&](const char* name, u64 argument) {
            if (!raw_fs) { call(ctx, name, argument); return; }
            const u32 number = !std::strcmp(name, "cellFsRead") ? 802 :
                !std::strcmp(name, "cellFsClose") ? 804 : !std::strcmp(name, "cellFsStat") ? 808 :
                !std::strcmp(name, "cellFsFstat") ? 809 : 818;
            ctx.gpr[11] = number; ctx.gpr[3] = argument; lv2_syscall(&ctx);
        };
        ctx.gpr[4] = !strcmp(argv[1], "fs_write") ? 1 : 0;
        ctx.gpr[5] = 0x31000;
        if (!std::strcmp(argv[1], "fs")) {
            ctx.gpr[11] = 801; ctx.gpr[3] = 0x30000; ctx.gpr[6] = ctx.gpr[7] = ctx.gpr[8] = 0;
            lv2_syscall(&ctx);
        } else call(ctx, "cellFsOpen", 0x30000);
        if (strcmp(argv[1], "fs") != 0 && !raw_fs) return 1;
        if (ctx.gpr[3]) return 21;
        auto fd = vm_read32(0x31000);
        std::memset(vm_base + 0x32100, 0xA5, 60);
        ctx.gpr[4] = 0x32104; file_call("cellFsFstat", fd);
        if (ctx.gpr[3] || vm_read64(0x32128) != 13 || vm_read64(0x32130) != 2048 ||
            vm_read32(0x32104) != 0x8124 || vm_read32(0x32108) != UINT32_MAX ||
            vm_read32(0x3210C) != UINT32_MAX || vm_read32(0x32100) != 0xA5A5A5A5 ||
            vm_read32(0x32138) != 0xA5A5A5A5) return 27;
        std::printf("[fstat-mtime] %llu\n", (unsigned long long)vm_read64(0x32118));
        for (const u64 invalid : {0ull, 0xFFFFFFFEull, 0x100032104ull}) {
            ctx.gpr[4] = invalid; file_call("cellFsFstat", fd);
            if ((u32)ctx.gpr[3] != (u32)CELL_EFAULT || vm_read64(0x32128) != 13) return 28;
        }
        ctx.gpr[4] = 0x32000; ctx.gpr[5] = 64; ctx.gpr[6] = 0x31010;
        file_call("cellFsRead", fd);
        if (ctx.gpr[3] || vm_read64(0x31010) != 13 || std::memcmp(vm_base + 0x32000, "own-fixture\0!", 13)) return 22;
        ctx.gpr[4] = 0x32104; file_call("cellFsFstat", fd);
        if (ctx.gpr[3]) return 29;
        ctx.gpr[4] = 0; ctx.gpr[5] = 1; ctx.gpr[6] = 0x31020; file_call("cellFsLseek", fd);
        if (ctx.gpr[3] || vm_read64(0x31020) != 13) return 30;
        file_call("cellFsClose", fd);
        if (ctx.gpr[3]) return 23;
        ctx.gpr[4] = 0x32104; file_call("cellFsFstat", fd);
        if ((u32)ctx.gpr[3] != (u32)CELL_EBADF) return 31;
        file_call("cellFsClose", fd);
        if ((int64_t)ctx.gpr[3] >= 0) return 24;
        vm_shutdown();
        std::puts("Verified typed HLE FS activation, real reads/counts and closed-handle error");
        return 0;
    }
    if (argc == 2 && strncmp(argv[1], "mutex_", 6) && strncmp(argv[1], "rw_", 3) && std::string(argv[1]) != "gcm_prepared_multiple" && std::string(argv[1]) != "gcm_prepared_backend_missing") {
        if (!strcmp(argv[1], "callback_missing")) {
            if (vm_init()) return 1;
            vm_write32(0x38000, 0x12380); vm_write32(0x38004, 0);
            ppu_guest_call(0x38000, 0,0,0,0,0,0,0,0);
        }
        if (!strcmp(argv[1], "callback_ct_missing")) ppu_guest_call_ct(0x12380, 0, 0,0,0,0,0,0,0,0);
        if (!strcmp(argv[1], "hle")) ps3_hle_call(0xDEADBEEFu, &ctx);
        if (!strcmp(argv[1], "ppu")) ppu_unlifted_stub(0x12340, &ctx);
        if (!strcmp(argv[1], "syscall")) { ctx.gpr[11] = 0xFFFF; lv2_syscall(&ctx); }
        if (!strcmp(argv[1], "unaudited_syscall")) { ctx.gpr[11] = 110; lv2_syscall(&ctx); }
        if (!strcmp(argv[1], "indirect_null")) { ctx.ctr = 0; ps3_indirect_call(&ctx); }
        if (!strcmp(argv[1], "storage_budget")) ps3_native_install_charge(1024ull * 1024 * 1024 + 1);
        if (!strcmp(argv[1], "ppu_null_read")) { if (vm_init()) return 1; volatile auto value = vm_read32(0); (void)value; }
        if (!strcmp(argv[1], "audio_probe") || !strcmp(argv[1], "audio_option")) {
            if (vm_init()) return 1;
            own_audio_output = -1;
            ctx.gpr[4] = 0; ctx.gpr[5] = 4; ctx.gpr[6] = !strcmp(argv[1], "audio_option") ? 1 : 0;
            call(ctx, "cellAudioOutGetSoundAvailability", 0);
        }
        if (!strcmp(argv[1], "sdk_missing")) {
            if (vm_init()) return 1;
            lv2_init_syscalls();
            ctx.gpr[11] = 348; ctx.gpr[3] = 0x10000; ctx.gpr[4] = 0x200; ctx.gpr[5] = 0x30000;
            lv2_syscall(&ctx);
            if (ctx.gpr[3]) return 1;
            const auto output = vm_read32(0x30000);
            ctx.gpr[11] = 1; lv2_syscall(&ctx);
            ctx.gpr[11] = 25; ctx.gpr[4] = output; lv2_syscall(&ctx);
        }
        if (!strcmp(argv[1], "rtc_calendar_invalid")) {
            if (vm_init()) return 1;
            ppu_sysprx_register();
            ctx.gpr[4] = 0; ctx.gpr[5] = 1;
            call(ctx, "cellRtcGetDayOfWeek", 2026);
        }
        if (!strcmp(argv[1], "indirect")) { ctx.ctr = 0x12340; ps3_indirect_call(&ctx); }
        if (!strcmp(argv[1], "spu")) { static spu_context spu{}; spu_interp_run(&spu, 0x100); }
        if (!strcmp(argv[1], "spu_indirect")) { static spu_context spu{}; spu.pc = 0x12340; spu_indirect_branch(&spu); }
        if (!strcmp(argv[1], "spurs_poll")) {
            static spu_context spu{}; spu.policy_mode = 1; spu.pc = SPURS_PM_SELECT_WORKLOAD_LS;
            spu_indirect_branch(&spu);
        }
        const uint8_t unprepared[64] = {1, 2, 3, 4};
        if (!strcmp(argv[1], "spu_missing")) spu_workload_dispatch(unprepared, sizeof(unprepared), 0);
        if (!strcmp(argv[1], "spu_missing_async")) spu_workload_dispatch_async(unprepared, sizeof(unprepared), 0);
        if (!strcmp(argv[1], "spu_missing_job")) spu_workload_dispatch_job(unprepared, sizeof(unprepared), 0, 0);
        if (!strcmp(argv[1], "sysutil_no_dispatcher")) call(ctx, "cellSysutilCheckCallback", 0);
        if (!strcmp(argv[1], "provider_not_registered")) call(ctx, "cellSysmoduleLoadModule", 0x18);
        if (!strcmp(argv[1], "gcm_unaudited")) call(ctx, "cellGcmSetZcull", 0);
        if (!strcmp(argv[1], "gcm_debug_level")) call(ctx, "cellGcmSetDebugOutputLevel", 3);
        if (!strcmp(argv[1], "gcm_timestamp_index")) call(ctx, "cellGcmGetTimeStamp", 2048);
        if (!strcmp(argv[1], "tile_compressed")) { ctx.gpr[8] = 10; call(ctx, "cellGcmSetTileInfo", 0); }
        if (!strcmp(argv[1], "io_unbacked")) {
            ctx.gpr[4] = 0x100000; ctx.gpr[5] = 0x20000;
            call(ctx, "cellGcmMapMainMemory", 0x40000000);
        }
        if (!strcmp(argv[1], "thread_flags")) { ctx.gpr[8] = 2; call(ctx, "sys_ppu_thread_create", 0); }
        if (!strcmp(argv[1], "thread_exit_unowned")) {
            if (vm_init()) return 1;
            ppu_sysprx_register();
            ctx.gpr[1] = 0xD00FFFF0; call(ctx, "sys_ppu_thread_exit", 0);
        }
        if (!strcmp(argv[1], "audited_not_registered")) {
            if (vm_init()) return 1;
            ctx.gpr[1] = 0xD00FFFF0; call(ctx, "sys_ppu_thread_exit", 0);
        }
        if (!strcmp(argv[1], "raw_spu_unreviewed")) { ctx.gpr[11] = 160; lv2_syscall(&ctx); }
        if (!strcmp(argv[1], "spu_image_protected")) {
            ctx.gpr[5] = 0; call(ctx, "sys_spu_image_import", 0);
        }
        if (!strcmp(argv[1], "once_invalid")) call(ctx, "sys_ppu_thread_once", 0x40000000);
        if (!strcmp(argv[1], "memory_unbacked")) {
            ctx.gpr[4] = 0; ctx.gpr[5] = 16; call(ctx, "_sys_memset", 0x40000000);
        }
        if (!strcmp(argv[1], "memory_overlap") || !strcmp(argv[1], "string_unterminated")) {
            if (vm_init()) return 1;
            lv2_init_syscalls();
            ctx.gpr[11] = 348; ctx.gpr[3] = 0x10000; ctx.gpr[4] = 0x200; ctx.gpr[5] = 0x30000;
            lv2_syscall(&ctx);
            if (ctx.gpr[3]) return 1;
            auto address = vm_read32(0x30000);
            if (!strcmp(argv[1], "string_unterminated")) {
                std::memset(vm_base + address, 0xA5, 0x10000);
                ctx.gpr[4] = address; call(ctx, "_sys_strcpy", address + 16);
            }
            ctx.gpr[4] = address; ctx.gpr[5] = 16; call(ctx, "_sys_memcpy", address + 1);
        }
        if (!strcmp(argv[1], "spurs_missing_policy")) {
            // Internal negative control; this does not enable the guest SPURS initializer.
            if (vm_init()) return 1;
            auto spurs = (CellSpurs*)(uintptr_t)0x40000;
            if (cellSpursInitialize(spurs, 1, 100, 1000, 0)) return 1;
            std::memset(vm_base + 0x42000, 0x5A, 64);
            std::memset(vm_base + 0x42100, 1, 8);
            if (cellSpursAddWorkload(spurs, (u32*)(uintptr_t)0x42200,
                (void*)(uintptr_t)0x42000, 64, 0, (u8*)(uintptr_t)0x42100, 1, 1)) return 1;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        return 1; // Every unsupported path must terminate before reaching here.
    }
    if (vm_init() != 0) return 1;
    ctx.gpr[1] = 0xD00FFFF0;
    ppu_sysprx_register();
    lv2_init_syscalls();
    ctx.gpr[11] = 1; lv2_syscall(&ctx);
#ifdef _WIN32
    const auto process_id = (uint32_t)_getpid();
#else
    const auto process_id = (uint32_t)getpid();
#endif
    if (ctx.gpr[3] != process_id || !process_id) return 181;
    lv2_syscall(&ctx);
    if (ctx.gpr[3] != process_id) return 182;
    call(ctx, "sceNpTerm", 0);
    if ((int64_t)ctx.gpr[3] != (int32_t)0x8002aa01u) return 44;
    ctx.gpr[4] = 0x40000;
    call(ctx, "sceNpInit", 0x1000);
    if ((int64_t)ctx.gpr[3] != (int32_t)0x8002aa0Fu) return 45;
    ctx.gpr[4] = 0;
    call(ctx, "sceNpInit", 0x20000);
    if ((int64_t)ctx.gpr[3] != (int32_t)0x8002aa03u) return 46;
    ctx.gpr[4] = 0x40000;
    call(ctx, "sceNpInit", 0x20000);
    if (ctx.gpr[3]) return 47;
    call(ctx, "sceNpInit", 0x20000);
    if ((int64_t)ctx.gpr[3] != (int32_t)0x8002aa02u) return 48;
    call(ctx, "sceNpManagerGetStatus", 0x25000);
    if (ctx.gpr[3] || (int32_t)vm_read32(0x25000) != -1) return 49;
    call(ctx, "sceNpTerm", 0);
    if (ctx.gpr[3]) return 50;
    call(ctx, "sceNpManagerGetStatus", 0x25000);
    if ((int64_t)ctx.gpr[3] != (int32_t)0x8002aa01u) return 51;
    call(ctx, "cellSysmoduleLoadModule", 0x15);
    if (ctx.gpr[3]) return 36;
    ps3_hle_register(ps3_compute_nid("sys_net_initialize_network_ex"),
                    "sys_net_initialize_network_ex", (void*)sys_net_initialize_network_ex);
    call(ctx, "cellSysmoduleLoadModule", 0);
    if (ctx.gpr[3]) return 246;
    call(ctx, "cellSysmoduleIsLoaded", 0);
    if (ctx.gpr[3]) return 247;
    ctx.gpr[4] = 0x12340; ctx.gpr[5] = 0x24000;
    call(ctx, "cellSysutilRegisterCallback", 0);
    if (ctx.gpr[3]) return 37;
    g_ps3_guest_caller = callback;
    cellSysutilQueueEvent(0, 0xABCD, 0x4321);
    call(ctx, "cellSysutilCheckCallback", 0);
    if (ctx.gpr[3] || callbacks != 1) return 38;
    call(ctx, "cellSysutilUnregisterCallback", 0);
    cellSysutilQueueEvent(0, 0xABCD, 0x4321);
    call(ctx, "cellSysutilCheckCallback", 0);
    if (ctx.gpr[3] || callbacks != 1) return 39;
    call(ctx, "cellSysutilRegisterCallback", 4);
    if ((int64_t)ctx.gpr[3] >= 0) return 40;
    // Original guest attribute layout, exact size, BE fields, signed errors and non-NUL prefix.
    call(ctx, "cellSysmoduleLoadModule", 0xA);
    if (ctx.gpr[3]) return 30;
    std::memset(vm_base + 0x33000, 0xA5, 520);
    ctx.gpr[4] = 2; ctx.gpr[5] = 0x450001; ctx.gpr[6] = 5;
    ctx.gpr[7] = 100; ctx.gpr[8] = 1001; ctx.gpr[9] = 1;
    call(ctx, "_cellSpursAttributeInitialize", 0x33000);
    if (ctx.gpr[3] || vm_read32(0x33000) != 2 || vm_read32(0x33004) != 0x450001 ||
        vm_read32(0x33008) != 5 || vm_read32(0x3300C) != 100 || vm_read32(0x33010) != 1001 ||
        vm_read8(0x33014) != 1 || vm_read8(0x331FF) || vm_read32(0x33200) != 0xA5A5A5A5) return 31;
    std::memcpy(vm_base + 0x34000, "123456789012345", 15);
    ctx.gpr[4] = 0x34000; ctx.gpr[5] = 15;
    call(ctx, "cellSpursAttributeSetNamePrefix", 0x33000);
    if (ctx.gpr[3] || std::memcmp(vm_base + 0x33015, vm_base + 0x34000, 15) || vm_read32(0x33024) != 15) return 32;
    ctx.gpr[5] = 16;
    call(ctx, "cellSpursAttributeSetNamePrefix", 0x33000);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x33024) != 15) return 33;
    call(ctx, "_cellSpursAttributeInitialize", 0x33001);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x33000) != 2) return 34;
    call(ctx, "_cellSpursAttributeInitialize", 0);
    if ((int64_t)ctx.gpr[3] >= 0) return 35;
    struct LockLineCase { uint32_t address; uint64_t flags; uint32_t result; };
    const LockLineCase lock_lines[] = {
        {0x10000, 2, 0}, {0x10000000, 3, 0}, {0x20000000, 1, 0},
        {0x10000, 0, 0x80010002}, {0x10000, 4, 0x80010002},
        {0x10000, 0x100000002ull, 0x80010002},
        {0xD0000000, 2, 0x80010009}, {0xF0000000, 1, 0x80010009},
        {0xF0000000, 2, 0}, {0x90000000, 2, 0x80010002},
    };
    for (const auto& check : lock_lines) {
        ctx.gpr[11] = 14; ctx.gpr[3] = check.address; ctx.gpr[4] = check.flags;
        lv2_syscall(&ctx);
        if ((uint32_t)ctx.gpr[3] != check.result) return 141;
    }
    ctx.gpr[11] = 147; lv2_syscall(&ctx);
    if (ctx.gpr[3] != 79800000) return 11;
    auto wall_before = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    ctx.gpr[11] = 145; ctx.gpr[3] = 0x26000; ctx.gpr[4] = 0x26008; lv2_syscall(&ctx);
    auto wall_after = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (ctx.gpr[3] != 0 || vm_read64(0x26000) < (uint64_t)wall_before ||
        vm_read64(0x26000) > (uint64_t)wall_after || vm_read64(0x26008) >= 1000000000) return 12;
    call(ctx, "cellSysmoduleLoadModule", 9);
    if (ctx.gpr[3]) return 41;
    call(ctx, "cellSysmoduleInitialize", 0xDEADBEEF);
    if (ctx.gpr[3]) return 236;
    call(ctx, "cellSysmoduleInitialize", 0);
    if (ctx.gpr[3]) return 237;
    call(ctx, "cellSysmoduleIsLoaded", 9);
    if (ctx.gpr[3]) return 238;
    call(ctx, "cellRtcGetCurrentTick", 0x26010);
    auto rtc_seconds = vm_read64(0x26010) / 1000000 - 62135596800ull;
    if (ctx.gpr[3] || rtc_seconds < (uint64_t)wall_before || rtc_seconds > (uint64_t)wall_after + 1) return 42;
    call(ctx, "cellRtcGetCurrentTick", 0);
    if ((int64_t)ctx.gpr[3] >= 0) return 43;
    const u32 dates[][4] = {{1,1,1,1}, {1970,1,1,4}, {1900,3,1,4}, {2000,2,29,2}, {2026,11,15,0}};
    for (const auto& date : dates) {
        ctx.gpr[4] = date[1]; ctx.gpr[5] = date[2];
        call(ctx, "cellRtcGetDayOfWeek", date[0]);
        if (ctx.gpr[3] != date[3]) return 235;
    }
    for (unsigned a = 0x27000; a < 0x27014; a += 4) vm_write32(a, 0xA5A5A5A5);
    ctx.gpr[11] = 144; ctx.gpr[3] = 0x27004; ctx.gpr[4] = 0x2700C; lv2_syscall(&ctx);
    auto zone = (int32_t)vm_read32(0x27004), dst = (int32_t)vm_read32(0x2700C);
    if (ctx.gpr[3] != 0 || zone < -1440 || zone > 1440 || dst < -180 || dst > 180 ||
        vm_read32(0x27000) != 0xA5A5A5A5 || vm_read32(0x27008) != 0xA5A5A5A5 || vm_read32(0x27010) != 0xA5A5A5A5) return 13;
    ctx.gpr[3] = 0; ctx.gpr[4] = 0x2700C; lv2_syscall(&ctx);
    if (ctx.gpr[3] == 0) return 14;
    g_ppu_main_stack_addr = 0xD0100000; g_ppu_main_stack_size = 0x20000;
    const u32 mutex_out = 0xD0101000, mutex_attr = mutex_out + 16;
    std::memset(vm_base + mutex_attr, 0, 40);
    vm_write32(mutex_attr, 1); vm_write32(mutex_attr + 4, 0x10);
    vm_write32(mutex_attr + 8, 0x200); vm_write32(mutex_attr + 12, 0x2000);
    if (argc > 1 && !strcmp(argv[1], "mutex_shared")) vm_write32(mutex_attr + 8, 0x100);
    if (argc > 1 && !strcmp(argv[1], "mutex_adaptive")) vm_write32(mutex_attr + 12, 0x1000);
    std::memcpy(vm_base + mutex_attr + 32, "ownmutex", 8);
    vm_write32(mutex_out - 4, 0xA5A5A5A5); vm_write32(mutex_out + 4, 0xA5A5A5A5);
    ctx.gpr[11] = 100; ctx.gpr[3] = mutex_out; ctx.gpr[4] = mutex_attr;
    lv2_syscall(&ctx);
    const u32 mutex_id = vm_read32(mutex_out);
    if (ctx.gpr[3] || !mutex_id || vm_read32(mutex_out - 4) != 0xA5A5A5A5 ||
        vm_read32(mutex_out + 4) != 0xA5A5A5A5) return 237;
    ctx.thread_id = 17;
    call(ctx, "sceNpTrophyTerm", 0);
    if ((u32)ctx.gpr[3] != 0x80022902u) return 295;
    ctx.gpr[4] = ctx.gpr[5] = 0; ctx.gpr[6] = 1;
    call(ctx, "sceNpTrophyInit", 0);
    if ((u32)ctx.gpr[3] != 0x80022903u) return 296;
    ctx.gpr[6] = 0; call(ctx, "sceNpTrophyInit", 0);
    if (ctx.gpr[3]) return 297;
    call(ctx, "sceNpTrophyInit", 0);
    if ((u32)ctx.gpr[3] != 0x80022901u) return 298;
    call(ctx, "sceNpTrophyTerm", 0);
    if (ctx.gpr[3]) return 299;
    ctx.gpr[4] = 0; ctx.gpr[5] = 4; ctx.gpr[6] = 0;
    call(ctx, "cellAudioOutGetSoundAvailability", 0);
    if (ctx.gpr[3] != 2) return 288;
    own_audio_output = 0;
    call(ctx, "cellAudioOutGetSoundAvailability", 0);
    if (ctx.gpr[3]) return 289;
    own_audio_output = 1;
    for (u32 coding : {1u, 2u}) {
        ctx.gpr[4] = coding; call(ctx, "cellAudioOutGetSoundAvailability", 0);
        if (ctx.gpr[3]) return 290;
    }
    ctx.gpr[4] = 0; call(ctx, "cellAudioOutGetSoundAvailability", 1);
    if (ctx.gpr[3]) return 291;
    std::memset(vm_base + 0xD0101200, 0, 16); vm_write8(0xD0101200, 2);
    ctx.gpr[4] = 0xD0101200; ctx.gpr[5] = ctx.gpr[6] = 0;
    call(ctx, "cellAudioOutConfigure", 0);
    if (ctx.gpr[3]) return 292;
    own_audio_output = 0; call(ctx, "cellAudioOutConfigure", 0);
    if ((u32)ctx.gpr[3] != 0x8002B244u) return 293;
    own_audio_output = 1;
    ctx.gpr[4] = 0x1D0101200ull; call(ctx, "cellAudioOutConfigure", 0);
    if ((u32)ctx.gpr[3] != 0x8002B242u) return 294;
    for (const u32 operation : {102u, 103u}) {
        ctx.gpr[11] = operation; ctx.gpr[3] = mutex_id; ctx.gpr[4] = 0;
        lv2_syscall(&ctx); if (ctx.gpr[3]) return 242;
    }
    ctx.gpr[11] = 101; ctx.gpr[3] = mutex_id; lv2_syscall(&ctx);
    if ((u32)ctx.gpr[3] != (u32)CELL_EBUSY) return 243;
    std::thread mutex_other([&] {
        ppu_context other{}; other.thread_id = 18;
        other.gpr[11] = 103; other.gpr[3] = mutex_id; lv2_syscall(&other);
        if ((u32)other.gpr[3] != (u32)CELL_EBUSY) std::abort();
        other.gpr[11] = 104; other.gpr[3] = mutex_id; lv2_syscall(&other);
        if ((u32)other.gpr[3] != (u32)CELL_EPERM) std::abort();
    });
    mutex_other.join();
    for (unsigned depth = 0; depth < 2; ++depth) {
        ctx.gpr[11] = 104; ctx.gpr[3] = mutex_id; lv2_syscall(&ctx);
        if (ctx.gpr[3]) return 244;
    }
    ctx.gpr[3] = mutex_id; lv2_syscall(&ctx);
    if ((u32)ctx.gpr[3] != (u32)CELL_EPERM) return 245;
    std::thread mutex_next([&] {
        ppu_context other{}; other.thread_id = 18;
        other.gpr[11] = 103; other.gpr[3] = mutex_id; lv2_syscall(&other);
        if (other.gpr[3]) std::abort();
        other.gpr[11] = 104; other.gpr[3] = mutex_id; lv2_syscall(&other);
        if (other.gpr[3]) std::abort();
    });
    mutex_next.join();
    const u32 cond_attr = mutex_attr + 48, cond_out = mutex_out + 96;
    std::memset(vm_base + cond_attr, 0, 24); vm_write32(cond_attr, 0x200);
    auto kernel_cond = [&](u32 number, u64 a, u64 b = 0, u64 c = 0) {
        ctx.gpr[11] = number; ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c;
        lv2_syscall(&ctx); return (u32)ctx.gpr[3];
    };
    vm_write32(cond_out - 4, 0xA5A5A5A5); vm_write32(cond_out + 4, 0xA5A5A5A5);
    for (u64 invalid : {0ull, 0xFFFFFFFEull, 0x1D0101060ull})
        if (kernel_cond(105, invalid, mutex_id, cond_attr) != (u32)CELL_EFAULT) return 268;
    if (kernel_cond(105, cond_out, 0, cond_attr) != (u32)CELL_ESRCH ||
        kernel_cond(105, cond_out, mutex_id, cond_attr)) return 269;
    const u32 cond_id = vm_read32(cond_out);
    if (!cond_id || vm_read32(cond_out - 4) != 0xA5A5A5A5 ||
        vm_read32(cond_out + 4) != 0xA5A5A5A5 ||
        kernel_cond(101, mutex_id) != (u32)CELL_EPERM ||
        kernel_cond(106, cond_id) || kernel_cond(106, cond_id) != (u32)CELL_ESRCH) return 270;
    ctx.gpr[11] = 101; ctx.gpr[3] = mutex_id; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 238;
    ctx.gpr[3] = mutex_id; lv2_syscall(&ctx);
    if ((u32)ctx.gpr[3] != (u32)CELL_ESRCH) return 239;
    vm_write32(mutex_out, 0xA5A5A5A5);
    for (const u64 invalid_out : {0ull, 0xFFFFFFFEull, 0x1D0101000ull}) {
        ctx.gpr[11] = 100; ctx.gpr[3] = invalid_out; ctx.gpr[4] = mutex_attr;
        lv2_syscall(&ctx);
        if ((u32)ctx.gpr[3] != (u32)CELL_EFAULT || vm_read32(mutex_out) != 0xA5A5A5A5) return 240;
    }
    vm_write32(mutex_attr, 0);
    ctx.gpr[3] = mutex_out; lv2_syscall(&ctx);
    if ((u32)ctx.gpr[3] != (u32)CELL_EINVAL || vm_read32(mutex_out) != 0xA5A5A5A5) return 241;
    vm_write32(mutex_attr, 2); vm_write32(mutex_attr + 4, 0x20);
    ctx.gpr[3] = mutex_out; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 248;
    const u32 nonrecursive_id = vm_read32(mutex_out);
    ctx.gpr[11] = 102; ctx.gpr[3] = nonrecursive_id; ctx.gpr[4] = 0; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 249;
    for (const u32 operation : {102u, 103u}) {
        ctx.gpr[11] = operation; ctx.gpr[3] = nonrecursive_id; lv2_syscall(&ctx);
        if ((u32)ctx.gpr[3] != (u32)CELL_EDEADLK) return 250;
    }
    ctx.gpr[11] = 104; ctx.gpr[3] = nonrecursive_id; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 251;
    ctx.gpr[11] = 101; ctx.gpr[3] = nonrecursive_id; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 252;
    auto create_mutexes = [&](unsigned index) {
        ppu_context local{}; local.thread_id = 31 + index;
        const u32 output = 0xD0101300 + index * 128;
        for (unsigned i = 0; i < 32; ++i) {
            local.gpr[11] = 100; local.gpr[3] = output; local.gpr[4] = mutex_attr;
            lv2_syscall(&local);
            const u32 id = vm_read32(output);
            if (local.gpr[3] || !id) std::abort();
            for (const u32 operation : {102u, 104u, 101u}) {
                local.gpr[11] = operation; local.gpr[3] = id; local.gpr[4] = 0;
                lv2_syscall(&local); if (local.gpr[3]) std::abort();
            }
        }
    };
    std::thread creator_first(create_mutexes, 0), creator_second(create_mutexes, 1);
    creator_first.join(); creator_second.join();
    auto rw = [&](u32 number, u64 a, u64 b = 0) {
        ctx.gpr[11] = number; ctx.gpr[3] = a; ctx.gpr[4] = b;
        lv2_syscall(&ctx); return (u32)ctx.gpr[3];
    };
    std::memset(vm_base + mutex_attr, 0, 32);
    vm_write32(mutex_attr, 2); vm_write32(mutex_attr + 4, 0x200);
    if (rw(120, 0, mutex_attr) != (u32)CELL_EFAULT ||
        rw(120, 0x1D0101000ull, mutex_attr) != (u32)CELL_EFAULT) return 253;
    vm_write32(mutex_attr, 0);
    if (rw(120, mutex_out, mutex_attr) != (u32)CELL_EINVAL) return 254;
    vm_write32(mutex_attr, 2);
    if (rw(120, mutex_out, mutex_attr)) return 255;
    const u32 rw_id = vm_read32(mutex_out);
    if (!rw_id || rw(122, rw_id) || rw(123, rw_id) || rw(148, rw_id) != (u32)CELL_EBUSY ||
        rw(121, rw_id) != (u32)CELL_EBUSY) return 256;
    std::thread reader_release([&] {
        ppu_context other{}; other.thread_id = 18;
        for (unsigned i = 0; i < 2; ++i) {
            other.gpr[11] = 124; other.gpr[3] = rw_id; lv2_syscall(&other);
            if (other.gpr[3]) std::abort();
        }
    });
    reader_release.join();
    if (rw(124, rw_id) != (u32)CELL_EPERM || rw(125, rw_id) ||
        rw(148, rw_id) != (u32)CELL_EDEADLK || rw(123, rw_id) != (u32)CELL_EBUSY) return 257;
    std::thread other_writer([&] {
        ppu_context other{}; other.thread_id = 18;
        other.gpr[11] = 127; other.gpr[3] = rw_id; lv2_syscall(&other);
        if ((u32)other.gpr[3] != (u32)CELL_EPERM) std::abort();
        if (argc > 1 && !strcmp(argv[1], "rw_contended")) {
            other.gpr[11] = 122; other.gpr[3] = rw_id; lv2_syscall(&other);
        }
    });
    other_writer.join();
    if (rw(127, rw_id) || rw(121, rw_id) || rw(122, rw_id) != (u32)CELL_ESRCH) return 258;
    ctx.gpr[11] = 49; ctx.gpr[3] = 0x28000; lv2_syscall(&ctx);
    if (ctx.gpr[3] != 0 || vm_read32(0x28000) != 0xD0100000 || vm_read32(0x28004) != 0x20000) return 15;
    ctx.gpr[3] = 0; lv2_syscall(&ctx);
    if (ctx.gpr[3] == 0) return 16;
    ctx.gpr[3] = 0xFFFFFFFC; lv2_syscall(&ctx);
    if (ctx.gpr[3] == 0) return 17;
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    auto available = vm_read32(0x25004);
    ctx.gpr[11] = 348; ctx.gpr[3] = 0x10000; ctx.gpr[4] = 0x200; ctx.gpr[5] = 0x25010; lv2_syscall(&ctx);
    if (ctx.gpr[3] != 0) return 6;
    auto allocation = vm_read32(0x25010);
    if (!allocation || vm_read32(allocation) != 0 || vm_read32(allocation + 0xFFFC) != 0) return 7;
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    if (vm_read32(0x25004) != available - 0x10000) return 8;
    const u32 local_output = allocation + 0x100;
    vm_write8(local_output - 1, 0xA5); vm_write8(local_output + 16, 0x5A);
    const auto local_before = std::time(nullptr);
    call(ctx, "cellRtcGetCurrentClockLocalTime", local_output);
    const auto local_after = std::time(nullptr);
    auto matches_local = [&](std::time_t epoch) {
        std::tm date{};
#ifdef _WIN32
        if (localtime_s(&date, &epoch)) return false;
#else
        if (!localtime_r(&epoch, &date)) return false;
#endif
        return vm_read16(local_output) == date.tm_year + 1900 && vm_read16(local_output + 2) == date.tm_mon + 1 &&
            vm_read16(local_output + 4) == date.tm_mday && vm_read16(local_output + 6) == date.tm_hour &&
            vm_read16(local_output + 8) == date.tm_min && vm_read16(local_output + 10) == date.tm_sec;
    };
    if (ctx.gpr[3] || (!matches_local(local_before) && !matches_local(local_after)) || vm_read32(local_output + 12) >= 1000000 ||
        vm_read8(local_output - 1) != 0xA5 || vm_read8(local_output + 16) != 0x5A) return 233;
    for (u64 invalid : {0ull, 0xFFFFFFF8ull, 0x100000000ull + local_output}) {
        call(ctx, "cellRtcGetCurrentClockLocalTime", invalid);
        if ((u32)ctx.gpr[3] != 0x80010602 || vm_read8(local_output - 1) != 0xA5 || vm_read8(local_output + 16) != 0x5A) return 234;
    }
    ctx.gpr[11] = 349; ctx.gpr[3] = allocation; lv2_syscall(&ctx);
    if (ctx.gpr[3] != 0) return 9;
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    if (vm_read32(0x25004) != available) return 10;
    ctx.gpr[11] = 330; ctx.gpr[3] = 0x10000000; ctx.gpr[4] = 0x40F;
    ctx.gpr[5] = 0; ctx.gpr[6] = 0x35000; lv2_syscall(&ctx);
    auto reserved_a = vm_read32(0x35000);
    if (ctx.gpr[3] || !reserved_a || (reserved_a & 0xFFFFFFFu)) return 55;
    ctx.gpr[3] = 0x10000000; ctx.gpr[5] = 0x20000000; ctx.gpr[6] = 0x35004; lv2_syscall(&ctx);
    auto reserved_b = vm_read32(0x35004);
    if (ctx.gpr[3] || !reserved_b || (reserved_b & 0x1FFFFFFFu) ||
        (reserved_a < reserved_b + 0x10000000u && reserved_b < reserved_a + 0x10000000u)) return 56;
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    auto mapper_before = vm_read32(0x25004);
    ctx.gpr[4] = 0x400; ctx.gpr[5] = 0x35100;
    call(ctx, "sys_mmapper_allocate_memory", 0x100000);
    auto shared_id = vm_read32(0x35100);
    if (ctx.gpr[3] || !shared_id) return 81;
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    if (vm_read32(0x25004) != mapper_before - 0x100000) return 82;
    ctx.gpr[4] = shared_id; ctx.gpr[5] = 0xF;
    call(ctx, "sys_mmapper_map_memory", reserved_a);
    if (ctx.gpr[3] || vm_read32(reserved_a) || vm_read32(reserved_a + 0xFFFFC)) return 83;
    vm_write32(reserved_a, 0xAABBCCDD);
    if (vm_read32(reserved_a) != 0xAABBCCDD) return 84;
    const u32 rsx_address = reserved_a + 0x200, rsx_secondary = rsx_address + 16;
    auto device_map = [&](u64 a, u64 b, u32 device) {
        ctx.gpr[11] = 675; ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = device;
        lv2_syscall(&ctx); return (u32)ctx.gpr[3];
    };
    if (ps3_native_guest_range_backed(VM_RSX_DEVICE_BASE, 4)) return 270;
    vm_write64(rsx_address, 0xA5A5A5A5A5A5A5A5ull); vm_write64(rsx_secondary, 0x123456789ABCDEF0ull);
    if (device_map(rsx_address, rsx_secondary, 9) != (u32)CELL_EINVAL ||
        vm_read64(rsx_address) != 0xA5A5A5A5A5A5A5A5ull) return 271;
    for (u64 bad : {0ull, 0xFFFFFFFEull, 0x100000000ull + rsx_address})
        if (device_map(bad, rsx_secondary, 8) != (u32)CELL_EFAULT) return 272;
    if (device_map(rsx_address, 0xFFFFFFFEull, 8) != (u32)CELL_EFAULT ||
        device_map(rsx_address, rsx_secondary, 8) || vm_read64(rsx_address) != VM_RSX_DEVICE_BASE ||
        vm_read64(rsx_secondary) != 0x123456789ABCDEF0ull ||
        !ps3_native_guest_range_backed(VM_RSX_DEVICE_BASE, VM_RSX_DEVICE_SIZE) ||
        ps3_native_guest_range_backed(VM_RSX_DEVICE_BASE, VM_RSX_DEVICE_SIZE + 1)) return 273;
    vm_write32(VM_RSX_DEVICE_BASE + VM_RSX_DEVICE_SIZE - 4, 0xCAFEBABE);
    if (device_map(rsx_address, 0, 8) || vm_read64(rsx_address) != VM_RSX_DEVICE_BASE ||
        vm_read32(VM_RSX_DEVICE_BASE + VM_RSX_DEVICE_SIZE - 4) != 0xCAFEBABE) return 274;
    const auto semaphore_attr = reserved_a + 0x4000, semaphore_output = reserved_a + 0x4100;
    std::memset(vm_base + semaphore_attr, 0, 32);
    vm_write32(semaphore_attr, 2); vm_write32(semaphore_attr + 4, 0x200);
    vm_write32(semaphore_output, 0xA5A5A5A5);
    ctx.gpr[11] = 90; ctx.gpr[3] = semaphore_output; ctx.gpr[4] = semaphore_attr;
    ctx.gpr[5] = 2; ctx.gpr[6] = 1; lv2_syscall(&ctx);
    if ((uint32_t)ctx.gpr[3] != 0x80010002 || vm_read32(semaphore_output) != 0xA5A5A5A5) return 142;
    ctx.gpr[3] = semaphore_output; ctx.gpr[5] = 0; vm_write32(semaphore_attr, 3); lv2_syscall(&ctx);
    if ((uint32_t)ctx.gpr[3] != 0x80010002 || vm_read32(semaphore_output) != 0xA5A5A5A5) return 143;
    ctx.gpr[3] = semaphore_output; vm_write32(semaphore_attr, 2); lv2_syscall(&ctx);
    const auto semaphore_id = vm_read32(semaphore_output);
    if (ctx.gpr[3] || !semaphore_id || semaphore_id == 0xA5A5A5A5) return 144;
    ctx.gpr[11] = 91; ctx.gpr[3] = semaphore_id; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 145;
    ctx.gpr[3] = semaphore_id; lv2_syscall(&ctx);
    if ((uint32_t)ctx.gpr[3] != 0x80010005) return 146;
    const auto image_source = reserved_a + 0x6000, image_output = reserved_a + 0x6200;
    std::memset(vm_base + image_source, 0, 256);
    std::memcpy(vm_base + image_source, "\177ELF\1\2\1", 7);
    vm_write32(image_source + 16, 0x00020017); vm_write32(image_source + 20, 1);
    vm_write32(image_source + 24, 0x100); vm_write32(image_source + 28, 52);
    vm_write32(image_source + 40, 0x00340020); vm_write32(image_source + 44, 0x00010000);
    vm_write32(image_source + 52, 1); vm_write32(image_source + 56, 128);
    vm_write32(image_source + 60, 0x100); vm_write32(image_source + 68, 4);
    vm_write32(image_source + 72, 8); vm_write32(image_source + 76, 5);
    vm_write32(image_source + 80, 16); vm_write32(image_source + 128, 0x2000);
    std::memset(vm_base + image_output, 0xA5, 16);
    ctx.gpr[4] = image_source; ctx.gpr[5] = 1;
    vm_write32(image_source + 68, 9);
    call(ctx, "sys_spu_image_import", image_output);
    if ((uint32_t)ctx.gpr[3] != 0x80010007 || vm_read32(image_output) != 0xA5A5A5A5) return 149;
    vm_write32(image_source + 68, 4); vm_write32(image_source + 56, 0xFFFFFFFC);
    call(ctx, "sys_spu_image_import", image_output);
    if ((uint32_t)ctx.gpr[3] != 0x80010007 || vm_read32(image_output) != 0xA5A5A5A5) return 150;
    vm_write32(image_source + 56, 128);
    call(ctx, "sys_spu_image_import", image_output);
    const auto image_segments = vm_read32(image_output + 8);
    if (ctx.gpr[3] || vm_read32(image_output) != 0 || vm_read32(image_output + 4) != 0x100 ||
        vm_read32(image_output + 12) != 2 || !image_segments || vm_read32(image_segments) != 1 ||
        vm_read32(image_segments + 4) != 0x100 || vm_read32(image_segments + 8) != 4 ||
        vm_read32(image_segments + 16) != image_source + 128 || vm_read32(image_segments + 20) ||
        vm_read32(image_segments + 24) != 2 || vm_read32(image_segments + 28) != 0x104 ||
        vm_read32(image_segments + 32) != 4 || vm_read32(image_segments + 40)) return 151;
    ctx.gpr[4] = 0x400; ctx.gpr[5] = 0x35104;
    vm_write32(0x35104, 0xA5A5A5A5);
    call(ctx, "sys_mmapper_allocate_memory", 0x123);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x35104) != 0xA5A5A5A5) return 85;
    call(ctx, "sys_mmapper_allocate_memory", 0x40000000);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x35104) != 0xA5A5A5A5) return 86;
    ctx.gpr[11] = 330; ctx.gpr[4] = 0;
    vm_write32(0x35008, 0xA5A5A5A5); ctx.gpr[3] = 0x1000; ctx.gpr[6] = 0x35008; lv2_syscall(&ctx);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x35008) != 0xA5A5A5A5) return 57;
    ctx.thread_id = 17;
    ctx.gpr[4] = 0x20000; ctx.gpr[5] = 0x100000; ctx.gpr[6] = 0x200000;
    call(ctx, "_cellGcmInitBody", 0x36000);
    auto gcm_context = vm_read32(0x36000);
    if (ctx.gpr[3] || !gcm_context || vm_read32(gcm_context) != 0x200000 ||
        vm_read32(gcm_context + 4) != 0x21FFFC || vm_read32(gcm_context + 8) != 0x200000 ||
        vm_read32(vm_read32(gcm_context + 12)) != VM_HLE_INJECT_BASE + 0x2F00u) return 58;
    vm_write32(0x36004, 0xA5A5A5A5);
    ctx.gpr[4] = 0x20000; ctx.gpr[5] = 0x100000; ctx.gpr[6] = 0x200000;
    call(ctx, "_cellGcmInitBody", 0x36004);
    if ((int64_t)ctx.gpr[3] != (int32_t)CELL_GCM_ERROR_FAILURE || vm_read32(0x36004) != 0xA5A5A5A5) return 59;
    call(ctx, "cellGcmGetConfiguration", 0x36100);
    if (ctx.gpr[3] || vm_read32(0x36100) != 0xC0000000 || vm_read32(0x36104) != 0x200000 ||
        vm_read32(0x3610C) != 0x100000) return 60;
    ctx.gpr[4] = 0x36120; call(ctx, "cellGcmAddressToOffset", 0x200040);
    if (ctx.gpr[3] || vm_read32(0x36120) != 0x40) return 61;
    ctx.gpr[4] = 0x36120; call(ctx, "cellGcmAddressToOffset", 0xC0000100);
    if (ctx.gpr[3] || vm_read32(0x36120) != 0x100) return 62;
    ctx.gpr[4] = 0x36120; call(ctx, "cellGcmAddressToOffset", 0x12000000);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x36120) != 0x100) return 63;
    call(ctx, "cellGcmGetControlRegister", 0);
    if (ctx.gpr[3] != VM_HLE_INJECT_BASE + 0x2000u) return 64;
    call(ctx, "cellGcmGetLabelAddress", 3);
    vm_write32(ctx.gpr[3], 0x12345678);
    if (vm_read32(VM_HLE_INJECT_BASE + 0x30u) != 0x12345678) return 65;
    call(ctx, "cellVideoOutGetNumberOfDevice", 0); if (ctx.gpr[3] != 1) return 66;
    call(ctx, "cellVideoOutGetNumberOfDevice", 1); if (ctx.gpr[3] != 0) return 67;
    ctx.gpr[4] = 0; ctx.gpr[5] = 0x36200; call(ctx, "cellVideoOutGetState", 0);
    if (ctx.gpr[3] || vm_read8(0x36200) || vm_read8(0x36208) != 2 || vm_read8(0x3620B) != 2) return 68;
    ctx.gpr[4] = 0x36300; call(ctx, "cellVideoOutGetResolution", 2);
    if (ctx.gpr[3] || vm_read16(0x36300) != 1280 || vm_read16(0x36302) != 720) return 69;
    ctx.gpr[4] = 1; ctx.gpr[5] = 0x36200; call(ctx, "cellVideoOutGetState", 0);
    if ((int64_t)ctx.gpr[3] >= 0) return 70;
    ctx.gpr[4] = 0; ctx.gpr[5] = 0x36400;
    call(ctx, "cellVideoOutGetDeviceInfo", 0);
    if ((int64_t)ctx.gpr[3] >= 0) return 71;
    synthetic_video_ready = 1; // ABI fixture only; real GPU validation is a separate test.
    std::memset(vm_base + 0x36400, 0xA5, 464);
    call(ctx, "cellVideoOutGetDeviceInfo", 0);
    if (ctx.gpr[3] || vm_read8(0x36404) != 1 || vm_read8(0x36405) != 1 ||
        vm_read8(0x36420) != 2 || vm_read32(0x365C8) != 0xA5A5A5A5) return 72;
    ctx.gpr[4] = 1; ctx.gpr[5] = 0; ctx.gpr[6] = 0;
    call(ctx, "cellVideoOutGetResolutionAvailability", 0); if (ctx.gpr[3]) return 73;
    ctx.gpr[4] = 2; call(ctx, "cellVideoOutGetResolutionAvailability", 0); if (ctx.gpr[3] != 1) return 74;
    std::memset(vm_base + 0x36600, 0, 16);
    vm_write8(0x36600, 2); vm_write8(0x36602, 2); vm_write32(0x3660C, 5120);
    ctx.gpr[4] = 0x36600; call(ctx, "cellVideoOutConfigure", 0); if (ctx.gpr[3]) return 75;
    ctx.gpr[4] = 0x36620; call(ctx, "cellVideoOutGetConfiguration", 0);
    if (ctx.gpr[3] || std::memcmp(vm_base + 0x36600, vm_base + 0x36620, 16)) return 76;
    call(ctx, "cellGcmGetTiledPitchSize", 0x1400); if (ctx.gpr[3] != 0x1400) return 77;
    call(ctx, "cellGcmGetTiledPitchSize", 0x10001); if (ctx.gpr[3]) return 78;
    vm_write32(0x37000, 0x12340); vm_write32(0x37004, 0);
    call(ctx, "cellGcmSetVBlankHandler", 0x37000);
    cellGcmTickVBlank(); ppu_gcm_pump();
    if (gcm_callbacks != 1) return 79;
    call(ctx, "cellGcmSetVBlankHandler", 0);
    cellGcmTickVBlank(); ppu_gcm_pump();
    if (gcm_callbacks != 1) return 80;
    ctx.gpr[4] = 0x100000; ctx.gpr[5] = 0x36700;
    call(ctx, "cellGcmMapMainMemory", reserved_a);
    if (ctx.gpr[3] || !vm_read32(0x36700)) return 87;
    auto mapped_io = vm_read32(0x36700);
    ctx.gpr[4] = 0x36704; call(ctx, "cellGcmAddressToOffset", reserved_a + 16);
    if (ctx.gpr[3] || vm_read32(0x36704) != mapped_io + 16) return 88;
    ctx.gpr[4] = 1; ctx.gpr[5] = 0x100000; ctx.gpr[6] = 0x20000;
    ctx.gpr[7] = 0x200; ctx.gpr[8] = 0; ctx.gpr[9] = 0; ctx.gpr[10] = 1;
    call(ctx, "cellGcmSetTileInfo", 1);
    rsx_tile tile{};
    if (ctx.gpr[3] || rsx_tiling_find(1, 0x100000, 16, &tile)) return 89;
    call(ctx, "cellGcmBindTile", 1);
    if (ctx.gpr[3] || !rsx_tiling_find(1, 0x100000, 16, &tile) || tile.pitch != 0x200 || tile.bank != 1) return 90;
    call(ctx, "cellGcmUnbindTile", 1);
    if (ctx.gpr[3] || rsx_tiling_find(1, 0x100000, 16, &tile)) return 91;
    ctx.gpr[4] = 0x10000; ctx.gpr[5] = 5120; ctx.gpr[6] = 1280; ctx.gpr[7] = 720;
    call(ctx, "cellGcmSetDisplayBuffer", 0);
    if (ctx.gpr[3] || !cellGcmOffsetIsDisplay(0x10000)) return 92;
    call(ctx, "cellGcmSetVBlankHandler", 0x37000);
    cellGcmTickVBlank();
    auto sleep_start = std::chrono::steady_clock::now();
    ctx.gpr[11] = 141; ctx.gpr[3] = 2000; lv2_syscall(&ctx);
    auto slept = std::chrono::steady_clock::now() - sleep_start;
    if (ctx.gpr[3] || gcm_callbacks != 2 || slept < std::chrono::microseconds(2000)) {
        std::fprintf(stderr, "usleep elapsed=%lldus callbacks=%u\n",
            (long long)std::chrono::duration_cast<std::chrono::microseconds>(slept).count(), gcm_callbacks);
        return 93;
    }
    call(ctx, "cellGcmSetVBlankHandler", 0);
    call(ctx, "sys_ppu_thread_get_id", 0x24000);
    if (vm_read64(0x24000) != 17) return 5;
    call(ctx, "_sys_process_atexitspawn", 0x21000);
    call(ctx, "_sys_process_atexitspawn", 0x22000);
    call(ctx, "_sys_process_at_Exitspawn", 0x23000);
    if (ps3_process_spawn_callback(0) != 0x21000 || ps3_process_spawn_callback(1) != 0x23000) return 2;
    vm_write32(0x21000, 2);
    // Own synthetic image data, independently of the actual main stack mapping.
    sys_memory_set_image_query([](uint64_t a, uint64_t end) -> uint64_t {
        return a < 0x40000 && end > 0x20000 ? 0x40000 : a;
    });
    vm_write32(0x21004, 0x20);
    ctx.gpr[4] = 0x21000;
    call(ctx, "sys_lwmutex_create", 0x20000);
    if (vm_read32(0x20008) != 0x22 || vm_read32(0x20000) != 0) return 3;
    unsigned value = 0;
    auto worker = [&](unsigned id) {
        ppu_context local{}; local.thread_id = id; local.gpr[1] = 0xD0200000 + id * 0x10000;
        for (unsigned i = 0; i < 1000; ++i) {
            call(local, "sys_lwmutex_lock", 0x20000);
            unsigned previous = value;
            std::this_thread::yield();
            value = previous + 1;
            call(local, "sys_lwmutex_unlock", 0x20000);
        }
    };
    std::thread first(worker, 1), second(worker, 2);
    first.join(); second.join();
    call(ctx, "sys_lwmutex_lock", 0x20000);
    call(ctx, "sys_lwmutex_destroy", 0x20000);
    if ((u32)ctx.gpr[3] != (u32)CELL_EBUSY) return 277;
    call(ctx, "sys_lwmutex_unlock", 0x20000);
    call(ctx, "sys_lwmutex_destroy", 0x20000);
    if (ctx.gpr[3] || vm_read32(0x20000) != 0xFFFFFFFEu) return 278;
    call(ctx, "sys_lwmutex_lock", 0x20000);
    if ((u32)ctx.gpr[3] != (u32)CELL_ESRCH) return 279;
    call(ctx, "sys_lwmutex_destroy", 0x20000);
    if ((u32)ctx.gpr[3] != (u32)CELL_ESRCH) return 280;
    ctx.gpr[4] = 0x21000; call(ctx, "sys_lwmutex_create", 0x20000);
    if (ctx.gpr[3] || vm_read32(0x20000)) return 281;
    call(ctx, "sys_time_get_system_time", 0);
    auto before = ctx.gpr[3];
    call(ctx, "sys_time_get_system_time", 0);
    if (ctx.gpr[3] < before || value != 2000 || vm_read32(0x20000) != 0) return 4;
    ctx.gpr[13] = 0xE001234;
    g_active_ctx = &ctx;
    vm_write32(0x38000, 0x12380); vm_write32(0x38004, 0x12340000);
    ppu_register_function(0x12380, native_guest_callback);
    auto callback_result = ppu_guest_call(0x38000, 10,2,0,0,0,0,0,0);
    if (callback_result != 12 || g_active_ctx != &ctx || callback_tls != ctx.gpr[13] ||
        callback_tid != 17 || callback_toc != 0x12340000) return 95;
    callback_result = ppu_guest_call_ct(0x12380, 0x12340000, 10,3,0,0,0,0,0,0);
    if (callback_result != 13 || g_active_ctx != &ctx || callback_tls != ctx.gpr[13] || callback_tid != 17) return 96;
    g_active_ctx = nullptr;
    ppu_install_thread_trampoline();
    ctx.thread_id = ppu_thread_register_main_priority(2000);
    ctx.gpr[11] = 48; ctx.gpr[3] = ctx.thread_id; ctx.gpr[4] = 0x39020; lv2_syscall(&ctx);
    if (ctx.gpr[3] || vm_read32(0x39020) != 2000) return 106;
    vm_write32(0x39020, 0xA5A5A5A5); ctx.gpr[3] = 9999; lv2_syscall(&ctx);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x39020) != 0xA5A5A5A5) return 107;
    vm_write32(0x39100, 0x12400); vm_write32(0x39104, 0);
    std::memcpy(vm_base + 0x39300, "own worker", 11);
    ppu_register_function(0x12400, native_guest_thread);
    vm_write32(0xD0101120, 0x12420); vm_write32(0xD0101124, 0xABCD0000);
    ppu_register_function(0x12420, native_thread_atexit);
    call(ctx, "sys_ppu_thread_register_atexit", 0x139120000ull);
    if ((u32)ctx.gpr[3] != (u32)CELL_EFAULT) return 271;
    call(ctx, "sys_ppu_thread_register_atexit", 0xD0101120);
    if (ctx.gpr[3]) return 272;
    call(ctx, "sys_ppu_thread_register_atexit", 0xD0101120);
    if ((u32)ctx.gpr[3] != (u32)CELL_EPERM) return 273;
    ctx.gpr[4] = 0x39100; ctx.gpr[5] = 0x39200; ctx.gpr[6] = 1001;
    ctx.gpr[7] = 0x10000; ctx.gpr[8] = 1; ctx.gpr[9] = 0x39300;
    call(ctx, "sys_ppu_thread_create", 0x39000);
    auto worker_id = vm_read64(0x39000);
    if (ctx.gpr[3] || !worker_id || worker_id == ctx.thread_id) return 97;
    ctx.gpr[11] = 44; ctx.gpr[3] = worker_id; ctx.gpr[4] = 0x39010; lv2_syscall(&ctx);
    if (ctx.gpr[3] || vm_read64(0x39010) != 0x1122334455667788ull ||
        vm_read32(0x39200) != 0xC0FFEE || continued_after_exit.load()) return 98;
    if (thread_atexit_calls.load() != 1) return 274;
    call(ctx, "sys_ppu_thread_unregister_atexit", 0xD0101120);
    if (ctx.gpr[3]) return 275;
    call(ctx, "sys_ppu_thread_unregister_atexit", 0xD0101120);
    if ((u32)ctx.gpr[3] != (u32)CELL_ESRCH) return 276;
    for (unsigned i = 0; i < 8; ++i) {
        vm_write32(0xD0101140 + i * 8, 0x12420);
        vm_write32(0xD0101144 + i * 8, 0xABCD0000);
        call(ctx, "sys_ppu_thread_register_atexit", 0xD0101140 + i * 8);
        if (ctx.gpr[3]) return 282;
    }
    call(ctx, "sys_ppu_thread_register_atexit", 0xD0101120);
    if ((u32)ctx.gpr[3] != (u32)CELL_ENOMEM) return 283;
    for (unsigned i = 0; i < 8; ++i) {
        call(ctx, "sys_ppu_thread_unregister_atexit", 0xD0101140 + i * 8);
        if (ctx.gpr[3]) return 284;
    }
    ctx.gpr[11] = 169; ctx.gpr[3] = 6; ctx.gpr[4] = 6; lv2_syscall(&ctx);
    if ((int64_t)ctx.gpr[3] >= 0) return 99;
    ctx.gpr[3] = 6; ctx.gpr[4] = 1; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 100;
    // Internal pool test only: guest raw-SPU creation/start remain outside the execution allowlist.
    ctx.gpr[11] = 160; ctx.gpr[3] = 0x39400; ctx.gpr[4] = 0;
    if (!lv2_try_syscall(&ctx) || ctx.gpr[3]) return 101;
    auto raw_id = vm_read32(0x39400);
    ctx.gpr[11] = 169; ctx.gpr[3] = 6; ctx.gpr[4] = 0; lv2_syscall(&ctx);
    if ((int64_t)ctx.gpr[3] >= 0) return 102;
    ctx.gpr[11] = 160; ctx.gpr[3] = 0x39404; vm_write32(0x39404, 0xA5A5A5A5);
    if (!lv2_try_syscall(&ctx) || (int64_t)ctx.gpr[3] >= 0 || vm_read32(0x39404) != 0xA5A5A5A5) return 103;
    ctx.gpr[11] = 161; ctx.gpr[3] = raw_id; if (!lv2_try_syscall(&ctx) || ctx.gpr[3]) return 104;
    ctx.gpr[11] = 169; ctx.gpr[3] = 6; ctx.gpr[4] = 0; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 105;
    const auto condition_mutex = reserved_a + 0x9000, condition = reserved_a + 0x9040;
    const auto condition_attr = reserved_a + 0x9060, condition_mutex_attr = reserved_a + 0x9080;
    vm_write32(condition_mutex_attr, 1); vm_write32(condition_mutex_attr + 4, 0x10);
    std::memcpy(vm_base + condition_attr, "own-cond", 8);
    ctx.gpr[4] = condition_mutex_attr; call(ctx, "sys_lwmutex_create", condition_mutex);
    ctx.gpr[4] = condition_mutex; ctx.gpr[5] = condition_attr; call(ctx, "sys_lwcond_create", condition);
    const auto condition_id = vm_read32(condition + 4);
    if (ctx.gpr[3] || vm_read32(condition) != condition_mutex || !condition_id) return 174;
    ctx.gpr[4] = 200; call(ctx, "sys_lwcond_wait", condition);
    if ((uint32_t)ctx.gpr[3] != CELL_EPERM) return 175;
    ctx.gpr[4] = 0; call(ctx, "sys_lwmutex_lock", condition_mutex); call(ctx, "sys_lwmutex_lock", condition_mutex);
    const auto condition_start = std::chrono::steady_clock::now();
    ctx.gpr[4] = 300; call(ctx, "sys_lwcond_wait", condition);
    if ((uint32_t)ctx.gpr[3] != CELL_ETIMEDOUT ||
        std::chrono::steady_clock::now() - condition_start < std::chrono::microseconds(300) ||
        vm_read32(condition_mutex) != ctx.thread_id || vm_read32(condition_mutex + 12) != 2) return 176;
    call(ctx, "sys_lwmutex_unlock", condition_mutex); call(ctx, "sys_lwmutex_unlock", condition_mutex);
    std::atomic<bool> condition_entered{false}, condition_finished{false}, condition_failed{false};
    std::thread condition_thread([&] {
        ppu_context local{}; local.thread_id = 18; local.gpr[1] = 0xD030FFF0;
        local.gpr[4] = 0; call(local, "sys_lwmutex_lock", condition_mutex);
        condition_entered.store(true);
        local.gpr[4] = 0; call(local, "sys_lwcond_wait", condition);
        condition_failed.store(local.gpr[3] || vm_read32(condition_mutex) != 18);
        call(local, "sys_lwmutex_unlock", condition_mutex);
        condition_finished.store(true);
    });
    while (!condition_entered.load()) std::this_thread::yield();
    ctx.gpr[4] = 0; call(ctx, "sys_lwmutex_lock", condition_mutex);
    call(ctx, "sys_lwcond_destroy", condition);
    const bool refused_destroy = (uint32_t)ctx.gpr[3] == CELL_EBUSY;
    call(ctx, "sys_lwcond_signal", condition);
    const bool signal_succeeded = ctx.gpr[3] == 0, waiter_blocked = !condition_finished.load();
    call(ctx, "sys_lwmutex_unlock", condition_mutex);
    condition_thread.join();
    if (!refused_destroy || !signal_succeeded || !waiter_blocked || condition_failed.load()) return 177;
    call(ctx, "sys_lwcond_signal_all", condition);
    if (ctx.gpr[3]) return 178;
    call(ctx, "sys_lwcond_destroy", condition);
    if (ctx.gpr[3] || vm_read32(condition + 4) != 0xFFFFFFFE) return 179;
    call(ctx, "sys_lwcond_destroy", condition);
    if ((uint32_t)ctx.gpr[3] != CELL_ESRCH) return 180;
    const auto group_attr = reserved_a + 0x7000, group_output = reserved_a + 0x7100;
    ps3_hle_register(ps3_compute_nid("cellPadInit"), "cellPadInit", (void*)cellPadInit);
    ps3_hle_register(ps3_compute_nid("cellPadEnd"), "cellPadEnd", (void*)cellPadEnd);
    call(ctx, "cellPadEnd", 0);
    if ((u32)ctx.gpr[3] != 0x80121104) return 217;
    const u32 pad_info = group_attr + 0x200;
    call(ctx, "cellPadGetInfo2", pad_info);
    if ((u32)ctx.gpr[3] != (u32)CELL_PAD_ERROR_NOT_OPENED) return 242;
    call(ctx, "cellPadInit", 0);
    if ((u32)ctx.gpr[3] != 0x80121102) return 218;
    call(ctx, "cellPadInit", 128);
    if ((u32)ctx.gpr[3] != 0x80121102) return 219;
    call(ctx, "cellPadInit", 4);
    if (ctx.gpr[3]) return 220;
    vm_write32(pad_info - 4, 0xA5A5A5A5); vm_write32(pad_info + sizeof(CellPadInfo2), 0xA5A5A5A5);
    call(ctx, "cellPadGetInfo2", pad_info);
    if (ctx.gpr[3] || vm_read32(pad_info) != 4 || vm_read32(pad_info + 4) > 4 ||
        vm_read32(pad_info - 4) != 0xA5A5A5A5 || vm_read32(pad_info + sizeof(CellPadInfo2)) != 0xA5A5A5A5) return 243;
    u32 connected_pads = 0;
    for (u32 port = 0; port < 7; ++port) {
        const u32 status = vm_read32(pad_info + 12 + port * 4), caps = vm_read32(pad_info + 68 + port * 4);
        if (status & CELL_PAD_STATUS_CONNECTED) {
            ++connected_pads;
            if (port >= 4 || !(caps & CELL_PAD_CAPABILITY_PS3_CONFORMITY) ||
                (caps & (CELL_PAD_CAPABILITY_PRESS_MODE | CELL_PAD_CAPABILITY_SENSOR_MODE))) return 244;
        } else if (caps) return 245;
    }
    if (connected_pads != vm_read32(pad_info + 4)) return 246;
    for (u64 bad : {0ull, 0xFFFFFFFEull, 0x100000000ull + pad_info}) {
        call(ctx, "cellPadGetInfo2", bad);
        if ((u32)ctx.gpr[3] != (u32)CELL_PAD_ERROR_INVALID_PARAMETER) return 247;
    }
    call(ctx, "cellPadInit", 4);
    if ((u32)ctx.gpr[3] != 0x80121103) return 221;
    call(ctx, "cellPadEnd", 0);
    if (ctx.gpr[3]) return 222;
    vm_write32(group_attr, 0); vm_write32(group_attr + 4, 0); vm_write32(group_attr + 8, 0x18);
    auto create_group = [&](uint32_t count) {
        ctx.gpr[11] = 170; ctx.gpr[3] = group_output; ctx.gpr[4] = count;
        ctx.gpr[5] = 127; ctx.gpr[6] = group_attr; lv2_syscall(&ctx);
        return (uint32_t)ctx.gpr[3];
    };
    vm_write32(group_output, 0xA5A5A5A5); vm_write32(group_attr, 129);
    if (create_group(5) != 0x80010002 || vm_read32(group_output) != 0xA5A5A5A5) return 154;
    vm_write32(group_attr, 0);
    if (create_group(5) || !vm_read32(group_output) || vm_read32(group_output) == 0xA5A5A5A5) return 155;
    const auto first_spu_group = vm_read32(group_output);
    vm_write32(group_output, 0xA5A5A5A5);
    if (create_group(2) != 0x8001000A || vm_read32(group_output) != 0xA5A5A5A5) return 156;
    vm_write32(group_attr + 8, 0);
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    const auto available_before_context = vm_read32(0x25004);
    if (create_group(1) || vm_read32(group_output) == first_spu_group) return 157;
    const auto event_spu_group = vm_read32(group_output);
    ctx.gpr[11] = 352; ctx.gpr[3] = 0x25000; lv2_syscall(&ctx);
    if (ctx.gpr[3] || vm_read32(0x25004) != available_before_context - 0x40000) return 216;
    ctx.gpr[11] = 173; ctx.gpr[3] = 9999; lv2_syscall(&ctx);
    if ((u32)ctx.gpr[3] != CELL_ESRCH) return 195;
    ctx.gpr[11] = 173; ctx.gpr[3] = first_spu_group; lv2_syscall(&ctx);
    if ((u32)ctx.gpr[3] != CELL_ESTAT) return 196;
    ctx.gpr[11] = 169; ctx.gpr[3] = 6; ctx.gpr[4] = 1; lv2_syscall(&ctx);
    if ((uint32_t)ctx.gpr[3] != 0x8001000A) return 158;
    const auto thread_attr = reserved_a + 0x7200, thread_args = reserved_a + 0x7300, thread_output = reserved_a + 0x7400;
    vm_write32(thread_output + 16, 0xA5A5A5A5); vm_write32(thread_output + 20, 0xA5A5A5A5);
    ctx.gpr[11] = 178; ctx.gpr[3] = 9999; ctx.gpr[4] = thread_output + 16; ctx.gpr[5] = thread_output + 20; lv2_syscall(&ctx);
    if ((u32)ctx.gpr[3] != CELL_ESRCH || vm_read32(thread_output + 16) != 0xA5A5A5A5 || vm_read32(thread_output + 20) != 0xA5A5A5A5) return 212;
    ctx.gpr[11] = 178; ctx.gpr[3] = first_spu_group; lv2_syscall(&ctx);
    if ((u32)ctx.gpr[3] != CELL_ESTAT || vm_read32(thread_output + 16) != 0xA5A5A5A5 || vm_read32(thread_output + 20) != 0xA5A5A5A5) return 213;
    vm_write32(thread_attr, 0); vm_write32(thread_attr + 4, 0); vm_write32(thread_attr + 8, 0);
    for (uint32_t i = 0; i < 4; ++i) vm_write64(thread_args + i * 8, 0x1122334400000042ull + i);
    auto initialize_spu = [&](uint32_t index) {
        ctx.gpr[11] = 172; ctx.gpr[3] = thread_output; ctx.gpr[4] = first_spu_group;
        ctx.gpr[5] = index; ctx.gpr[6] = image_output; ctx.gpr[7] = thread_attr; ctx.gpr[8] = thread_args;
        lv2_syscall(&ctx); return (uint32_t)ctx.gpr[3];
    };
    vm_write32(thread_output, 0xA5A5A5A5);
    if (initialize_spu(0) != 0x80010002 || vm_read32(thread_output) != 0xA5A5A5A5) return 161;
    vm_write32(image_source + 68, 16); vm_write32(image_source + 72, 32);
    ctx.gpr[4] = image_source; ctx.gpr[5] = 1; call(ctx, "sys_spu_image_import", image_output);
    if (ctx.gpr[3] || initialize_spu(0)) return 162;
    static spu_context deployed_spu{};
    spu_lifted_thread_desc deployed_desc{};
    deployed_desc.tid = 0x1234; deployed_desc.group_id = first_spu_group;
    deployed_desc.entry = 0x100; deployed_desc.img_ea = image_output; deployed_desc.image_id = 901;
    deployed_desc.args[0] = 0x1122334455667788ull;
    const auto deploy_segments = vm_read32(image_output + 8);
    vm_write32(deploy_segments + 40, 0x1234ABCD);
    spu_lifted_thread_setup(&deployed_spu, &deployed_desc);
    if (deployed_spu.pc != 0x100 || deployed_spu.image_id != 901 || deployed_spu.gpr[1]._u32[0] ||
        deployed_spu.gpr[3]._u32[0] != 0x11223344 || deployed_spu.gpr[3]._u32[1] != 0x55667788 ||
        std::memcmp(deployed_spu.ls + 0x100, vm_base + image_source + 128, 16)) return 197;
    for (u32 i = 0; i < 16; i += 4)
        if (spu_ls_read32(&deployed_spu, 0x110 + i) != 0x1234ABCD) return 198;
    vm_write32(deploy_segments + 40, 0);
    const auto prepared_spu = vm_read32(thread_output);
    if (!prepared_spu || prepared_spu == 0xA5A5A5A5 || spu_thread_get_group_id(prepared_spu) != first_spu_group) return 163;
    vm_write32(thread_output, 0xA5A5A5A5);
    if (initialize_spu(0) != 0x8001000A || vm_read32(thread_output) != 0xA5A5A5A5 ||
        initialize_spu(256) != 0x80010002 || vm_read32(thread_output) != 0xA5A5A5A5) return 164;
    vm_write32(thread_attr + 8, 2);
    if (initialize_spu(1) || vm_read32(thread_output) == prepared_spu) return 165;
    ctx.gpr[3] = thread_output; ctx.gpr[4] = event_spu_group; ctx.gpr[5] = 0; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 168;
    const auto event_spu_thread = vm_read32(thread_output);
    const auto once_opd = reserved_a + 0x2000, once_control = once_opd + 0x20;
    vm_write32(once_opd, 0x12480); vm_write32(once_opd + 4, 0);
    vm_write32(once_control, 0);
    ppu_register_function(0x12480, native_once_initializer);
    g_ps3_guest_caller = nullptr;
    if (!sys_ppu_thread_once(once_control, once_opd) || vm_read32(once_control)) return 108;
    g_ps3_guest_caller = synchronous_guest;
    std::atomic<bool> second_started{false}, second_finished{false}, once_bad{false};
    auto once_worker = [&](bool second) {
        ppu_context local{}; local.thread_id = second ? 102 : 101;
        local.gpr[1] = second ? 0xD032FFF0 : 0xD031FFF0;
        local.gpr[1] = second ? 0xD040FFF0 : 0xD030FFF0;
        g_active_ctx = &local;
        if (second) second_started.store(true);
        local.gpr[4] = once_opd; call(local, "sys_ppu_thread_once", once_control);
        if (vm_read32(once_control) != 1 || vm_read32(0x39600) != 0xC001C0DE) once_bad.store(true);
        if (second) second_finished.store(true);
        g_active_ctx = nullptr;
    };
    std::thread once_first(once_worker, false);
    while (!once_entered.load()) std::this_thread::yield();
    if (vm_read32(once_control) != 0) once_bad.store(true);
    std::thread once_second(once_worker, true);
    while (!second_started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    bool returned_early = second_finished.load();
    once_release.store(true);
    once_first.join(); once_second.join();
    if (returned_early || once_bad.load() || once_callback_bad.load() ||
        !once_callback_stack.load() || ps3_native_guest_range_backed(once_callback_stack.load(), 24) || once_calls != 1 ||
        sys_ppu_thread_once(once_control, once_opd) || once_calls != 1) return 109;
    const auto event_attr = reserved_a + 0x2100, event_output = event_attr + 0x20;
    vm_write32(event_attr, 1); vm_write32(event_attr + 4, 1);
    vm_write64(event_attr + 8, 0x4F574E5445535400ull);
    auto create_queue = [&](uint64_t key, int size) {
        ctx.gpr[11] = 128; ctx.gpr[3] = event_output; ctx.gpr[4] = event_attr;
        ctx.gpr[5] = key; ctx.gpr[6] = size; lv2_syscall(&ctx);
        return (int64_t)ctx.gpr[3];
    };
    vm_write32(event_output, 0xA5A5A5A5);
    if (create_queue(0, 0) >= 0 || vm_read32(event_output) != 0xA5A5A5A5 ||
        create_queue(0, 128) >= 0) return 110;
    vm_write32(event_attr, 99);
    if (create_queue(0, 127) >= 0 || vm_read32(event_output) != 0xA5A5A5A5) return 111;
    vm_write32(event_attr, 1);
    if (create_queue(0x12345678, 127) || !vm_read32(event_output)) return 112;
    auto first_queue = vm_read32(event_output);
    vm_write32(event_output, 0xA5A5A5A5);
    if (create_queue(0x12345678, 127) >= 0 || vm_read32(event_output) != 0xA5A5A5A5) return 113;
    if (create_queue(0, 1) || vm_read32(event_output) == first_queue) return 114;
    const auto one_slot_queue = vm_read32(event_output);
    ctx.gpr[11] = 134; ctx.gpr[3] = event_output; ctx.gpr[4] = 1;
    ctx.gpr[5] = 0xFFFFFFFF0000DEADull; lv2_syscall(&ctx);
    if (ctx.gpr[3] || !vm_read32(event_output)) return 116;
    const auto event_port = vm_read32(event_output);
    ctx.gpr[11] = 136; ctx.gpr[3] = event_port; ctx.gpr[4] = 0; lv2_syscall(&ctx);
    if ((int64_t)ctx.gpr[3] >= 0) return 117;
    ctx.gpr[3] = event_port; ctx.gpr[4] = first_queue; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 118;
    ctx.gpr[3] = event_port; lv2_syscall(&ctx);
    if ((int64_t)ctx.gpr[3] >= 0) return 119;
    ctx.gpr[11] = 134; ctx.gpr[3] = event_output; ctx.gpr[4] = 1; ctx.gpr[5] = 0; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 183;
    const auto anonymous_port = vm_read32(event_output);
    auto send_port = [&](uint32_t port) {
        ctx.gpr[11] = 138; ctx.gpr[3] = port; ctx.gpr[4] = 0x1111222233334444ull;
        ctx.gpr[5] = 0x5555666677778888ull; ctx.gpr[6] = 0x9999AAAABBBBCCCCull; lv2_syscall(&ctx);
        return (uint32_t)ctx.gpr[3];
    };
    if (send_port(anonymous_port) != CELL_ENOTCONN) return 184;
    ctx.gpr[11] = 136; ctx.gpr[3] = anonymous_port; ctx.gpr[4] = one_slot_queue; lv2_syscall(&ctx);
    if (ctx.gpr[3] || send_port(anonymous_port) || send_port(anonymous_port) != CELL_EBUSY) return 185;
    ctx.gpr[11] = 130; ctx.gpr[3] = one_slot_queue; ctx.gpr[4] = 0; ctx.gpr[5] = 2000; lv2_syscall(&ctx);
    if (ctx.gpr[3] || ctx.gpr[4] != (((uint64_t)process_id << 32) | anonymous_port) ||
        ctx.gpr[5] != 0x1111222233334444ull || ctx.gpr[6] != 0x5555666677778888ull ||
        ctx.gpr[7] != 0x9999AAAABBBBCCCCull) return 186;
    if (send_port(event_port)) return 187;
    ctx.gpr[11] = 130; ctx.gpr[3] = first_queue; ctx.gpr[4] = 0; ctx.gpr[5] = 2000; lv2_syscall(&ctx);
    if (ctx.gpr[3] || ctx.gpr[4] != 0xFFFFFFFF0000DEADull) return 188;
    if (create_queue(0, 1)) return 189;
    const auto unmatched_queue = vm_read32(event_output);
    if (send_port(unmatched_queue) != CELL_ESRCH) return 190;
    ctx.gpr[11] = 130; ctx.gpr[3] = unmatched_queue; ctx.gpr[4] = 0; ctx.gpr[5] = 200; lv2_syscall(&ctx);
    if ((uint32_t)ctx.gpr[3] != CELL_ETIMEDOUT) return 191;
    // A raw receive returns four registers and never writes its dummy buffer.
    std::memset(vm_base + event_output + 8, 0xA5, 32);
    const auto event_clock = std::chrono::steady_clock::now();
    ctx.gpr[11] = 130; ctx.gpr[3] = first_queue; ctx.gpr[4] = event_output + 8;
    ctx.gpr[5] = 200; lv2_syscall(&ctx);
    if ((uint32_t)ctx.gpr[3] != 0x8001000B ||
        std::chrono::steady_clock::now() - event_clock < std::chrono::microseconds(200)) return 147;
    std::atomic<bool> receive_entered{false}, receive_finished{false};
    ppu_context receiver{};
    receiver.gpr[11] = 130; receiver.gpr[3] = first_queue; receiver.gpr[4] = event_output + 8;
    receiver.thread_id = 18;
    std::thread receive_thread([&] {
        receive_entered.store(true); lv2_syscall(&receiver); receive_finished.store(true);
    });
    while (!receive_entered.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool premature_receive = receive_finished.load();
    const auto pushed = sys_event_queue_push_by_id(first_queue, 0x123456789ABCDEF0ull, 2, 3, 4);
    receive_thread.join();
    if (pushed || premature_receive || receiver.gpr[3] || receiver.gpr[4] != 0x123456789ABCDEF0ull ||
        receiver.gpr[5] != 2 || receiver.gpr[6] != 3 || receiver.gpr[7] != 4 ||
        vm_read64(event_output + 8) != 0xA5A5A5A5A5A5A5A5ull) return 148;
    const auto port_output = reserved_a + 0x8400;
    auto connect_spu_group = [&](uint32_t group, uint32_t queue, uint64_t mask) {
        ctx.gpr[11] = 251; ctx.gpr[3] = group; ctx.gpr[4] = queue;
        ctx.gpr[5] = mask; ctx.gpr[6] = port_output; lv2_syscall(&ctx);
        return (uint32_t)ctx.gpr[3];
    };
    vm_write32(port_output, 0xA5A5A5A5);
    if (connect_spu_group(first_spu_group, first_queue, 2) != CELL_ESTAT ||
        connect_spu_group(event_spu_group, 9999, 2) != CELL_ESRCH ||
        connect_spu_group(event_spu_group, first_queue, 0) != CELL_EINVAL ||
        vm_read32(port_output) != 0xA5A5A5A5) return 169;
    if (connect_spu_group(event_spu_group, first_queue, 1ull << 63) ||
        vm_read8(port_output) != 63 || vm_read8(port_output + 1) != 0xA5) return 170;
    vm_write32(port_output, 0xA5A5A5A5);
    if (connect_spu_group(event_spu_group, first_queue, 1ull << 63) != CELL_EISCONN ||
        vm_read32(port_output) != 0xA5A5A5A5 ||
        connect_spu_group(event_spu_group, first_queue, (1ull << 63) | 2) || vm_read8(port_output) != 1) return 171;
    static spu_context event_spu{};
    spu_context_init(&event_spu, event_spu_thread);
    event_spu.spu_id = event_spu_thread; event_spu.spu_group_id = event_spu_group;
    spu_channel_write(&event_spu.ch_out_mbox, 0xCAFEBABE);
    if (!g_spu_user_event_hook || !g_spu_user_event_hook(&event_spu, 0x3F001234) ||
        event_spu.ch_out_mbox.count || event_spu.ch_in_mbox.count != 1 || event_spu.ch_in_mbox.value) return 172;
    ctx.gpr[11] = 130; ctx.gpr[3] = first_queue; ctx.gpr[4] = 0; ctx.gpr[5] = 2000; lv2_syscall(&ctx);
    if (ctx.gpr[3] || ctx.gpr[4] != 0xFFFFFFFF53505501ull || ctx.gpr[5] != event_spu_thread ||
        ctx.gpr[6] != 0x3F00001234ull || ctx.gpr[7] != 0xCAFEBABE) return 173;
    auto connect_lifecycle = [&](uint32_t group, uint32_t queue, uint32_t type) {
        ctx.gpr[11] = 185; ctx.gpr[3] = group; ctx.gpr[4] = queue; ctx.gpr[5] = type; lv2_syscall(&ctx);
        return (uint32_t)ctx.gpr[3];
    };
    if (connect_lifecycle(9999, first_queue, 2) != CELL_ESRCH ||
        connect_lifecycle(event_spu_group, first_queue, 3) != CELL_EINVAL ||
        connect_lifecycle(event_spu_group, first_queue, 4) != CELL_EINVAL ||
        connect_lifecycle(event_spu_group, 9999, 2) != CELL_ESRCH) return 192;
    if (connect_lifecycle(event_spu_group, first_queue, 2) ||
        connect_lifecycle(event_spu_group, 9999, 2) != CELL_EBUSY ||
        connect_lifecycle(event_spu_group, unmatched_queue, 1)) return 193;
    ctx.gpr[11] = 186; ctx.gpr[3] = event_spu_group; ctx.gpr[4] = 2; lv2_syscall(&ctx);
    if (ctx.gpr[3] || connect_lifecycle(event_spu_group, first_queue, 2) ||
        connect_lifecycle(event_spu_group, unmatched_queue, 1) != CELL_EBUSY) return 194;
    ctx.gpr[11] = 130; ctx.gpr[3] = unmatched_queue; ctx.gpr[4] = 0; ctx.gpr[5] = 200; lv2_syscall(&ctx);
    if ((uint32_t)ctx.gpr[3] != CELL_ETIMEDOUT) return 195;
    const u32 thread_words[] = {(0x00du << 21) | (29u << 7) | 7u,
        (0x10du << 21) | (28u << 7) | 7u, 0x102u, 0x40200000u};
    for (u32 i = 0; i < 4; ++i) vm_write32(image_source + 128 + i * 4, thread_words[i]);
    spu_begin_image(901); thread_spu_recomp_register(); spu_begin_image(0);
    spu_workload_register_img(spu_workload_fingerprint(vm_base + image_source, 144), thread_spu_func_00000100, 901, "own thread exit");
    const spu_overlay_code_segment thread_code{0x100, 16, vm_base + image_source + 128};
    if (!spu_overlay_register_initial_image(901, &thread_code, 1)) return 203;
    ctx.gpr[11] = 173; ctx.gpr[3] = event_spu_group; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 204;
    ctx.gpr[11] = 130; ctx.gpr[3] = unmatched_queue; ctx.gpr[4] = 0; ctx.gpr[5] = 1000; lv2_syscall(&ctx);
    if (ctx.gpr[3] || ctx.gpr[4] != 0xFFFFFFFF53505500ull || ctx.gpr[5] != event_spu_group || ctx.gpr[6] || ctx.gpr[7]) return 205;
    std::atomic<bool> join_entered{false}, join_finished{false};
    u32 join_result = UINT32_MAX;
    const auto join_cause = reserved_a + 0x9500, join_status = join_cause + 4;
    vm_write32(join_cause, 0xA5A5A5A5); vm_write32(join_status, 0xA5A5A5A5);
    std::thread group_joiner([&] {
        ppu_context local{}; local.thread_id = 120; local.gpr[1] = 0xD033FFF0;
        local.gpr[11] = 178; local.gpr[3] = event_spu_group;
        local.gpr[4] = join_cause; local.gpr[5] = join_status;
        join_entered.store(true); lv2_syscall(&local);
        join_result = (u32)local.gpr[3]; join_finished.store(true);
    });
    while (!join_entered.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool joined_early = join_finished.load() || vm_read32(join_cause) != 0xA5A5A5A5 || vm_read32(join_status) != 0xA5A5A5A5;
    // Internal mailbox delivery; guest syscall 190 retains its separate audit gate.
    ctx.gpr[11] = 190; ctx.gpr[3] = event_spu_thread; ctx.gpr[4] = 42;
    if (!lv2_try_syscall(&ctx) || ctx.gpr[3]) std::abort();
    group_joiner.join();
    if (joined_early || join_result || vm_read32(join_cause) != 2 || vm_read32(join_status) != 0) return 211;
    const auto thread_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        // Internal completion check; guest syscall 165 remains gated pending its full state audit.
        ctx.gpr[11] = 165; ctx.gpr[3] = event_spu_thread; ctx.gpr[4] = thread_output;
        if (!lv2_try_syscall(&ctx)) return 206;
        if ((u32)ctx.gpr[3] != CELL_ESTAT) break;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < thread_deadline);
    if (ctx.gpr[3] || vm_read32(thread_output) != 42) return 207;
    ctx.gpr[11] = 173; ctx.gpr[3] = event_spu_group; lv2_syscall(&ctx);
    if (ctx.gpr[3]) return 214;
    ctx.gpr[11] = 130; ctx.gpr[3] = unmatched_queue; ctx.gpr[4] = 0; ctx.gpr[5] = 1000; lv2_syscall(&ctx);
    if (ctx.gpr[3] || ctx.gpr[4] != 0xFFFFFFFF53505500ull || ctx.gpr[5] != event_spu_group) return 215;
    ctx.gpr[11] = 190; ctx.gpr[3] = event_spu_thread; ctx.gpr[4] = 17;
    if (!lv2_try_syscall(&ctx) || ctx.gpr[3]) return 216;
    ctx.gpr[11] = 178; ctx.gpr[3] = event_spu_group; ctx.gpr[4] = join_cause; ctx.gpr[5] = join_status; lv2_syscall(&ctx);
    if (ctx.gpr[3] || vm_read32(join_cause) != 2 || vm_read32(join_status)) return 217;
    ctx.gpr[11] = 165; ctx.gpr[3] = event_spu_thread; ctx.gpr[4] = thread_output;
    if (!lv2_try_syscall(&ctx) || ctx.gpr[3] || vm_read32(thread_output) != 17) return 218;
    const auto command_start = vm_read32(gcm_context + 8);
    const auto control = (u32)(uintptr_t)cellGcmGetControlRegister();
    const auto previous_put = vm_read32(control), previous_flips = cellGcm_flip_request_count();
    rsx_backend report_backend{}; report_backend.report = synthetic_report; report_backend.submit = synthetic_submit;
    auto* previous_backend = rsx_get_backend(); rsx_set_backend(&report_backend);
    ps3_hle_register(ps3_compute_nid("cellGcmSetFlipImmediate"), "cellGcmSetFlipImmediate", (void*)cellGcmSetFlipImmediate);
    if (ps3_compute_nid("cellGcmSetDebugOutputLevel") != 0x51C9D62Bu) return 240;
    for (u32 level = 0; level <= 2; ++level) call(ctx, "cellGcmSetDebugOutputLevel", level);
    if (ps3_compute_nid("cellGcmGetFlipStatus") != 0x72A577CEu ||
        ps3_compute_nid("cellGcmResetFlipStatus") != 0xB2E761D4u) return 241;
    call(ctx, "cellGcmResetFlipStatus", 0);
    call(ctx, "cellGcmGetFlipStatus", 0);
    if (ctx.gpr[3] != CELL_GCM_FLIP_STATUS_WAITING) return 248;
    cellGcmTickFlip();
    call(ctx, "cellGcmGetFlipStatus", 0);
    if (ctx.gpr[3] != CELL_GCM_FLIP_STATUS_WAITING || cellGcm_flip_request_count() != previous_flips) return 249;
    call(ctx, "cellGcmSetFlipImmediate", 0);
    if ((u32)ctx.gpr[3] != 0x802100FF || cellGcm_flip_request_count() != previous_flips) return 223;
    call(ctx, "cellGcmSetFlipImmediate", 8);
    if ((u32)ctx.gpr[3] != 0x802100FF || cellGcm_flip_request_count() != previous_flips) return 224;
    ctx.gpr[4] = 7; call(ctx, "cellGcmSetPrepareFlip", gcm_context);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(gcm_context + 8) != command_start) return 120;
    ctx.gpr[4] = 0; call(ctx, "cellGcmSetPrepareFlip", gcm_context);
    if (argc > 1 && std::string(argv[1]) == "gcm_prepared_multiple") {
        ctx.gpr[4] = 0; call(ctx, "cellGcmSetPrepareFlip", gcm_context); return 225;
    }
    if (ctx.gpr[3] != 1 || vm_read32(command_start) != 0x0004E944 || vm_read32(command_start + 4) ||
        vm_read32(gcm_context + 8) != command_start + 8 ||
        vm_read32(control) != previous_put + 8 || cellGcm_flip_request_count() != previous_flips ||
        cellGcm_take_flip_pending()) return 121;
    call(ctx, "cellGcmSetFlipImmediate", 0);
    if ((u32)ctx.gpr[3] != 0x802100FF) return 226;
    call(ctx, "cellGcmSetFlipImmediate", 1);
    if (ctx.gpr[3] || cellGcm_take_flip_pending()) return 227;
    cellGcmTickFlip();
    if (cellGcm_flip_request_count() != previous_flips + 1 || cellGcm_take_flip_pending()) return 122;
    if (argc > 1 && std::string(argv[1]) == "gcm_prepared_backend_missing") report_backend.submit = nullptr;
    cellGcm_rsx_process_fifo();
    if (synthetic_submits != 1 || cellGcm_flip_request_count() != previous_flips + 1 || !cellGcm_take_flip_pending() ||
        cellGcm_take_flip_pending()) return 123;
    cellGcmTickFlip(); ppu_gcm_pump();
    if (cellGcmGetFlipStatus()) return 228;
    for (u32 ordinal = 2; ordinal <= 8; ++ordinal) {
        ctx.gpr[4] = 0; call(ctx, "cellGcmSetPrepareFlip", gcm_context);
        const u32 index = ordinal & 7;
        if (ctx.gpr[3] != index || cellGcm_take_flip_pending()) return 229;
        cellGcm_rsx_process_fifo();
        if (cellGcm_take_flip_pending() || cellGcm_flip_request_count() != previous_flips + ordinal - 1) return 230;
        call(ctx, "cellGcmSetFlipImmediate", index);
        if (ctx.gpr[3] || !cellGcm_take_flip_pending() || cellGcm_flip_request_count() != previous_flips + ordinal) return 231;
        cellGcmTickFlip(); ppu_gcm_pump();
        if (cellGcmGetFlipStatus()) return 232;
    }
    auto report_method = [&](u32 method, u32 argument) {
        const auto cursor = vm_read32(gcm_context + 8);
        vm_write32(cursor, (1u << 18) | method); vm_write32(cursor + 4, argument);
        vm_write32(gcm_context + 8, cursor + 8); vm_write32(control, cursor + 8 - 0x200000);
        cellGcm_rsx_process_fifo();
    };
    report_method(0x17C8, 1); report_method(0x17CC, 1);
    report_method(0x1800, (1u << 24) | (7 * 16));
    call(ctx, "cellGcmGetTimeStamp", 7);
    if (ctx.gpr[3] != 0x12345678ABCDEF01ull || cellGcmGetReportDataAddress(7)->value != 0x89ABCDEF) return 208;
    synthetic_report_target = reserved_a + 0x9000;
    std::memset(vm_base + synthetic_report_target - 1, 0xA5, 18);
    report_method(0x1A8, 0xBAD68000);
    report_method(0x1800, (1u << 24) | (mapped_io + 0x9000));
    if (synthetic_report_early || synthetic_report_calls != 4 || vm_read64(synthetic_report_target) != 0x12345678ABCDEF01ull ||
        vm_read32(synthetic_report_target + 8) != 0x89ABCDEF || vm_read32(synthetic_report_target + 12) ||
        vm_read8(synthetic_report_target - 1) != 0xA5 || vm_read8(synthetic_report_target + 16) != 0xA5) return 209;
    report_method(0x1A8, 0x66626660); rsx_set_backend(previous_backend);
    ctx.gpr[4] = 0x21000; ctx.gpr[5] = 0x22000; ctx.gpr[6] = 0x23000;
    call(ctx, "_sys_spu_printf_initialize", 0x20000);
    if (ctx.gpr[3] || ps3_spu_printf_callback(0) != 0x20000 ||
        ps3_spu_printf_callback(1) != 0x21000 || ps3_spu_printf_callback(2) != 0x22000 ||
        ps3_spu_printf_callback(3) != 0x23000 || ps3_spu_printf_callback(4)) return 124;
    call(ctx, "_sys_spu_printf_finalize", 0);
    if (ctx.gpr[3] || ps3_spu_printf_callback(0) || ps3_spu_printf_callback(1) ||
        ps3_spu_printf_callback(2) || ps3_spu_printf_callback(3)) return 125;
    call(ctx, "_sys_spu_printf_attach_group", first_spu_group);
    if ((uint32_t)ctx.gpr[3] != 0x8001000F) return 166;
    const auto printf_opd = reserved_a + 0x8000;
    vm_write32(printf_opd, 0x12380); vm_write32(printf_opd + 4, 0x12340000);
    ctx.gpr[4] = printf_opd; ctx.gpr[5] = printf_opd; ctx.gpr[6] = printf_opd;
    call(ctx, "_sys_spu_printf_initialize", printf_opd);
    const auto original_toc = ctx.gpr[2], original_stack = ctx.gpr[1], original_tls = ctx.gpr[13];
    for (const char* name : {"_sys_spu_printf_attach_group", "_sys_spu_printf_detach_group",
         "_sys_spu_printf_attach_thread", "_sys_spu_printf_detach_thread"}) {
        ctx.gpr[4] = 0; call(ctx, name, first_spu_group);
        if (ctx.gpr[3] != first_spu_group || ctx.gpr[1] != original_stack || ctx.gpr[2] != original_toc ||
            ctx.gpr[13] != original_tls || callback_tls != original_tls || callback_tid != ctx.thread_id ||
            callback_toc != 0x12340000 || g_active_ctx) return 167;
    }
    call(ctx, "_sys_spu_printf_finalize", 0);
    vm_write32(0x33028, 0x40);
    call(ctx, "cellSpursAttributeEnableSpuPrintfIfAvailable", 0x33000);
    if (ctx.gpr[3] || vm_read32(0x33028) != 0x10000040) return 126;
    call(ctx, "cellSpursAttributeEnableSpuPrintfIfAvailable", 0x33001);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x33028) != 0x10000040) return 127;
    ctx.gpr[4] = 0x18; call(ctx, "cellSpursAttributeSetSpuThreadGroupType", 0x33000);
    if (ctx.gpr[3] || vm_read32(0x33028) != 0x30000040) return 128;
    ctx.gpr[4] = 99; call(ctx, "cellSpursAttributeSetSpuThreadGroupType", 0x33000);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x33028) != 0x30000040) return 129;
    ctx.gpr[4] = 0; call(ctx, "cellSpursAttributeSetSpuThreadGroupType", 0x33000);
    if (ctx.gpr[3] || vm_read32(0x33028) != 0x10000040) return 130;
    vm_write32(0x33028, 0x40000040);
    ctx.gpr[4] = 0x18; call(ctx, "cellSpursAttributeSetSpuThreadGroupType", 0x33000);
    if ((int64_t)ctx.gpr[3] >= 0 || vm_read32(0x33028) != 0x40000040) return 131;
    const auto bulk = reserved_a + 0x3078, bulk_source = reserved_a + 0x3200;
    call(ctx, "cellGcmGetTimeStamp", 2047);
    if (ctx.gpr[3]) return 199;
    auto* report = cellGcmGetReportDataAddress(2047);
    if (!report) return 200;
    report->timestamp = 0x12345678ABCDEF01ull;
    call(ctx, "cellGcmGetTimeStamp", 2047);
    if (ctx.gpr[3] != 0x12345678ABCDEF01ull || report->timestamp != ctx.gpr[3]) return 201;
    call(ctx, "cellGcmGetTimeStamp", 2047);
    if (ctx.gpr[3] != 0x12345678ABCDEF01ull) return 202;
    std::memcpy(vm_base + bulk_source, "own-absent-module", 18);
    ctx.gpr[4] = 0; ctx.gpr[5] = 0; call(ctx, "sys_prx_get_module_id_by_name", bulk_source);
    if ((s32)ctx.gpr[3] != (s32)0x8001112E) return 180;
    vm_write32(bulk, 0xCAFEBABE);
    ctx.gpr[4] = 0; ctx.gpr[5] = bulk; call(ctx, "sys_prx_get_module_id_by_name", bulk_source);
    if ((s32)ctx.gpr[3] != (s32)0x80010002 || vm_read32(bulk) != 0xCAFEBABE) return 181;
    ctx.gpr[4] = 1; ctx.gpr[5] = 0; call(ctx, "sys_prx_get_module_id_by_name", bulk_source);
    if ((s32)ctx.gpr[3] != (s32)0x80010002) return 182;
    ctx.gpr[4] = 0; call(ctx, "sys_prx_get_module_id_by_name", 0);
    if ((s32)ctx.gpr[3] != (s32)0x80010002) return 183;
    std::memcpy(vm_base + bulk_source, "own-string", 11);
    vm_write8(bulk - 1, 0x12); vm_write8(bulk + 11, 0x34);
    ctx.gpr[4] = bulk_source; call(ctx, "_sys_strcpy", bulk);
    if (ctx.gpr[3] != bulk || std::memcmp(vm_base + bulk, "own-string", 11) ||
        vm_read8(bulk - 1) != 0x12 || vm_read8(bulk + 11) != 0x34) return 152;
    std::memcpy(vm_base + bulk_source, "-tail", 6); vm_write8(bulk + 16, 0x56);
    ctx.gpr[4] = bulk_source; call(ctx, "_sys_strcat", bulk);
    if (ctx.gpr[3] != bulk || std::memcmp(vm_base + bulk, "own-string-tail", 16) ||
        vm_read8(bulk - 1) != 0x12 || vm_read8(bulk + 16) != 0x56) return 153;
    std::memcpy(vm_base + bulk_source, "XYZ", 3); vm_write8(bulk + 18, 0x78);
    ctx.gpr[4] = bulk_source; ctx.gpr[5] = 2; call(ctx, "_sys_strncat", bulk);
    if (ctx.gpr[3] != bulk || std::memcmp(vm_base + bulk, "own-string-tailXY", 18) ||
        vm_read8(bulk + 18) != 0x78) return 159;
    call(ctx, "_sys_strlen", bulk);
    if (ctx.gpr[3] != 17) return 160;
    vm_write8(bulk - 1, 0x12); vm_write8(bulk + 32, 0x34);
    static ppu_context reserved_ppu{};
    reserved_ppu.reserve_addr = (bulk & ~127u) + 128 + 80;
    ppu_resv_register(&reserved_ppu);
    static spu_context reserved_spu{};
    reserved_spu.resv_ea = bulk & ~127u; reserved_spu.resv_valid = 1;
    spu_lockline_lock(); spu_coh_reserve(&reserved_spu, bulk); spu_lockline_unlock();
    ctx.gpr[4] = 0xA5; ctx.gpr[5] = 32; call(ctx, "_sys_memset", bulk);
    if (ctx.gpr[3] != bulk || vm_read8(bulk - 1) != 0x12 || vm_read8(bulk + 32) != 0x34 ||
        reserved_ppu.reserve_addr <= UINT32_MAX || reserved_spu.resv_valid || !(reserved_spu.event_status & SPU_EVENT_LR)) return 132;
    for (unsigned i = 0; i < 32; ++i) if (vm_read8(bulk + i) != 0xA5) return 133;
    g_active_ctx = &reserved_ppu;
    if (ppu_stwcx32((bulk & ~127u) + 128 + 80, 0, 1)) return 134;
    g_active_ctx = nullptr;
    spu_coh_unregister(&reserved_spu);
    std::memset(vm_base + bulk_source, 0x5A, 32);
    ctx.gpr[4] = bulk_source; ctx.gpr[5] = 32; call(ctx, "_sys_memcpy", bulk);
    if (ctx.gpr[3] != bulk || std::memcmp(vm_base + bulk, vm_base + bulk_source, 32) ||
        vm_read8(bulk - 1) != 0x12 || vm_read8(bulk + 32) != 0x34) return 135;
    reserved_ppu.reserve_addr = bulk & ~127u;
    reserved_spu.resv_ea = bulk & ~127u; reserved_spu.resv_valid = 1; reserved_spu.event_status = 0;
    spu_lockline_lock(); spu_coh_reserve(&reserved_spu, bulk); spu_lockline_unlock();
    const u8 own_host_payload[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    ppu_write_coherent(bulk, own_host_payload, sizeof own_host_payload);
    if (std::memcmp(vm_base + bulk, own_host_payload, sizeof own_host_payload) ||
        reserved_ppu.reserve_addr <= UINT32_MAX || reserved_spu.resv_valid || !(reserved_spu.event_status & SPU_EVENT_LR)) return 210;
    spu_coh_unregister(&reserved_spu);
    vm_shutdown();
    std::puts("Verified first-callback retention, real mutual exclusion and monotonic clock");
    return 0;
}
