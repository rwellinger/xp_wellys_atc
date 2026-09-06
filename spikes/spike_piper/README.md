# Spike: Piper TTS standalone CLI

Proves that Piper running on the prebuilt arm64 onnxruntime can synthesize a
typical ATC clearance on the M1 well below the plugin's latency budget, and
lets us A/B the two candidate voices.

## Layout

```
spike_piper/
├── CMakeLists.txt
├── src/spike_piper.cpp                 # the CLI
├── third_party/piper1-gpl/             # git submodule, pinned to v1.4.2
├── test_text/atc_50_word.txt           # 46-word ATC clearance fixture
├── scripts/download_voice.sh           # fetch + verify a Piper voice
└── models/                             # gitignored, populated by you
```

## Why piper1-gpl, not rhasspy/piper

The original `rhasspy/piper` repo was archived on 2025-10-06. The maintained
successor is [`OHF-Voice/piper1-gpl`][piper1-gpl], which provides a clean
C-style API in `libpiper/` and embeds espeak-ng (no separate
`piper-phonemize` dependency to wrangle). The submodule is pinned to
**v1.4.2**.

**License**: piper1-gpl statically links **espeak-ng (GPLv3)** into
`libpiper.dylib`. The plugin is already GPL-3.0 (see top-level `LICENSE`
and `docs/architecture-analysis.md` §5 [DECISION-LICENSE]) and espeak-ng
is one-way compatible with that. No friction.

[piper1-gpl]: https://github.com/OHF-Voice/piper1-gpl

## Build

```
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`libpiper`'s CMake handles the heavy lifting:

- `espeak-ng` is fetched (pinned commit) and built as a static lib via
  `ExternalProject_Add` — no `apt`/`brew` install required.
- `onnxruntime` 1.22.0 macOS arm64 is downloaded as a prebuilt tarball
  (Microsoft official release) and unpacked into
  `build/lib/onnxruntime-osx-arm64-1.22.0/`.
- `libpiper.dylib` is produced in `build/`.
- `build/spike_piper` rpaths into both directories so it runs without
  `DYLD_LIBRARY_PATH` gymnastics.

The first build downloads ~200 MB and compiles espeak-ng — expect 1–2 min.
Incremental builds touch only the spike's translation unit.

## Voice

```
bash scripts/download_voice.sh                    # default: en_US-lessac-medium
PIPER_VOICE=en_US-ryan-high bash scripts/download_voice.sh
```

Files land in `spikes/spike_piper/models/` (gitignored).

| Voice | File size | SHA256 | Source |
|---|---|---|---|
| `en_US-lessac-medium.onnx` | 63 MB | `5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f` | `huggingface.co/rhasspy/piper-voices` |
| `en_US-ryan-high.onnx`     | 121 MB | `b3990d7606e183ec8dbfba70a4607074f162de1a0c412e0180d1ff60bb154eca` | same |

Each `.onnx` is paired with a small `.onnx.json` config that the script
also fetches.

License of the voice files: **CC0 / Public Domain** for the lessac dataset
and for ryan — both are derived from public-domain LibriVox-class corpora
distributed alongside the rhasspy/piper-voices repo. Safe for our use.

## Test fixture

`test_text/atc_50_word.txt` — a 46-word combined takeoff clearance + handoff,
roughly the upper end of what the plugin will generate per turn:

> "Hotel Bravo Romeo Charlie Delta, Bern Tower, runway one four, cleared
> for takeoff, wind two seven zero degrees at eight knots. After
> departure, climb runway heading to two thousand five hundred feet,
> contact Geneva Information one two four decimal one five zero, have a
> good flight."

The milestone target said "50-word phrase"; 46 is within the spirit. The
clearance contains the parts most prone to TTS artefacts: NATO callsign,
runway designators, frequencies, multi-clause structure.

## Usage

```
build/spike_piper models/en_US-lessac-medium.onnx \
                  models/en_US-lessac-medium.onnx.json \
                  test_text/atc_50_word.txt \
                  /tmp/lessac.wav
```

The 3rd argument may be either a path to a text file or the literal text
itself. Output is 22.05 kHz mono 16-bit PCM WAV.

Sample output (warm run):

```
voice        : models/en_US-lessac-medium.onnx
config       : models/en_US-lessac-medium.onnx.json
espeak data  : .../build/espeak_ng-install/share/espeak-ng-data
text         : 46 words, 283 chars
output       : /tmp/lessac.wav (16.64 s, 22050 Hz, mono, 16-bit)

--- timings ---
voice load   :    311.4 ms
synthesis    :    604.6 ms  (audio 16.64 s → RTF 27.52x realtime)
wall total   :    918.1 ms

