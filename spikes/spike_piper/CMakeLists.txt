cmake_minimum_required(VERSION 3.26)
project(spike_piper LANGUAGES C CXX)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# This spike targets Apple Silicon only — guard against accidental cross-builds.
if(NOT APPLE)
    message(FATAL_ERROR "spike_piper is Apple Silicon only")
endif()

# libpiper produces a SHARED library. ExternalProject pulls espeak-ng (static)
# and downloads the prebuilt onnxruntime arm64 1.22.0 release. We just plug the
# subdir in and link against the `piper` target.
add_subdirectory(third_party/piper1-gpl/libpiper EXCLUDE_FROM_ALL)

# espeak-ng-data is installed by libpiper's ExternalProject under the top-level
# build dir. The CLI needs that path at runtime; pass it as a compile-time
# default so users can run the binary without an extra argument.
set(SPIKE_PIPER_ESPEAKNG_DATA_DIR
    "${CMAKE_BINARY_DIR}/espeak_ng-install/share/espeak-ng-data"
    CACHE PATH "Path to espeak-ng-data directory used at runtime")

add_executable(spike_piper src/spike_piper.cpp)

target_compile_options(spike_piper PRIVATE -Wall -Wextra -Wpedantic)

target_compile_definitions(spike_piper PRIVATE
    SPIKE_PIPER_ESPEAKNG_DATA_DIR="${SPIKE_PIPER_ESPEAKNG_DATA_DIR}"
)

target_include_directories(spike_piper PRIVATE
    third_party/piper1-gpl/libpiper/include
)

target_link_libraries(spike_piper PRIVATE piper)

# Ensure the binary can find libpiper.dylib + libonnxruntime.dylib in the build
# tree without DYLD_LIBRARY_PATH gymnastics.
set_target_properties(spike_piper PROPERTIES
    BUILD_WITH_INSTALL_RPATH FALSE
    BUILD_RPATH "${CMAKE_BINARY_DIR};${CMAKE_BINARY_DIR}/lib/onnxruntime-osx-arm64-1.22.0/lib"
)
