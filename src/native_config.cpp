#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <new>
#include "ps3emu/error_codes.h"

extern "C" uint64_t sys_event_queue_generation(uint32_t);
extern "C" int ps3_native_guest_range_backed(uint32_t, uint32_t);
extern "C" void ppu_write_coherent(uint32_t, const void*, uint32_t);
extern "C" [[noreturn]] void ps3_native_stop(void);

namespace {
struct Session { uint32_t queue; uint64_t generation; };
struct Listener { uint32_t config; uint64_t service, verbosity; uint32_t mode; };
std::mutex mutex;
std::map<uint32_t, Session> sessions;
std::map<uint32_t, Listener> listeners;
uint32_t next_id = 0x10000;
}

extern "C" int32_t ps3_native_config_open(uint32_t queue, uint64_t output) {
    // Retail 4.93 0xA0038 retains a queue, allocates a type-0x41 object,
    // registers its handle and copies four bytes out; 0x9707C emits no event.
    const uint64_t generation = sys_event_queue_generation(queue);
    if (!generation) return int32_t(CELL_ESRCH);
    std::lock_guard<std::mutex> lock(mutex);
    if (next_id == UINT32_MAX) {
        std::fprintf(stderr, "[native-blocked] Config handle namespace exhausted\n");
        std::fflush(stderr); ps3_native_stop();
    }
    const uint32_t id = next_id++;
    try { sessions.emplace(id, Session{queue, generation}); }
    catch (const std::bad_alloc&) { return int32_t(CELL_ENOMEM); }
    if (output > UINT32_MAX || !ps3_native_guest_range_backed((uint32_t)output, 4)) {
        sessions.erase(id); return int32_t(CELL_EFAULT);
    }
    const uint8_t bytes[] = {uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
    ppu_write_coherent((uint32_t)output, bytes, sizeof bytes);
    return CELL_OK;
}

extern "C" int32_t ps3_native_config_close(uint32_t id) {
    // Retail 4.93 0xA0C90 removes the type-0x41 handle before releasing its state.
    std::lock_guard<std::mutex> lock(mutex);
    if (!sessions.erase(id)) return int32_t(CELL_ESRCH);
    for (auto it = listeners.begin(); it != listeners.end();)
        if (it->second.config == id) it = listeners.erase(it); else ++it;
    return CELL_OK;
}

extern "C" int32_t ps3_native_config_listen(uint32_t config, uint64_t service, uint64_t verbosity,
                                           uint64_t input, uint64_t size, uint32_t mode, uint64_t output) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!sessions.count(config)) return int32_t(CELL_ESRCH);
    if (!(service >> 63) || size || mode > 1) {
        std::fprintf(stderr, "[native-blocked] config listener requires verified LV2 producer, filter or mode service=0x%016llX\n",
                     (unsigned long long)service);
        std::fflush(stderr); ps3_native_stop();
    }
    (void)input; // No bytes are read for the supported empty-filter ABI.
    if (next_id == UINT32_MAX) {
        std::fprintf(stderr, "[native-blocked] Config handle namespace exhausted\n");
        std::fflush(stderr); ps3_native_stop();
    }
    const uint32_t id = next_id++;
    try { listeners.emplace(id, Listener{config, service, verbosity, mode}); }
    catch (const std::bad_alloc&) { return int32_t(CELL_ENOMEM); }
    if (output > UINT32_MAX || !ps3_native_guest_range_backed((uint32_t)output, 4)) {
        listeners.erase(id); return int32_t(CELL_EFAULT);
    }
    const uint8_t bytes[] = {uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
    ppu_write_coherent((uint32_t)output, bytes, sizeof bytes);
    // No user services have been registered: syscall 521 remains unsupported,
    // and implicit LV2 services above stop before accepting a subscription.
    return CELL_OK;
}

extern "C" int32_t ps3_native_config_unlisten(uint32_t config, uint32_t listener) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!sessions.count(config)) return int32_t(CELL_ESRCH);
    const auto it = listeners.find(listener);
    if (it == listeners.end() || it->second.config != config) return int32_t(CELL_ESRCH);
    listeners.erase(it); return CELL_OK;
}
