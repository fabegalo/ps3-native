#pragma once
#include <cstdint>
#include <cstddef>

struct ppu_context;
struct NativeLleFunction { uint32_t address; void (*entry)(ppu_context*); };
struct NativeLleExport { uint32_t nid, opd; };
struct NativeLleModule {
    const char* name;
    const char* library;
    uint32_t base, size;
    const uint8_t* image;
    const NativeLleFunction* functions;
    size_t function_count;
    const NativeLleExport* exports;
    size_t export_count;
    uint32_t start_opd = 0;
};
// Startup only; loaded modules remain resident until the session process ends.
extern "C" int ps3_native_lle_load(const NativeLleModule*);
extern "C" int ps3_native_lle_dispatch(uint32_t, ppu_context*);
extern "C" int ps3_native_lle_owns(uint32_t);
extern "C" int ps3_native_lle_library(const char*);
extern "C" int ps3_native_lle_named(const char*);
extern "C" int ps3_native_lle_start(const char*, ppu_context*);
extern "C" int ps3_native_lle_defer_start(const char*);
extern "C" void ps3_native_lle_start_all(ppu_context*);
extern "C" const NativeLleModule* ps3_native_firmware_extra(size_t);
extern "C" void ps3_native_call_opd(ppu_context*, uint32_t);
extern "C" const NativeLleModule* ps3_native_firmware_definition(void);
