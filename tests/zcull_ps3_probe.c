/* BSD-3-Clause. Standalone PSL1GHT probe; no game/firmware/SDK-derived code.
 * Build only with the user's existing open PSL1GHT installation. */
#include <rsx/rsx.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "zcull_cases.h"

static gcmContextData* context;
static volatile unsigned char* local;
static uint32_t base_offset, serial;
static void emit(uint32_t method, uint32_t value) {
    if (context->current + 2 > context->end) {
        fprintf(stderr, "probe command capacity exceeded\n"); exit(2);
    }
    *context->current++ = (1u << 18) | method;
    *context->current++ = value;
}
static void store(uint32_t offset, uint32_t value) {
    if (offset < base_offset || (uint64_t)offset + 4 > (uint64_t)base_offset + 0x200000) exit(2);
    volatile unsigned char* p = local + offset - base_offset;
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (24 - 8 * i));
}
static int complete(void) {
    volatile uint32_t* label = gcmGetLabelAddress(64);
    const uint32_t expected = ++serial;
    *label = 0;
    __sync_synchronize();
    rsxSetWriteBackendLabel(context, 64, expected);
    rsxFlushBuffer(context);
    for (unsigned tries = 0; tries < 5000; ++tries) {
        __sync_synchronize();
        if (*label == expected) return 1;
        usleep(1000);
    }
    fprintf(stderr, "probe timeout; no result inferred\n"); return 0;
}
static int bind_region(uint32_t index, uint32_t offset, uint32_t start) {
    if (!complete()) return 0;
    const int result = gcmBindZcull(index, offset, 64, 64, start, GCM_ZCULL_Z24S8,
        0, GCM_ZCULL_LESS, GCM_ZCULL_LONES, GCM_SCULL_SFUNC_GREATER, 128, 255);
    if (result) { fprintf(stderr, "probe bind failed: %d\n", result); return 0; }
    return 1;
}
static int read_report(const char* name, uint32_t type, uint32_t expected) {
    /* Index 100 gives the exact GET_REPORT offset 0x640 used by God of War. */
    rsxSetReport(context, type, 100);
    if (!complete()) return 0;
    volatile gcmReportData* report = gcmGetReportDataAddress(100);
    if (!report) return 0;
    __sync_synchronize();
    const uint64_t timer = report->timer;
    const uint32_t value = report->value, padding = report->zero;
    printf("{\"backend\":\"ps3\",\"case\":\"%s\",\"type\":%u,\"available\":true,"
           "\"timestamp\":\"%016llx\",\"value\":\"%08x\",\"padding\":\"%08x\",\"zpass_expected\":%u}\n",
           name, type, (unsigned long long)timer, value, padding, expected);
    fflush(stdout);
    /* A transport failure invalidates the probe; unknown types have no golden value. */
    return padding == 0 && (type != 1 || value == expected);
}
int main(void) {
    void* io = memalign(0x100000, 0x100000);
    if (!io || rsxInit(&context, 0x10000, 0x100000, io)) return 1;
    local = rsxMemalign(0x10000, 0x200000);
    if (!local || rsxAddressToOffset((const void*)local, &base_offset)) return 1;
    printf("{\"probe\":\"ps3-native-zcull-v1\",\"backend\":\"ps3\",\"local_base\":%u,"
           "\"width\":64,\"height\":64,\"samples\":1}\n", base_offset);
    zcull_probe_scene(emit, store, base_offset);
    __sync_synchronize();
    emit(0x1A8, 0x66626660);
    if (!zcull_probe_cases(emit, read_report, bind_region, base_offset)) return 2;
    printf("{\"probe_complete\":true,\"counter_semantics_verified\":false}\n");
    return 0;
}
