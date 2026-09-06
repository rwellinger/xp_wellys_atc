# Welly's ATC — Technical Documentation

> ← Back to the [product overview](../README.md)

This file gathers the **technical details** of Welly's ATC: installation,
backend modes, building from source, local inference models,
configuration, architecture and development workflow. A concise product
description (what the plugin covers, scope boundaries, training focus and
disclaimer) is in the [product README](../README.md).

AI-powered voice ATC plugin for VFR flights in X-Plane 12.

Talk to ATC over push-to-talk through your microphone. The plugin
transcribes your speech (locally with whisper.cpp, via the OpenAI Whisper
API or via Mistral's Voxtral STT — your choice), interprets your intent
through a rule-based ATC state machine — with a low-confidence fallback to
a local Llama-3.2-3B classifier, OpenAI's `gpt-4o-mini` or Mistral Small —
and plays the ATC responses back, synthesized locally with Piper or over
the OpenAI / Mistral TTS API.

**Measured pipeline latency** (warm, M4, local inference, end-to-end
spike): STT 321 ms · LM 634 ms · TTS 200 ms · **total ≈ 1.16 s per
request** — well below the 3 s acceptance target. The cloud modes (OpenAI /
Mistral) are typically slower: 2–3 s warm, dominated by API latency.

## Table of Contents

- [Features](#features)
- [Hardware requirements](#hardware-requirements)
- [Software requirements](#software-requirements)
- [Quick start](#quick-start-prebuilt-release)
- [Backend modes](#backend-modes)
- [Radio-failure recovery (TTS failure protection)](#radio-failure-recovery-tts-failure-protection)
- [Building from source](#building-from-source)
- [Local inference models](#local-inference-models)
- [Configuration](#configuration)
- [Usage](#usage)
- [Make targets](#make-targets)
- [Domain foundations (BZF / NfL)](#domain-foundations-bzf--nfl)
- [Known limitations](#known-limitations)
- [FAQ](#faq)
- [Project structure](#project-structure)
- [Third-party dependencies](#third-party-dependencies)
- [Development workflow](#development-workflow)
- [License](#license)

## Features

- **Push-to-talk** — via X-Plane command binding (keyboard or joystick)
- **Three-backend inference** — pick **Local** (Apple Silicon or Windows
  x64), **OpenAI Cloud** or **Mistral Cloud** (both on any Mac or Windows
  PC, your own API key) in the settings tab. Switch at runtime, no plugin
  restart. Every inference call is tagged in X-Plane's `Log.txt` with
  `[STT-LOCAL]` / `[STT-OPENAI]` / `[STT-MISTRAL]` (and correspondingly
  for LM/TTS), so you can trace which side served each request.
- **Local speech-to-text** — `whisper.cpp` `small-q5_1` (multilingual,
  German), GPU-accelerated (Metal on Apple Silicon, Vulkan on Windows)
- **Local LLM** — `llama.cpp` with Llama 3.2 3B Instruct (Q4_K_M),
  Metal-accelerated; used for intent disambiguation when the rule-based
  parser is unsure. The repair output is digit-validated to suppress
  hallucinated runways or frequencies.
- **Local text-to-speech** — Piper, German voice
  (`de_DE-thorsten-medium`), CPU + onnxruntime
- **OpenAI cloud option** — `whisper-1` for STT, `gpt-4o-mini` for the
  intent classifier (JSON mode for constrained output), `tts-1` with six
  selectable voices (`alloy/echo/fable/onyx/nova/shimmer`; `onyx` comes
  closest to real ATC). Key in the macOS Keychain via `Security.framework`,
  never in `settings.json`, never logged in full (only the last 4
  characters appear in audit lines).
- **Mistral cloud option** — `voxtral-mini-2507` for STT (with
  `context_bias[]` airport biasing), `mistral-small-latest` for the intent
  classifier (JSON mode), `voxtral-mini-tts-2603` with 30 preset voices of
  British, American and French speakers across 7–9 emotional registers
  (default per role: `gb_oliver_neutral` for ATIS, `en_paul_confident` for
  Tower, `en_paul_neutral` for Ground). Voxtral TTS is multilingual and
  speaks German without a US accent. Separate Keychain entry, so the
  OpenAI and Mistral keys coexist; switching modes never requires
  re-pasting.
- **Two language profiles — DE (NfL/BZF) & EN (ICAO)** — the ATC
  phraseology language is switchable at runtime via `atc_language`
  (default German). The **interface language** is decoupled from it
  (`ui_language`, default English) and applies immediately without a
  restart — so you can run the operating UI in English while the radio
  follows German NfL standard (Issue #56).
- **ATC state machine** — VFR phraseology for controlled and uncontrolled
  airfields
- **Flight-phase detection** — context-aware guards prevent unrealistic
  ATC interactions depending on aircraft state (parked, taxiing, airborne,
  etc.)
- **Live traffic detection (v2.1) + landing sequencing (v2.2)** —
  provider-independent `sim/cockpit2/tcas/targets/...` reader feeding a
  2 Hz `TrafficContext` snapshot. Traffic advisories with voice
  acknowledgement ("in sight" / "negative" / "looking out") on a side
  channel that does not block the main ATC flow. **v2.2 adds VFR landing
  sequencing** — sequence number and "follow the traffic" when other
  traffic is on final or in the pattern, "continue approach, traffic on
  the runway" when the active runway is blocked, and an unprompted,
  tower-instructed go-around within 1 NM of the threshold when the runway
  stays occupied. The master switch `traffic_features_enabled` in the
  settings disables the whole subsystem with one click.
- **ATIS generation** — automatic ATIS broadcasts from the sim's live
  weather data, on COM1 *or* COM2 (active or standby). Aborts the running
  broadcast when the pilot re-tunes the playing COM.
- **Radio-discipline coaching** — ATC politely reminds you when the pilot
  uses inappropriate language, escalating on repetition
- **Phraseology hints** — context-aware cheat sheet with full phraseology
  on hover
- **Cross-country support** — full VFR departure, en-route frequency
  changes and the approach flow between airfields. The approach controller
  proactively hands you off to the tower with the destination frequency.
- **Aircraft registration display** — pilot callsign tied to the actual
  registration read from X-Plane
- **"Disregard" recovery** — flow-aware reset (PATTERN_ENTRY airborne near
  the home field, EN_ROUTE in transit, IDLE on the ground)
- **TTS failure recovery (radio-failure protection)** — when speech
  synthesis or playback fails (OpenAI timeout, network drop, Piper I/O
  error), the plugin does not strand the pilot in a state the tower never
  announced. Before every pilot transmission a snapshot of the ATC state
  machine is taken; on a failure the plugin plays a short squelch burst on
  the active COM and either reverts the state ("say again") or — if an
  auto-correction has run on in the meantime — keeps the unsent clearance
  reachable via `REQUEST_REPEAT` ("say again"). See
  [Radio-failure recovery](#radio-failure-recovery-tts-failure-protection).
- **Radio power detection** — the ATC panel disables itself when the COM
  radio has no power, with an optional bypass for exotic aircraft
- **In-plugin model downloader** — the first launch shows an ImGui dialog,
  HTTPS-resumable downloads from HuggingFace, SHA256-verified before use
- **ImGui UI** — in-sim ATC panel with frequency management, phraseology
  hints, transcript history, a Models tab for download / re-verification
  and an optional Traffic tab (debug) listing the 10 nearest aircraft

## Hardware requirements

On **macOS** the plugin ships as a **Universal Binary** — one `.xpl`, two
slices. X-Plane automatically loads the matching one. For **Windows**
there is a separate build (`win_x64/xp_wellys_vfr_atc.xpl`).

| Platform | Loaded slice / build | Available backends |
|---|---|---|
| macOS · Apple Silicon (M1 / M2 / M3 / M4) | `mac_x64` (arm64) | **Local** (Metal), **OpenAI Cloud** *or* **Mistral Cloud** |
| macOS · Intel (x86_64) | `mac_x64` (x86_64) | **Cloud only** — **OpenAI** or **Mistral** (local inference needs Metal + Apple Silicon) |
| Windows 11 (x64) | `win_x64` | **Local** (Vulkan), **OpenAI Cloud** *or* **Mistral Cloud** |

**Windows status:** Fully supported. The **cloud modes** are verified
end-to-end on real hardware — a complete VFR round trip out of
**Friedrichshafen (EDNY)** on a Shadow cloud PC (Windows 11, NVIDIA GPU):
plugin loading, microphone capture (WASAPI via miniaudio) + PTT, the full
STT→ATC→TTS pipeline and the API key in the Windows Credential Manager all
work flawlessly. **Local mode also runs on Windows**, with whisper + llama
on the **Vulkan** ggml backend and the German Piper `de_DE-thorsten` voice,
at response times comparable to Apple Silicon.

Vulkan rather than CUDA is a deliberate choice: no GPU redistributable has
to ship (`vulkan-1.dll` comes with the graphics driver, and X-Plane 12
renders through Vulkan on Windows anyway), and it covers AMD and Intel GPUs
too. The cost is roughly 10–30 % less throughput than CUDA on the same card.
Where no Vulkan compute device is available — some virtualised cloud-PC
GPUs — ggml falls back to its CPU backend rather than failing.

That statement is about the *GPU* stack only. The bundle is not free of
external runtime dependencies: `piper.dll` and `onnxruntime.dll` are built
against the dynamic CRT and import `MSVCP140.dll` + `VCRUNTIME140.dll` from
the **Microsoft Visual C++ 2015–2022 Redistributable (x64)**. That is a
prerequisite on the target machine — we deliberately do not ship those DLLs
(redistribution terms plus a servicing burden we do not want), and in
practice X-Plane 12 systems have it already.

The failure mode is worth knowing because it mimics a different bug. Since
`piper.dll` is delay-loaded, a missing redistributable does **not** stop the
plugin from loading and does not affect the cloud backends: everything works
until the first `piper_create`, which then fails with `0xC06D007E`
(`MOD_NOT_FOUND`) — the very same code you get when the DLL search path is
wrong. Rule out the redistributable first; it is the cheaper check.

| Resource | Local mode | OpenAI / Mistral cloud mode |
|---|---|---|
| RAM | 32 GB recommended (X-Plane 12 + ~3 GB headroom for the inference stack) | 16 GB (no model in RAM — calls are stateless HTTP requests) |
| Disk | ~2.5 GB free for the models | ~50 MB for the plugin bundle (no models loaded) |
| GPU | Apple Silicon: any Metal-capable GPU on the same chip. Windows: a Vulkan-capable GPU — note the models share VRAM with X-Plane's renderer | not used |
| Network | not used at runtime (one-time model download from HuggingFace) | required — every PTT release triggers HTTPS calls to `api.openai.com` or `api.mistral.ai` |

Both cloud modes cost money per request (STT + LM + TTS APIs). Mistral is
typically cheaper per token than OpenAI (`mistral-small` ≈ 33% cheaper
input / 50% cheaper output than `gpt-4o-mini`). STT and TTS are roughly at
price parity. The latency of both clouds is typically 2–3 s warm vs.
1–1.5 s warm for local inference.

## Software requirements

| Item | Requirement |
|---|---|
| macOS | **13.3 or newer** (onnxruntime 1.22.0 requires this on the arm64 slice; the x86_64 slice inherits the same deployment target so the lipo'd binary stays consistent) |
| Windows | **Windows 11 (x64)**, verified. All three backends; the cloud API key lives in the Windows Credential Manager. Local mode needs a Vulkan-capable GPU for acceleration (falls back to CPU otherwise). The artifact carries `piper.dll` + `onnxruntime.dll` next to the `.xpl` — libcurl and whisper/llama/ggml are linked statically. |
| X-Plane | X-Plane 12 (12.0 or newer) |
| OpenAI / Mistral account | Only if you want to use a cloud mode — needs an API key with billing enabled at the respective provider. The Local mode has no cloud dependency. |
| To build from source | CMake 3.26+, Homebrew LLVM (`brew install llvm`), Xcode Command Line Tools |

## Quick start (prebuilt release)

1. Download `xp_wellys_vfr_atc-vX.Y.Z.zip` from the GitHub releases page.
   The macOS `.xpl` inside is a Universal Binary for arm64 and x86_64; the
   Windows `.xpl` is in the `win_x64/` folder.
2. Unzip into `X-Plane 12/Resources/plugins/`. Result:
   ```
   X-Plane 12/Resources/plugins/xp_wellys_vfr_atc/
     ├── mac_x64/
     │     ├── xp_wellys_vfr_atc.xpl       (universal: arm64 + x86_64)
     │     ├── libpiper.dylib          (used by the arm64 slice only)
     │     ├── libonnxruntime.1.22.0.dylib
     │     └── libonnxruntime.dylib
     ├── win_x64/
     │     ├── xp_wellys_vfr_atc.xpl       (Windows x64)
     │     ├── piper.dll
     │     ├── onnxruntime.dll
     │     └── onnxruntime_providers_shared.dll
     ├── Resources/
     │     └── espeak-ng-data/   (~19 MB, used by local TTS on both platforms)
     └── data/
           └── (ATC profile bundle, prompt templates, VRP database, etc.)
   ```
   On **Windows**, X-Plane 12 loads the `win_x64/` folder. The folder name
   and the file name `xp_wellys_vfr_atc.xpl` must stay exactly like this —
   a generically named `win.xpl` is **silently not** loaded by X-Plane 12
   on Windows.
3. Start X-Plane. Open the plugin window via *Plugins → Welly's ATC*.
4. **Pick your backend** in the **Settings** tab:
   - **Local** (Apple Silicon and Windows x64, default): the **Models** tab
     shows the rows in red. Click **Download all missing** — the plugin
     downloads ~2.0 GB from HuggingFace over HTTPS. Resumable; cancelable;
     SHA256-verified after each file. Once all rows show **Ready** (green),
     the PTT-disabled banner in the Status tab disappears.
   - **OpenAI Cloud** (any Mac **and Windows**): paste your OpenAI API key
     into the **OpenAI API Key** field in the settings (use the `[Paste]`
     button — Cmd+V is unreliable in X-Plane's ImGui context). Click
     **Save Key**. The key is stored on macOS in the Keychain under the
     service `com.xp_wellys_devfr_atc.openai`, on Windows in the
     **Credential Manager**. PTT is active immediately; no model download.
   - **Mistral Cloud** (any Mac **and Windows**): paste your Mistral API
     key into the **Mistral API key** field (same `[Paste]` pattern).
     Click **Save Key##mistral**. The key is stored under a separate entry
     `com.xp_wellys_devfr_atc.mistral` (macOS Keychain or Windows
     Credential Manager), so the OpenAI key (if present) stays untouched
     and you can switch providers without re-pasting. PTT is active
     immediately.
   - **Windows Local mode** is available **from v0.8**. Earlier Windows
     builds were cloud-only and silently rewrote `backend_mode: local` to
     `openai` at startup.
5. Fly. The banner in the Status tab shows the active mode, and `Log.txt`
   carries a single-line `BACKEND MODE: ...` banner on every load, so you
   can prove after the fact which side served the session.

## Backend modes

You can switch at any time in the Settings tab — the plugin shuts down the
active inference stack and brings up another, without an X-Plane restart.
Source-level invariant: each backend family lives in its own set of
`.cpp` files, and the three families share neither header nor code path.
The local backends (`whisper_stt.cpp`, `llama_lm.cpp`, `piper_tts.cpp`)
contain no `#include` of a cloud client and zero `curl_easy_perform`
calls; the OpenAI clients (`openai_stt.cpp`, `openai_lm.cpp`,
`openai_tts.cpp`) contain no `#include` of `whisper.h` / `llama.h` /
`piper.h` and no Mistral endpoints; the Mistral clients
(`mistral_stt.cpp`, `mistral_lm.cpp`, `mistral_tts.cpp`) carry neither
local headers nor `api.openai.com`. So in one mode no code path can call
into the other two — verified at compile and grep time by
`tests/test_audit_logging.cpp`.

To trace which mode served a request: grep `Log.txt`.

| Tag in `Log.txt` | Meaning |
|---|---|
| `[xp_wellys_vfr_atc] BACKEND MODE: LOCAL ...` | The loader brought up the local pipeline. |
| `[xp_wellys_vfr_atc] BACKEND MODE: OPENAI (api.openai.com) ...` | The loader brought up the OpenAI cloud pipeline. |
| `[xp_wellys_vfr_atc] BACKEND MODE: MISTRAL (api.mistral.ai) ...` | The loader brought up the Mistral cloud pipeline. |
| `[STT-LOCAL] / [LM-LOCAL] / [TTS-LOCAL]` | Per-call audit for each local inference. |
| `[STT-OPENAI] / [LM-OPENAI] / [TTS-OPENAI]` | Per-call audit for each OpenAI cloud inference. The API key is truncated to its last 4 characters (`sk-...ABCD`). |
| `[STT-MISTRAL] / [LM-MISTRAL] / [TTS-MISTRAL]` | Per-call audit for each Mistral cloud inference. The API key is truncated to its last 4 characters (`...ABCD`; no `sk-` prefix — Mistral keys are not OpenAI-formatted). |

## Radio-failure recovery (TTS failure protection)

The pilot-driven TTS path is wrapped in a snapshot/revert guard, so that a
synthesis or playback failure (OpenAI `curl error: Timeout`, transient
5xx, dropped Wi-Fi, local Piper I/O error, audio-bus glitch) cannot carry
the ATC state machine beyond what the pilot actually heard. The mechanism
is uniform across all backend modes — the same code path handles Local and
Cloud.

How it works:

- Before every pilot transmission goes into `atc_state_machine::process()`,
  the plugin captures an opaque snapshot of the full machine state
  (current state, transition history, runway lock, readback flag,
  departure type, last clearance text, last tower utterance). A monotonic
  generation counter is incremented on every semantic mutation — per-frame
  heartbeats (timestamps, auto-correction timers) are not counted, so they
  cannot invalidate the snapshot.
- On TTS success: nothing further happens. The state runs on as before,
  the pilot hears the response, the snapshot is discarded.
- On TTS failure: a short squelch burst (~350 ms of pink noise plus a
  click) is played on the active COM. The burst is generated in-process
  from a deterministically seeded PRNG — it cannot fail the same way the
  TTS call just did, and it works in VR or under the IFR hood when the
  panel is not visible. Then one of two branches runs:
  - **Restore branch** — no one else has changed the state machine in the
    meantime. The pre-transmission snapshot is restored, the transcript
    panel shows a muted-amber system entry
    `-- Funkstoerung — bitte den Funkspruch wiederholen --`, and the pilot
    can cleanly re-send the same call.
  - **Stale branch** — a later auto-correction (or another callback) has
    already moved the generation counter beyond the value expected by the
    snapshot. A rollback would silently undo that legitimate transition,
    so the rollback is refused. The clearance text the pilot never heard
    stays parked in `last_tower_response_text_`; a system entry
    `-- Funkstoerung — sagen Sie 'Wiederholen Sie' fuer die verpasste
    Anweisung --` steers the pilot to the `REQUEST_REPEAT` path, which
    repeats the missed clearance verbatim. After the repeat the pilot
    reads back normally and the state machine re-synchronizes.

ATIS broadcasts, traffic advisories and the unprompted go-around prompt
use the unguarded TTS path — they are stateless render-only events. If a
tick fails, the next tick simply retries.

Implementation:

- `src/atc/atc_state_machine.{hpp,cpp}` — `AtcStateSnapshot`,
  `capture_snapshot()`, `current_gen()`, `restore_snapshot_if_gen()`,
  generation-counter discipline (a banner comment in the cpp file lays out
  which fields bump gen and which are heartbeat only).
- `src/atc/atc_session.cpp` — `speak_response_guarded()` wraps the
  `engine::process_transcript` callback for state-changing tower
  responses.
- `src/audio/audio_player.{hpp,cpp}` — `play_squelch_burst(com)`, no WAV
  asset, no network.
- `tests/test_state_revert_guard.cpp` — four behavioral cases:
  snapshot+restore round trip, generation monotonicity, stale-branch
  refusal, `REQUEST_REPEAT`-after-stale recovery.

## Building from source

```sh
git clone <repo-url>
cd xp_wellys_vfr_atc
make setup     # Downloads the prebuilt xp_wellys_libs bundle (arm64 local-inference
               # libs, SHA256-verified) + X-Plane SDK, Dear ImGui,
               # nlohmann/json, Catch2. No more submodules.
make build     # Universal release build → build/xp_wellys_vfr_atc.xpl (arm64
               # with all three backends + x86_64 cloud-only, lipo'd into one
               # .xpl). This is the only build target.
make install   # Code signing + installation into the X-Plane plugins directory
```

`make build` runs CMake twice (arm64 with
`XPWELLYS_USE_LOCAL_INFERENCE=ON` in `build-arm64/`, x86_64 with the same
flag `OFF` in `build-x86_64/`) and `lipo`-merges the two `.xpl`s into one
Universal Binary. The build time is roughly twice that of a single-arch
build; that is the deliberate trade-off so dev and release artifacts are
byte-for-byte identical in their form. For tag-driven release builds you
pass `RELEASE_FLAG=-DRELEASE=ON` (`make release-build` does that for you —
embedding the version from `VERSION.txt`).

The heavy local inference libraries (whisper.cpp, llama.cpp, ggml with
Metal, Piper, espeak-ng, onnxruntime) are **no longer built from source**.
They come as a prebuilt arm64 bundle from the separate repo
[`xp_wellys_libs`](https://github.com/rwellinger/xp_wellys_libs), which
`make setup` downloads from its GitHub release — version pinned in
`PREBUILT_LIBS_VERSION`, SHA256-verified against the bundle's
`manifest.txt` — and unpacks into `vendor/prebuilt/xp_wellys_libs/`
(including the onnxruntime dylib + espeak-ng-data). This eliminates the
~50-min cold compile: the release build now only compiles its own ~40 TUs
(~5–8 min, deterministic). The x86_64 slice has no onnxruntime / Piper /
whisper / llama dependency whatsoever; it links only against libcurl + the
system frameworks (Security, AudioToolbox, etc.) and the cloud clients.

**Windows build.** The `win_x64/xp_wellys_vfr_atc.xpl` is built with **MSVC
via CMake on `windows-latest` in CI** — there is no local Windows build path.
Since v0.8 it carries the **full local stack**
(`XPWELLYS_USE_LOCAL_INFERENCE=ON`): whisper + llama + ggml as static archives
with the **Vulkan** GPU backend, plus Piper for TTS. OpenAI and Mistral come
over libcurl (static from vcpkg, `x64-windows-static`, Schannel TLS).

Platform specifics: mic capture uses **miniaudio** (WASAPI) instead of Core
Audio, and the API key lives in the Windows Credential Manager instead of the
Keychain. The artifact ships `piper.dll` + `onnxruntime.dll`
(+ `onnxruntime_providers_shared.dll`) next to the `.xpl` — Vulkan itself adds
no DLL, since `vulkan-1.dll` comes from the graphics driver.

Two Windows-only load-time details, both load-bearing — see the comments in
`CMakeLists.txt` and `loader.cpp` before touching either:

- `piper.dll` and `vulkan-1.dll` are **delay-loaded**, so users who never
  reach local TTS or local inference do not map them. Making `piper.dll` a
  load-time import instead was tried and is worse: X-Plane then refuses the
  plugin outright and cloud mode is lost with it.
- Because the delay-load helper resolves DLLs against the **process** search
  path — which excludes the plugin's `win_x64/` — `loader.cpp` preloads
  `piper.dll` by absolute path (`LoadLibraryExW` +
  `LOAD_WITH_ALTERED_SEARCH_PATH`), derived from the module's own location.

The bundle's `piper.dll` also carries a patch that constructs onnxruntime's
`Ort::Env` lazily; upstream declares it at namespace scope, where its
constructor runs under the Windows loader lock and crashes. See
`patches/piper-lazy-ort-env.patch` in `xp_wellys_libs` (≥ 0.4.2).

### Updating the prebuilt libs (`xp_wellys_libs`)

The local inference libraries live in the separate repo
[`xp_wellys_libs`](https://github.com/rwellinger/xp_wellys_libs) and are
compiled there **once per upstream pin bump** and released as a versioned
bundle. Here is how you bring a new version into the plugin:

**1. In `xp_wellys_libs` — release a new bundle**

```sh
# optional: set newer whisper.cpp/llama.cpp/piper1-gpl pins
#   cd third_party/<repo> && git checkout <sha> && cd -
echo "0.2.0" > VERSION.txt                  # MUST match the tag
git commit -am "bump to 0.2.0 (+ pins)"
git push                                     # CI builds + smoke-tests (pre-flight safety)
git tag v0.2.0 && git push origin v0.2.0     # CI publishes the bundle tarball
```

**2. In the plugin — pull the new version**

```sh
echo "0.2.0" > PREBUILT_LIBS_VERSION
rm -rf vendor/prebuilt/xp_wellys_libs        # remove the old bundle (see below)
make setup                                   # downloads + SHA256-verifies 0.2.0
make build && make test                      # check against the new libs
# afterwards: commit PREBUILT_LIBS_VERSION, PR, merge
```

**Two rules you must follow:**

- **Version identical in three places:** `VERSION.txt` (libs) = tag
  `vX.Y.Z` = `PREBUILT_LIBS_VERSION` (plugin). The tarball name is built
  from `VERSION.txt`, the plugin download expects exactly
  `xp_wellys_libs-arm64-macos-<PREBUILT_LIBS_VERSION>.tar.gz` — if they do
  not line up, `make setup` fails with a 404.
- **`rm -rf vendor/prebuilt/xp_wellys_libs` before `make setup`:** the
  Makefile sentinel (`.../lib/libwhisper.a`) otherwise skips the download
  as long as the old bundle is still there — you would silently link the
  old version. In CI this is irrelevant (every runner starts fresh), only
  locally.

Before the plugin release tag you can check the full release build as a
dry run (builds both slices + uploads artifacts, but publishes nothing):

```sh
gh workflow run build.yml --ref main
```

## Local inference models

The plugin ships **without** the model files (~2.0 GB together). They live
under `<plugin>/Resources/models/` and are downloaded on the first launch
via the **Models** tab. Every download is HTTPS, resumable (`Range`
header), streamed directly onto the installation volume (no temp detour
via the system disk — important for users who run X-Plane on an external
SSD) and SHA256-verified before it is renamed from `<file>.part` to the
final file name.

### Manual fallback (restrictive networks)

If the plugin downloader cannot reach HuggingFace (corporate proxy,
captive portal, etc.), download these files manually and place them in
`<plugin>/Resources/models/`. The plugin re-verifies on the next launch
and picks them up automatically if the hashes match.

| Model | Language | Size | SHA256 | URL |
|---|---|---:|---|---|
| `ggml-small-q5_1.bin` | de (multilingual) | 181 MB | `ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb` | [`huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin`](https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin) |
| `Llama-3.2-3B-Instruct-Q4_K_M.gguf` | — | 1.88 GB | `6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff` | [`huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf`](https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf) |
| `de_DE-thorsten-medium.onnx` | de | 60 MB | `7e64762d8e5118bb578f2eea6207e1a35a8e0c30595010b666f983fc87bb7819` | [`huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx`](https://huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx) |
| `de_DE-thorsten-medium.onnx.json` | de | 4.7 KB | `974adee790533adb273a1ac88f49027d2a1b8f0f2cf4905954a4791e79264e85` | [`huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx.json`](https://huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx.json) |

The Whisper model is the multilingual variant (`ggml-small-q5_1.bin`),
since the DE profile needs German transcription. Llama is multilingual and
is shared. The Piper voice is the German `de_DE-thorsten-medium`.

After you place the files, reopen the plugin window — the Models tab runs
the SHA256 verification in the background and flips the rows to **Ready**
as soon as each hash matches.

### SHA256 verification procedure (DE models)

The DE hashes above were captured on 2026-06-04 against HuggingFace
`main`. To re-verify (or re-pin after an upstream update):

```bash
# Whisper small multilingual (~184 MB)
curl -L -o /tmp/ggml-small-q5_1.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin
shasum -a 256 /tmp/ggml-small-q5_1.bin
stat -f%z /tmp/ggml-small-q5_1.bin

# Piper de_DE-thorsten-medium (.onnx ~63 MB, .onnx.json ~5 KB)
curl -L -o /tmp/de_DE-thorsten-medium.onnx \
  https://huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx
curl -L -o /tmp/de_DE-thorsten-medium.onnx.json \
  https://huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx.json
shasum -a 256 /tmp/de_DE-thorsten-medium.onnx /tmp/de_DE-thorsten-medium.onnx.json
stat -f%z /tmp/de_DE-thorsten-medium.onnx /tmp/de_DE-thorsten-medium.onnx.json
```

Enter the hashes + sizes in:
- `src/persistence/model_manifest.cpp` `voice_catalog()` (Thorsten row:
  two hashes + two sizes)
- `src/persistence/model_manifest.cpp` `manifest()` (multilingual Whisper:
  one hash + one size)
- The table above

### Expected download time on first launch

5–30 minutes on typical home internet; the bottleneck is HuggingFace's
download throughput, not the plugin. The downloader resumes over
HTTP `Range` if the connection drops, so a Wi-Fi hiccup in the middle of
the Llama download does not restart the 1.88 GB pull from scratch.

## Configuration

The settings live in `<plugin>/data/settings.json`. The OpenAI and Mistral
API keys are the only secrets — both live in the macOS Keychain under
separate service entries (`com.xp_wellys_devfr_atc.openai`,
`com.xp_wellys_devfr_atc.mistral`), never in this file.

> **Note:** The Keychain service names, the plugin signature
> (`ch.thWelly.wellys_devfr_atc`), the PTT commands
> (`xp_wellys_devfr_atc/ptt` etc.) and the user directories under
> `Output/preferences/xp_wellys_devfr_atc` / `Output/xp_wellys_devfr_atc`
> still carry the old `devfr` name. This is intentional: it lets existing
> installations keep their stored API keys, key bindings and local
> overrides after the project was renamed to `xp_wellys_vfr_atc`.

The **ATC phraseology language** is selectable via `atc_language`
(`de` | `en`) in the Settings tab (default `de`); from it `atc_profile`
(`DE` = NfL DACH-VFR / `EN` = ICAO-VFR) and the backend language are
derived. The strict/conformance mode applies only in the DE profile.
**Decoupled** from it is the **interface language** `ui_language`
(`de` | `en`, default `en`): it drives only the menu, button and label
strings (`ui_strings.json`), not the spoken phraseology. So you can run
the operating UI in English, for example, while the radio stays on German
NfL standard. The interface language applies immediately without a
restart; the phraseology language from the next plugin start (Issue #56).

| Setting | Default | Description |
|---|---|---|
| `atc_language` | `de` | ATC phraseology language (`de` = NfL DACH-VFR / `en` = ICAO-VFR). Derived from it: `atc_profile` (`DE`/`EN`) and the backend language. Selectable in the Settings tab; applies from the next plugin start (profile and local TTS voice are loaded at init). The strict/conformance mode applies only in the DE profile. |
| `ui_language` | `en` | Interface language (`de` | `en`) for menus, buttons and labels (`ui_strings.json`) — **decoupled** from `atc_language` (Issue #56). A change in the Settings tab applies immediately without a restart. Existing configs without this key inherit their `atc_language` on migration, so the interface looks unchanged. |
| `pilot_callsign` | *(empty)* | Phonetic callsign (set in the plugin settings) |
| `active_com` | `1` | Active COM radio (1 or 2) |
| `volume` | `1.0` | Playback volume (0.0–1.0) |
| `pattern_direction` | `left` | Default pattern direction (left/right) — overridden per airfield/runway by `airport_vrps.json` |
| `disable_default_atc` | `false` | Suppresses X-Plane's built-in default ATC |
| `skip_radio_power_check` | `false` | Bypasses radio power detection (workaround for exotic aircraft) |
| `show_phraseology_hints` | `true` | Shows the phraseology cheat sheet in the ATC panel |
| `auto_correction_factor` | `1.0` | ATC recovery-time multiplier (0.5 = faster, 2.0 = slower) |
| `start_mode` | `engines_running` | Start state assumed by the state machine. `engines_running` (default) places the pilot with warm engines on the apron → the first call is Ground for taxi; `cold_and_dark` allows a clearance-delivery / engine-start sequence before taxi. |
| `bzf_strict_mode` | `false` | When `true`, the tower checks every READBACK against the NfL §25 b) Nr. 1 mandatory list (runway, QNH, frequency, squawk, callsign) and answers with a corrective hint if something is missing — the state does **not** run on until the readback is clean. The toggle in the Settings tab is always visible. See [DE profile & BZF phraseology](#de-profile--bzf-phraseology). |
| `debug_logging` | `false` | Enables verbose debug output |
| `debug_traffic` | `false` | Shows the Traffic tab in the ATC panel (lists the 10 nearest aircraft from the TCAS DataRefs) |
| `debug_text_input` | `false` | Shows an InputText field below the transcript in the Status tab. Typed text is fed directly into `engine::process_transcript` via `atc_session::submit_text()` — STT is skipped, LM + state machine + TTS run as in the voice path. Helpful without a headset and to isolate ATC-logic bugs from STT errors. PTT stays active in parallel; the shorthand `REG` expands to the phonetic callsign. |
| `traffic_features_enabled` | `true` | Master switch for the traffic subsystem (advisories, landing sequencing, go-around trigger). Off → `traffic_context::update()` returns an empty snapshot and every downstream consumer becomes a no-op. Needs a traffic provider anyway (LiveTraffic, xPilot, swift, X-IvAp, native AI). |
| `backend_mode` | `local` | `local` (whisper + llama + Piper — arm64-macOS and win-x64), `openai` (Whisper API + Chat Completions + TTS API) or `mistral` (Voxtral STT + Mistral Chat Completions + Voxtral TTS). The **x86_64-macOS slice** silently rewrites `local` to `openai` at startup, since it has no local backends compiled in; the other two modes are honored everywhere. |
| `api_key_saved` | `false` | Flag only — set automatically when the user clicks **Save Key** in the settings. The actual OpenAI key lives in the macOS Keychain under service `com.xp_wellys_devfr_atc.openai` / account `default`. Cleared by **Delete Key**. |
| `openai_stt_model` | `whisper-1` | OpenAI Whisper model ID for the STT call. |
| `openai_lm_model` | `gpt-4o-mini` | OpenAI Chat Completions model ID for the intent classifier. JSON mode is enabled automatically. |
| `openai_tts_model` | `tts-1` | OpenAI TTS model ID. Set `tts-1-hd` for higher (slower) quality. |
| `openai_tts_voice_atis` / `openai_tts_voice_tower` / `openai_tts_voice_ground` | `onyx` / `echo` / `alloy` | OpenAI voice per role. One of `alloy / echo / fable / onyx / nova / shimmer`. `onyx` comes closest to real ATC. |
| `mistral_api_key_saved` | `false` | Flag only — set when **Save Key##mistral** is clicked. The actual Mistral key lives in the macOS Keychain under service `com.xp_wellys_devfr_atc.mistral` / account `default`, separate from the OpenAI entry. |
| `mistral_stt_model` | `voxtral-mini-2507` | Voxtral STT model ID. |
| `mistral_lm_model` | `mistral-small-latest` | Mistral Chat Completions model ID for the intent classifier. JSON mode automatic. `ministral-3b-latest` / `ministral-8b-latest` also work and are cheaper. |
| `mistral_tts_model` | `voxtral-mini-tts-2603` | Voxtral TTS model ID. |
| `mistral_tts_voice_atis` / `mistral_tts_voice_tower` / `mistral_tts_voice_ground` | `gb_oliver_neutral` / `en_paul_confident` / `en_paul_neutral` | Voxtral preset voice per role. The UI dropdown lists 30 voices of British (`gb_oliver_*`, `gb_jane_*`), American (`en_paul_*`) and French (`fr_marie_*`) speakers across 7–9 emotional registers. Voxtral TTS is multilingual and speaks German without a US accent. Your own voice clones from the Mistral dashboard can be set by editing this field directly in `settings.json`. |

The ATC response templates live in
`data/atc_profiles/de/atc_templates.json`. Flight-phase thresholds, ATC
precondition guards, frequency guards and auto-correction rules are in
`data/atc_profiles/de/flight_rules.json`. All data files can be edited
without rebuilding the plugin.

### Airport database (`data/vrps/airport_vrps.json`)

Per-airfield configuration for visual reporting points (VRPs) and pattern
directions. A single global file — VRPs are geographic facts from the AIP,
not phraseology. Pre-populated for common Swiss and German VFR fields;
other fields only provide `pattern_direction` (`vrps: []`) until they are
checked against an authoritative source. Each top-level key is an ICAO
code with optional fields:

- `name` — display name
- `pattern_direction` — per runway `"left"` / `"right"` (overrides the
  global `pattern_direction` setting); accepts a string for an
  unconditional default or an object keyed by runway designator with an
  optional `_default`
- `vrps` — array of `{ name, lat, lon, alt_ft }`; `name` is the phonetic
  spelling (e.g. `"November"`) so Whisper and Piper process it cleanly
- `arrival_routes` — per runway, ordered list of VRP names for the
  approach guidance
- `_source` / `_comment` — optional audit annotations; ignored by the
  loader

#### Optional user override (Navigraph Charts workflow)

With a **Navigraph Charts** subscription you can supply your own VRP
coordinates without forking the plugin:

1. Place a JSON file at
   `<X-Plane>/Output/preferences/xp_wellys_devfr_atc/airport_vrps.json`.
   The directory is created on the first plugin start. This path survives
   plugin re-installs.
2. Use the same schema as the bundled file. Per-ICAO entries replace the
   plugin defaults entirely — there is no field merging, so take the
   complete entry for each airfield you want to override.
3. Restart X-Plane (or `Reload Settings` from the menu) — a log banner in
   `Log.txt` confirms the load:
   `Airport VRPs loaded: N airports (X plugin, Y user overrides: Z replaced, W added) from <path>`

Navigraph Charts workflow per airfield:
- Open the **VFR approach chart** (German charts: AD 2 EDxx, section
  *Visual Approach* or *VFR-Anflug*).
- Read the VRP code (W/N/E/S/Z…), translate it into the phonetic name
  (`W` → `Whiskey`, `N` → `November`, …) — that is what Whisper
  transcribes and Piper pronounces.
- Hover the chart for the cursor lat/lon (Navigraph Charts shows the
  pointer coordinates in the toolbar).
- Read the published transit altitude from the chart legend.
- Note the pattern direction per runway from the AIP AD 2.22 (Flight
  Procedures).

The Navigraph **FMS Data** add-on for X-Plane Custom Data does *not*
contain VRPs (ARINC-424 is IFR-only). You need the Navigraph **Charts**
product.

### ATC response templates (`data/atc_profiles/de/atc_templates.json`)

Defines the ATC response text for every combination of airfield type, ATC
state and pilot intent. Sections `towered` (full ATC flow) and
`uncontrolled` (CTAF/UNICOM self-announcement); each entry has `response`,
`next_state`, `requires_readback`. The special key `_INVALID` is the
fallback. Variables are substituted at runtime from the `XPlaneContext`.

### Flight rules (`data/atc_profiles/de/flight_rules.json`)

Sections for phase-detection thresholds + hysteresis, intent
preconditions, auto-correction rules (state and frequency),
intent-to-frequency mapping, pilot phraseology, state-machine guards
(`state_frequency_validity`, `idle_redirects`, `state_reverts`,
`tower_only_auto_advance`) and frequency hint texts.

### LLM prompt templates (`data/atc_prompt_templates.json`)

Prompts the engine sends to the language model:

| Key | Purpose |
|---|---|
| `whisper_prompt` | Initial prompt hint for whisper.cpp, to bias the transcription toward aviation vocabulary and the NATO alphabet |
| `gpt_classify_prompt_de` | System prompt for low-confidence intent classification (variables: `{state}`, `{valid_intents}`, `{transcript}`, `{frequency_type}`, `{on_ground}`, `{altitude_ft}`, `{groundspeed_kts}`, `{airport}`) |
| `gpt_fallback_prompt_de` | Reserve prompt for emergency response generation |

The key name keeps the `gpt_*` prefix for compatibility reasons; the local
pipeline feeds this prompt unchanged to Llama 3.2.

**Push-to-talk** is configured via X-Plane's keyboard or joystick
settings. The plugin registers the command `xp_wellys_devfr_atc/ptt`,
which can be bound to any key or joystick button.

## Usage

1. Tune COM1/COM2 in X-Plane to the appropriate frequency (or click a
   frequency in the ATC panel to set it as standby, then flip-flop).
2. Hold the PTT key and speak your call — the **phraseology hints** panel
   shows you what to say (hover for the full phraseology).
3. Release PTT — the plugin transcribes, processes through the state
   machine and plays the ATC response back.
4. Check the ImGui overlay for transcript history and current ATC state.
5. If you get stuck in a loop, click **Disregard** to reset.

**No headset?** Turn on `debug_text_input` in the settings — an InputText
field appears below the transcript in the Status tab. Typed text goes
directly into the engine (STT is skipped), but LM, state machine and TTS
keep running, so the tower response is spoken normally over the active
backend. The shorthand `REG` expands to your phonetic callsign.

## Make targets

```sh
make all           # clean + format + build + lint + test (full local CI)
make build         # universal: arm64 (local + both clouds) + x86_64 (clouds only), lipo'd
make release-build # like `make build`, but with -DRELEASE=ON (embeds VERSION.txt)
make test          # unit tests + scenario tests
make install       # code signing + installation into X-Plane
make repl          # builds the headless atc_repl tool
make format        # clang-format
make lint          # clang-tidy (some rules promoted to errors)
make clean         # removes build/, build-arm64/, build-x86_64/, build-lint/, build-sanitize/
make distclean     # additionally removes sdk/, vendor/
```

## Domain foundations (BZF / NfL)

The DE/BZF profile is **fully grounded in official primary sources**. All
authoritative regulations and exam materials are stored as plaintext in
the [`docs/bzf/`](bzf/README.md) directory — they are the single source of
truth for every wording and phraseology rule in the plugin. A detailed
description of each source is in [`docs/bzf/README.md`](bzf/README.md).

| Source | Role | Status |
|---|---|---|
| [`dfs_nfl_sprechfunk_2024.txt`](bzf/dfs_nfl_sprechfunk_2024.txt) — **DFS Bekanntmachung ueber die Sprechfunkverfahren** (NfL 2024-1-3266, BAF) | **Primary source & wording anchor** — entire VFR phraseology, procedural flows, mandatory contents (initial call, readback §25) | valid from 28.11.2024 |
| [`bnetza_pruefungsfragen_2024.txt`](bzf/bnetza_pruefungsfragen_2024.txt) — **BNetzA Pruefungsfragen "Kenntnisse"** BZF I / BZF II | exam/test relevance; basis for distinguishing NfL requirement vs. BZF didactics in strict mode | valid from 01.05.2024 |
| [`nfl_funk_teilb_2010.txt`](bzf/nfl_funk_teilb_2010.txt) — **NfL Teil B (Phrasen)**, Anlagen 7 & 8 (NfL I 226/10) | bilingual standard phrases (DE/EN) as a phrase anchor | in force since 15.10.2010 |
| [`bzf_coverage.md`](bzf/bzf_coverage.md) — **BZF coverage matrix** | maps the BZF II mandatory elements against the state of the `de` profile (✓/◐/✗, prio K/M/N, bucket A/B/C) | continuously maintained |

Bindingness: Every change to phraseology, intent rules, templates or the
conformance checks
(`src/atc/{bzf_compliance,initial_call_conformance,de_phraseology}.*`) must
be substantiated against `dfs_nfl_sprechfunk_2024.txt` (secondary: BNetzA
exam questions and NfL Teil B).

**English (ICAO-VFR):** The planned English profile (Epic #35) has its own
source foundation under [`docs/icao/`](icao/README.md) — ICAO Doc 4444
Ch. 12, Annex 10 Vol II §5.2 and EASA SERA (CAP 413 illustrative only). The
[`icao_coverage.md`](icao/icao_coverage.md) maps every intent onto the
English standard phrase. English VFR radio is **not a translation** of the
NfL, but a self-contained phraseology. The `en` profile is not yet
shipped; see [`docs/icao/README.md`](icao/README.md).

## Known limitations

### DE profile & BZF phraseology

The DE profile follows the NfL Sprechfunk 2024 (DACH-VFR phraseology). No
official certification, no exam substitute — corrections from BZF holders
expressly welcome. The authoritative primary sources (DFS NfL 2024, BNetzA
exam questions, NfL Teil B) and the coverage matrix are under
[`docs/bzf/`](bzf/README.md) — see [Domain foundations](#domain-foundations-bzf--nfl).

**State of implementation** (BZF coverage matrix re-anchor 2026-06-05):

- **Wording corrections (Bucket B)** — five NfL patches in the `de`
  profile: radio-check reply "Hoere Sie fuenf.", touch-and-go templates
  "frei zum Aufsetzen und Durchstarten" (3×), pilot keyword "aufsetzen und
  durchstarten", frequency-change approval "Verlassen der Frequenz
  genehmigt" (2×), fallback "wiederholen Sie" instead of "sagen Sie
  nochmals" (3×, NfL §18 c) Nr. 4).
- **Callsign pronunciation verified (Bucket C)** —
  `de_phraseology::expand_callsign_phonetic()` expands D-/HB-/N-prefix
  digit by digit (e.g. `N123AB` → "November eins zwo drei Alfa Bravo"),
  covered by 7 Catch2 tests in `tests/test_de_phraseology.cpp`.
- **Strict-mode MVP (Bucket A)** — settings toggle `bzf_strict_mode`
  (default off), SDK-free `src/atc/bzf_compliance.{hpp,cpp}`,
  `apply_bzf_strict_check()` hook in the state machine with
  `last_clearance_text_` tracking. Triggers on the READBACK intent against
  the NfL §25 b) Nr. 1 mandatory list (runway, QNH, frequency, squawk,
  callsign) — 18 Catch2 tests in `tests/test_bzf_compliance.cpp`.

| Limitation | Impact | Effort |
|---|---|---|
| **No IFR — by design** | This plugin models VFR radio only: no IFR clearances, no flight-plan filing, no FMS/routing, no SID/STAR. There is genuinely no IFR flow in the code (the word "ifr" appears only as a TTS acronym and as a recognition token in the intention keyword lists — recognition surface, not a feature). | Not planned here. IFR is a separate product with its own plugin: **[Welly's IFR ATC](https://github.com/rwellinger/xp_welly_llm_atc)** — clearance delivery, SID/STAR from X-Plane's CIFP data, SimBrief route integration, en-route sector handoffs, approach and landing. English/ICAO only (no NfL/BZF profile) and needs extra data installed (SimBrief OFP, OpenAir airspace file). The two plugins install side by side. |
| **No local inference on Intel Macs** | The x86_64 macOS slice runs the plugin in OpenAI or Mistral cloud mode only (API key + billing needed) — no local offline mode. Apple Silicon (Metal) and Windows x64 (Vulkan) both have it. | Unlikely to change: GitHub retired the Intel `macos-13` runners, so the prebuilt bundle for that slice cannot be produced in CI at all. Intel Macs are a shrinking niche and lose nothing but the offline option. |
| **German & English, no FR/IT** | VFR phraseology comes as a German (NfL/BZF, default) and an English (ICAO) profile, switchable via `atc_language`. The interface language is independently selectable (`ui_language`). Further languages (French/Italian for western Switzerland or Ticino) are not planned | By design — the focus stays on DACH VFR |
| **OpenAI voices speak German with a US accent** | In `backend_mode=openai` Whisper transcribes correctly and the LM answers correctly in German, but the `tts-1` voices (`alloy`, `echo`, `fable`, `onyx`, `nova`, `shimmer`) are English-trained and render German with an audible US accent — NATO letters in particular sound anglophone (e.g. "Tschaar-lie" instead of "Tschar-li"). Acceptable for casual practice, unrealistic for BZF/AZF training. | Solved for Local mode by Piper `de_DE-thorsten`. For cloud users, **Mistral Cloud** is the alternative — Voxtral TTS is natively multilingual and speaks German without a US accent. |
| **Local STT mishears spelled callsigns** | In Local mode the small `ggml-small-q5_1` Whisper often mishears German NATO spelling sequences as real words ("Whiskey Romeo Oscar" → "Wisskrieg"/"Wiesbaecki"), which breaks intent recognition. The phraseology itself is recognized well — only the callsign suffers. Consequence: **BZF strict mode** can wrongly reject correct readbacks with Local (and Mistral). The `initial_prompt` is already pre-conditioned with the own callsign (`atc_session.cpp`), which raises the hit rate but does not fully lift the model limit. | Local mode: leave strict mode off (the Settings tab warns when strict is active). A larger Whisper (`large-v3-turbo`, ~547 MB) was tested and rejected — too slow, sim stalls on approach. **OpenAI** is much more robust on spelled callsigns and the recommendation if strict mode is wanted. |
| **Single-voice TTS** | All ATC speakers (Tower, Ground, ATIS) use the same Piper voice in Local mode; ATIS speaks more slowly via `length_scale=1.18` | Low — could ship more voices and add a per-frequency selector |
| **"via Alpha" hardcoded** — the taxiway name is always Alpha | Unrealistic at airfields with a different taxiway layout | High — would need taxiway data from apt.dat or WED |
| **No wake-turbulence separation** — the sequencing in v2.2 only picks by distance, no light/medium/heavy split | Acceptable for GA patterns; missing for mixed weight classes | Phase 5 on the roadmap |
| **No callsign validation** | ATC accepts any callsign | Low priority in single-player |
| **Large hub airports (LSZH, LSGG, …) not officially supported** — the pilot can depart/arrive, but the delivery workflow (slot/VFR clearance), RWY-specific tower routing and AIP VFR reporting points are not modeled | Generic hints at large hubs do not match the real procedures | High — would need per-airfield AIP research + a new delivery intent + slot setting + multi-tower disambiguation |

## FAQ

**Does the plugin support IFR or flight planning?**
No — this plugin is VFR-only. No IFR clearances, no flight-plan filing, no
FMS/routing integration. IFR is a separate product with its own plugin:
**[Welly's IFR ATC](https://github.com/rwellinger/xp_welly_llm_atc)** —
clearance delivery, SID/STAR from CIFP, SimBrief route integration, sector
handoffs, approach and landing (English/ICAO only). The two install side by
side; pick by what you fly.

**Will there be a virtual co-pilot or checklist reader?**
Not currently planned. The plugin is a single-pilot pilot↔ATC voice
interface; intercom and checklists are not implemented.

**Is it compatible with all XP12 aircraft and add-ons?**
In principle yes. The plugin is aircraft-agnostic and uses only standard
X-Plane DataRefs — no aircraft-specific code paths, no compatibility list.
It works with the default fleet (C172, etc.) and any add-on that provides
the standard `sim/cockpit/radios/*` DataRefs. For exotic aircraft without
`com_power`, set `skip_radio_power_check: true` in `settings.json`.
Laminar's default ATC can be suppressed via `disable_default_atc`.

**Can I fly on the yoke without focusing the plugin window?**
Yes — that is the intent. Bind push-to-talk once to a yoke button or a key
(X-Plane command `xp_wellys_devfr_atc/ptt`). After that, every interaction
is voice: press PTT, speak, release, hear the ATC response. The plugin
window needs no keyboard focus in flight, and every inference runs on
background threads, so X-Plane never stutters.

**Does the plugin read my COM1/COM2 frequencies automatically?**
Yes. Active and standby frequencies of both COM radios are read live from
X-Plane DataRefs. The plugin also detects which radio is active and
classifies the frequency type (ATIS / Ground / Tower / Approach / UNICOM)
automatically against the apt.dat frequency database. No manual frequency
entry.

**Does the plugin set the transponder / squawk code?**
No — spoken only. ATC can say "Squawk 7000", but the plugin does not read
or write the transponder DataRefs. You set the squawk manually.

**How does it compare to BeyondATC or SayIntentions?**
Strengths: 100% offline option on Apple Silicon (no subscription, no
cloud, no constant internet required — at the user's discretion), ~1.16 s
warm pipeline latency in Local mode, German NfL DACH-VFR phraseology with
realistic tower reactions to pilot mistakes. Two cloud options — **OpenAI**
and **Mistral** — are available as paid opt-ins (your own key). Mistral
usually costs less per token and is the cleaner choice for German ATC,
since Voxtral TTS speaks German natively.
Today's limits: VFR-only (IFR is the separate
[Welly's IFR ATC](https://github.com/rwellinger/xp_welly_llm_atc) plugin),
no routing, no wake-turbulence separation (sequencing in v2.2 is
distance-based only — Phase 5 on the roadmap), no transponder data link,
no co-pilot.

**Is there an introductory video?**
Not yet.

**How does it compare to OpenSquawk?**
Not yet evaluated.

## Project structure

```
src/
├── main.cpp                # XPlugin* entry points, menu, flight loop
├── atc/                    # session coordinator, state machine, intent
│                           #   parser + rules, templates, ATIS, flight
│                           #   phase, engine, traffic_advisor /
│                           #   traffic_dialog, landing_sequence,
│                           #   phraseology_hints, DE-specific:
│                           #   bzf_compliance + de_phraseology, plus
│                           #   flows/ (ground_operations, pattern_flow,
│                           #   crosscountry_flow, flow_coordinator)
├── audio/                  # push-to-talk, mic capture, PCM playback
│                           #   on the X-Plane radio bus (COM1 or COM2),
│                           #   mic permission
├── backends/               # strategy interfaces + manager (async
│                           #   dispatch) + loader (verify + load) +
│                           #   downloader (libcurl + resume + SHA256).
│                           #   Concrete backends separated by mode:
│                           #   Local: WhisperStt / LlamaLm / PiperTts
│                           #     (arm64 slice only, gated on
│                           #     XPWELLYS_USE_LOCAL_INFERENCE).
│                           #   OpenAI: OpenAiStt / OpenAiLm / OpenAiTts
│                           #     (both slices, libcurl + JSON).
│                           #   Mistral: MistralStt / MistralLm /
│                           #     MistralTts (both slices, libcurl +
│                           #     JSON; Voxtral TTS returns a JSON
│                           #     envelope with base64-encoded WAV).
│                           #   The three client sets share neither header
│                           #   nor code path — audit invariant enforced
│                           #   by tests.
├── core/                   # logging, cross_country_log (per-flight JSON
│                           #   radio logger + ATC logbook),
│                           #   XPlaneContext (SDK-free struct +
│                           #   SDK-coupled DataRef reader)
├── data/                   # airport VRPs, apt.dat-derived airspace
│                           #   index, traffic_context (struct + 2 Hz
│                           #   TCAS reader), traffic_geometry +
│                           #   traffic_phase_classifier
├── persistence/            # settings.json, keychain (OpenAI + Mistral
│                           #   API keys), model_paths, model_manifest,
│                           #   models_catalog
└── ui/                     # Dear ImGui ATC panel + Models + Traffic
                            #   tabs, ui_strings (i18n), clipboard helper
```

The CMake **OBJECT** library `xp_atc_engine` compiles the SDK-free
translation units (`atc/`, `core/logging`, `core/cross_country_log`,
`core/xplane_context` struct, `data/`, `backends/manager.cpp`,
`persistence/model_manifest`, `persistence/models_catalog`). Both the
plugin module and the headless `atc_repl` tool reuse it. The plugin module
adds the SDK-coupled units (`main.cpp`, `audio/`,
`core/xplane_context_runtime.cpp`,
`backends/{loader,downloader,openai_*,mistral_*}.cpp`,
`persistence/{settings,model_paths,keychain}.cpp`, `ui/atc_ui.cpp`). The
arm64 slice additionally compiles
`backends/{whisper_stt,llama_lm,piper_tts}.cpp` and links statically
against `whisper`, `llama`, `common` plus a shared `libpiper.dylib` that
resolves `libonnxruntime.1.22.0.dylib` via `@loader_path` — both dylibs
live in the plugin bundle next to the `.xpl`. The x86_64 slice has none of
these dependencies; it links only libcurl + Security + the audio
frameworks and ships both cloud-provider clients.

## Flight logbook / cross-country measurement session (`data/flightlog/`)

The `core/cross_country_log` module writes **one valid, pretty-printed
JSON document per flight** to
`<plugin>/data/flightlog/YYYY-MM-DD_HHMM_<AIRPORT>.json`. It serves twice:
as a measurement record for the cross-country evaluation **and** as an
ATC-side flight logbook. The logger is **purely observational** — it never
interferes with matching, routing or classification, it just threads
together the values `engine::process_transcript` produces anyway and
writes them at the point where `outcome` is settled.

The **entire document is rewritten after every transmission** atomically
(temp file + `rename`). It is thus complete, valid and editor-formattable
at any time — even on a sim crash everything up to the last transmission
is on disk. A "flight done" event is not needed: the `summary` is
recomputed on every transmission, the last state is always the final one.

Document structure:

```jsonc
{
  "version": 1,
  "flight": { "started_at": "...", "departure_airport": "EDNY",
              "pilot_callsign": "..." },
  "summary": { "transmissions": 6, "classified": 6, "unknown": 0,
               "garbled": 0, "lm_fallbacks": 0, "readback_issues": 0,
               "phases": ["PARKED","PATTERN"] },
  "transmissions": [ { /* one transmission, fields see below */ } ]
}
```

The `summary` block is counted purely from the logged transmissions (no
new classification heuristic): `garbled` = `tower_reported_garbled`,
`lm_fallbacks` = transmissions with an LM call, `readback_issues` =
READBACKs with a non-empty `readback_missing_elements`, `phases` = flight
phases passed through in order of first appearance.

**Flight separation** is pure logging logic and changes no ATC behavior:
if the open flight was already airborne and a transmission comes in again
on the ground in `IDLE`, that counts as a new departure → a new file.
Touch-and-go / pattern stays **one** flight; a cross-country airfield
change mid-air does **not** rotate. Additionally, `begin_new_flight()`
forces a new file — wired to (a) the X-Plane reposition/plane-reload hook
(`XPLM_MSG_AIRPORT_LOADED` / `PLANE_LOADED` in `main.cpp`) and (b) the
**"New flight" button** in the Settings tab as a manual backstop. So A→B→C
produces three clean files, whether flown through or repositioned via the
menu.

Fields per transmission (`transmissions[]`):

| Field | Meaning |
|---|---|
| `time` | Wall-clock timestamp of the transmission (`YYYY-MM-DDTHH:MM:SS`) |
| `transcript`, `quality` | Raw Whisper output (unchanged) + Whisper quality |
| `atc_response` | Tower response the transmission produced (clearance / instruction / correction / "say again"); empty = silent state change. Pilot and tower side are thus in the same record, which makes clearances and their readbacks readable as a complete exchange. |
| `intent`, `confidence` | classified intent + confidence |
| `path` | `rule_skip_lm` \| `lm_fallback` \| `clearance_match` — which path carried the transmission |
| `lm_used`, `lm_backend`, `lm_ready` | whether the LM ran; if so the backend (`openai`/`mistral`/`local`) + ready flag, otherwise `null` |
| `outcome` | `classified` \| `unknown` \| `tower_reported_garbled` |
| `state`, `flight_phase` | ATC state + flight phase **at the time of the transmission** (snapshotted before any state transition) |
| `expected_intent` | CSV of the intents valid in this state (`valid_intents`) — raw material for the manual verdict |
| `vrp_name_set`, `vrp_name` | whether a VRP/place name was recognized + which |
| `readback_missing_elements` | only on READBACK: missing target elements (empty array = complete), otherwise `null` |
| `failure_locus` | **non-binding** heuristic suggestion (`phraseology`/`proper_name`/`mixed`/`unclear`); only set when `outcome != classified` OR the LM fallback kicked in, otherwise `null` |

`failure_locus` is deliberately just a blunt keyword scan and cannot
influence behavior. You make the call yourself from `transcript` +
`expected_intent` + `vrp_name` status: VRP set but intent missed → a
proper-name problem; a garbled phraseology token with an empty VRP →
phraseology. The backend label is set by the `loader` at bring-up
(`cross_country_log::set_lm_backend(mode)`); engine code never inspects
`backend_mode` itself — see **Backend Adapter Rule** in `CLAUDE.md`.

> Limits: A plugin restart mid-flight begins a new flight file (the
> heuristic cannot bridge that). The timestamp is local wall-clock time,
> not sim Zulu time. The `data/flightlog/` directory is generated runtime
> data and not checked in.

## Third-party dependencies

See [`THIRD_PARTY.md`](../THIRD_PARTY.md) for the full list of bundled or
linked libraries, their licenses and how they are vendored.

## Development workflow

### CI pipeline

The GitHub Actions pipeline (`.github/workflows/build.yml`):

- **Push / PR merge to `main`** — fast cloud-only sanity build +
  unit/scenario tests (`build-macos-fast`, ~4 min). Since the local
  inference libs are prebuilt (bundle), there is no more heavy compile on
  `main` — the former `warm-deps` cache warmer is gone.
- **Push of a version tag `v*`** — full universal release build
  (`build-macos-universal`, ~5–8 min, against the prebuilt bundle) +
  Windows slice, then publication of a GitHub release with the packed ZIP
  + force-push of the SkunkCrafts update tree.
- **Manual `workflow_dispatch`** — pre-release dry run: builds both slices
  and uploads the artifacts, **but publishes nothing** (the `release` job
  stays tag-only). Use it before a tag to check that the release build
  passes:
  ```sh
  gh workflow run build.yml --ref main
  ```

### Merge to `main`

The branch protection requires:

1. PR (no direct pushes)
2. Status check `build-macos` green (`make all` successful)
3. PR branch up to date with main

## License

This project is licensed under the
[GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html).
GPLv3 is required because espeak-ng (GPL-3.0-or-later) is statically linked
into the bundled `libpiper.dylib`. Compatible with all other bundled
third-party libraries; see [`THIRD_PARTY.md`](../THIRD_PARTY.md) for the
per-dependency breakdown.
