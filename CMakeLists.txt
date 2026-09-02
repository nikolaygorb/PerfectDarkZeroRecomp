# perfectdarkzerorecomp - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(perfectdarkzerorecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(PERFECTDARKZERORECOMP_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(perfectdarkzerorecomp WIN32 ${PERFECTDARKZERORECOMP_SOURCES})
else()
    add_executable(perfectdarkzerorecomp ${PERFECTDARKZERORECOMP_SOURCES})
endif()

rexglue_setup_target(perfectdarkzerorecomp GPU_PLUGINS xenos)

# Codegen regenerates generated/default/*.cpp from scratch and can reintroduce
# cross-function tail-jumps that don't compile (see  tools/fix-unresolved-tail-jumps.ps1). Reapply the workaround automatically
# after every codegen run, before anything tries to compile the result.
if(WIN32)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/tailjump_patch.stamp"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/fix-unresolved-tail-jumps.ps1"
                -GeneratedDir "${CMAKE_CURRENT_SOURCE_DIR}/generated/default"
        COMMAND ${CMAKE_COMMAND} -E touch
                "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/tailjump_patch.stamp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp"
        COMMENT "Patching unresolved cross-function tail-jumps in generated code"
        VERBATIM
    )
    add_custom_target(perfectdarkzerorecomp_fix_tailjumps
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/tailjump_patch.stamp")
    add_dependencies(perfectdarkzerorecomp_recomp perfectdarkzerorecomp_fix_tailjumps)
endif()

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
