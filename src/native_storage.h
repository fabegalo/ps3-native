#pragma once
#include <cstdint>
#include <cstdio>
extern "C" int ps3_native_install_validate(const char*);
extern "C" int32_t ps3_native_install_create(const char*, const uint8_t*, char*, char*);
extern "C" int32_t ps3_native_install_permit(const char*, char*, char*);
extern "C" int32_t ps3_native_install_string(int32_t, const char*);
extern "C" int32_t ps3_native_install_open(const char*, uint32_t, FILE**);
extern "C" void ps3_native_install_charge(uint64_t);
