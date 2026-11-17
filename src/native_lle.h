#pragma once
#include <cstdint>
#include <cstddef>

struct ppu_context;
struct NativeLleFunction { uint32_t address; void (*entry)(ppu_context*); };
struct NativeLleExport { uint32_t nid, opd; };
struct NativeLleDataExport { const char* library; uint32_t nid, address; };
// Optional ADDR32 references to data only; preparation rejects code fixups, addends and TLS.
struct NativeLleDataImport { const char* library; uint32_t nid, slot; };
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
    const char* const* additional_libraries = nullptr;
    size_t additional_library_count = 0;
    const NativeLleDataExport* data_exports = nullptr;
    size_t data_export_count = 0;
    const NativeLleDataImport* data_imports = nullptr;
    size_t data_import_count = 0;
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
