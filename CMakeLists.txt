# perfectdarkzerorecomp - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(perfectdarkzerorecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# Fix git symlinks on Windows (libmspack in SDK uses symlinks)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fix_symlinks.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_optimizations.cmake)

# Build ReXGlue SDK from source (git submodule)
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk" CACHE PATH "Path to rexglue-sdk source tree" FORCE)

# Enable both graphics backends
if(WIN32)
    set(REXGLUE_USE_D3D12 ON CACHE BOOL "Enable D3D12 graphics backend")
    set(REXGLUE_USE_D3D11 ON CACHE BOOL "Enable D3D11 graphics backend [experimental]")
endif()
set(REXGLUE_USE_VULKAN ON CACHE BOOL "Enable Vulkan graphics backend")


include(generated/rexglue.cmake)

set(PERFECTDARKZERORECOMP_SOURCES
    src/main.cpp
    src/game_patches.cpp
    src/game_cvars.cpp
    src/local_profiles.cpp
    src/mouse/mouse_look.cpp
    src/mouse/mouse_look_hook.cpp
    src/mouse/steer_look.cpp
    src/mouse/locktopus_input.cpp
    src/mouse/menu_pointer.cpp
    src/render/gpu_wait_hook.cpp
    src/render/graphics_backend.cpp
    src/render/native_memory_hook.cpp
    src/render/engine/engine.cpp
    src/render/engine/scene_probe.cpp
    src/render/engine/render_hooks.cpp
    src/timing/physics_step_hook.cpp
    src/timing/cutscene_frame_cap_hook.cpp
    src/timing/rate_fixes.cpp
    src/timing/ramp_step.cpp
    src/fixes/tu_fixes.cpp
    src/fixes/distant_lights.cpp
    src/fixes/ai_rate_fixes.cpp
    src/mod/third_person.cpp
    src/mod/fov_settings.cpp
    src/mod/bot_limit.cpp
)

if(WIN32)
    list(APPEND PERFECTDARKZERORECOMP_SOURCES
        src/perfectdarkzerorecomp.rc
    )
endif()

if(WIN32)
    add_executable(perfectdarkzerorecomp WIN32 ${PERFECTDARKZERORECOMP_SOURCES})
else()
    add_executable(perfectdarkzerorecomp ${PERFECTDARKZERORECOMP_SOURCES})
endif()

rexglue_setup_target(perfectdarkzerorecomp GPU_PLUGINS xenos native)

add_custom_target(pdz_stage_runtime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rexruntime> $<TARGET_FILE:rexgpu-native> $<TARGET_FILE:rexgpu-xenos>
            $<TARGET_FILE_DIR:perfectdarkzerorecomp>
    DEPENDS rexruntime rexgpu-native rexgpu-xenos
    VERBATIM)
add_dependencies(perfectdarkzerorecomp pdz_stage_runtime)

rexglue_add_launcher(perfectdarkzerorecomp ICON_RC src/perfectdarkzerorecomp.rc)

# SDL looks for this file (name set by settings/mapping.toml's
# hid_mappings_file) next to the exe at startup to fill in gamepads it
# doesn't already recognize. Sourced from
# https://github.com/mdqinc/SDL_GameControllerDB, see settings/README.md.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/settings/gamecontrollerdb.txt")
    add_custom_command(TARGET perfectdarkzerorecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/settings/gamecontrollerdb.txt"
                "$<TARGET_FILE_DIR:perfectdarkzerorecomp>/gamecontrollerdb.txt"
        COMMENT "Copying gamecontrollerdb.txt next to perfectdarkzerorecomp.exe")
endif()
