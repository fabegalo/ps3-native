file(GLOB lifted CONFIGURE_DEPENDS "${PS3_NATIVE_RECOMP_DIR}/ppu_recomp*.c" "${PS3_NATIVE_RECOMP_DIR}/ppu_recomp*.cpp")
if(NOT lifted OR NOT EXISTS "${PS3_NATIVE_RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR "Local AOT output is missing")
endif()

set_source_files_properties(${lifted} PROPERTIES LANGUAGE CXX)
set(nids "${CMAKE_CURRENT_BINARY_DIR}/module/ppu_hle_nids.cpp")
file(GLOB_RECURSE hle_sources CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/libs/*.c")
add_custom_command(OUTPUT "${nids}" COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
    --out "${nids}" --all DEPENDS ${hle_sources} "${PS3RECOMP_DIR}/tools/gen_hle_nids.py")
add_executable(ps3-native-module ${lifted} "${nids}" src/native_guard.cpp src/module_hooks.cpp src/native_lle.cpp
    "${PS3RECOMP_DIR}/templates/project/main.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp" "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp" "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp")
target_include_directories(ps3-native-module PRIVATE "${PS3_NATIVE_RECOMP_DIR}"
    "${PROJECT_SOURCE_DIR}/src"
    "${PS3RECOMP_DIR}/include" "${PS3RECOMP_DIR}/runtime/ppu" "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/platform" "${PS3RECOMP_DIR}/runtime/memory" "${PS3RECOMP_DIR}/libs/spurs"
    "${PS3RECOMP_DIR}/runtime/syscalls" "${PS3RECOMP_DIR}/libs/video" "${PS3RECOMP_DIR}/libs/system")
target_compile_definitions(ps3-native-module PRIVATE PS3_NATIVE_STRICT)

set(PS3_NATIVE_FIRMWARE_DIR "" CACHE PATH "Verified original PRX AOT generation")
if(PS3_NATIVE_FIRMWARE_DIR)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c
        "import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); from ps3native.firmware import firmware_artifacts; firmware_artifacts(Path(sys.argv[2]), Path(sys.argv[3]))"
        "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}" "${PS3_NATIVE_FIRMWARE_DIR}"
        RESULT_VARIABLE firmware_rc)
    if(NOT firmware_rc EQUAL 0)
        message(FATAL_ERROR "Original PRX preparation verification failed")
    endif()
    set(firmware_source "${PS3_NATIVE_FIRMWARE_DIR}/lift/ppu_recomp.c")
    set_source_files_properties("${firmware_source}" PROPERTIES LANGUAGE CXX)
    target_sources(ps3-native-module PRIVATE "${firmware_source}" "${PS3_NATIVE_FIRMWARE_DIR}/bundle.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS3_NATIVE_FIRMWARE_DIR}/receipt.json")
endif()

set(PS3_NATIVE_EXTRA_FIRMWARE_DIRS "" CACHE STRING "Additional verified original PRX AOT generations")
if(PS3_NATIVE_EXTRA_FIRMWARE_DIRS)
    set(registry "#include \"native_lle.h\"\n")
    set(entries "")
    set(index 0)
    foreach(extra IN LISTS PS3_NATIVE_EXTRA_FIRMWARE_DIRS)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c
            "import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); from ps3native.firmware import firmware_artifacts; firmware_artifacts(Path(sys.argv[2]), Path(sys.argv[3]))"
            "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}" "${extra}" RESULT_VARIABLE extra_rc)
        if(NOT extra_rc EQUAL 0)
            message(FATAL_ERROR "Additional original PRX verification failed")
        endif()
        set(extra_source "${extra}/lift/ppu_recomp.c")
        set_source_files_properties("${extra_source}" PROPERTIES LANGUAGE CXX)
        set_source_files_properties("${extra}/bundle.cpp" PROPERTIES COMPILE_DEFINITIONS "ps3_native_firmware_definition=ps3_native_extra_${index}")
        target_sources(ps3-native-module PRIVATE "${extra_source}" "${extra}/bundle.cpp")
        string(APPEND registry "extern \"C\" const NativeLleModule* ps3_native_extra_${index}(void);\n")
        string(APPEND entries "case ${index}: return ps3_native_extra_${index}();\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${extra}/receipt.json")
        math(EXPR index "${index} + 1")
    endforeach()
    string(APPEND registry "extern \"C\" const NativeLleModule* ps3_native_firmware_extra(size_t index) { switch(index) { ${entries} default: return nullptr; } }\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/module/firmware_registry.cpp" CONTENT "${registry}")
    target_sources(ps3-native-module PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/module/firmware_registry.cpp")
endif()

set(PS3_NATIVE_SPU_DIR "" CACHE PATH "Verified local SPU AOT generation")
if(PS3_NATIVE_SPU_DIR)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c
        "import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); from ps3native.preparation import spu_artifacts; spu_artifacts(Path(sys.argv[2]), Path(sys.argv[3]))"
        "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}" "${PS3_NATIVE_SPU_DIR}"
        RESULT_VARIABLE spu_rc)
    if(NOT spu_rc EQUAL 0)
        message(FATAL_ERROR "SPU preparation verification failed")
    endif()
    file(READ "${PS3_NATIVE_SPU_DIR}/inventory.json" spu_inventory)
    string(JSON spu_count LENGTH "${spu_inventory}" programs)
    math(EXPR spu_last "${spu_count} - 1")
    target_sources(ps3-native-module PRIVATE "${PS3_NATIVE_SPU_DIR}/spu_workloads.c")
    foreach(i RANGE 0 ${spu_last})
        string(JSON status GET "${spu_inventory}" programs ${i} status)
        if(status STREQUAL "registered")
            string(JSON name GET "${spu_inventory}" programs ${i} name)
            target_sources(ps3-native-module PRIVATE "${PS3_NATIVE_SPU_DIR}/${name}/spu_recomp.c")
        endif()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS3_NATIVE_SPU_DIR}/inventory.json")
endif()
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
