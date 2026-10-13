# xrprobe: standalone OpenXR + Vulkan probe for SteamVR pacing and frame
# synthesis on the Steam Frame. Not part of the game build.
#   cmake -S tools/xrprobe -B tools/xrprobe/build -G Ninja -DCMAKE_C_COMPILER=clang
#   flock ~/Games/GalaxyQuest/.build.lock cmake --build tools/xrprobe/build
cmake_minimum_required(VERSION 3.20)
project(xrprobe C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)

# OpenXR headers: the repo's third_party/openxr (tools/fetch_openxr.sh). In a
# git worktree third_party/ is not checked out, so also look next to the main
# working tree.
set(_xr_candidates ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openxr/prefab/modules/headers/include)
execute_process(COMMAND git rev-parse --path-format=absolute --git-common-dir
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE _git_common OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(_git_common)
  list(APPEND _xr_candidates ${_git_common}/../third_party/openxr/prefab/modules/headers/include)
endif()
find_path(OPENXR_INCLUDE openxr/openxr.h PATHS ${_xr_candidates} NO_DEFAULT_PATH)
if(NOT OPENXR_INCLUDE)
  message(FATAL_ERROR "OpenXR headers not found: run tools/fetch_openxr.sh or pass -DOPENXR_INCLUDE=<dir>")
endif()
set(OPENXR_LOADER /opt/steamvr/bin/linuxarm64/libopenxr_loader.so CACHE FILEPATH "OpenXR loader")
if(NOT EXISTS ${OPENXR_LOADER})
  message(FATAL_ERROR "OpenXR loader not found at ${OPENXR_LOADER} (pass -DOPENXR_LOADER=...)")
endif()
get_filename_component(OPENXR_LOADER_DIR ${OPENXR_LOADER} DIRECTORY)
message(STATUS "xrprobe: OpenXR headers ${OPENXR_INCLUDE}, loader ${OPENXR_LOADER}")

set(SPV)
foreach(src quad.vert scene.frag mv.frag fullscreen.vert heavy.frag)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${src}.inc)
  add_custom_command(OUTPUT ${out}
    COMMAND ${GLSLC} --target-env=vulkan1.2 -O -I${CMAKE_CURRENT_SOURCE_DIR}/shaders -mfmt=c
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${src} -o ${out}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${src} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.glsl
    COMMENT "glslc ${src}")
  list(APPEND SPV ${out})
endforeach()

add_executable(xrprobe xrprobe.c ${SPV})
target_include_directories(xrprobe PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${OPENXR_INCLUDE})
target_compile_options(xrprobe PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(xrprobe PRIVATE ${OPENXR_LOADER} Vulkan::Vulkan m pthread)
set_target_properties(xrprobe PROPERTIES BUILD_RPATH ${OPENXR_LOADER_DIR})
