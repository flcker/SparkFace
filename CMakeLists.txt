cmake_minimum_required(VERSION 3.27)

project(SparkFacePhase0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "SparkFace Phase-0 requires Windows 11.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SPARKFACE_BUILD_ENV_GATE "Build the Phase-0 environment gate" ON)
if(SPARKFACE_BUILD_ENV_GATE)
  add_subdirectory(spikes/env)
endif()
add_subdirectory(spikes/fallback)
