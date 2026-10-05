# bo1 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bo1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BO1_SOURCES
    src/main.cpp
    src/bo1_capture.cpp
    src/bo1_debug.cpp
    src/bo1_debug_ui.cpp
    src/bo1_engine.cpp
    src/bo1_audio.cpp
    src/bo1_framerate.cpp
    src/bo1_graphics.cpp
    src/bo1_players.cpp
    src/bo1_kernel.cpp
    src/bo1_settings.cpp
    src/bo1_storage.cpp
    src/bo1_tu.cpp
)

# Default game path (written to bo1.toml the first time; after that the .toml rules)
set(BO1_DEFAULT_GAME_ROOT "" CACHE PATH "Decrypted game folder")
# Title Update #11 extracted with tools/stfs.py; the code is recompiled from assets/default_tu11.xex
set(BO1_DEFAULT_UPDATE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/assets/tu/tu11" CACHE PATH "Title Update data")

if(WIN32)
    add_executable(bo1 WIN32 ${BO1_SOURCES})
else()
    add_executable(bo1 ${BO1_SOURCES})
endif()

rexglue_setup_target(bo1 GPU_PLUGINS xenos)

target_compile_definitions(bo1 PRIVATE BO1_DEFAULT_GAME_ROOT="${BO1_DEFAULT_GAME_ROOT}"
    BO1_DEFAULT_UPDATE_ROOT="${BO1_DEFAULT_UPDATE_ROOT}")
# Bootstrap build: translated from the ORIGINAL executable (config/*_manifest.base.toml) only to
# dump the Title Update #11 one with --bo1_dump_xex (see README.md). The engine hooks, whose
# addresses belong to the Title Update, are left out.
option(BO1_BOOTSTRAP "Bootstrap build from the original executable (no engine hooks)" OFF)
if(BO1_BOOTSTRAP)
    target_compile_definitions(bo1 PRIVATE BO1_BOOTSTRAP=1)
endif()
# shell32: "Saved Games" folder; ole32: CoTaskMemFree;
# advapi32: Windows user name for the default gamertag;
# winmm: timeBeginPeriod (1 ms timer).
target_link_libraries(bo1 PRIVATE shell32 ole32 advapi32 winmm)

# Native build of the translated code for x86-64-v3 CPUs (Haswell or newer). What it brings to
# the PPC -> x64 translation:
#   -mmovbe          load/store + endianness swap in a single instruction (MOVBE)
#   -mbmi -mbmi2     rlwinm/rldicl/etc. (rotates and masks) with RORX/BZHI/PEXT
#   -mlzcnt          cntlzw/cntlzd straight to LZCNT
#   -mavx2 -mf16c    VMX128 (through simde) with 256-bit instructions and half conversions
#   -mfma            PowerPC fmadd/fmsub with real FMA (same rounding as the original)
#   -ffp-contract=off  the compiler does NOT fuse mul+add pairs that were separate on PowerPC,
#                      so floating point results do not change (physics, animation, sync)
#   -O3              full optimization of the translated code
option(BO1_NATIVE_CPU "Build the translated code for x86-64-v3 (AVX2/BMI2/MOVBE/FMA)" ON)
if(BO1_NATIVE_CPU AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(BO1_NATIVE_FLAGS -O3 -mavx2 -mbmi -mbmi2 -mlzcnt -mmovbe -mf16c -mfma -ffp-contract=off)
    if(TARGET bo1_recomp)
        target_compile_options(bo1_recomp PRIVATE ${BO1_NATIVE_FLAGS})
    endif()
    target_compile_options(bo1 PRIVATE ${BO1_NATIVE_FLAGS})
endif()
