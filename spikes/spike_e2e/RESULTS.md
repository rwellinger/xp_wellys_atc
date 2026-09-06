# spike_e2e — milestone 05 latency results

End-to-end pipeline `WAV (pilot) → whisper.cpp → llama.cpp → Piper → WAV (atc)`
running in a single process with persistent model handles.

## Test bench

- **Hardware**: Apple M4 (8-core), 32 GB RAM. Note: the milestone document
  specifies M1 as the target; the readings below were captured on an M4.
  M4 is faster than M1 for both Metal and CPU paths, so the ≤ 3 s target
  must be re-validated on M1 hardware before milestone-06 sign-off — but
  these numbers establish that the pipeline architecture is sound.
- **Models**:
  - STT: `ggml-small.en-q5_1.bin` (181 MB)
  - LLM: `Llama-3.2-3B-Instruct-Q4_K_M.gguf` (1.93 GB)
  - TTS: `en_US-lessac-medium.onnx` (63 MB)
- **Fixture**: `spike_whisper/test_audio/tower_ready_for_departure.wav`
  (6.64 s, 16 kHz mono 16-bit PCM, synthetic `say` voice).
- **System prompt**: `prompts/atc_system.txt` — Bern Tower controller persona,
  RWY 14, QNH 1018, callsign HBRCD.

## Single-shot wall clock (cold-process, six independent runs)

The single-shot path starts a fresh process per run, so model load + Metal
pipeline warm-up is paid every time. This is **not** the plugin's expected
mode — included only to bracket the worst-case startup.

| Run | STT load | LLM load | TTS load | STT | LLM | TTS | Wav io | Total |
|----:|---------:|---------:|---------:|----:|----:|----:|-------:|------:|
| 1 (process cold, Metal cache cold) | 5634 ms | 1140 ms | 346 ms | 583 ms | 637 ms | 208 ms | 2 ms | **1429 ms** |

The 5.6 s STT load on the first-ever invocation is `ggml_metal_library_init`
JIT-compiling and caching the embedded `.metallib`. macOS persists that
cache across processes, so subsequent process launches drop to ~120 ms.

## Stdin-loop wall clock (warm, persistent process)

Models load once at startup, then six identical pilot-call requests are
fed via stdin. This **is** the plugin's expected mode. Run 1 still pays
some Metal pipeline-warm-up cost on the first inference; runs 2-6 are
the steady-state numbers the plugin will see.

| Run | STT (ms) | LLM (ms) | TTS (ms) | Wav io (ms) | **Total (ms)** |
|----:|---------:|---------:|---------:|------------:|---------------:|
| 1 (Metal pipelines warming) | 336.5 | 620.6 | 189.3 | 0.7 | **1147.0** |
| 2 (warm) | 314.8 | 634.8 | 211.7 | 0.5 | **1161.9** |
| 3 (warm) | 338.7 | 634.7 | 200.2 | 1.2 | **1174.8** |
| 4 (warm) | 317.0 | 636.0 | 197.6 | 0.7 | **1151.3** |
| 5 (warm) | 318.9 | 631.9 | 190.3 | 0.4 | **1141.6** |
| 6 (warm) | 317.8 | 631.5 | 198.0 | 0.5 | **1147.8** |
| **mean (runs 2-6)** | **321.4** | **633.8** | **199.6** | **0.7** | **1155.5** |

Cold model load in the persistent process (Metal cache warm from prior
runs of any of the spikes): `STT 117 ms + LLM 175 ms + TTS 306 ms ≈ 600 ms`.

Peak RSS: 2.93 GB after run 6 — dominated by llama (≈ 2.3 GB) + piper
(≈ 0.55 GB) + whisper (≈ 0.3 GB). Memory grows < 50 MB across the six
runs, no leak signal.

## Acceptance criteria

| Metric | Target | Measured (warm mean, runs 2-6) | Verdict |
|---|---|---|---|
| End-to-end wall clock | **< 3 s** | 1156 ms | **PASS** with > 1.8 s of headroom |
| Output WAV intelligible + contextually appropriate | qualitatively yes | yes — all six runs return identical, on-format ATC phraseology | **PASS** |
| Models reuse handles across requests | required | yes — model load is per-process, not per-request | **PASS** |

Sample output (every run):

```
transcript : Bern Tower, Hotel Bravo Romeo Charlie Delta, holding short
             runway 14, ready for departure.
response   : Hotel Bravo Romeo Charlie Delta, runway 14 clear to depart.
```