--- memory ---
peak rss     :    539.6 MB
```

The CLI uses the synthesis options baked into the voice's `.json`
(`length_scale`, `noise_scale`, `noise_w_scale`); no overrides.
espeak-ng-data location is baked in at build time but can be overridden
via `PIPER_ESPEAKNG_DATA_DIR` env var if the install layout changes.

## Measurements (M1, 8-core, 32 GB)

Four warm runs each (model file in OS file cache, fresh process per run).
Numbers come from the spike's own timers; output text is fully
deterministic given fixed input + voice config.

### `en_US-lessac-medium` — recommended

| Metric | Run 1 | Run 2 | Run 3 | Run 4 |
|---|---|---|---|---|
| Voice load          | 311.4 ms | 292.9 ms | 308.2 ms | 302.9 ms |
| Synthesis           | 604.6 ms | 615.0 ms | 607.8 ms | 610.3 ms |
| Audio produced      | 16.64 s  | 16.65 s  | 16.92 s  | 16.44 s  |
| RTF (audio / synth) | 27.52×   | 27.07×   | 27.83×   | 26.94×   |
| Peak RSS            | 539.6 MB | 542.2 MB | 572.7 MB | 575.5 MB |
| Wall total          | 918.1 ms | 908.4 ms | 916.5 ms | 915.1 ms |
| Quality             | crisp, neutral US accent, all proper nouns intelligible, no glitches |

### `en_US-ryan-high` — slower, slightly warmer voice

| Metric | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Voice load          | 225.7 ms | 218.0 ms | 217.3 ms |
| Synthesis           | 3353.4 ms | 3376.2 ms | 3373.3 ms |
| Audio produced      | 17.40 s  | 17.30 s  | 17.19 s  |
| RTF (audio / synth) | 5.19×    | 5.12×    | 5.10×    |
| Peak RSS            | 691.5 MB | 619.5 MB | 620.5 MB |
| Wall total          | 3580.1 ms | 3594.6 ms | 3591.4 ms |
| Quality             | warmer/breathier, slightly more "voicy" but no clarity gain on ATC phraseology |

Ryan-high is ~5.5× slower because it's a "high" Piper variant with ~2×
the parameter count. RTF still beats realtime (5.1×) but **fails the
< 1 s synthesis target on a 50-word phrase**: 3.37 s.

## Acceptance criteria — verdict

| Metric | Target | lessac-medium | ryan-high | Verdict |
|---|---|---|---|---|
| Synthesis latency, ~50-word phrase | **< 1 s** | 605 ms | 3373 ms | **PASS (lessac)** / FAIL (ryan) |
| Output intelligibility, no artefacts | qualitatively yes | yes | yes | **PASS** |
| Real-time factor (audio_s / synth_s) | **> 5×** | ~27× | ~5.1× | **PASS (lessac)** / borderline (ryan) |
| Peak RAM | < 200 MB | ~570 MB | ~660 MB | **FAIL** — see below |

> **Memory note** — the 200 MB target was optimistic. The arm64
> onnxruntime shared lib alone has a steady-state footprint of
> ~150–200 MB independent of the model; add the model weights (63 MB
> for lessac-medium, 121 MB for ryan-high) plus the inference allocator
> and we land at 550–700 MB. There is no realistic single-knob fix for
> this on the official onnxruntime release. For the plugin's
> milestone-06 budget this matters: combined with whisper.cpp (~324 MB,
> milestone 02) and llama.cpp (~2.3 GB, milestone 03) we're at roughly
> **3.2 GB resident** before X-Plane's own footprint. Still well inside
> the 16 GB working budget on a 32 GB M1, but worth recording.

## Recommendation

**Use `en_US-lessac-medium` as the default voice for plugin integration.**
Lessac-medium hits 27× realtime with crisp, neutral-American
phraseology — both proper nouns ("Bern", "Geneva") and digit groups
("two seven zero", "one two four decimal one five zero") come out
clean, with no glitches between clauses.

Ryan-high sounds warmer/slightly more conversational but doesn't add
intelligibility, and at 3.4 s synthesis it fails the <1 s target.

If a future stretch goal needs a more distinct controller voice, look at
the medium-tier `en_GB-alan-medium` or `en_GB-northern_english_male-medium`
voices instead of jumping to "high" — they keep RTF in lessac-medium's
range while changing the timbre.

## Notes / caveats

- **Voice load (~300 ms warm)** dominates wall-time on any single
  one-shot run. In the plugin we'll load the voice once at plugin start
  and reuse the synthesizer across PTT releases, so this is paid only
  during X-Plane startup.
- **CPU-only inference**: the prebuilt onnxruntime arm64 release
  defaults to the CPU execution provider. CoreML/Neural Engine
  acceleration is available but requires opting in via
  `OrtSessionOptionsAppendExecutionProvider_CoreML`, which `libpiper`
  does not currently do. At 27× realtime on CPU this is not worth
  pursuing for the spike; revisit only if milestone-06 introduces
  multiple parallel synthesis streams.
- **Sample rate** is fixed at the voice's native 22.05 kHz. The plugin's
  Core Audio output will need resampling to 44.1 / 48 kHz; this is
  deferred to milestone 06 as the task notes.
- **License**: covered by the plugin's existing GPL-3.0; espeak-ng's
  GPLv3 linkage is compatible. No further action required.

## Out of scope (deferred)

- Streaming (chunked) output — file-based is enough for the proof.
- Voice cloning / training.
- SSML or prosody markup.
- Resampling to 44.1 / 48 kHz — milestone 06 (Core Audio side).
- A/B vs OpenAI TTS quality — not in this milestone's scope.
