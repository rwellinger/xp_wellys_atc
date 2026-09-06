# Architecture Analysis — Cloud-Call Sites & Strategy Refactoring Proposal

**Status:** DRAFT — awaiting user approval before any refactoring starts (Milestone 01).
**Source baseline:** `rwellinger/xp_welly_atc` HEAD as cloned on 2026-04-30.

This document inventories every place the plugin currently talks to OpenAI, describes the
control flow that connects those calls, and proposes Strategy/Backend interfaces that will
let us swap the cloud stack for local inference (`whisper.cpp` + `llama.cpp` + Piper) without
touching the ATC state machine.

---

## 1. Inventory of cloud call sites

### 1.1 The three wrappers (all calls to OpenAI live here)

| File | Namespace | Purpose | LOC | Endpoint |
|---|---|---|---|---|
| `src/openai/whisper_client.{hpp,cpp}` | `whisper_client` | STT — pilot mic → text | 47 / 222 | `POST /v1/audio/transcriptions` (multipart, model `whisper-1`) |
| `src/openai/gpt_client.{hpp,cpp}`     | `gpt_client`     | LLM — intent classification (and unused free-form ask) | 44 / 283 | `POST /v1/chat/completions` (model from settings, default `gpt-4o-mini`) |
| `src/openai/tts_client.{hpp,cpp}`     | `tts_client`     | TTS — ATC text → speech | 42 / 157 | `POST /v1/audio/speech` (model `tts-1`, MP3 response) |

All three follow the same pattern:

- `init()` / `stop()` lifecycle, called from `main.cpp` (`XPluginStart` / `XPluginStop`).
- Public `*_async()` entry point that detaches a `std::thread` doing libcurl synchronously.
- Thread-side result is `enqueue_callback(...)`'d into a shared queue.
- `drain_callback_queue()` is invoked once per flight-loop frame from `main.cpp:82–84`,
  guaranteeing the user-supplied callback runs on the main (X-Plane) thread.

The thread/queue/callback machinery is the contract of the call site — any replacement
backend has to keep it intact, otherwise the ATC state machine (which assumes its callback
runs on the main thread) breaks.

### 1.2 Direct callers of the wrappers

| Caller | Wrapper used | Where |
|---|---|---|
| `src/atc/atc_session.cpp:74`  | `tts_client::speak_async`           | Speak ATC response (and ATIS) |
| `src/atc/atc_session.cpp:189` | `whisper_client::transcribe_async`  | After PTT release, send recorded WAV |
| `src/atc/engine.cpp:133`      | `gpt_client::classify_intent_async` | Departure sub-variant disambiguation (pattern vs. cross-country) |
| `src/atc/engine.cpp:233`      | `gpt_client::classify_intent_async` | Generic intent classification when rule parser confidence < 0.7 |
| `src/main.cpp:38–40, 82–84, 141–143, 188–190` | all three  | init/stop + per-frame callback drain |

`gpt_client::ask_async` is declared and implemented but **never called** anywhere in the
codebase (verified via `grep -rn ask_async src/ tools/ tests/`). It is dead code we can
drop in the refactoring step.

### 1.3 Indirect dependencies (settings, UI, defaults)

| File | Concern | Impact |
|---|---|---|
| `src/persistence/settings.cpp:43`        | Keychain account name `"openai_api_key"` | Setting key for cloud auth — local stack doesn't need an API key, but we may want to keep the slot to allow runtime backend switching |
| `src/persistence/settings.cpp:50–52`     | Defaults `tts_model="tts-1"`, `whisper_model="whisper-1"`, `gpt_model="gpt-4o-mini"` | Backend-specific config — needs to become backend-agnostic (or at minimum, a "backend" key plus per-backend sub-config) |
| `src/persistence/settings.cpp:261–266`   | Getters `tts_model()`, `whisper_model()`, `gpt_model()` | Currently passed straight into HTTP bodies. With Strategy boundary, these become inputs to the backend factory, not to the wrappers |
| `src/persistence/settings.{cpp,hpp}` (`get_api_key`, `save_api_key`, `delete_api_key`, `api_key_saved`) | Keychain wrapper | Stays — only the cloud backend will read it |
| `src/atc/atc_session.cpp:131`            | `if (settings::get_api_key().empty()) return;` | Hard PTT block on missing API key. Becomes "if backend not ready" check |
| `src/ui/atc_ui.cpp:572`                  | UI label `"OpenAI API Key:"`             | Cosmetic — needs to become backend-aware (or just say "API Key" and only show when cloud backend is selected) |
| `CMakeLists.txt:30, 64, 125, 143`        | `find_package(CURL REQUIRED)` and link  | `CURL` only required when cloud backend is built. Will be a CMake option |
| `CMakeLists.txt:44–46`                   | Engine OBJECT lib pulls in all three wrappers | Becomes selectable: `inference/openai/*` or `inference/local/*` (or both) |

