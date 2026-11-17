#include "native_system_manager.h"
#include <array>
#include <cstdio>
#include <mutex>

extern "C" int ppu_process_control_at_least(uint64_t);
extern "C" int ps3_native_guest_range_backed(uint32_t, uint32_t);
extern "C" void ppu_write_coherent(uint32_t, const void*, uint32_t);
extern "C" [[noreturn]] void ps3_native_stop(void);

namespace {
std::mutex mutex;
uint64_t epoch = 0;
bool available = false;
std::array<uint8_t, 16> boot{};
uint64_t big_endian(const uint8_t* data, unsigned size) {
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | data[i];
    return value;
}
}

extern "C" uint64_t ps3_native_sm_begin(void) {
    std::lock_guard lock(mutex);
    if (epoch == UINT64_MAX) return 0;
    ++epoch; available = false; boot.fill(0);
    return epoch;
}
extern "C" int ps3_native_sm_receive_boot(uint64_t session, const uint8_t* message, size_t size) {
    // VUART framing and service 11 are audited at 4.93 LV2 0x264B68..0x264DB4.
    if (!message || size != 32 || message[0] != 1 || message[1] != 16 ||
        big_endian(message + 2, 2) || big_endian(message + 4, 4) != 16 ||
        big_endian(message + 8, 2) != 11 || big_endian(message + 10, 2)) return 0;
    std::lock_guard lock(mutex);
    if (!session || session != epoch) return 0;
    for (unsigned i = 0; i < 16; ++i) boot[i] = message[16 + i];
    available = true;
    return 1;
}
extern "C" int32_t ps3_native_sm_get_params(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    // Original permission check precedes availability and output validation.
    if (!ppu_process_control_at_least(UINT64_C(0x2000000000000000))) return int32_t(0x80010003u);
    std::array<uint8_t, 16> data;
    {
        std::lock_guard lock(mutex);
        if (!available) {
            std::fprintf(stderr, "[native-blocked] System Manager service 11 boot message is unavailable; verified startup payload required\n");
            std::fflush(stderr); ps3_native_stop();
        }
        data = boot;
    }
    // 0x261A94 receives bytes; 0xA798 publishes FOUR u64s, in a,b,d,c order.
    const uint64_t address[] = {a, b, d, c};
    const uint64_t value[] = {data[1], data[2], big_endian(data.data() + 8, 8), big_endian(data.data() + 4, 4)};
    for (unsigned i = 0; i < 4; ++i) {
        if (!address[i] || address[i] > UINT32_MAX || !ps3_native_guest_range_backed(uint32_t(address[i]), 8))
            return int32_t(0x8001000Du);
        uint8_t bytes[8];
        for (unsigned j = 0; j < 8; ++j) bytes[j] = uint8_t(value[i] >> (56 - j * 8));
        ppu_write_coherent(uint32_t(address[i]), bytes, 8);
    }
    return 0;
}
