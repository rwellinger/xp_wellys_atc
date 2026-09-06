cmake_minimum_required(VERSION 3.26)
# Objective-C++ is only used by the macOS UI/permission bridges
# (clipboard.mm, mic_permission.mm). MSVC has no ObjC++ frontend, so the
# language is enabled per-platform below rather than in project().
project(xp_wellys_vfr_atc CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-wide compile definitions. libcurl transitively drags in
# <windows.h>, which otherwise defines min()/max() macros (breaking
# std::min/std::max) and pulls the legacy WinSock1 symbols that clash
# with <winsock2.h>. Applied to every target (engine lib, ImGui, plugin)
# so a stray <windows.h> include anywhere stays benign.
if(WIN32)
    # _USE_MATH_DEFINES: MSVC only exposes M_PI et al. from <cmath> when
    # this is defined. As a global compile definition it is set before any
    # header is seen, so every TU gets the constants.
    add_compile_definitions(
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
endif()

option(BUILD_TESTS "Build Catch2 unit tests" ON)

# ── Local-inference toggle ───────────────────────────────────────────────────
# When ON, the plugin module is compiled with whisper.cpp + llama.cpp +
# Piper. The ggml GPU backend depends on the platform and comes baked into the
# prebuilt bundle: Metal on Apple Silicon, Vulkan on Windows x64. When OFF,
# those backends are skipped entirely and only the cloud clients are built —
# letting the same source tree produce an x86_64 (Intel) macOS slice that runs
# against api.openai.com / api.mistral.ai.
#
# A universal macOS binary is assembled by building twice (once per
# architecture) and lipo-ing the two .xpl outputs together — see
# `make build`. Windows is a single x64 build, CI only.
option(XPWELLYS_USE_LOCAL_INFERENCE
    "Compile local whisper.cpp/llama.cpp/Piper backends (arm64-macOS + win-x64)"
    ON)

# ── Local-TTS toggle (Piper) ─────────────────────────────────────────────────
# Piper is pure-CPU TTS (onnxruntime + espeak-ng, NO Metal), so unlike
# whisper.cpp/llama.cpp it is not bound to Apple Silicon. This flag compiles
# the local Piper TTS backend independently of the Metal STT/LM backends,
# enabling the hybrid mode (cloud STT/LM + local German Piper voice, issue #66)
# on the x86_64-macOS and Windows slices too (issue #69 / #70). Whisper/Llama
# stay gated on XPWELLYS_USE_LOCAL_INFERENCE.
#
# Full local inference (STT+LM+TTS) always implies local TTS — see below.
option(XPWELLYS_USE_LOCAL_TTS
    "Compile local Piper TTS backend (CPU, cross-platform)"
    ON)
if(XPWELLYS_USE_LOCAL_INFERENCE AND NOT XPWELLYS_USE_LOCAL_TTS)
    set(XPWELLYS_USE_LOCAL_TTS ON CACHE BOOL
        "Compile local Piper TTS backend (CPU, cross-platform)" FORCE)
endif()

# Any local backend (Metal STT/LM or CPU Piper TTS) needs the prebuilt
# xp_wellys_libs bundle (headers + libpiper + onnxruntime + espeak-ng-data).
# On arm64 both flags are ON, so this is identical to the old
# XPWELLYS_USE_LOCAL_INFERENCE-only gating; on the cloud-only slices both are
# OFF. Splitting the bundle into a Piper-only sub-target lands in #71/#72/#73.
if(XPWELLYS_USE_LOCAL_INFERENCE OR XPWELLYS_USE_LOCAL_TTS)
    set(XPWELLYS_USE_LOCAL_BUNDLE ON)
endif()

# ── Sanitizer build (ASan + UBSan) ───────────────────────────────────────────
# When ON, instruments the SDK-free engine OBJECT lib + atc_repl + Catch2
# tests with AddressSanitizer + UndefinedBehaviorSanitizer. The plugin
# module (`xp_wellys_vfr_atc.xpl`) and vendored libs (whisper/llama/piper/
# ggml/imgui) are deliberately NOT instrumented — ASan inside the X-Plane
# process is fragile on macOS ARM64, and those vendor trees aren't built
# clean enough for sanitizers anyway.
#
# Use a separate build dir (e.g. `build-sanitize/`) so the normal
# `build/` Release tree stays untouched. Driven by `make sanitize`.
option(XP_WELLYS_ATC_SANITIZE "Enable ASan + UBSan on xp_atc_engine, atc_repl, and tests" OFF)
if(XP_WELLYS_ATC_SANITIZE)
    set(XP_WELLYS_ATC_SANITIZER_FLAGS
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    message(STATUS "Sanitizer build: ASan + UBSan on xp_atc_engine, atc_repl, tests")
endif()

# ── ccache (optional) ────────────────────────────────────────────────────────
# Re-uses compiled object files across builds. Big win for the vendored
# llama.cpp / whisper.cpp / ggml / onnxruntime / espeak-ng / Piper trees,
# which change rarely between commits but are otherwise the long pole at
# 80%+ progress (single heavy TUs like t5-enc.cpp). No-op if ccache is not
# installed (brew install ccache on macOS, action handles it in CI).
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER      "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER    "${CCACHE_PROGRAM}")
    set(CMAKE_OBJCXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
endif()

# ── Version from VERSION.txt ────────────────────────────────────────────────
file(READ "${CMAKE_SOURCE_DIR}/VERSION.txt" VERSION_STRING)
string(STRIP "${VERSION_STRING}" VERSION_STRING)

# Architecture + deployment target (macOS only — Windows is x64 by definition).
#
# On macOS, local inference ships Apple Silicon Metal kernels and uses an
# arm64-only onnxruntime prebuilt, so any Apple build that enables
# XPWELLYS_USE_LOCAL_INFERENCE must target arm64.
# The OpenAI-only build is libcurl + system frameworks all the way
# down and is architecture-neutral, so it cleanly produces an x86_64
# slice for Intel Macs. The two slices are then lipo'd together into
# one universal .xpl by `make build`.
if(APPLE)
    if(XPWELLYS_USE_LOCAL_INFERENCE)
        if(NOT CMAKE_OSX_ARCHITECTURES)
            set(CMAKE_OSX_ARCHITECTURES "arm64")
        endif()
        # onnxruntime 1.22.0's prebuilt arm64 dylib is built against
        # the macOS 13.3 SDK; setting the deployment target lower
        # triggers a "newer version 13.3" linker warning and risks
        # runtime crashes on macOS 12.
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")
    else()
        # OpenAI-only build: caller picks the architecture (typically
        # x86_64 for the universal-binary's Intel slice). Keep the
        # 13.3 deployment target so both slices match and lipo is
        # happy — bumping it lower would split the merged binary's
        # LC_BUILD_VERSION load commands.
        if(NOT CMAKE_OSX_ARCHITECTURES)
            set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        endif()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")
    endif()
endif()

# ── Dear ImGui ────────────────────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
)

# libcurl is only needed by the plugin module (model downloader). The
# headless atc_repl tool no longer pulls it in.
find_package(CURL REQUIRED)

# ── Local inference backends (whisper.cpp, llama.cpp, Piper) ─────────────────
#
# Consumed PREBUILT from the xp_wellys_libs bundle — the heavy third-party
# trees are compiled once in that separate repo and downloaded here by
# `make setup`. No submodules, no add_subdirectory, no local compile.

if(XPWELLYS_USE_LOCAL_BUNDLE)
    # Prebuilt local-inference bundle (xp_wellys_libs). The heavy third-party
    # trees (whisper/llama/ggml/Piper/espeak) are compiled ONCE in that repo
    # and consumed here as a versioned arm64 binary bundle — no
    # add_subdirectory, no submodules, no local compile. `make setup`
    # downloads + SHA256-verifies it into vendor/prebuilt/. Building from
    # source now lives entirely in the xp_wellys_libs repo.
    set(XP_WELLYS_LIBS_ROOT "${CMAKE_SOURCE_DIR}/vendor/prebuilt/xp_wellys_libs")
    if(NOT EXISTS "${XP_WELLYS_LIBS_ROOT}/xp_wellys_libs.cmake")
        message(FATAL_ERROR
            "Prebuilt xp_wellys_libs bundle missing at ${XP_WELLYS_LIBS_ROOT}. "
            "Run `make setup` to download it.")
    endif()
    message(STATUS "Building with local backend(s) (prebuilt xp_wellys_libs bundle): "
                   "inference=${XPWELLYS_USE_LOCAL_INFERENCE} tts=${XPWELLYS_USE_LOCAL_TTS}")
    # Defines xp_wellys_libs::inference (static whisper/llama/ggml + libpiper
    # dylib + onnxruntime + Metal/Accelerate frameworks, in link order).
    include("${XP_WELLYS_LIBS_ROOT}/xp_wellys_libs.cmake")

    # espeak-ng-data + onnxruntime dylibs live inside the bundle; staged next
    # to the .xpl below in the same layout the old from-source build produced,
    # so the Makefile install + CI release steps are unchanged.
    set(XP_WELLYS_ATC_ESPEAKNG_DATA_DIR
        "${XP_WELLYS_LIBS_ROOT}/share/espeak-ng-data"
        CACHE PATH "Path to espeak-ng-data used at runtime (dev fallback)")
    set(ONNXRUNTIME_LIB_DIR "${XP_WELLYS_LIBS_ROOT}/lib"
        CACHE PATH "Directory holding libonnxruntime.<ver>.dylib")
else()
    message(STATUS "Building OpenAI-only (no whisper/llama/piper)")
endif()

# ── Engine OBJECT library (X-Plane SDK-frei) ─────────────────────────────────
# Must compile and link without any XPLM / XPWidgets symbols.
# The intentional absence of -DXPLM200 etc. is a guardrail: any engine
# translation unit that pulls in <XPLM*.h> will fail to compile here.
#
# `backends/manager.cpp` lives here because it touches only std::thread
# + the abstract interfaces. The concrete WhisperStt / LlamaLm /
# PiperTts translation units live in the plugin module so the engine
# library does not pull whisper.h / llama.h / piper.h headers (and thus
# their static-lib link requirements) into the headless atc_repl tool.
add_library(xp_atc_engine OBJECT
    src/core/logging.cpp
    src/core/cross_country_log.cpp
    src/atc/engine.cpp
    src/atc/intent_parser.cpp
    src/atc/intent_rules.cpp
    src/atc/atc_state_machine.cpp
    src/atc/atc_templates.cpp
    src/atc/phraseology_hints.cpp
    src/atc/flight_phase.cpp
    src/atc/atis_generator.cpp
    src/atc/de_phraseology.cpp
    src/atc/en_phraseology.cpp
    src/atc/bzf_compliance.cpp
    src/atc/initial_call_conformance.cpp
    src/atc/traffic_advisor.cpp
    src/atc/traffic_dialog.cpp
    src/atc/landing_sequence.cpp
    src/atc/flows/ground_operations.cpp
    src/atc/flows/flow_coordinator.cpp
    src/atc/flows/pattern_flow.cpp
    src/atc/flows/crosscountry_flow.cpp
    src/backends/manager.cpp
    # Sample-rate conversion for the STT input path. The capture layer is
    # plugin-only (Core Audio / WASAPI), but the conversion itself is pure
    # DSP -- it lives here so tests/test_pcm_resample.cpp can prove the
    # anti-aliasing (issue #85).
    src/audio/pcm_resample.cpp
    src/data/airport_vrps.cpp
    src/data/airspace_db.cpp
    src/data/openair_db.cpp
    src/data/traffic_context.cpp
    src/data/traffic_geometry.cpp
    src/data/traffic_phase_classifier.cpp
    src/core/xplane_context.cpp
    # Model manifest is SDK-free (data + SHA256 via CommonCrypto),
    # so it lives here and the headless atc_repl tool can use it
    # too if it ever needs to verify a local model.
    src/persistence/model_manifest.cpp
    # Catalog reads data/models_catalog.json at startup and feeds the
    # manifest plus the Settings UI dropdowns. SDK-free, JSON-only.
    src/persistence/models_catalog.cpp
)
target_include_directories(xp_atc_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/src
    # manager.cpp includes <curl/curl.h>. The OBJECT lib does not link
    # (consumers do), but it must see curl's headers at compile time. On
    # macOS system libcurl these sit on the default search path; via
    # vcpkg (Windows) they are exposed only through the imported target's
    # interface include dirs.
    $<TARGET_PROPERTY:CURL::libcurl,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_definitions(xp_atc_engine PRIVATE
    XP_WELLYS_ATC_VERSION="${VERSION_STRING}"
)
if(APPLE)
    target_compile_options(xp_atc_engine PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fvisibility=hidden
    )
endif()
if(XP_WELLYS_ATC_SANITIZE)
    target_compile_options(xp_atc_engine PRIVATE
        ${XP_WELLYS_ATC_SANITIZER_FLAGS} -O1 -g
    )
endif()

# ── Plugin MODULE (Engine + X-Plane-spezifisch) ──────────────────────────────
add_library(xp_wellys_vfr_atc MODULE
    $<TARGET_OBJECTS:xp_atc_engine>
    src/main.cpp
    src/atc/atc_session.cpp
    src/core/xplane_context_runtime.cpp
    src/data/traffic_context_runtime.cpp
    src/audio/ptt_input.cpp
    src/audio/audio_recorder.cpp
    src/audio/audio_player.cpp
    # OpenAI cloud clients — libcurl + nlohmann/json, never include
    # any local backend header. Each TU carries a kBackendTag
    # constant so every inference call shows up under
    # [STT-OPENAI] / [LM-OPENAI] / [TTS-OPENAI] in Log.txt.
    # Always compiled — even in the Intel slice that does not have
    # the local backends, OpenAI is the only available path.
    src/backends/openai_common.cpp
    src/backends/openai_stt.cpp
    src/backends/openai_lm.cpp
    src/backends/openai_tts.cpp
    # Mistral cloud clients — same shape as OpenAI's (libcurl +
    # nlohmann/json), reuse the provider-agnostic helpers in
    # openai_common (WAV encode/decode, last4). Each TU carries a
    # [STT-MISTRAL] / [LM-MISTRAL] / [TTS-MISTRAL] tag so every call
    # is attributable in Log.txt. Compiled in both slices — Mistral
    # is cloud-only and works on x86_64 as well as arm64.
    src/backends/mistral_stt.cpp
    src/backends/mistral_lm.cpp
    src/backends/mistral_tts.cpp
    # Loader orchestrates verification + concrete backend load on a
    # worker thread; uses XPLMGetPluginInfo (via model_paths) so it
    # has to live in the plugin module, not the engine library.
    src/backends/loader.cpp
    # Downloader: libcurl + std::filesystem on a worker thread.
    # Plugin-only because it depends on libcurl + plugin-relative
    # paths via model_paths.
    src/backends/downloader.cpp
    src/persistence/model_paths.cpp
    src/persistence/settings.cpp
    src/persistence/keychain.cpp
    src/ui/atc_ui.cpp
    src/ui/ui_strings.cpp
    ${IMGUI_SOURCES}
)

# Platform UI/permission bridges. macOS uses the Objective-C++ sources
# (NSPasteboard + AVFoundation); Windows uses the Win32 .cpp counterparts
# (issue #22: real clipboard via the Win32 clipboard API + a no-op mic
# permission — Windows has no per-app mic prompt). Windows capture itself
# lives in audio_recorder.cpp behind #elif defined(_WIN32) (miniaudio,
# issue #21); its single-header vendor dep is fetched by `make setup`
# / the CI deps step and needs no extra link libraries.
if(APPLE)
    target_sources(xp_wellys_vfr_atc PRIVATE
        src/audio/mic_permission.mm
        src/ui/clipboard.mm
    )
elseif(WIN32)
    target_sources(xp_wellys_vfr_atc PRIVATE
        src/audio/mic_permission_win.cpp
        src/ui/clipboard_win.cpp
    )
endif()

if(XPWELLYS_USE_LOCAL_INFERENCE)
    # Concrete Metal STT/LM backends — pull whisper.h / llama.h
    # transitively. Static libs come from the prebuilt bundle. In the
    # cloud-only build these TUs are not compiled at all (and the loader
    # is built without their includes).
    target_sources(xp_wellys_vfr_atc PRIVATE
        src/backends/whisper_stt.cpp
        src/backends/llama_lm.cpp
    )
endif()

if(XPWELLYS_USE_LOCAL_TTS)
    # Local Piper TTS backend — pulls piper.h transitively. CPU-only, so
    # gated on its own flag (not XPWELLYS_USE_LOCAL_INFERENCE) to allow the
    # hybrid mode on non-Apple-Silicon slices (issue #69 / #70). loader.cpp
    # is the only legitimate consumer of piper_tts.hpp / piper.h.
    target_sources(xp_wellys_vfr_atc PRIVATE
        src/backends/piper_tts.cpp
    )
endif()

target_include_directories(xp_wellys_vfr_atc PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/sdk/XPLM
    ${CMAKE_SOURCE_DIR}/sdk/XPWidgets
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

if(XPWELLYS_USE_LOCAL_INFERENCE)
    # Backend headers (whisper.h / llama.h / ggml / common / piper.h) come
    # from the prebuilt bundle via xp_wellys_libs::inference's INTERFACE
    # include dirs (propagated by the link below) — no explicit paths needed.
    #
    # Surface the flag to C++ code so loader.cpp can #ifdef the Metal STT/LM
    # includes + backend instantiation calls.
    target_compile_definitions(xp_wellys_vfr_atc PRIVATE
        XPWELLYS_USE_LOCAL_INFERENCE=1
    )
endif()

if(XPWELLYS_USE_LOCAL_TTS)
    # Surface the Piper-TTS flag so loader.cpp / atc_ui.cpp can #ifdef the
    # Piper include + the hybrid-TTS override + the UI voice controls
    # independently of the Metal STT/LM flag (issue #69 / #70).
    target_compile_definitions(xp_wellys_vfr_atc PRIVATE
        XPWELLYS_USE_LOCAL_TTS=1
    )
endif()

# X-Plane resolves fat plugins by the platform subfolder (mac_x64/,
# win_x64/). The .xpl file MUST carry the plugin name on every platform:
# X-Plane 12 on Windows silently fails to load a plugin whose per-arch file
# is named generically (win.xpl) — it must be <plugin>/win_x64/
# xp_wellys_vfr_atc.xpl, matching macOS mac_x64/xp_wellys_vfr_atc.xpl.
# The .xpl suffix stays on both — on Windows it is a renamed DLL.
set_target_properties(xp_wellys_vfr_atc PROPERTIES
    OUTPUT_NAME "xp_wellys_vfr_atc"
    SUFFIX      ".xpl"
    PREFIX      ""
)

if(APPLE)
    target_compile_definitions(xp_wellys_vfr_atc PRIVATE
        APL=1 IBM=0 LIN=0
        XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM400=1
        XP_WELLYS_ATC_VERSION="${VERSION_STRING}"
    )

    # Suppress OpenGL deprecation warnings in our code only (ImGui defines its own)
    set_source_files_properties(
        src/main.cpp src/ui/atc_ui.cpp
        PROPERTIES COMPILE_DEFINITIONS "GL_SILENCE_DEPRECATION"
    )
    target_compile_options(xp_wellys_vfr_atc PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fvisibility=hidden
    )

    set(SDK_MAC_LIBS "${CMAKE_SOURCE_DIR}/sdk/Libraries/Mac")
    target_link_libraries(xp_wellys_vfr_atc PRIVATE
        "${SDK_MAC_LIBS}/XPLM.framework/Versions/Current/XPLM"
        "${SDK_MAC_LIBS}/XPWidgets.framework/Versions/Current/XPWidgets"
        "-framework OpenGL"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreAudio"
        "-framework AVFoundation"
        "-framework CoreFoundation"
        # AppKit is used by src/ui/clipboard.mm to read NSPasteboard.
        "-framework AppKit"
        # Security.framework is used by persistence/keychain.cpp to
        # store the OpenAI API key in the macOS Keychain. Always
        # linked — Keychain is the storage for both build modes.
        "-framework Security"
        # libcurl is used by the plugin's model downloader and by
        # the three OpenAI cloud clients. Both modes need it.
        CURL::libcurl
    )

    if(XPWELLYS_USE_LOCAL_BUNDLE)
        # All local libs come from the prebuilt bundle: the static
        # whisper/llama/ggml archives + libpiper.dylib + onnxruntime, in the
        # correct link order, plus the Metal / Accelerate frameworks. Piper is
        # shared and resolved at runtime via @loader_path — see rpath below.
        # NOTE (#71/#72): the monolithic xp_wellys_libs::inference target still
        # pulls whisper/llama/Metal even in a TTS-only build; a Piper-only
        # sub-target (libpiper + onnxruntime, no Metal) lands with the
        # x86_64/Windows prebuilts. On arm64 both flags are ON, so this is
        # identical to the previous behaviour.
        target_link_libraries(xp_wellys_vfr_atc PRIVATE xp_wellys_libs::inference)

        # rpath: dev builds find dylibs in build/ and build/lib/<ort>;
        # the installed bundle finds them via @loader_path (Makefile
        # install target copies libpiper.dylib +
        # libonnxruntime.<ver>.dylib next to the .xpl).
        set_target_properties(xp_wellys_vfr_atc PROPERTIES
            BUILD_WITH_INSTALL_RPATH FALSE
            BUILD_RPATH "${CMAKE_BINARY_DIR};${ONNXRUNTIME_LIB_DIR}"
            INSTALL_RPATH "@loader_path"
        )

        # Stage the bundle's runtime dylibs + espeak-ng-data next to the built
        # .xpl so a fresh `cmake --build` produces a directory the Makefile
        # install + CI release steps copy verbatim — same layout the old
        # from-source build produced (build-arm64/libpiper.dylib,
        # libonnxruntime.*, espeak_ng-install/share/espeak-ng-data). The
        # versioned + unversioned onnxruntime names both need to be present so
        # libpiper's @rpath/libonnxruntime.1.22.0.dylib resolves.
        add_custom_command(TARGET xp_wellys_vfr_atc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${XP_WELLYS_LIBS_ROOT}/lib/libpiper.dylib"
                "${CMAKE_BINARY_DIR}/libpiper.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.1.22.0.dylib"
                "${CMAKE_BINARY_DIR}/libonnxruntime.1.22.0.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.dylib"
                "${CMAKE_BINARY_DIR}/libonnxruntime.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${XP_WELLYS_ATC_ESPEAKNG_DATA_DIR}"
                "${CMAKE_BINARY_DIR}/espeak_ng-install/share/espeak-ng-data"
            COMMENT "Staging libpiper + onnxruntime + espeak-ng-data alongside xp_wellys_vfr_atc.xpl"
        )
    else()
        # OpenAI-only build: no shared dylibs to stage. @loader_path
        # still applies for the install (so future shared libs would
        # be found), but nothing actually resolves through it.
        set_target_properties(xp_wellys_vfr_atc PROPERTIES
            BUILD_WITH_INSTALL_RPATH FALSE
            INSTALL_RPATH "@loader_path"
        )
    endif()
elseif(WIN32)
    # ── Windows: full local stack (XPWELLYS_USE_LOCAL_INFERENCE=ON) ──────
    # whisper + llama (ggml with the Vulkan GPU backend) + Piper, all from the
    # prebuilt xp_wellys_libs win-x64 bundle, alongside the OpenAI/Mistral cloud
    # backends over libcurl. Built by MSVC in CI (windows-latest) with libcurl
    # from vcpkg. No @loader_path / lipo, but the artifact does carry the Piper
    # runtime DLLs — see the staging block below.
    target_compile_definitions(xp_wellys_vfr_atc PRIVATE
        APL=0 IBM=1 LIN=0
        XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM400=1
        XP_WELLYS_ATC_VERSION="${VERSION_STRING}"
    )
    target_compile_options(xp_wellys_vfr_atc PRIVATE /W3 /utf-8)

    set(SDK_WIN_LIBS "${CMAKE_SOURCE_DIR}/sdk/Libraries/Win")
    target_link_libraries(xp_wellys_vfr_atc PRIVATE
        "${SDK_WIN_LIBS}/XPLM_64.lib"
        "${SDK_WIN_LIBS}/XPWidgets_64.lib"
        opengl32
        # Ws2_32: sockets pulled in by libcurl. Crypt32: Schannel TLS
        # certificate store used by the statically linked curl. Bcrypt:
        # CNG, for the Windows SHA256 path (issue #19) + curl's RNG.
        # Advapi32: registry / Credential Manager (issue #20).
        Ws2_32
        Crypt32
        Bcrypt
        Advapi32
        CURL::libcurl
    )

    if(XPWELLYS_USE_LOCAL_BUNDLE)
        # ::inference carries the whole graph (whisper/llama/ggml archives +
        # ggml-vulkan + vulkan-1 + piper.lib + onnxruntime.lib); ::piper is the
        # TTS-only subset for a build without local STT/LM.
        if(XPWELLYS_USE_LOCAL_INFERENCE)
            target_link_libraries(xp_wellys_vfr_atc PRIVATE xp_wellys_libs::inference)
        else()
            target_link_libraries(xp_wellys_vfr_atc PRIVATE xp_wellys_libs::piper)
        endif()

        # Delay-load piper.dll and vulkan-1.dll, so neither is mapped for users
        # who never reach the code path that needs it:
        #
        #  * piper.dll (and onnxruntime.dll behind it) — only touched once the
        #    user selects local TTS or the hybrid override (issue #74).
        #  * vulkan-1.dll comes from the graphics driver, not from us. As a
        #    load-time import a missing or broken loader would stop the .xpl
        #    from loading at all, with no in-cockpit diagnostic, even for a user
        #    who never runs ggml.
        #
        # Historical note: this delay-load was briefly blamed for a crash on
        # Windows and turned into a load-time import, which only made it worse
        # (X-Plane then refused the plugin outright). The real fault was in
        # libpiper — a global Ort::Env whose constructor ran under the loader
        # lock — and is fixed upstream in xp_wellys_libs 0.4.2. Delay-loading
        # is unproblematic and remains the right default.
        #
        # delayimp provides the thunks.
        target_link_libraries(xp_wellys_vfr_atc PRIVATE delayimp)
        target_link_options(xp_wellys_vfr_atc PRIVATE /DELAYLOAD:piper.dll)
        if(XPWELLYS_USE_LOCAL_INFERENCE)
            target_link_options(xp_wellys_vfr_atc PRIVATE /DELAYLOAD:vulkan-1.dll)
        endif()

        # Stage every bundle DLL (piper.dll + onnxruntime.dll +
        # onnxruntime_providers_shared.dll) next to the built .xpl so the Windows
        # loader resolves them, and espeak-ng-data into the build dir for the CI
        # stage step to place under <plugin>/Resources/espeak-ng-data. The bundle
        # is extracted before configure, so the glob is populated here.
        file(GLOB _win_piper_dlls "${XP_WELLYS_LIBS_ROOT}/lib/*.dll")
        foreach(_dll ${_win_piper_dlls})
            add_custom_command(TARGET xp_wellys_vfr_atc POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dll}" "$<TARGET_FILE_DIR:xp_wellys_vfr_atc>"
                VERBATIM)
        endforeach()
        add_custom_command(TARGET xp_wellys_vfr_atc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${XP_WELLYS_ATC_ESPEAKNG_DATA_DIR}"
                "${CMAKE_BINARY_DIR}/espeak-ng-data"
            COMMENT "Staging Piper DLLs + espeak-ng-data next to xp_wellys_vfr_atc.xpl"
            VERBATIM)
    endif()
endif()

# ── CLI Executable (ohne X-Plane) ────────────────────────────────────────────
# Host-arch only — no need for universal binary on a dev tool. The
# x_atc_engine OBJECT library is compiled for whatever
# CMAKE_OSX_ARCHITECTURES the plugin targets, so for a cross-arch
# plugin build (e.g. the x86_64 slice of the universal binary built
# on an arm64 host) we cannot link an arm64 atc_repl against x86_64
# engine objects. Skip the dev tools in that case — the universal
# build pipeline runs each arch as its own configure dir and only
# needs the .xpl from each.
set(_xp_engine_arch_matches_host TRUE)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    if(NOT "${CMAKE_HOST_SYSTEM_PROCESSOR}" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(_xp_engine_arch_matches_host FALSE)
        message(STATUS "Cross-arch build (host ${CMAKE_HOST_SYSTEM_PROCESSOR} -> "
                       "target ${CMAKE_OSX_ARCHITECTURES}); skipping atc_repl "
                       "+ unit tests")
    endif()
endif()

if(_xp_engine_arch_matches_host)
add_executable(atc_repl
    $<TARGET_OBJECTS:xp_atc_engine>
    tools/atc_repl/main.cpp
    tools/atc_repl/scenario.cpp
    tools/atc_repl/repl.cpp
    tools/atc_repl/settings_stub.cpp
    tools/atc_repl/traffic_fixture.cpp
    tools/atc_repl/xplane_context_stub.cpp
)
target_include_directories(atc_repl PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/tools/atc_repl
)
# manager.cpp (in the engine OBJECT lib) calls curl_global_init/cleanup
# now, so the headless tool also needs libcurl at link time even
# though it never actually performs an HTTP transfer (no concrete
# backend ever registers in the CLI).
target_link_libraries(atc_repl PRIVATE CURL::libcurl)
if(APPLE)
    target_compile_options(atc_repl PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
    set_target_properties(atc_repl PROPERTIES
        OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}"
    )
endif()
if(XP_WELLYS_ATC_SANITIZE)
    target_compile_options(atc_repl PRIVATE
        ${XP_WELLYS_ATC_SANITIZER_FLAGS} -O1 -g
    )
    target_link_options(atc_repl PRIVATE ${XP_WELLYS_ATC_SANITIZER_FLAGS})
endif()
set_target_properties(atc_repl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
endif() # _xp_engine_arch_matches_host

# ── Tests ────────────────────────────────────────────────────────────────────
if(BUILD_TESTS AND _xp_engine_arch_matches_host)
    add_subdirectory(tests)
endif()