### 1.4 Audio format coupling

`tts_client::speak_async` returns `std::vector<uint8_t>` containing **MP3** bytes. The
consumer `audio_player::play(mp3_data, volume)` (defined in `src/audio/audio_player.hpp:34`)
decodes MP3 internally. Piper produces **WAV / raw PCM**, not MP3.

`audio_player.hpp:37` already exposes `play_wav(wav_data, volume)` — so the receiving side
*can* handle WAV today, but the TTS callback signature is hardcoded to "vector of MP3
bytes". This is a real coupling that has to be broken in the Strategy boundary (proposal
in §3.3).

---

## 2. Current call flow

### 2.1 PTT-driven request flow (the hot path)

```
                                 main.cpp
                            (flight-loop, 60 Hz)
                                     │
              ┌──────────────────────┼─────────────────────────────┐
              │                      │                             │
              ▼                      ▼                             ▼
        ptt_input::update     drain callback queues          atc_session::update
              │              (whisper, gpt, tts)                    │
              ▼                                                     ▼
        on_ptt_pressed                                      handles PLAYING→IDLE
              │                                              + ATIS auto-trigger
              ▼
  audio_recorder::start_recording
              │
       (user releases)
              │
              ▼
        on_ptt_released ──────────► audio_recorder::stop_recording → encode_wav
                                              │
                                              ▼
                            ┌─►  whisper_client::transcribe_async(wav, cb1)
                            │             │
                            │             ▼ (worker thread, HTTPS)
                            │       OpenAI  /v1/audio/transcriptions
                            │             │
                            │             ▼ (drained on main thread)
                            │       cb1(TranscriptResult{text, quality})
                            │             │
                            │             ▼
                            │     engine::process_transcript
                            │             │
                            │      ┌──────┴───────────────┐
                            │      │ rule-based parse     │
                            │      │ (intent_parser)      │
                            │      │                      │
                            │      │ if low confidence OR │
                            │      │ departure-variant:   │
                            │      └─► gpt_client::        │
                            │            classify_intent_   │
                            │            async (cb2)        │
                            │             │ (worker thread, HTTPS)
                            │             ▼
                            │       OpenAI /v1/chat/completions
                            │             │
                            │             ▼ (drained on main thread)
                            │       cb2(intent_key)
                            │             │
                            │             ▼
                            │     atc_state_machine::process(msg, ctx)
                            │             │
                            │             ▼
                            │       Output{response_text}
                            │             │
                            │             ▼
                            └────  tts_client::speak_async(text, cb3)
                                          │ (worker thread, HTTPS)
                                          ▼
                                    OpenAI /v1/audio/speech (mp3)
                                          │
                                          ▼ (drained on main thread)
                                    cb3(mp3_bytes)
                                          │
                                          ▼
                                    audio_player::play(mp3, volume)
```

Three async network round-trips per pilot transmission in the worst case (Whisper → GPT →
TTS), two on the common path (Whisper → TTS, when rule parser confidence ≥ 0.7).

### 2.2 ATIS path (no STT, no LLM, only TTS)

`atc_session::update` runs every frame; when the COM radio is tuned to an ATIS frequency
for ≥ 2 s, it calls `atis_generator::generate_atis_text(ctx)` → `tts_client::speak_async`.
No Whisper, no GPT involved.

### 2.3 Threading & lifetime invariants the refactor must preserve

1. **All callbacks run on the main thread.** This is established by the per-frame
   `drain_callback_queue()` calls. The state machine and audio_player are not thread-safe.
