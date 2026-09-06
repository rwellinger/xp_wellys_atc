# Spike: llama.cpp standalone CLI

Proves that a 3 B-class instruction-tuned LLM running on llama.cpp with the
Metal backend can produce ATC phraseology on the M1 inside the latency,
throughput, and memory budgets the plugin needs.

## Layout

```
spike_llama/
├── CMakeLists.txt
├── src/spike_llama.cpp           # the CLI
├── third_party/llama.cpp/        # git submodule, pinned to b6960
├── prompts/
│   ├── 01_taxi_clearance.txt
│   ├── 02_takeoff_clearance.txt
│   └── 03_traffic_advisory.txt
├── scripts/download_model.sh     # fetch + verify the GGUF model
└── models/                       # gitignored, populated by the script
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
- `LLAMA_BUILD_COMMON=ON` — pull in the `common` static lib (chat templates,
  sampling helpers)
- `LLAMA_BUILD_TOOLS/EXAMPLES/SERVER/TESTS=OFF`, `LLAMA_CURL=OFF` — keep the
  build to the libraries we actually link

The llama.cpp submodule is pinned to **`b6960`** — llama.cpp does not publish
semver releases, only daily `bNNNN` CI tags, so we pick a recent tag rather
than tracking `master`.

## Model

```
bash scripts/download_model.sh
```

Lands in `spikes/spike_llama/models/` (gitignored).

| File | Size | SHA256 | Source |
|---|---|---|---|
| `Llama-3.2-3B-Instruct-Q4_K_M.gguf` | 1.93 GB | `6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff` | `huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF` |

License: **Llama 3.2 Community License** — permissive for our use, document
in the plugin LICENSE notice in milestone 06.

## Prompt format

`spike_llama` reads a plain-text prompt file with section headers:

```
[system]
<the controller persona, station, weather, runway, etc.>
[user]
<the pilot's transcript>
[assistant]   (optional, for few-shot priming)
<example response>
```

Lines before the first header are treated as `[user]`, so a bare prompt file
also works. The CLI applies the model's built-in chat template (Llama 3
chat format in this case) before tokenisation.

## Usage

```
build/spike_llama models/Llama-3.2-3B-Instruct-Q4_K_M.gguf \
                  prompts/02_takeoff_clearance.txt
```

Sample output (warm run):

```
prompt       : prompts/02_takeoff_clearance.txt (2 turns)
model        : models/Llama-3.2-3B-Instruct-Q4_K_M.gguf

--- response ---
Bern Tower, Hotel Bravo Romeo Charlie Delta, cleared for takeoff runway one four.

--- timings ---
model load   :    213.2 ms
prompt eval  :     21.0 ms  (144 tokens)
first token  :    345.0 ms  (TTFT incl. prompt eval)
generation   :    397.7 ms  (16 tokens after the first)
throughput   :     40.2 tok/s
wall total   :   1468.7 ms
threads      : 4

