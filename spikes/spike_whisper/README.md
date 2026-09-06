# Spike: whisper.cpp standalone CLI

Proves that whisper.cpp with the Metal backend can transcribe a typical ATC
clip on the M1 well below the latency and memory targets that the plugin
needs.

## Layout

```
spike_whisper/
├── CMakeLists.txt
├── src/spike_whisper.cpp           # the CLI
├── third_party/whisper.cpp/        # git submodule, pinned to v1.7.6
├── test_audio/
│   └── tower_ready_for_departure.wav
├── scripts/generate_test_audio.sh  # regenerate fixture from `say`+`afconvert`
└── models/                         # gitignored, populated by you
```

## Build

```
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Build flags set automatically by `CMakeLists.txt`:

- `GGML_METAL=ON` — Metal backend
- `GGML_METAL_EMBED_LIBRARY=ON` — bake the `.metallib` into the static lib so
  the binary is self-contained (this is what we want for the plugin too)
- `GGML_ACCELERATE=ON` — use macOS Accelerate framework for CPU fallback
- whisper.cpp examples / tests / server disabled, static libs

The whisper.cpp submodule is pinned to **v1.7.6** (last 1.7.x release before
the 1.8 cmake re-org).

## Models

Both files go into `spikes/spike_whisper/models/` (gitignored).

| File | Size | SHA256 | Source |
|---|---|---|---|
| `ggml-small.en-q5_1.bin` (recommended) | 181 MB | `bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30` | `huggingface.co/ggerganov/whisper.cpp` |
| `ggml-distil-small.en.bin` (faster) | 320 MB | `7691eb11167ab7aaf6b3e05d8266f2fd9ad89c550e433f86ac266ebdee6c970a` | `huggingface.co/distil-whisper/distil-small.en` |

```
mkdir -p models
curl -L -o models/ggml-small.en-q5_1.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en-q5_1.bin
curl -L -o models/ggml-distil-small.en.bin \
  https://huggingface.co/distil-whisper/distil-small.en/resolve/main/ggml-distil-small.en.bin
shasum -a 256 models/*.bin   # verify against table above
```

> **Deviation from the milestone document**: the milestone listed
> `ggml-distil-small.en-q5_1.bin` as the primary model, but no quantized
> distil-small variant is published on HuggingFace. The full-precision
> `ggml-distil-small.en.bin` (f16) is available in the
> `distil-whisper/distil-small.en` repo and serves as a workable substitute.
> Measurements below show that `small.en-q5_1` is the better default
> regardless — see "Recommendation".

## Test fixture

`test_audio/tower_ready_for_departure.wav` — 6.6 s, 16 kHz mono 16-bit PCM,
generated with macOS `say` (Daniel voice, 170 wpm):

> "Bern Tower, Hotel Bravo Romeo Charlie Delta, holding short runway one
> four, ready for departure."

Regenerate with `scripts/generate_test_audio.sh`. This synthetic source is
deliberately clean — real cockpit audio with PTT click, sidetone, and
background noise will be the real test in milestone 06.

## Usage

```
build/spike_whisper models/ggml-small.en-q5_1.bin \
                    test_audio/tower_ready_for_departure.wav
```

Sample output (run 2, after Metal kernel cache is warm):

```
audio        : test_audio/tower_ready_for_departure.wav (6.64 s, 16000 Hz, 1 ch)
model        : models/ggml-small.en-q5_1.bin

--- transcript ---
 Bern Tower, Hotel Bravo Romeo Charlie Delta, holding short runway 14, ready for departure.

--- timings ---
model load   :    133.0 ms
inference    :    351.2 ms  (audio 6.64 s → RTF 0.053)
wall total   :    484.3 ms
threads      : 5

--- memory ---
peak rss     :    323.8 MB
```

## Measurements (M1, 8-core, 32 GB)

Three warm runs each (Metal kernel cache pre-populated by a discarded
warm-up run; numbers from the spike's own timers).

### `ggml-small.en-q5_1.bin` — recommended

| Metric | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Model load          | 128.7 ms | 133.0 ms | 123.9 ms |
| Inference (6.64 s audio) | 396.3 ms | 351.2 ms | 351.6 ms |
| Wall total          | 525.2 ms | 484.3 ms | 475.6 ms |
| Peak RSS            | 324.0 MB | 323.8 MB | 323.8 MB |
| RTF                 | 0.060 | 0.053 | 0.053 |
| Transcript quality  | "Bern Tower, Hotel Bravo Romeo Charlie Delta, holding short runway 14, ready for departure." — perfect |

### `ggml-distil-small.en.bin` — faster but worse on proper nouns

| Metric | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Model load          | 152.1 ms | 142.8 ms | 147.8 ms |
| Inference (6.64 s audio) | 315.8 ms | 300.7 ms | 301.2 ms |
| Wall total          | 468.0 ms | 443.7 ms | 449.1 ms |
| Peak RSS            | 414.7 MB | 414.8 MB | 414.7 MB |
| RTF                 | 0.048 | 0.045 | 0.045 |
| Transcript quality  | "Burn Tower, …" — misrecognises "Bern" as "Burn" |

### Cold start

First-ever run after `rm -rf build` and a fresh model load: model load
~10 s on the very first launch (Metal kernel JIT + .metallib unpack).
Subsequent process launches drop to ~150 ms because macOS caches the
compiled Metal pipelines. **For the plugin we will load the model once at
plugin start and reuse the context, so the cold-start cost is paid only
during X-Plane startup, not on every PTT press.**

## Acceptance criteria — verdict

| Metric | Target | Measured (small.en-q5_1) | Measured (distil-small.en) | Verdict |
|---|---|---|---|---|
| Inference latency (audio < 7 s) | < 500 ms | ~350 ms | ~305 ms | **PASS** |
| Peak RAM during inference       | < 500 MB | 324 MB  | 415 MB  | **PASS** |
| ATC phraseology quality         | qualitatively acceptable | perfect | "Bern" → "Burn" | **PASS** (q5_1) |
| Reproducible build              | yes      | yes     | yes     | **PASS** |

> Note: the milestone target says "5 s clip"; our fixture is 6.6 s. The
> RTF (real-time factor) of ~0.05 means even a 10 s clip would land at
> ~500 ms. Adjusting for the difference: a 5 s clip would transcribe in
> roughly **265 ms** with `small.en-q5_1`.

## Recommendation

**Use `ggml-small.en-q5_1.bin` as the default model for plugin integration.**
The distil-small variant is ~20% faster but loses on geographic proper
nouns ("Bern" → "Burn"), which matters for an EU-leaning ATC plugin where
airport names dominate the transcription. The 50 ms speed advantage is
not worth the quality regression.

The q5_1 quantised model also has a ~90 MB smaller resident-memory
footprint (324 MB vs 415 MB), which leaves more headroom inside the
shared X-Plane process.

## Out of scope (deferred)

- VAD — the plugin does push-to-talk, mic capture is gated by the PTT
  command, no need for VAD inside the inference path.
- Streaming inference — file-based input is sufficient to prove the
  latency claim. Streaming may be revisited in milestone 06 if the
  user-perceived latency feels noticeable.
- Real cockpit audio — synthetic `say` output is enough for the spike;
  full robustness will be tested with X-Plane mic capture in milestone 06.