2. **Each `*_async` call detaches a thread.** No back-pressure, no cancellation. With
   local inference we get deterministic latency and can do better, but for milestone 01 we
   only need to *match* the existing contract.
3. **Callbacks may fire after `stop()`.** The wrappers use a `stopped_` atomic to drop late
   callbacks. Local backends must do the same (especially relevant for Metal: a llama.cpp
   inference may still be running on the GPU when the plugin shuts down).
4. **`engine.cpp` snapshots `XPlaneContext` before the GPT call** (line 231) so the async
   callback sees the world as it was when the pilot spoke. Backend swap doesn't change
   this — but a faster local LLM lowers the chance of stale ctx anyway.

---

## 3. Proposed Strategy/Backend boundaries

### 3.1 Naming and location — proposed defaults (open decisions)

> **[DECISION-1]** Place the interfaces in a new `src/inference/` directory.
> Existing `src/openai/` becomes `src/inference/openai/` (cloud impl). New
> `src/inference/local/` will hold the whisper.cpp / llama.cpp / Piper impls (added in
> milestones 02–05). The headers `src/inference/{stt,llm,tts}_backend.hpp` define the
> abstract surface. Rationale: a single root for *all* inference code, easy to grep, and
> the plugin's `main.cpp` only needs to include the abstract headers + a factory.
>
> **[DECISION-2]** Naming convention: `SpeechToTextBackend`, `LanguageBackend`,
> `TextToSpeechBackend` (suffix `Backend`). Rationale: avoids the Hungarian-ish `IXxx`
> prefix; "Backend" reads better than "Strategy" because it signals *what is swappable*
> (the inference backend), and it doesn't collide with the GoF "Strategy" term elsewhere
> in the codebase.

Both decisions are reversible; user override welcome before milestone 02 starts.

### 3.2 Interface surface

All three follow the same shape: `init()` / `stop()` / `<verb>_async(... , callback)` /
`drain_callback_queue()`. Backend concrete classes hold their own thread+queue state, just
like the existing wrappers. The plugin sees only the abstract interface plus a
`make_*_backend()` factory.

#### 3.2.1 `src/inference/stt_backend.hpp` (replaces `whisper_client`)

```cpp
namespace inference {

struct TranscriptResult {
  std::string text;
  float quality = 1.0f;   // 0.0 = noise, 1.0 = confident
  bool success = false;
};

struct STTRequest {
  std::vector<uint8_t> wav_data;        // 16-bit PCM, mono, 16 kHz, RIFF-wrapped
  std::string airport_context;          // prompt-bias hint (optional)
};

class SpeechToTextBackend {
 public:
  virtual ~SpeechToTextBackend() = default;
  virtual void init() = 0;
  virtual void stop() = 0;
  virtual void transcribe_async(
      STTRequest req,
      std::function<void(TranscriptResult)> callback) = 0;
  virtual void drain_callback_queue() = 0;
};

std::unique_ptr<SpeechToTextBackend> make_stt_backend();   // factory, picks impl

} // namespace inference
```

Notes:
- WAV input format is fixed and documented — both cloud and whisper.cpp accept it. Piper
  is irrelevant here (TTS only). If we later want raw PCM in for whisper.cpp to skip a
  parse step, we can add an overload; not now.
- `quality` semantics: cloud computes it from `verbose_json` (see whisper_client.cpp:170+);
  whisper.cpp must derive an equivalent (e.g., from `whisper_full_get_segment_no_speech_prob`
  + decode probabilities). The state machine only checks `quality >= 0.3f`, so the local
  backend just needs a reasonable mapping.

#### 3.2.2 `src/inference/llm_backend.hpp` (replaces `gpt_client`)

Drop the unused `ask_async`. Keep only the actually-used surface:

```cpp
namespace inference {

class LanguageBackend {
 public:
  virtual ~LanguageBackend() = default;
  virtual void init() = 0;
  virtual void stop() = 0;
  virtual void classify_intent_async(
      std::string transcript,
      std::string system_prompt,
      std::function<void(std::string intent_key, bool success)> callback) = 0;
  virtual void drain_callback_queue() = 0;
};

std::unique_ptr<LanguageBackend> make_llm_backend();

} // namespace inference
```

