/* Own probe sequence. Unknown counter payloads are observations, never oracles. */
#ifndef PS3_NATIVE_ZCULL_CASES_H
#define PS3_NATIVE_ZCULL_CASES_H
#include "zcull_scene.h"
typedef int (*probe_read_fn)(const char*, uint32_t, uint32_t);
typedef int (*probe_bind_fn)(uint32_t, uint32_t, uint32_t);
static void probe_draw(probe_word_fn emit) {
    emit(0x1808, 8); emit(0x1814, 3u << 24); emit(0x1808, 0);
}
static int probe_snapshot(probe_read_fn read, const char* name, uint32_t expected_zpass) {
    for (uint32_t type = 1; type <= 5; ++type)
        if (!read(name, type, expected_zpass)) return 0;
    return 1;
}
static int zcull_probe_cases(probe_word_fn emit, probe_read_fn read, probe_bind_fn bind, uint32_t base) {
    emit(0x1D8C, 0xFFFFFF07); emit(0x1D94, 3);
    if (!bind(0, base + 0x100000, 0)) return 0;
    emit(0x1D94, 3);
    emit(0x1D84, 3); emit(0x1804, 1); emit(0x17CC, 1);
    emit(0x17C8, 1); emit(0x17C8, 2);
    if (!probe_snapshot(read, "reset", 0)) return 0;
    probe_draw(emit);
    if (!probe_snapshot(read, "visible", 4096)) return 0;
    emit(0x17C8, 1); probe_draw(emit);
    if (!probe_snapshot(read, "hidden", 0)) return 0;
    emit(0x17C8, 1); emit(0x17C8, 2);
    if (!probe_snapshot(read, "reset_after_geometry", 0)) return 0;
    emit(0x1804, 0); emit(0x1D94, 3); probe_draw(emit);
    if (!probe_snapshot(read, "stats_disabled", 4096)) return 0;
    emit(0x17C8, 1); emit(0x1804, 1); emit(0x1D94, 3); probe_draw(emit);
    emit(0x1804, 0);
    if (!probe_snapshot(read, "enabled_then_disabled", 4096)) return 0;
    emit(0x1804, 1);
    if (!bind(1, base + 0x110000, 4096)) return 0;
    emit(0x214, base + 0x110000); emit(0x1D94, 3); emit(0x17C8, 1); probe_draw(emit);
    if (!probe_snapshot(read, "surface_swap", 4096)) return 0;
    emit(0x214, base + 0x100000); emit(0x17C8, 1); probe_draw(emit);
    if (!probe_snapshot(read, "surface_restore", 0)) return 0;
    static const char* regions[8] = {"region_0","region_1","region_2","region_3",
                                     "region_4","region_5","region_6","region_7"};
    for (uint32_t index = 0; index < 8; ++index) {
        const uint32_t offset = base + 0x100000 + index * 0x10000;
        if (!bind(index, offset, index * 4096)) return 0;
        emit(0x214, offset); emit(0x1D94, 3); emit(0x17C8, 1); probe_draw(emit);
        if (!probe_snapshot(read, regions[index], 4096)) return 0;
    }
    emit(0x17C8, 1); emit(0x17C8, 2);
    return probe_snapshot(read, "final_reset", 0);
}
#endif
