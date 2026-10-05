# bo1mp - Call of Duty: Black Ops (multiplayer), recompiled with ReXGlue
#
# Shares all the port code with the campaign (../src): only the guest executable
# (default_mp.xex), the name and the analysis configuration (../config/default_mp_tu11.toml)
# change.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bo1mp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(BO1_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(BO1MP_SOURCES
    src/main.cpp
    ${BO1_SRC}/bo1_capture.cpp
    ${BO1_SRC}/bo1_debug.cpp
    ${BO1_SRC}/bo1_debug_ui.cpp
    ${BO1_SRC}/bo1_engine.cpp
    ${BO1_SRC}/bo1_audio.cpp
    ${BO1_SRC}/bo1_framerate.cpp
    ${BO1_SRC}/bo1_graphics.cpp
    ${BO1_SRC}/bo1_players.cpp
    ${BO1_SRC}/bo1_kernel.cpp
    ${BO1_SRC}/bo1_settings.cpp
    ${BO1_SRC}/bo1_storage.cpp
    ${BO1_SRC}/bo1_tu.cpp
)

set(BO1_DEFAULT_GAME_ROOT "" CACHE PATH "Decrypted game folder")
set(BO1_DEFAULT_UPDATE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../assets/tu/tu11" CACHE PATH "Title Update data")
# bo1.exe folder: bo1mp.exe is copied there so each one can launch the other from the game menus
# (XamLoaderLaunchTitle) and they share the runtime DLLs.
set(BO1_SHARED_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../out/build/local-relwithdebinfo"
    CACHE PATH "Campaign output folder (bo1.exe)")

if(WIN32)
    add_executable(bo1mp WIN32 ${BO1MP_SOURCES})
else()
    add_executable(bo1mp ${BO1MP_SOURCES})
endif()

rexglue_setup_target(bo1mp GPU_PLUGINS xenos)
target_include_directories(bo1mp PRIVATE ${BO1_SRC})
# BO1_APP_NAME / BO1_XEX_NAME / BO1_WINDOW_TITLE are set in src/main.cpp (a title with spaces and
# ':' does not survive the compiler command line).
# BO1_MP: src/bo1_engine.cpp uses the multiplayer engine addresses.
target_compile_definitions(bo1mp PRIVATE BO1_MP=1 BO1_DEFAULT_GAME_ROOT="${BO1_DEFAULT_GAME_ROOT}"
    BO1_DEFAULT_UPDATE_ROOT="${BO1_DEFAULT_UPDATE_ROOT}")
# Bootstrap build from the original default_mp.xex, only to dump the Title Update one (see
# ../CMakeLists.txt and README.md).
option(BO1_BOOTSTRAP "Bootstrap build from the original executable (no engine hooks)" OFF)
if(BO1_BOOTSTRAP)
    target_compile_definitions(bo1mp PRIVATE BO1_BOOTSTRAP=1)
endif()
# winmm: timeBeginPeriod (1 ms timer).
target_link_libraries(bo1mp PRIVATE shell32 ole32 advapi32 winmm)

# Same native optimizations as the campaign (see ../CMakeLists.txt for the explanation).
option(BO1_NATIVE_CPU "Build the translated code for x86-64-v3 (AVX2/BMI2/MOVBE/FMA)" ON)
if(BO1_NATIVE_CPU AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(BO1_NATIVE_FLAGS -O3 -mavx2 -mbmi -mbmi2 -mlzcnt -mmovbe -mf16c -mfma -ffp-contract=off)
    if(TARGET bo1mp_recomp)
        target_compile_options(bo1mp_recomp PRIVATE ${BO1_NATIVE_FLAGS})
    endif()
    target_compile_options(bo1mp PRIVATE ${BO1_NATIVE_FLAGS})
endif()

if(EXISTS "${BO1_SHARED_OUTPUT_DIR}")
    add_custom_command(TARGET bo1mp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:bo1mp> "${BO1_SHARED_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:bo1mp> "${BO1_SHARED_OUTPUT_DIR}"
        COMMENT "Copying bo1mp.exe next to bo1.exe"
        VERBATIM)
endif()
