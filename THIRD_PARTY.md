# Third-Party Dependencies

This document lists every third-party component bundled with, statically
linked into, or dynamically loaded by `xp_wellys_vfr_atc`. The plugin itself
is licensed under **GNU GPL-3.0-or-later**; the table below shows that
all dependencies are GPLv3-compatible.

## At-a-glance

| Component | Version | License | How it ships |
|---|---|---|---|
| [whisper.cpp](https://github.com/ggml-org/whisper.cpp) | v1.9.1 | MIT | Static library inside the `.xpl` |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | b9733 | MIT | Static library inside the `.xpl` |
| [Piper](https://github.com/OHF-Voice/piper1-gpl) | v1.4.2 | MIT (libpiper); GPL-3.0 (espeak-ng) | `libpiper.dylib` next to the `.xpl` |
| [onnxruntime](https://github.com/microsoft/onnxruntime) | 1.22.0 | MIT | `libonnxruntime.1.22.0.dylib` next to the `.xpl` (prebuilt vendor binary) |
| [espeak-ng](https://github.com/espeak-ng/espeak-ng) | bundled with Piper | GPL-3.0-or-later | Statically linked inside `libpiper.dylib`; data dir bundled |
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.92.8 | MIT | Compiled into the `.xpl` |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.12.0 | MIT | Header-only, compiled into the `.xpl` |
| [Catch2](https://github.com/catchorg/Catch2) | v3.15.1 | BSL-1.0 | Test-only; not shipped with the release |
| libcurl | system (`/usr/lib/libcurl.4.dylib`) | curl/MIT-style | macOS-system; dynamically loaded |
| X-Plane SDK | XPSDK430 | freely redistributable for plugin development | Headers only at build time |

## Per-component detail

### whisper.cpp — MIT

Inference engine for the bundled Whisper STT model. Compiled with
Metal backend enabled (`GGML_METAL=ON`, `GGML_METAL_EMBED_LIBRARY=ON`)
and Apple Accelerate framework. Source is vendored as a git submodule
under `spikes/spike_whisper/third_party/whisper.cpp` and linked
statically into the plugin module.

The bundled Whisper *model* (`ggml-small-q5_1.bin`) is downloaded by
the user at first launch from
[`huggingface.co/ggerganov/whisper.cpp`](https://huggingface.co/ggerganov/whisper.cpp);
the model is licensed under MIT in line with whisper.cpp itself.

### llama.cpp — MIT

Inference engine for the bundled Llama LLM model. Same Metal/Accelerate
setup as whisper.cpp; vendored under
`spikes/spike_llama/third_party/llama.cpp` and linked statically. The
plugin uses the public `llama` and `common` targets.

The bundled *model* (`Llama-3.2-3B-Instruct-Q4_K_M.gguf`) is licensed
under the **Llama 3.2 Community License Agreement** by Meta, accepted
when downloading the file on first launch. Read it at
<https://www.llama.com/llama3_2/license/>. Key terms: free for
commercial and non-commercial use, but redistribution requires the
same license + attribution. The plugin neither redistributes nor
modifies the weights — it downloads them straight from HuggingFace
to the user's disk.

### Piper — MIT (libpiper) + GPL-3.0 (espeak-ng inside)

Neural TTS used for the ATC voice. `libpiper` itself is MIT-licensed;
its phonemizer dependency `espeak-ng` is GPL-3.0-or-later and is
statically linked into `libpiper.dylib`. Because espeak-ng is GPLv3
and is linked into a binary we ship, the combined Piper artifact is
effectively GPLv3 — same license as this plugin, so no conflict.

Source is vendored under
`spikes/spike_piper/third_party/piper1-gpl/libpiper`. The build
configuration uses libpiper's own CMake which fetches espeak-ng via
ExternalProject and downloads the prebuilt onnxruntime arm64 release.

The bundled *voice model* (`en_US-lessac-medium.onnx` + `.onnx.json`)
comes from the [rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices)
collection. The Piper voices are licensed under MIT
(see <https://github.com/rhasspy/piper/blob/master/VOICES.md>). The
underlying voice recordings are CC0 / public-domain.

The `espeak-ng-data/` directory (~19 MB of phonemizer dictionaries)
ships **inside the plugin bundle** at
`<plugin>/Resources/espeak-ng-data/` so users do not need to install
espeak-ng system-wide.

### onnxruntime — MIT

Microsoft's neural-network inference runtime. Used by Piper. We ship
the **prebuilt arm64 macOS dylib** released by Microsoft on GitHub —
building onnxruntime from source is a multi-day undertaking and not
realistic for this project. The dylib (`libonnxruntime.1.22.0.dylib`,
~33 MB) is downloaded once during build by libpiper's CMake, then
copied alongside the `.xpl` so it resolves through `@loader_path`
rpath at runtime.

### Dear ImGui — MIT

In-sim UI library. Compiled directly into the plugin `.xpl` (no
shared lib). Vendored under `vendor/imgui/` by `make setup`.

### nlohmann/json — MIT

Header-only JSON parser. Used for `settings.json`, ATC templates,
flight rules, airport VRPs. Vendored under `vendor/json.hpp`.

### Catch2 — BSL-1.0

Unit-test framework. Test-only dependency: not built into the
release artifact. Vendored under `vendor/catch2/`.

### libcurl — curl/MIT-style

Used by the in-plugin model downloader (HTTPS GET with `Range` resume
to HuggingFace). Plugin links against the system libcurl on macOS
(`/usr/lib/libcurl.4.dylib`); not redistributed.

### X-Plane SDK

Laminar Research's plugin SDK (`XPLM430`). Headers + framework symbols
under `sdk/`, populated by `make setup`. The SDK is freely
redistributable as part of plugins per
<https://developer.x-plane.com/sdk/>; the plugin links the
`XPLM.framework` and `XPWidgets.framework` directly.

## Why GPL-3.0 for the plugin?

The plugin is licensed under GPL-3.0-or-later because espeak-ng
(GPL-3.0-or-later) is statically linked into the `libpiper.dylib` we
ship. Linking GPLv3 code into a non-GPLv3 binary would conflict, so the
plugin itself adopts GPLv3 to stay compatible.

All other bundled / linked components above are GPLv3-compatible (MIT,
BSD-style, BSL-1.0, freely redistributable SDK).

## Source availability

This plugin is open source. The full source is at
`https://github.com/rwellinger/xp_wellys_vfr_atc`. In line with GPLv3,
binary releases include or link to the source repository in the
release notes.

For the bundled third-party static libraries (whisper.cpp, llama.cpp,
Piper, espeak-ng), the source is publicly available at the project
URLs listed in the table above; the plugin links them at fixed
git revisions which are recorded in `.gitmodules` and visible in the
spike directories.
