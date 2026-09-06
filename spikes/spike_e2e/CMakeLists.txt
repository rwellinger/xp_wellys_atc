cmake_minimum_required(VERSION 3.26)
project(spike_e2e LANGUAGES C CXX)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# This spike targets Apple Silicon only — guard against accidental cross-builds.
if(NOT APPLE)
    message(FATAL_ERROR "spike_e2e is Apple Silicon only")
endif()

set(SPIKES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Both whisper.cpp and llama.cpp vendor `ggml` and check `if (NOT TARGET ggml)`
# before calling `add_subdirectory(ggml)`. By adding llama.cpp's source tree
# first we materialise the `ggml` target with llama's pinned ggml version;
# whisper.cpp then reuses the same target instead of pulling in a second
# (incompatible) copy. Order matters.

# --- shared ggml / metal options (apply to whichever subdir wins the ggml --
# add_subdirectory race) -------------------------------------------------
set(BUILD_SHARED_LIBS           OFF CACHE BOOL "" FORCE)
set(GGML_METAL                  ON  CACHE BOOL "" FORCE)
set(GGML_METAL_EMBED_LIBRARY    ON  CACHE BOOL "" FORCE)
set(GGML_ACCELERATE             ON  CACHE BOOL "" FORCE)
set(GGML_BLAS                   OFF CACHE BOOL "" FORCE)

# --- llama.cpp (provides `ggml`, `llama`, `common`) ---------------------
set(LLAMA_BUILD_COMMON       ON  CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS        OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER       OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL               OFF CACHE BOOL "" FORCE)
add_subdirectory(${SPIKES_DIR}/spike_llama/third_party/llama.cpp
                 ${CMAKE_BINARY_DIR}/llama.cpp EXCLUDE_FROM_ALL)

# --- whisper.cpp (provides `whisper`; reuses llama's ggml target) -------
set(WHISPER_BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER        OFF CACHE BOOL "" FORCE)
add_subdirectory(${SPIKES_DIR}/spike_whisper/third_party/whisper.cpp
                 ${CMAKE_BINARY_DIR}/whisper.cpp EXCLUDE_FROM_ALL)

# --- piper (provides `piper`; pulls in espeak-ng + onnxruntime) ---------
add_subdirectory(${SPIKES_DIR}/spike_piper/third_party/piper1-gpl/libpiper
                 ${CMAKE_BINARY_DIR}/libpiper EXCLUDE_FROM_ALL)

# espeak-ng-data is installed by libpiper's ExternalProject under the top-level
# build dir; bake the path in so the CLI runs without an extra argument.
set(SPIKE_E2E_ESPEAKNG_DATA_DIR
    "${CMAKE_BINARY_DIR}/espeak_ng-install/share/espeak-ng-data"
    CACHE PATH "Path to espeak-ng-data directory used at runtime")

# --- spike_e2e binary ---------------------------------------------------
add_executable(spike_e2e
    src/spike_e2e.cpp
    src/whisper_stt.cpp
    src/llama_lm.cpp
    src/piper_tts.cpp
    src/wav.cpp
)

target_compile_options(spike_e2e PRIVATE -Wall -Wextra -Wpedantic)

target_compile_definitions(spike_e2e PRIVATE
    SPIKE_E2E_ESPEAKNG_DATA_DIR="${SPIKE_E2E_ESPEAKNG_DATA_DIR}"
)

target_include_directories(spike_e2e PRIVATE
    src
    ${SPIKES_DIR}/spike_whisper/third_party/whisper.cpp/include
    ${SPIKES_DIR}/spike_llama/third_party/llama.cpp/include
    ${SPIKES_DIR}/spike_llama/third_party/llama.cpp/ggml/include
    ${SPIKES_DIR}/spike_llama/third_party/llama.cpp/common
    ${SPIKES_DIR}/spike_piper/third_party/piper1-gpl/libpiper/include
)

target_link_libraries(spike_e2e PRIVATE
    whisper
    common
    llama
    piper
)

# Same rpath dance as spike_piper: libpiper.dylib + libonnxruntime.dylib
# both live inside the build tree, no DYLD_LIBRARY_PATH gymnastics needed.
set_target_properties(spike_e2e PROPERTIES
    BUILD_WITH_INSTALL_RPATH FALSE
    BUILD_RPATH "${CMAKE_BINARY_DIR};${CMAKE_BINARY_DIR}/lib/onnxruntime-osx-arm64-1.22.0/lib"
)
