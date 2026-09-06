cmake_minimum_required(VERSION 3.21)
project(spike_whisper LANGUAGES C CXX)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# This spike targets Apple Silicon only — guard against accidental cross-builds.
if(NOT APPLE)
    message(FATAL_ERROR "spike_whisper is Apple Silicon only")
endif()

# whisper.cpp build options — set BEFORE add_subdirectory.
set(BUILD_SHARED_LIBS           OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER        OFF CACHE BOOL "" FORCE)
set(GGML_METAL                  ON  CACHE BOOL "" FORCE)
set(GGML_METAL_EMBED_LIBRARY    ON  CACHE BOOL "" FORCE)
set(GGML_ACCELERATE             ON  CACHE BOOL "" FORCE)
set(GGML_BLAS                   OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/whisper.cpp EXCLUDE_FROM_ALL)

add_executable(spike_whisper src/spike_whisper.cpp)

target_compile_options(spike_whisper PRIVATE -Wall -Wextra -Wpedantic)

target_include_directories(spike_whisper PRIVATE
    third_party/whisper.cpp/include
    third_party/whisper.cpp/ggml/include
)

target_link_libraries(spike_whisper PRIVATE whisper)