Notes:
- `classify_intent_async` is the only LLM call the plugin actually makes. The system
  prompt is fully built by `engine.cpp` from templates in `atc_templates`; the backend
  doesn't compose prompts.
- Callback gets a free-form string + success flag. The state machine maps the string back
  to a `PilotIntent`. No structured output / JSON — keeps both backends simple.
- If the spike later wants free-form GPT replies (the dead `ask_async`), we add it then;
  YAGNI.

#### 3.2.3 `src/inference/tts_backend.hpp` (replaces `tts_client`)

This is the only interface that *changes shape* materially, because the audio format is
no longer MP3-only:

```cpp
namespace inference {

enum class AudioFormat { MP3, WAV };  // PCM-raw deferred until needed

struct AudioBuffer {
  std::vector<uint8_t> data;
  AudioFormat format = AudioFormat::WAV;
};

struct TTSRequest {
  std::string text;
  std::string voice;       // backend-specific id (OpenAI voice name OR Piper model id)
  float speed = 1.0f;      // backend maps to its own units
};

class TextToSpeechBackend {
 public:
  virtual ~TextToSpeechBackend() = default;
  virtual void init() = 0;
  virtual void stop() = 0;
  virtual void speak_async(
      TTSRequest req,
      std::function<void(AudioBuffer audio, bool success)> callback) = 0;
  virtual void drain_callback_queue() = 0;
};

std::unique_ptr<TextToSpeechBackend> make_tts_backend();

} // namespace inference
```

Consumer-side change: `atc_session::speak_response` becomes one call:

```cpp
audio_player::play(audio.data, audio.format, volume);   // dispatches MP3 vs WAV
```

`audio_player::play_wav` already exists (`audio_player.hpp:37`), so this is a small
adapter on top, not new decoding work.

> **[DECISION-3]** Voice identifier scheme. Cloud uses names like `"alloy"`, `"echo"`.
> Piper uses `.onnx` model files, e.g. `en_US-ryan-medium.onnx`. Proposal: keep the
> existing `tts_voice_tower` / `tts_voice_ground` / `tts_voice_atis` settings as opaque
> strings, and let each backend resolve them. The cloud backend uses the string verbatim;
> the Piper backend looks up `Resources/models/<voice>.onnx`. No interface change needed.

### 3.3 Lifecycle (`init` / `stop` / drain) — unchanged contract

```
XPluginStart        XPluginStop
    │                    │
    ├─ stt->init()       ├─ tts->stop()       (reverse order; release Metal contexts)
    ├─ llm->init()       ├─ llm->stop()
    ├─ tts->init()       └─ stt->stop()
    │
    └─ flight-loop:  stt->drain(); llm->drain(); tts->drain();
```

For the local backends, `init()` is where models are mmap'd and Metal contexts are
created — this is also where load failures must be reported clearly to the user (the
plugin must not crash on a missing `.gguf`). Detail belongs to milestones 02–04, but the
interface already gives us the right hook.

### 3.4 Backend selection

Out of scope for this analysis to fully specify, but the rough plan:

- A new `settings::inference_backend()` returning `"openai"` or `"local"` (default
  `"openai"` initially, flipped after milestone 06 acceptance).
- The three `make_*_backend()` factories read that setting and pick the impl.
- CMake gains `XP_WELLY_LLM_ATC_BUILD_OPENAI=ON` and `XP_WELLY_LLM_ATC_BUILD_LOCAL=ON`
  options. Spike builds turn one or the other off freely. Final plugin builds both and
  picks at runtime — unless we decide the cloud path is dead weight by milestone 06.

---

## 4. Files to change vs. files that stay untouched

### 4.1 Stays untouched (state machine + plumbing must not move)

These are the **hard "do not touch" list** for any milestone in this spike:

- `src/atc/atc_state_machine.{hpp,cpp}`  — state transitions and ATC logic
- `src/atc/intent_parser.{hpp,cpp}`      — rule-based intent recognition
- `src/atc/atc_templates.{hpp,cpp}`      — phraseology templates and prompt loader
- `src/atc/atis_generator.{hpp,cpp}`     — ATIS text composition
- `src/atc/flight_phase.{hpp,cpp}`       — flight-phase guards
- `src/atc/engine.cpp` *internals*       — the routing logic itself stays. Only the
  `#include "openai/gpt_client.hpp"` and the two `gpt_client::classify_intent_async` calls
  swap to the abstract `LanguageBackend` (mechanical rename)
