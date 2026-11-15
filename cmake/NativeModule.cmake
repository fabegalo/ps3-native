file(GLOB lifted CONFIGURE_DEPENDS "${PS3_NATIVE_RECOMP_DIR}/ppu_recomp*.c" "${PS3_NATIVE_RECOMP_DIR}/ppu_recomp*.cpp")
if(NOT lifted OR NOT EXISTS "${PS3_NATIVE_RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR "Local AOT output is missing")
endif()
set_source_files_properties(${lifted} PROPERTIES LANGUAGE CXX)
set(nids "${CMAKE_CURRENT_BINARY_DIR}/module/ppu_hle_nids.cpp")
file(GLOB_RECURSE hle_sources CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/libs/*.c")
add_custom_command(OUTPUT "${nids}" COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
    --out "${nids}" --all DEPENDS ${hle_sources} "${PS3RECOMP_DIR}/tools/gen_hle_nids.py")
add_executable(ps3-native-module ${lifted} "${nids}" src/native_guard.cpp src/module_hooks.cpp
    "${PS3RECOMP_DIR}/templates/project/main.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp" "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp" "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp")
target_include_directories(ps3-native-module PRIVATE "${PS3_NATIVE_RECOMP_DIR}"
    "${PS3RECOMP_DIR}/include" "${PS3RECOMP_DIR}/runtime/ppu" "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/platform" "${PS3RECOMP_DIR}/runtime/memory" "${PS3RECOMP_DIR}/libs/spurs"
    "${PS3RECOMP_DIR}/runtime/syscalls" "${PS3RECOMP_DIR}/libs/video" "${PS3RECOMP_DIR}/libs/system")
target_compile_definitions(ps3-native-module PRIVATE PS3_NATIVE_STRICT)
target_compile_options(ps3-native-module PRIVATE -O1 -fwrapv -fno-strict-aliasing)
if(MSVC)
    target_compile_options(ps3-native-module PRIVATE /clang:-ffp-contract=off)
else()
    target_compile_options(ps3-native-module PRIVATE -ffp-contract=off)
endif()
target_link_libraries(ps3-native-module PRIVATE ps3recomp_runtime)
if(WIN32)
    target_link_libraries(ps3-native-module PRIVATE dbghelp user32 gdi32 winmm shlwapi synchronization psapi)
    target_link_options(ps3-native-module PRIVATE /STACK:268435456)
endif()
