/* Own RSX shader/geometry fixture. Include the production backend to inspect
 * the swap chain's real present count without adding a diagnostic product API. */
#include "../external/ps3recomp/libs/video/rsx_d3d12_backend.c"
#include "rsx_test_programs.h"
#include "cellGcmSys.h"
#include "vm.h"

uint8_t* vm_base;
/* This fixture owns real VM commits; inspect their host backing instead of
 * the standalone host adapter's deliberately empty guest-allocation table. */
int ps3_native_guest_range_backed(u32 address, u32 size)
{
    if (!vm_base || !size || (u64)address + size > 0x100000000ull) return 0;
    uintptr_t at = (uintptr_t)vm_base + address, end = at + size;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void*)at, &info, sizeof info) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        const uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (next <= at) return 0;
        at = next;
    }
    return 1;
}
extern int cellGcm_syscall_bind_context(u32, u32);
extern u32 cellGcm_syscall_bringup(u32);
extern void cellGcm_rsx_process_fifo(void);
static rsx_state state;
static void method(u32 address, u32 value)
{
    if (rsx_process_method(&state, address, value)) {
        fprintf(stderr, "unhandled fixture method 0x%X\n", address); exit(1);
    }
}
static void scalar(u32 address, float value)
{
    u32 bits; memcpy(&bits, &value, 4); method(address, bits);
}
static void word(u32 address, u32 value)
{
    u8* p = vm_base + address;
    for (u32 i = 0; i < 4; ++i) p[i] = (u8)(value >> (24 - i * 8));
}
static void draw(void)
{
    method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    method(NV4097_DRAW_ARRAYS, 3u << 24);
    method(NV4097_SET_BEGIN_END, 0);
}
static int report(u64* timestamp, u32* value)
{
    return s_d3d12_backend.report(s_d3d12_backend.userdata, 0x1800, 1u << 24, timestamp, value);
}
static DWORD WINAPI submit_from_worker(void* unused)
{
    (void)unused;
    return d3d12_submit(NULL) ? 0 : 1;
}
int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (vm_init() || vm_commit(0xC0000000, 0x200000) || vm_commit(0x10000000, 0x100000) ||
        cellGcmInit(0x100000, 0x100000, 0x10000000) ||
        rsx_d3d12_backend_init(64, 64, "ps3-native GPU report regression")) return 1;
    rsx_state_init(&state);
    method(NV4097_SET_SURFACE_FORMAT, 0x148);
    method(NV4097_SET_SURFACE_COLOR_AOFFSET, 0);
    method(NV4097_SET_SURFACE_ZETA_OFFSET, 0x100000);
    method(NV4097_SET_SURFACE_PITCH_A, 256);
    method(NV4097_SET_SURFACE_PITCH_Z, 256);
    method(NV4097_SET_SURFACE_CLIP_HORIZONTAL, 64u << 16);
    method(NV4097_SET_SURFACE_CLIP_VERTICAL, 64u << 16);
    method(NV4097_SET_SURFACE_COLOR_TARGET, 1);
    method(NV4097_SET_VIEWPORT_HORIZONTAL, 64u << 16);
    method(NV4097_SET_VIEWPORT_VERTICAL, 64u << 16);
    method(NV4097_SET_SCISSOR_HORIZONTAL, 64u << 16);
    method(NV4097_SET_SCISSOR_VERTICAL, 64u << 16);
    scalar(0xA20, 32); scalar(0xA24, 32); scalar(0xA28, 0.5f); scalar(0xA2C, 0);
    scalar(0xA30, 32); scalar(0xA34, -32); scalar(0xA38, 0.5f); scalar(0xA3C, 0);
    method(NV4097_SET_DEPTH_TEST_ENABLE, 1);
    method(NV4097_SET_DEPTH_FUNC, 0x201);
    method(NV4097_SET_DEPTH_MASK, 1);
    method(NV4097_SET_CULL_FACE_ENABLE, 0);
    method(NV4097_SET_COLOR_MASK, 0x01010101);
    method(NV4097_SET_SHADER_CONTROL, 0x40);
    rsx_test_fp_mov_r0_col0(vm_base + 0xC0030000u, RSX_TEST_FP_SWZ_IDENT);
    method(NV4097_SET_SHADER_PROGRAM, 0x30001);
    static const float xy[4][2] = {{-1,1},{1,1},{1,-1},{-1,-1}};
    for (u32 i = 0; i < 4; ++i) {
        const float vertex[8] = {xy[i][0],xy[i][1],0,1,1,1,1,1};
        for (u32 k = 0; k < 8; ++k) {
            u32 bits; memcpy(&bits, vertex + k, 4); word(0xC0020000u + i * 32 + k * 4, bits);
        }
    }
    u8 vp[32];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    method(NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    for (u32 i = 0; i < 8; ++i) {
        u32 value; memcpy(&value, vp + i * 4, 4); method(NV4097_SET_TRANSFORM_PROGRAM + i * 4, value);
    }
    method(NV4097_SET_TRANSFORM_PROGRAM_START, 0);
    for (u32 i = 0; i < 2; ++i) {
        const u32 attribute = i ? 3 : 0;
        method(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + attribute * 4, 0x20000 + i * 16);
        method(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + attribute * 4, 2 | (4 << 4) | (32 << 8));
    }
    UINT presents_before = 0, presents_after = 0;
    if (FAILED(s_d3d.swap_chain->lpVtbl->GetLastPresentCount(s_d3d.swap_chain, &presents_before))) return 2;
    u64 first = 0, second = 0; u32 count = 0;
    if (!d3d12_report(NULL, 0x17C8, 1, NULL, NULL) || !d3d12_report(NULL, 0x17CC, 1, NULL, NULL)) return 3;
    method(NV4097_SET_COLOR_CLEAR_VALUE, 0xFF000000);
    method(NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF00);
    if (argc == 2) {
        CellGcmZcullInfo uninitialized = {0x100000,64,64,0,2,0,0,1,5,128,255,1};
        if (rsx_zcull_set(0, &uninitialized, 0x200000)) return 60;
        if (!strcmp(argv[1], "zcull_format")) method(NV4097_SET_SURFACE_FORMAT, 0x128);
        draw(); u64 stamp; u32 samples;
        report(&stamp, &samples); return 61;
    }
    method(NV4097_CLEAR_SURFACE, 0xF3);
    for (u32 type = 2; type <= 5; ++type) {
        count = 0xCAFEBABE;
        if (!d3d12_report(NULL, 0x1800, type << 24, &first, &count) || count || first < second ||
            FAILED(s_d3d.swap_chain->lpVtbl->GetLastPresentCount(s_d3d.swap_chain, &presents_after)) ||
            presents_after != presents_before) return 18;
        second = first;
    }
    draw();
    if (!report(&first, &count) || count != 4096 || first < second) { fprintf(stderr, "visible samples=%u\n", count); return 4; }
    if (!d3d12_report(NULL, 0x17C8, 1, NULL, NULL)) return 5;
    draw();
    if (!report(&second, &count) || count || second < first) { fprintf(stderr, "preserved depth samples=%u\n", count); return 6; }
    if (!d3d12_report(NULL, 0x17C8, 1, NULL, NULL)) return 11;
    method(NV4097_SET_SCISSOR_HORIZONTAL, (16u << 16) | 8u);
    method(NV4097_SET_SCISSOR_VERTICAL, (16u << 16) | 8u);
    method(NV4097_CLEAR_SURFACE, 3);
    method(NV4097_SET_SCISSOR_HORIZONTAL, 64u << 16);
    method(NV4097_SET_SCISSOR_VERTICAL, 64u << 16);
    draw();
    if (!report(&first, &count) || count != 256 || first < second) return 12;
    if (!d3d12_report(NULL, 0x17CC, 0, NULL, NULL)) return 7;
    method(NV4097_CLEAR_SURFACE, 3);
    draw();
    if (!report(&second, &count) || count != 256 || second < first) return 8;
    for (u32 zeta = 1; zeta <= 2; ++zeta) {
        method(NV4097_SET_SURFACE_FORMAT, 0x108 | (zeta << 5));
        method(NV4097_SET_ZSTENCIL_CLEAR_VALUE, zeta == 1 ? 0xFFFF : 0xFFFFFF00);
        if (!d3d12_report(NULL, 0x17C8, 1, NULL, NULL) || !d3d12_report(NULL, 0x17CC, 1, NULL, NULL)) return 13;
        method(NV4097_CLEAR_SURFACE, 1);
        draw();
        if (!report(&first, &count) || count != 4096 || first < second) return 14;
        second = first;
    }
    if (FAILED(s_d3d.swap_chain->lpVtbl->GetLastPresentCount(s_d3d.swap_chain, &presents_after)) || presents_after != presents_before) return 9;
    method(NV4097_CLEAR_SURFACE, 0xF0);
    const u64 fence_before = s_d3d.fence_values[s_d3d.frame_index];
    method(0x1D6C, 0x90);
    method(0x1D74, 0x12345678);
    if (memcmp(vm_base + VM_HLE_INJECT_BASE + 0x90, "\x12\x34\x56\x78", 4) ||
        s_d3d.fence_values[s_d3d.frame_index] <= fence_before ||
        s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[s_d3d.frame_index] ||
        FAILED(s_d3d.swap_chain->lpVtbl->GetLastPresentCount(s_d3d.swap_chain, &presents_after)) || presents_after != presents_before) return 17;
    if (!s_pending_display_content || s_d3d.draw_count) return 15;
    rsx_d3d12_backend_present();
    if (FAILED(s_d3d.swap_chain->lpVtbl->GetLastPresentCount(s_d3d.swap_chain, &presents_after)) || presents_after != presents_before + 1 || s_pending_display_content) return 16;
    for (u32 type = 2; type <= 6; ++type) {
        first = 0xAABBCCDD11223344ull; count = 0xCAFEBABE;
        if (d3d12_report(NULL, 0x1800, type << 24, &first, &count) || first != 0xAABBCCDD11223344ull || count != 0xCAFEBABE) return 10;
    }
    u32 preserved = 0;
    if (!report(&first, &preserved) || !d3d12_report(NULL, 0x17C8, 2, NULL, NULL) ||
        !report(&second, &count) || count != preserved || second < first) return 27;
    for (u32 type = 2; type <= 5; ++type)
        if (!d3d12_report(NULL, 0x1800, type << 24, &first, &count) || count || first < second) return 28;
    draw(); first = 0xAABBCCDD11223344ull; count = 0xCAFEBABE;
    if (d3d12_report(NULL, 0x1800, 2u << 24, &first, &count) || first != 0xAABBCCDD11223344ull || count != 0xCAFEBABE ||
        !d3d12_report(NULL, 0x17C8, 3, NULL, NULL) || !report(&first, &count) || count ||
        !d3d12_report(NULL, 0x1800, 2u << 24, &second, &count) || count || second < first) return 29;
    CellGcmZcullInfo region = {0x100000,64,64,0,2,0,0,1,5,128,255,1};
    method(NV4097_SET_SURFACE_FORMAT, 0x148);
    method(NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF07);
    method(NV4097_CLEAR_SURFACE, 3);
    const u64 region_fence = s_d3d.fence_values[s_d3d.frame_index];
    if (rsx_zcull_set(0, &region, 0x200000) || !rsx_zcull_get(0)->bound ||
        s_d3d.fence_values[s_d3d.frame_index] <= region_fence ||
        s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[s_d3d.frame_index]) return 40;
    method(NV4097_CLEAR_SURFACE, 3);
    if (!d3d12_report(NULL, 0x17C8, 1, NULL, NULL)) return 41;
    draw(); if (!report(&first, &count) || count != 4096) return 42;
    CellGcmZcullInfo unsupported_region = region; unsupported_region.aaFormat = 3;
    if (d3d12_set_zcull(NULL, 0, &unsupported_region) || rsx_zcull_get(0)->aaFormat) return 55;
    region.offset = 0x110000; region.cullStart = 4096;
    if (rsx_zcull_set(1, &region, 0x200000)) return 43;
    if (!report(&first, &count) || count != 4096) return 56;
    first = 0xAABBCCDD11223344ull; count = 0xCAFEBABE;
    if (d3d12_report(NULL, 0x1800, 2u << 24, &first, &count) || first != 0xAABBCCDD11223344ull || count != 0xCAFEBABE) return 57;
    method(NV4097_SET_SURFACE_ZETA_OFFSET, region.offset);
    method(NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF09); method(NV4097_CLEAR_SURFACE, 3);
    if (!d3d12_report(NULL, 0x17C8, 1, NULL, NULL)) return 44;
    draw(); if (!report(&first, &count) || count != 4096) return 45;
    region = *rsx_zcull_get(0); region.bound = 0;
    if (rsx_zcull_set(0, &region, 0x200000)) return 46;
    region.bound = 1;
    if (rsx_zcull_set(0, &region, 0x200000)) return 47;
    method(NV4097_SET_SURFACE_ZETA_OFFSET, region.offset);
    if (!d3d12_report(NULL, 0x17C8, 1, NULL, NULL)) return 48;
    draw(); if (!report(&first, &count) || count) return 49;
    method(NV4097_SET_DEPTH_TEST_ENABLE, 0); method(NV4097_SET_STENCIL_TEST_ENABLE, 1);
    method(NV4097_SET_STENCIL_FUNC, 0x202); method(NV4097_SET_STENCIL_FUNC_REF, 7);
    method(NV4097_SET_STENCIL_FUNC_MASK, 255); method(NV4097_SET_STENCIL_MASK, 0);
    draw(); if (!report(&first, &count) || count != 4096) return 50;
    method(NV4097_SET_SURFACE_ZETA_OFFSET, 0x110000);
    if (!d3d12_report(NULL, 0x17C8, 1, NULL, NULL)) return 51;
    draw(); if (!report(&first, &count) || count) return 52;
    for (u32 i = 0; i < 2; ++i) {
        region = *rsx_zcull_get(i); region.bound = 0;
        if (rsx_zcull_set(i, &region, 0x200000)) return 53;
    }
    method(NV4097_SET_STENCIL_TEST_ENABLE, 0);
    if (FAILED(s_d3d.swap_chain->lpVtbl->GetLastPresentCount(s_d3d.swap_chain, &presents_after)) || presents_after != presents_before + 1) return 54;
    if (cellGcm_syscall_bringup(0x200000) != 0xC0000000u || vm_commit(VM_RSX_CONTEXT_BASE, VM_RSX_CONTEXT_SIZE) ||
        !cellGcm_syscall_bind_context(VM_RSX_CONTEXT_BASE + 0x40, VM_RSX_CONTEXT_BASE + 0x200000)) return 19;
    if (cellGcmSetDisplayBuffer(0, 0, 256, 64, 64) || cellGcmSetDisplayBuffer(1, 0x10000, 256, 64, 64) ||
        cellGcmSetDisplayBuffer(2, 0x40000, 256, 64, 64)) return 26;
    for (u32 i = 0; i < 64 * 64; ++i) word(0xC0040000u + i * 4, 0xFF00FF00);
    method(NV4097_CLEAR_SURFACE, 0);
    if (s_d3d.draw_count || off_rt_find(0) >= 0) return 31;
    method(NV4097_SET_SURFACE_FORMAT, 0x148);
    method(NV4097_SET_SURFACE_COLOR_AOFFSET, 0);
    method(NV4097_SET_COLOR_CLEAR_VALUE, 0xFFFF0000);
    method(NV4097_CLEAR_SURFACE, 0xF0);
    if (!d3d12_submit(NULL)) return 20;
    method(NV4097_SET_SURFACE_COLOR_AOFFSET, 0x10000);
    method(NV4097_SET_COLOR_CLEAR_VALUE, 0xFF0000FF);
    method(NV4097_CLEAR_SURFACE, 0xF0);
    if (!d3d12_submit(NULL) || off_rt_find(0) < 0 || off_rt_find(0) == off_rt_find(0x10000)) return 21;
    s_dump_name = "ps3-native-scanout-fixture";
    for (u32 i = 0; i < 5; ++i) {
        if (i == 4) for (u32 pixel = 0; pixel < 64 * 64; ++pixel) word(0xC0040000u + pixel * 4, 0xFFFFFF00);
        s_d3d.dump_frames_left = 1;
        if (d3d12_scanout(NULL, i >= 3 ? 0x40000 : i == 1 ? 0x10000 : 0, 256, 64, 64) != 1) return 22;
        void* mapped = NULL; D3D12_RANGE read = {0, (SIZE_T)s_d3d.readback_pitch * 64};
        if (FAILED(s_d3d.readback_buf->lpVtbl->Map(s_d3d.readback_buf, 0, &read, &mapped)) || !mapped) return 23;
        const u8* pixel = (const u8*)mapped + 32 * s_d3d.readback_pitch + 32 * 4;
        const int correct = pixel[0] == (i == 1 || i == 3 ? 0 : 255) && pixel[1] == (i >= 3 ? 255 : 0) && pixel[2] == (i == 1 ? 255 : 0);
        D3D12_RANGE written = {0, 0}; s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf, 0, &written);
        char path[80]; snprintf(path, sizeof path, "ps3-native-scanout-fixture_%03u.bmp", i); remove(path);
        if (!correct) return 24;
    }
    s_dump_name = NULL;
    if (FAILED(s_d3d.swap_chain->lpVtbl->GetLastPresentCount(s_d3d.swap_chain, &presents_before)) ||
        d3d12_scanout(NULL, 0x20000, 256, 64, 64) != -1 || d3d12_scanout(NULL, 0, 252, 64, 64) != -1 ||
        FAILED(s_d3d.swap_chain->lpVtbl->GetLastPresentCount(s_d3d.swap_chain, &presents_after)) || presents_before != presents_after) return 25;
    method(NV4097_SET_COLOR_CLEAR_VALUE, 0xFF00FFFF); method(NV4097_CLEAR_SURFACE, 0xF0);
    const u64 reference_fence = s_d3d.fence_values[s_d3d.frame_index];
    word(0x10000000, 0x40050); word(0x10000004, 0xC0FFEE);
    word(VM_RSX_CONTEXT_BASE + 0x40, 8); word(VM_RSX_CONTEXT_BASE + 0x44, 0); word(VM_RSX_CONTEXT_BASE + 0x48, 0xA5A5A5A5);
    cellGcm_rsx_process_fifo();
    if (memcmp(vm_base + VM_RSX_CONTEXT_BASE + 0x48, "\x00\xC0\xFF\xEE", 4) ||
        s_d3d.fence_values[s_d3d.frame_index] <= reference_fence ||
        s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[s_d3d.frame_index] ||
        FAILED(s_d3d.swap_chain->lpVtbl->GetLastPresentCount(s_d3d.swap_chain, &presents_after)) || presents_after != presents_before) return 30;
    /* The window owner waits without pumping while a guest worker submits.
     * A synchronous window-title update here deadlocks against the FIFO owner. */
    method(NV4097_CLEAR_SURFACE, 0xF0);
    Sleep(1100);
    HANDLE worker = CreateThread(NULL, 0, submit_from_worker, NULL, 0, NULL);
    if (!worker || WaitForSingleObject(worker, 3000) != WAIT_OBJECT_0) return 55;
    DWORD worker_result = 1;
    if (!GetExitCodeThread(worker, &worker_result) || worker_result) return 56;
    CloseHandle(worker);
    if (rsx_d3d12_backend_pump_messages()) return 57;
    char title[256];
    GetWindowTextA(s_d3d.hwnd, title, sizeof title);
    if (!strstr(title, "draws:")) return 58;
    rsx_d3d12_backend_shutdown(); vm_shutdown();
    puts("Verified real GPU samples, timestamps, semaphore completion, preserved depth and no implicit present");
    return 0;
}