- `src/audio/{audio_recorder, ptt_input, mic_permission}.{hpp,cpp,mm}`
- `src/core/{logging, xplane_context, xplane_context_runtime}.{hpp,cpp}`
- `src/data/{airport_vrps, airspace_db}.{hpp,cpp}`
- `src/ui/atc_ui.*`  except for the cosmetic API-key label (one-line tweak, deferred)

The state machine in particular is the value of this codebase — local inference is just a
backend swap underneath it.

### 4.2 Changes required by the refactor (milestone 06; not earlier)

| Change | File(s) | Type |
|---|---|---|
| Introduce abstract backend headers | `src/inference/stt_backend.hpp`, `llm_backend.hpp`, `tts_backend.hpp` (new) | Add |
| Move + adapt cloud impl | `src/openai/{whisper,gpt,tts}_client.{hpp,cpp}` → `src/inference/openai/*` and conform to the abstract interface | Move + thin adapt |
| Add local impls | `src/inference/local/{whisper_cpp,llama_cpp,piper}_backend.{hpp,cpp}` (new) | Add (milestones 02–05 deliver these as standalone CLIs first) |
| Swap consumers to interface | `src/atc/atc_session.cpp` (TTS + STT call sites), `src/atc/engine.cpp` (LLM call sites) | Edit (mechanical: namespace + factory call) |
| Plugin lifecycle | `src/main.cpp` (replace 3× concrete `init/stop/drain` with abstract handles) | Edit |
| Audio format dispatch | `src/audio/audio_player.{hpp,cpp}` (single `play(data, format, volume)` entry) | Add overload, keep existing for compat |
| Settings | `src/persistence/settings.{hpp,cpp}` (add `inference_backend()` getter; keep `get_api_key` as-is, used only by cloud impl) | Add |
| UI label | `src/ui/atc_ui.cpp:572` (label hides when local backend selected) | Trivial |
| CMake | `CMakeLists.txt` (build options, source list, optional CURL) | Edit |
| `.gitignore` | add `Resources/models/` | Edit (this milestone) |

Non-mandatory:

- Drop `gpt_client::ask_async` (dead code, no callers).
- Rename keychain account `openai_api_key` → keep for backwards-compat or migrate; this
  is a settings-migration question, not an architecture one. Punt to milestone 06.

---

## 5. Decisions (resolved 2026-04-30)

1. **[DECISION-1] Interfaces under `src/inference/`** — **resolved: yes**.
   New abstract headers live in `src/inference/{stt,llm,tts}_backend.hpp`. Cloud impl
   moves to `src/inference/openai/`, local impls land in `src/inference/local/`.
2. **[DECISION-2] Naming `XxxBackend`** — **resolved: yes**.
   Concrete classes: `SpeechToTextBackend`, `LanguageBackend`, `TextToSpeechBackend`.
3. **[DECISION-3] Voice IDs stay opaque per-backend strings** — **resolved: yes**.
   Cloud uses OpenAI voice names verbatim; Piper resolves them as
   `Resources/models/<voice>.onnx`. No interface change.
4. **[DECISION-LICENSE]** — **resolved: keep GPL-3.0**.
   All required local-inference dependencies (whisper.cpp, llama.cpp, Piper core, ONNX
   Runtime) are MIT and therefore one-way compatible with GPL-3.0. Milestone 04 links
   `espeak-ng` (GPL-3.0), which is also compatible. Plugin source headers and
   `LICENSE` stay as-is. The original "License: MIT" goal in early milestone planning
   was guidance for *dependencies* (avoid copyleft libs that would force a relicense),
   not for the plugin itself.

---

## 6. Acceptance checklist (this milestone)

- [x] Inventory of cloud call sites — §1
- [x] Current call-flow diagram — §2.1
- [x] Proposed interface surface with method signatures + lifecycle — §3
- [x] Files-to-change vs. files-untouched, state machine on the "stay" list — §4
- [x] Open decisions resolved — §5
- [x] User review + approval — 2026-04-30