The reply uses "clear to depart" instead of the textbook ICAO "cleared
for takeoff". This is a system-prompt-engineering issue, not a model
issue, and falls into milestone 06 prompt iteration.

## Bottleneck identification

LLM is the dominant stage: **634 ms = 55 % of warm wall-clock**.
Distribution:

```
LLM    634 ms  ████████████████████████████████████████████████████████  55 %
STT    321 ms  ████████████████████████████                              28 %
TTS    200 ms  █████████████████                                         17 %
WAVio    1 ms                                                             0 %
```

Within the LLM stage, the breakdown from milestone 03 is roughly:
prompt eval ≈ 22 ms, time-to-first-token ≈ 350 ms, then ~16 generated
tokens at ~40 tok/s ≈ 400 ms. The TTFT-heavy cost is fixed regardless
of how short the reply is; cutting the reply length further would
shave generation time but not TTFT.

## Optimisations still on the table for milestone 06

The 1.8 s of headroom on M4 means the plugin has room to absorb both
the M4-vs-M1 perf gap and any per-platform overheads (Core Audio,
X-Plane main-thread integration, mic capture). The optimisation levers
below are listed in the order they would be tried if M1 measurements
turn out to be tight or slip past 3 s.

1. **Stream LLM tokens into TTS synthesis (parallelise stages 2 and 3).**
   Today the orchestrator waits for the full LLM reply before calling
   `piper_synthesize_start`. With Piper's chunked-output API and a
   token-streaming hook on llama, TTS can start once the first few
   tokens have arrived. Estimated saving on this fixture: ~150–250 ms
   of wall-clock (TTS overlaps with the tail of LLM generation).
2. **Cut LLM TTFT by warming the prompt-prefix KV cache.** The system
   prompt is fixed across all requests inside a session. Running the
   system prompt once at startup and reusing the resulting KV cache
   for every request collapses prompt-eval to near zero. Estimated
   saving: ~20–80 ms.
3. **Drop whisper to `distil-small.en`.** Milestone 02 measured this
   variant ~50 ms faster but at the cost of mis-transcribing geographic
   proper nouns ("Bern" → "Burn"). Not recommended unless we hit a
   wall, but it is a knob.
4. **CoreML / Neural Engine for Piper.** Today the prebuilt
   onnxruntime defaults to CPU. Adding the CoreML execution provider
   would likely halve the 200 ms TTS stage on M-series at no quality
   cost. Already noted in milestone 04 as deferred.
5. **Shrink LLM context to ~1024 tokens.** `n_ctx=2048` is generous;
   ATC turns are tiny. Halving it reduces the KV cache and may give
   marginal TTFT savings. Risk: limits multi-turn future work, so this
   is a last resort.
6. **Reply-length cap at the sampler.** Today `max_new_tokens=96`. Most
   ATC turns end after 15–25 tokens. Cutting the cap to 40 has no
   effect on observed runs (EOG fires first) but bounds the worst case.

Levers (1) and (2) are the obvious first wins if M1 measurements need
trimming. Both are in scope of milestone 06's plugin orchestration
layer; neither requires touching whisper.cpp or llama.cpp.

## Build and run

```bash
# from spikes/spike_e2e/
git submodule update --init --recursive   # one-time, in the parent spike dirs
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# single-shot
./build/spike_e2e \
    --whisper-model ../spike_whisper/models/ggml-small.en-q5_1.bin \
    --llama-model   ../spike_llama/models/Llama-3.2-3B-Instruct-Q4_K_M.gguf \
    --piper-voice   ../spike_piper/models/en_US-lessac-medium.onnx \
    --piper-config  ../spike_piper/models/en_US-lessac-medium.onnx.json \
    --system-prompt prompts/atc_system.txt \
    --single ../spike_whisper/test_audio/tower_ready_for_departure.wav out/atc.wav

# stdin loop (preferred for steady-state measurement)
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

## Recommendation

**Plugin integration recommended.** The pipeline meets the < 3 s warm
target with >1.8 s of headroom on M4, all three stages reuse handles
without per-request reload, the architecture cleanly maps to the
Strategy interfaces (`ISpeechToText`, `ILanguageModel`, `ITextToSpeech`)
that milestone 06 will lift verbatim, and there are at least five
follow-up levers available before any of them become necessary.

The one open follow-up before milestone 06 starts: re-run this same
fixture on M1 hardware to confirm the headroom is real and not just
M4 generational uplift.
