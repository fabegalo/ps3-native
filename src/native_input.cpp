#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include "ps3emu/error_codes.h"

extern "C" [[noreturn]] void ps3_native_stop(void);
extern "C" uint8_t* vm_base;
extern "C" int ps3_native_guest_range_backed(uint32_t, uint32_t);
extern "C" void ppu_write_coherent(uint32_t, const void*, uint32_t);
namespace {
struct Wrapper {
    std::mutex data_mutex;
    std::condition_variable condition;
    uint32_t owner;
    explicit Wrapper(uint32_t pid) : owner(pid) {}
};
std::mutex wrapper_mutex;
std::unique_ptr<Wrapper> wrapper;
struct InputOwnership {
    std::mutex mutex;
    uint32_t owner = 0;
    uint8_t registrations[3][7]{};
};
std::unique_ptr<InputOwnership> ownership;
}

extern "C" int32_t ps3_native_input_query(uint32_t command, uint64_t buffer, uint64_t size) {
    if (command != 14 || size != 1) {
        std::fprintf(stderr, "[native-blocked] input manager query requires audit command=%u size=%llu\n",
                     command, (unsigned long long)size);
        std::fflush(stderr); ps3_native_stop();
    }
    if (buffer > UINT32_MAX || !ps3_native_guest_range_backed(uint32_t(buffer), 1))
        return int32_t(CELL_EFAULT);
    const uint8_t type = vm_base[uint32_t(buffer)];
    std::lock_guard<std::mutex> lock(wrapper_mutex);
    try { if (!ownership) ownership = std::make_unique<InputOwnership>(); }
    catch (const std::bad_alloc&) { return int32_t(CELL_ENOMEM); }
    std::lock_guard<std::mutex> state_lock(ownership->mutex);
    // 4.93 C3488 copies the selector in and back out; 2883B8 tests
    // the seven registration slots of types 1..3, initially empty (2880AC).
    int32_t result = int32_t(CELL_EINVAL);
    if (type >= 1 && type <= 3) {
        result = 0;
        for (uint8_t entry : ownership->registrations[type - 1]) if (entry) result = 1;
    }
    ppu_write_coherent(uint32_t(buffer), &type, 1);
    return result;
}

extern "C" int32_t ps3_native_input_ownership(void) {
    std::lock_guard<std::mutex> lock(wrapper_mutex);
    // Retail 4.93 C3860 -> 2880AC initializes the owner to zero;
    // 287E94 returns 2 while unclaimed. Ownership mutators remain gated.
    try { if (!ownership) ownership = std::make_unique<InputOwnership>(); }
    catch (const std::bad_alloc&) { return int32_t(CELL_ENOMEM); }
    std::lock_guard<std::mutex> state_lock(ownership->mutex);
    if (ownership->owner == 0) return 2;
    std::fprintf(stderr, "[native-blocked] claimed input ownership requires audit\n");
    std::fflush(stderr); ps3_native_stop();
}

extern "C" int32_t ps3_native_input_wrapper(uint32_t command, uint32_t pid) {
    if (command != 10) {
        std::fprintf(stderr, "[native-blocked] gamepad wrapper command requires audit command=%u\n", command);
        std::fflush(stderr); ps3_native_stop();
    }
    std::lock_guard<std::mutex> lock(wrapper_mutex);
    if (wrapper) return int32_t(CELL_EINVAL);
    // 4.93 0xE0574 -> 0xDFD64 creates two locks/a condition and records
    // the caller; it neither registers a device nor publishes an input report.
    try { wrapper = std::make_unique<Wrapper>(pid); }
    catch (const std::bad_alloc&) { return int32_t(CELL_ENOMEM); }
    catch (const std::system_error&) {
        std::fprintf(stderr, "[native-blocked] host input synchronization unavailable\n");
        std::fflush(stderr); ps3_native_stop();
    }
    return CELL_OK;
}
