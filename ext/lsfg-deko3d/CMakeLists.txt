# Native LSFG shader extraction and scheduling, adapted from Cemu-NX.
# Copyright 2026 Dolphin Emulator Project
# Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later

find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/ext/SPIRV-Cross"
  OUTPUT_VARIABLE LSFG_SPIRV_CROSS_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
  WORKING_DIRECTORY "${UAM_ROOT}"
  OUTPUT_VARIABLE LSFG_UAM_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/include/lsfg/common/version.hpp" @ONLY)

add_library(SwitchLsfgDekoShaders STATIC
  src/binding.cpp src/cache_format.cpp src/cache_load.cpp src/cache_store.cpp
  src/compile_uam.cpp src/dksh.cpp src/error.cpp src/image_graph.cpp
  src/layout.cpp src/pe_resources.cpp src/prepare.cpp src/schedule.cpp
  src/sha256.cpp src/shader_set.cpp src/spirv_module.cpp src/translate.cpp)
target_include_directories(SwitchLsfgDekoShaders PUBLIC
  include "${CMAKE_CURRENT_BINARY_DIR}/include"
  PRIVATE "${CMAKE_SOURCE_DIR}/Common/GPU/Deko3D")
set_target_properties(SwitchLsfgDekoShaders PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(SwitchLsfgDekoShaders PRIVATE -fexceptions)
target_link_libraries(SwitchLsfgDekoShaders PRIVATE spirv-cross-glsl uam)

add_library(SwitchLsfgDekoPrepare STATIC "${CMAKE_SOURCE_DIR}/Common/GPU/Deko3D/LSFGShaders.cpp")
set_target_properties(SwitchLsfgDekoPrepare PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(SwitchLsfgDekoPrepare PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(SwitchLsfgDekoPrepare PUBLIC SwitchLsfgDekoShaders)

if(USE_LIBNX)
  add_library(SwitchLsfgDekoRuntime STATIC "${CMAKE_SOURCE_DIR}/Common/GPU/Deko3D/LSFGFrameGenerator.cpp")
  set_target_properties(SwitchLsfgDekoRuntime PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_include_directories(SwitchLsfgDekoRuntime PUBLIC "${CMAKE_SOURCE_DIR}")
  target_link_libraries(SwitchLsfgDekoRuntime PUBLIC SwitchLsfgDekoPrepare)
endif()
