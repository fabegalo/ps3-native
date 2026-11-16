/* Own 64x64 synthetic RSX scene, shared by the host GPU test and PS3 probe.
 * Instruction encodings are exercised against ps3recomp's MIT test assemblers. */
#ifndef PS3_NATIVE_ZCULL_SCENE_H
#define PS3_NATIVE_ZCULL_SCENE_H
#include <stdint.h>
#include <string.h>
typedef void (*probe_word_fn)(uint32_t, uint32_t);
static void probe_scalar(probe_word_fn emit, uint32_t address, float value) {
    uint32_t bits; memcpy(&bits, &value, 4); emit(address, bits);
}
static void zcull_probe_scene(probe_word_fn emit, probe_word_fn store, uint32_t base) {
    emit(0x00000208, 0x148);
    emit(0x00000210, base);
    emit(0x00000214, base + 0x100000);
    emit(0x0000020C, 256);
    emit(0x0000022C, 256);
    emit(0x00000200, 64u << 16);
    emit(0x00000204, 64u << 16);
    emit(0x00000220, 1);
    emit(0x00000A00, 64u << 16);
    emit(0x00000A04, 64u << 16);
    emit(0x000008C0, 64u << 16);
    emit(0x000008C4, 64u << 16);
    probe_scalar(emit, 0xA20, 32); probe_scalar(emit, 0xA24, 32); probe_scalar(emit, 0xA28, 0.5f); probe_scalar(emit, 0xA2C, 0);
    probe_scalar(emit, 0xA30, 32); probe_scalar(emit, 0xA34, -32); probe_scalar(emit, 0xA38, 0.5f); probe_scalar(emit, 0xA3C, 0);
    emit(0x00000A74, 1);
    emit(0x00000A6C, 0x201);
    emit(0x00000A70, 1);
    emit(0x0000183C, 0);
    emit(0x00000324, 0x01010101);
    emit(0x00001D60, 0x40);
    store(base + 0x30000, 0x3E010100); store(base + 0x30004, 0xC8011C9D);
    store(base + 0x30008, 0); store(base + 0x3000C, 0);
    emit(0x000008E4, base + 0x30001);
    static const float xy[4][2] = {{-1,1},{1,1},{1,-1},{-1,-1}};
    for (uint32_t i = 0; i < 4; ++i) {
        const float vertex[8] = {xy[i][0],xy[i][1],0,1,1,1,1,1};
        for (uint32_t k = 0; k < 8; ++k) {
            uint32_t bits; memcpy(&bits, vertex + k, 4); store(base + 0x20000u + i * 32 + k * 4, bits);
        }
    }
    const uint32_t vp[8] = {0x401F8000,0x0040000D,0x8106C043,0x6021FF80,
                            0x401F8000,0x0040030D,0x8106C043,0x6021FF85};
    emit(0x00001E9C, 0);
    for (uint32_t i = 0; i < 8; ++i) emit(0x00000B80 + i * 4, vp[i]);
    emit(0x00001EA0, 0);
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t attribute = i ? 3 : 0;
        emit(0x00001680 + attribute * 4, base + 0x20000 + i * 16);
        emit(0x00001740 + attribute * 4, 2 | (4 << 4) | (32 << 8));
    }
}
#endif
