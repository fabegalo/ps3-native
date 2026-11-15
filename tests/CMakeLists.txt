set(gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GLOB tools "${PS3RECOMP_DIR}/tools/*.py")
add_custom_command(OUTPUT "${gen}/ppu_recomp.c" "${gen}/ppu_recomp.h" "${gen}/ppu.elf"
    "${gen}/spu_recomp.c" "${gen}/spu_recomp.h"
    "${gen}/spu_bytes.h"
    "${gen}/thread/spu_recomp.c" "${gen}/thread/spu_recomp.h"
    "${gen}/lle_bridge.cpp" "${gen}/lle_recomp.cpp"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_fixtures.py" "${PROJECT_SOURCE_DIR}" "${gen}"
    DEPENDS generate_fixtures.py lle_fixture.py "${PROJECT_SOURCE_DIR}/src/ps3native/firmware.py" ${tools})
set(scaffold "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp")
set_source_files_properties("${gen}/ppu_recomp.c" PROPERTIES LANGUAGE CXX)
add_library(fixture_ppu OBJECT ${scaffold} "${gen}/ppu_recomp.c" "${PROJECT_SOURCE_DIR}/src/native_guard.cpp" "${PROJECT_SOURCE_DIR}/src/module_hooks.cpp" "${PROJECT_SOURCE_DIR}/src/native_lle.cpp")
target_compile_definitions(fixture_ppu PRIVATE PS3_NATIVE_STRICT)
target_include_directories(fixture_ppu PUBLIC "${PROJECT_SOURCE_DIR}/src" "${gen}" "${PS3RECOMP_DIR}/include" "${PS3RECOMP_DIR}/libs/system" "${PS3RECOMP_DIR}/libs/spurs"
    "${PS3RECOMP_DIR}/runtime/ppu" "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory" "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform" "${PS3RECOMP_DIR}/libs/video")
target_link_libraries(fixture_ppu PUBLIC ps3recomp_runtime)
add_executable(ps3-native-ppu ppu_main.cpp)
target_link_libraries(ps3-native-ppu PRIVATE fixture_ppu)
add_executable(ps3-native-spu spu_main.c "${gen}/spu_recomp.c")
target_compile_definitions(ps3-native-spu PRIVATE PS3_NATIVE_STRICT)
target_link_libraries(ps3-native-spu PRIVATE fixture_ppu)
add_executable(ps3-native-lle lle_main.cpp "${gen}/lle_bridge.cpp" "${gen}/lle_recomp.cpp")
target_link_libraries(ps3-native-lle PRIVATE fixture_ppu)
target_compile_definitions(ps3-native-lle PRIVATE PS3_NATIVE_STRICT)
set_target_properties(ps3-native-spu PROPERTIES LINKER_LANGUAGE CXX)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fixture_ppu PRIVATE -fwrapv -fno-strict-aliasing)
    target_compile_options(ps3-native-lle PRIVATE -fwrapv -fno-strict-aliasing)
    if(MSVC)
        target_compile_options(fixture_ppu PRIVATE /clang:-ffp-contract=off)
        target_compile_options(ps3-native-lle PRIVATE /clang:-ffp-contract=off)
    else()
        target_compile_options(fixture_ppu PRIVATE -ffp-contract=off)
        target_compile_options(ps3-native-lle PRIVATE -ffp-contract=off)
    endif()
endif()
if(WIN32)
    foreach(target ps3-native-ppu ps3-native-spu ps3-native-lle)
        target_link_libraries(${target} PRIVATE dbghelp user32 gdi32 winmm shlwapi synchronization psapi)
    endforeach()
    add_executable(ps3-native-rsx "${PS3RECOMP_DIR}/libs/video/tests/test_live_draw_z16_clear.c"
        "${PS3RECOMP_DIR}/runtime/host/host_stubs.c")
    target_include_directories(ps3-native-rsx PRIVATE "${PS3RECOMP_DIR}/include"
        "${PS3RECOMP_DIR}/libs/video" "${PS3RECOMP_DIR}/runtime")
    target_link_libraries(ps3-native-rsx PRIVATE ps3recomp_runtime d3d12 dxgi d3dcompiler dxguid
        gdi32 user32 ole32 ws2_32 xinput bcrypt winmm shlwapi synchronization psapi dbghelp)
    set_target_properties(ps3-native-rsx PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME rsx.d3d12.pixels COMMAND ps3-native-rsx)
    set_tests_properties(rsx.d3d12.pixels PROPERTIES TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(ps3-native-rsx-reports rsx_reports.c "${PS3RECOMP_DIR}/runtime/host/host_stubs.c")
    target_compile_definitions(ps3-native-rsx-reports PRIVATE PS3_NATIVE_STRICT)
    target_include_directories(ps3-native-rsx-reports PRIVATE "${PS3RECOMP_DIR}/include"
        "${PS3RECOMP_DIR}/libs/video" "${PS3RECOMP_DIR}/runtime" "${PS3RECOMP_DIR}/runtime/memory")
    target_link_libraries(ps3-native-rsx-reports PRIVATE ps3recomp_runtime d3d12 dxgi d3dcompiler dxguid
        gdi32 user32 ole32 ws2_32 xinput bcrypt winmm shlwapi synchronization psapi dbghelp)
    set_target_properties(ps3-native-rsx-reports PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME rsx.d3d12.reports COMMAND ps3-native-rsx-reports)
    set_tests_properties(rsx.d3d12.reports PROPERTIES TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
add_test(NAME ppu.aot COMMAND ps3-native-ppu "${gen}/ppu.elf")
add_test(NAME spu.aot.dma COMMAND ps3-native-spu)
add_test(NAME ppu.aot.negative COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/expect_failure.py"
    $<TARGET_FILE:ps3-native-ppu> "${gen}/ppu.elf" wrong-oracle)
add_test(NAME spu.aot.dma.negative COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/expect_failure.py"
    $<TARGET_FILE:ps3-native-spu> corrupt-input)
set_tests_properties(ppu.aot spu.aot.dma PROPERTIES TIMEOUT 30)
set_tests_properties(ppu.aot.negative spu.aot.dma.negative PROPERTIES TIMEOUT 35)
add_executable(ps3-native-contracts native_contracts.cpp "${gen}/thread/spu_recomp.c")
target_include_directories(ps3-native-contracts PRIVATE "${PS3RECOMP_DIR}/libs/input")
target_compile_definitions(ps3-native-contracts PRIVATE PS3_NATIVE_STRICT)
target_link_libraries(ps3-native-contracts PRIVATE fixture_ppu)
if(WIN32)
    target_link_libraries(ps3-native-contracts PRIVATE dbghelp user32 gdi32 winmm shlwapi synchronization psapi)
endif()
add_test(NAME runtime.fail_closed COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_blocked.py"
    $<TARGET_FILE:ps3-native-contracts> $<TARGET_FILE:ps3-native-spu> $<TARGET_FILE:ps3-native-lle> "${gen}/ppu.elf")
add_test(NAME runtime.boot_services COMMAND ps3-native-contracts)
set_tests_properties(runtime.fail_closed runtime.boot_services PROPERTIES TIMEOUT 30)
add_test(NAME runtime.lle.aot COMMAND ps3-native-lle "${gen}/ppu.elf")
add_test(NAME runtime.lle.overlap COMMAND ps3-native-lle "${gen}/ppu.elf" overlap)
set_tests_properties(runtime.lle.aot runtime.lle.overlap PROPERTIES TIMEOUT 30)
