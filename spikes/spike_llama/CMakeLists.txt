cmake_minimum_required(VERSION 3.21)
project(spike_llama LANGUAGES C CXX)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# This spike targets Apple Silicon only — guard against accidental cross-builds.
if(NOT APPLE)
    message(FATAL_ERROR "spike_llama is Apple Silicon only")
endif()

# llama.cpp build options — set BEFORE add_subdirectory.
# We want: static libs, common helpers, Metal backend, no curl, no examples/tools/tests/server.
set(BUILD_SHARED_LIBS        OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON       ON  CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS        OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER       OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL               OFF CACHE BOOL "" FORCE)
set(GGML_METAL               ON  CACHE BOOL "" FORCE)
set(GGML_METAL_EMBED_LIBRARY ON  CACHE BOOL "" FORCE)
set(GGML_ACCELERATE          ON  CACHE BOOL "" FORCE)
set(GGML_BLAS                OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)

add_executable(spike_llama src/spike_llama.cpp)

target_compile_options(spike_llama PRIVATE -Wall -Wextra -Wpedantic)

target_include_directories(spike_llama PRIVATE
    third_party/llama.cpp/include
    third_party/llama.cpp/ggml/include
    third_party/llama.cpp/common
)

# `common` pulls in `llama` (and ggml + Metal) transitively.
target_link_libraries(spike_llama PRIVATE common llama)
