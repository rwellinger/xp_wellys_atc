# Spike: end-to-end CLI (STT → LLM → TTS)

Stitches the three previous spikes (whisper.cpp, llama.cpp, Piper) into a
single CLI that reads a pilot's WAV, transcribes, generates an ATC reply
through a fixed system prompt, synthesizes the reply, and writes a response
WAV — all in one process with persistent model handles.

See [`RESULTS.md`](RESULTS.md) for the latency report and the plugin-
integration recommendation that came out of this spike.

## Layout

```
spike_e2e/
├── CMakeLists.txt
├── RESULTS.md                       # latency report + go/no-go
├── prompts/
│   └── atc_system.txt               # iterable ATC controller persona
└── src/
    ├── i_speech_to_text.hpp         # Strategy interfaces — lifted into
    ├── i_language_model.hpp         #   the plugin in milestone 06
    ├── i_text_to_speech.hpp
    ├── whisper_stt.hpp/.cpp         # ISpeechToText  via whisper.cpp
    ├── llama_lm.hpp/.cpp            # ILanguageModel via llama.cpp
    ├── piper_tts.hpp/.cpp           # ITextToSpeech  via Piper
    ├── wav.hpp/.cpp                 # 16-bit PCM read (16 kHz mono) + write
    └── spike_e2e.cpp                # CLI: argparse + stdin loop + timings
```

There is no `third_party/` or `models/` directory of its own — this
spike consumes the submodules and downloaded models populated by the
three sibling spikes:

- `../spike_whisper/third_party/whisper.cpp` (submodule)
- `../spike_llama/third_party/llama.cpp` (submodule)
- `../spike_piper/third_party/piper1-gpl` (submodule)
- `../spike_whisper/models/ggml-small.en-q5_1.bin`
- `../spike_llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf`
- `../spike_piper/models/en_US-lessac-medium.{onnx,onnx.json}`

Make sure those three spikes are checked out and their models
downloaded (`scripts/download_*.sh` in each) before building this one.

## Build

Both whisper.cpp and llama.cpp vendor their own copy of `ggml`. Adding
both as `add_subdirectory` would normally collide on the `ggml` CMake
target name, but each of their CMakeLists guards the add with
`if (NOT TARGET ggml)`. So the spike's `CMakeLists.txt` adds llama.cpp
**first** — letting it materialise the `ggml` target with its (newer)
ggml version — and then adds whisper.cpp, which detects the existing
target and reuses it. Whisper.cpp v1.7.6 builds cleanly against
llama.cpp b6960's ggml; if a future bump breaks the ABI, the fallback
is to repackage one of the two as a private dylib with hidden ggml
symbols.

```bash
# from spikes/spike_e2e/
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The first build downloads + compiles espeak-ng (≈ 1–2 min, via libpiper's
`ExternalProject_Add`); subsequent builds touch only the spike's own
translation units.

## Run

Single-shot — fresh process, dies after one request. Useful for
sanity-checking the pipeline.

```bash
./build/spike_e2e \
    --whisper-model ../spike_whisper/models/ggml-small.en-q5_1.bin \
    --llama-model   ../spike_llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf \
    --piper-voice   ../spike_piper/models/en_US-lessac-medium.onnx \
    --piper-config  ../spike_piper/models/en_US-lessac-medium.onnx.json \
    --system-prompt prompts/atc_system.txt \
    --single ../spike_whisper/test_audio/tower_ready_for_departure.wav out/atc.wav
```

Stdin loop — model handles persist; each line is one request. This is
the mode that yields warm steady-state latencies and the mode the
plugin will use in milestone 06.

```bash
printf '%s\n' \
  "../spike_whisper/test_audio/tower_ready_for_departure.wav out/r1.wav" \
  "../spike_whisper/test_audio/tower_ready_for_departure.wav out/r2.wav" \
  | ./build/spike_e2e \
        --whisper-model ../spike_whisper/models/ggml-small.en-q5_1.bin \
        --llama-model   ../spike_llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf \
        --piper-voice   ../spike_piper/models/en_US-lessac-medium.onnx \
        --piper-config  ../spike_piper/models/en_US-lessac-medium.onnx.json \
        --system-prompt prompts/atc_system.txt
```

`--piper-espeak-data <dir>` overrides the espeak-ng-data location;
the default is baked in at compile time and points into this build
tree (`build/espeak_ng-install/share/espeak-ng-data`).

## Output format

- **In**: 16 kHz mono 16-bit PCM WAV. The minimal reader rejects
  anything else (no resampling — the plugin's mic capture in milestone
  06 already records at 16 kHz mono per the existing audio path).
- **Out**: 22.05 kHz mono 16-bit PCM WAV (Piper's native rate;
  resampling to Core Audio's 44.1 / 48 kHz is a milestone-06 task).

## ATC system prompt

[`prompts/atc_system.txt`](prompts/atc_system.txt) is iterable without
rebuilds. Today it pins the controller to LSZB Tower with RWY 14 and
QNH 1018, matching the test fixture. Milestone 06's plugin
orchestrator will substitute live values from `XPlaneContext` into a
templated version of the same prompt.

## Out of scope (deferred to milestone 06)

- VAD / mic capture — handled by the existing plugin audio path.
- Multi-turn dialogue — owned by the ATC state machine.
- Sample-rate conversion to Core Audio's 44.1 / 48 kHz output.
- LLM-token-into-TTS streaming (the obvious first latency lever, see
  [RESULTS.md](RESULTS.md) §"Optimisations still on the table").
- KV-cache prefix reuse across requests (second lever).
- Any UI.