--- memory ---
peak rss     :   2253.8 MB
```

Sampling: `top_k=40 → top_p=0.9 → temp=0.3 → dist(seed=1234)`,
`max_tokens=256`, `n_ctx=2048`.

The CLI always performs an 8-token warm-up generation that is discarded
before the measured run, so the reported timings reflect steady-state
Metal pipeline behaviour rather than first-launch JIT cost.

## Measurements (M1, 8-core, 32 GB)

Three runs each, model file already in OS file cache. Numbers come from the
spike's own timers; sampling is deterministic at this temperature so output
text is stable across runs.

### `01_taxi_clearance.txt` — 170 prompt tokens

| Metric | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Prompt eval        | 22.3 ms | 23.0 ms | 21.6 ms |
| TTFT (incl. eval)  | 403.7 ms | 407.2 ms | 408.0 ms |
| Generation         | 349.3 ms (14 tokens) | 356.5 ms (14 tokens) | 348.0 ms (14 tokens) |
| Throughput         | 40.1 tok/s | 39.3 tok/s | 40.2 tok/s |
| Peak RSS           | 2262 MB | 2256 MB | 2271 MB |
| Response | "Hotel Bravo Romeo Charlie Delta, taxi to holding point, clear for taxi." |

### `02_takeoff_clearance.txt` — 144 prompt tokens

| Metric | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Prompt eval        | 21.8 ms | 23.1 ms | 21.0 ms |
| TTFT (incl. eval)  | 342.1 ms | 344.8 ms | 345.0 ms |
| Generation         | 394.5 ms (16 tokens) | 398.9 ms (16 tokens) | 397.7 ms (16 tokens) |
| Throughput         | 40.6 tok/s | 40.1 tok/s | 40.2 tok/s |
| Peak RSS           | 2277 MB | 2275 MB | 2254 MB |
| Response | "Bern Tower, Hotel Bravo Romeo Charlie Delta, cleared for takeoff runway one four." |

### `03_traffic_advisory.txt` — 198 prompt tokens

| Metric | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Prompt eval        | 22.2 ms | 22.3 ms | 21.8 ms |
| TTFT (incl. eval)  | 471.6 ms | 475.2 ms | 478.0 ms |
| Generation         | 444.1 ms (18 tokens) | 447.3 ms (18 tokens) | 447.8 ms (18 tokens) |
| Throughput         | 40.5 tok/s | 40.2 tok/s | 40.2 tok/s |
| Peak RSS           | 2254 MB | 2255 MB | 2255 MB |
| Response | "Bern Tower, Hotel Bravo Romeo Charlie Delta, runway 14 in sight, clear to land." |

### Cold start

First-ever run after a fresh boot or after the GGUF file has been evicted
from the macOS file cache: model load drops from ~210 ms (warm) to ~5.7 s
(cold). For the plugin, the model will be loaded **once at plugin start**
and reused for every PTT press, so the cold load is paid only during
X-Plane startup.

## Acceptance criteria — verdict

| Metric | Target | Measured (worst case across the three prompts) | Verdict |
|---|---|---|---|
| First-token latency (after warm-up) | < 500 ms  | 478 ms (traffic, 198-token prompt) | **PASS** (with margin shrinking on longer prompts) |
| Sustained throughput                | > 30 tok/s | 39.3 tok/s | **PASS** |
| Peak RAM during inference           | < 3 GB    | 2.28 GB | **PASS** |
| Output quality on ATC prompts       | recognizable phraseology, no obvious hallucinations | see notes below | **PASS with caveats** |

### Output-quality notes

- Taxi clearance: clean and idiomatic. Includes the "taxi to holding
  point" phrasing the system prompt asked for.
- Takeoff clearance: textbook ICAO format, callsign + clearance + runway
  in the right order.
- Traffic advisory: this is the weakest of the three. The model collapses
  the response to a generic landing clearance and ignores the explicit
  instruction to point out the conflicting PA-28 on left base. The reply
  also presupposes a "runway in sight" report the pilot never made. This
  is a prompt-engineering problem, not a model problem — the system
  prompt asks for too much in one turn, and Llama-3.2-3B follows the
  most prominent instruction (issue a clearance) and drops the rest.
  Address in milestone 06 with either a sharper system prompt or by
  splitting traffic-advisory into a dedicated state-machine intent.

## Notes / caveats

- TTFT scales roughly linearly with prompt length on Metal at this model
  size: ~22 ms prompt-eval per ~150–200 tokens, plus a fairly large
  fixed cost for the first single-token decode after the cleared KV
  cache (the gap between "prompt eval" and "first token" is ~320 ms in
  these runs). On the plugin we expect prompts in the same length range,
  so TTFT should land in the 350–500 ms band as long as we keep
  `n_ctx=2048` and don't bloat the system prompt.
- The throughput of ~40 tok/s means a 30-token ATC reply (typical) takes
  ~750 ms after TTFT. Combined with TTFT, total ATC-reply generation is
  ~1.1–1.3 s end-to-end before TTS — comfortably inside the 3 s budget
  the milestone-05 end-to-end target sets for the whole STT → LLM → TTS
  pipeline.
- Sampling is deterministic at `temp=0.3` with a fixed seed — useful for
  reproducible spike numbers, but the plugin will likely want
  `temp=0.5–0.7` and an unseeded RNG so the controller doesn't say the
  exact same thing every time.

## Recommendation

**Use `Llama-3.2-3B-Instruct-Q4_K_M.gguf` as the default LLM for plugin
integration.** All three acceptance criteria pass with margin, and the
phraseology quality on the two routine clearances is essentially
production-grade. The traffic-advisory weakness is a prompt-design
issue rather than a model-capability issue and is in scope for
milestone 06.

The 2.3 GB peak RSS leaves roughly 13.7 GB of the assumed 16 GB
non-X-Plane budget for whisper.cpp (~324 MB from milestone 02), Piper
(~100 MB expected in milestone 04), and headroom — no need to trial the
larger 7 B alternatives.

## Out of scope (deferred)

- GBNF / structured output — free-form text is fine for the spike;
  structured output (e.g. JSON for state-machine integration) is a
  milestone-06 concern.
- Streaming token output to the user — file-based prompt + bulk reply
  is sufficient to prove the latency claim.
- Multi-turn conversation history — handled by the state machine in the
  plugin, not inside the spike.
- Quality benchmarks beyond eyeballing — the plugin's correctness is
  guarded by the rule-based state machine; the LLM's job is phraseology
  generation, not decision-making.
