# nier - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nier LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NIER_SOURCES
    src/main.cpp
    src/achievements.cpp
    src/dlc.cpp
    src/memo_achievements.cpp
    src/pc_text.cpp
    src/video_options_menu.cpp
    src/game_assets.cpp
    src/game_text.cpp
    src/game_text_hooks.cpp
    src/player_skin.cpp
    src/mods.cpp
    src/key_capture.cpp
    src/game_patches.cpp
    src/crash_log.cpp
    src/title_carousel.cpp
    src/model_guards.cpp
    src/bullet_trace.cpp
    src/pad_remap.cpp
    src/perf_overlay.cpp
    src/write_watch.cpp
    src/jukebox/jukebox.cpp
)

if(WIN32)
    add_executable(nier WIN32 ${NIER_SOURCES})
    # "NieR Gestalt.exe" (the target stays "nier"); its icon is the title tile from the player's own disc, which
    # setup.ps1 writes to game\nier.ico before the build (never in the repo).
    set_target_properties(nier PROPERTIES OUTPUT_NAME "NieR Gestalt")
    if(EXISTS "${CMAKE_SOURCE_DIR}/game/nier.ico")
        enable_language(RC)
        file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/game/nier.ico" NIER_ICO)
        file(WRITE "${CMAKE_BINARY_DIR}/nier_icon.rc" "1 ICON \"${NIER_ICO}\"\n")
        target_sources(nier PRIVATE "${CMAKE_BINARY_DIR}/nier_icon.rc")
    endif()
elseif(ANDROID)
    # Loaded by SDLActivity (android/), which looks for libmain.so.
    add_library(nier SHARED ${NIER_SOURCES})
    set_target_properties(nier PROPERTIES OUTPUT_NAME main)
else()
    add_executable(nier ${NIER_SOURCES})
endif()

rexglue_setup_target(nier GPU_PLUGINS xenos)

if(WIN32)
    # Performance meter: GPU usage from the "GPU Engine" counters.
    target_link_libraries(nier PRIVATE pdh)
endif()

# The Jukebox's MP3s (src/jukebox): the SDK's own FFmpeg (libavcodec with its MP3 decoder and parser, built for the
# game's WMA/XMA). Its headers come from the SDK's source tree; without them the Music folder takes WAV only.
set(NIER_FFMPEG_DIR "" CACHE PATH "FFmpeg headers of the SDK (thirdparty/FFmpeg)")
if(NOT NIER_FFMPEG_DIR)
    if(REXSDK_DIR)
        set(NIER_FFMPEG_DIR "${REXSDK_DIR}/thirdparty/FFmpeg")
    else()
        set(NIER_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/../sdk-src/thirdparty/FFmpeg")
    endif()
endif()
if(EXISTS "${NIER_FFMPEG_DIR}/libavcodec/avcodec.h")
    if(TARGET rex::libavcodec)
        set(NIER_AVCODEC rex::libavcodec rex::libavutil)
    elseif(TARGET libavcodec)
        set(NIER_AVCODEC libavcodec libavutil)
    endif()
    if(NIER_AVCODEC)
        target_include_directories(nier PRIVATE "${NIER_FFMPEG_DIR}")
        target_compile_definitions(nier PRIVATE NIER_JUKEBOX_MP3=1)
        target_link_libraries(nier PRIVATE ${NIER_AVCODEC})
        message(STATUS "Jukebox: MP3 through the SDK's libavcodec")
    endif()
endif()
if(ANDROID AND REXSDK_DIR AND EXISTS "${REXSDK_DIR}/thirdparty/tomlplusplus/include")
    target_include_directories(nier PRIVATE "${REXSDK_DIR}/thirdparty/tomlplusplus/include")
endif()

# Renut's Direct3D-level renderer (experimental, PC only): cmake/nier_renut_gpu.cmake. Off: the build is as before.
option(NIER_RENUT "Build Renut's native renderer (chosen at boot with nier_renderer = renut)" OFF)
if(NIER_RENUT)
    include(cmake/nier_renut_gpu.cmake)
endif()

# Nier Plus (2026-11-15): the example code mod (mods/trainer), built apart from the game into MODS/Trainer of the
# build folder (the game loads what is there; a release leaves it out: players install mods themselves).
option(NIER_BUILD_EXAMPLE_MODS "Build the example code mods (mods/) into MODS/" ON)
if(NIER_BUILD_EXAMPLE_MODS)
    add_library(nier_mod_trainer SHARED mods/trainer/trainer.cpp)
    set_target_properties(nier_mod_trainer PROPERTIES
        OUTPUT_NAME trainer
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/MODS/Trainer"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/MODS/Trainer"
        CXX_VISIBILITY_PRESET hidden)
    # The moveset mod's test panel (2026-11-19): which shot pattern replaces Dark Lance, its damage.
    add_library(nier_mod_powers SHARED mods/powers/powers.cpp)
    set_target_properties(nier_mod_powers PROPERTIES
        OUTPUT_NAME powers
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/MODS/Poderes"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/MODS/Poderes"
        CXX_VISIBILITY_PRESET hidden)
endif()

if(ANDROID AND REXSDK_DIR)
    # Built with the SDK's source tree, which doesn't export ImGui's headers.
    target_include_directories(nier PRIVATE "${REXSDK_DIR}/thirdparty/imgui")
endif()
