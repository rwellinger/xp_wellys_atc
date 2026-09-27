# xp_wellys_atc

C++17 plugin for X-Plane 12 that simulates VFR radio communication
(STT → intent → TTS).

Download and project website:
[thwelly.ch/xplane-plugins/xp-wellys-vfr-atc](https://thwelly.ch/xplane-plugins/xp-wellys-vfr-atc/)

## Scope

- **VFR only.** No IFR, no flight plan, no FMS/routing.
- **Two phraseology profiles**, switchable at runtime
  (`settings::atc_language()`):
  - `de` (default) — NfL Sprechfunk 2024, DACH VFR, optional
    BZF strict mode.
  - `en` — ICAO VFR (Annex 10 Vol II / Doc 4444 / SERA), self-contained,
    not a translation of the DE profile.
- **Flows:** traffic pattern (entry, downwind, base, final, landing,
  touch-and-go, go-around, landing sequencing), cross-country
  (departure clearance, frequency change, handoff, inbound),
  airfield types uncontrolled (UNICOM/CTAF) / Tower / Tower+Ground /
  AFIS.
- **Concurrent:** automatic ATIS broadcast from sim weather,
  traffic advisories from TCAS DataRefs, context-aware
  phraseology hints in the UI.

## Platforms

| Slice | Backends | GPU |
|---|---|---|
| macOS arm64 | Local, OpenAI, Mistral | Metal |
| macOS x86_64 | OpenAI, Mistral | — |
| Windows x64 | Local, OpenAI, Mistral | Vulkan |

`backend_mode` is a runtime setting; the same binary serves all
compiled-in backends. The x86_64 slice silently rewrites `local` → `openai`
at startup.

Windows: Vulkan instead of CUDA (no redist DLLs). The build needs the
MSVC Redistributable only because of `piper.dll` / `onnxruntime.dll`; if it
is missing, only the first TTS playback fails with `0xC06D007E` (Piper
is delay-loaded).

## Backends

| Mode | STT | LM | TTS |
|---|---|---|---|
| Local | whisper.cpp `small-q5_1` | llama.cpp Llama 3.2 3B Q4_K_M | Piper `de_DE-thorsten-medium` |
| OpenAI | `whisper-1` | `gpt-4o-mini` (JSON) | `tts-1` |
| Mistral | `voxtral-mini-2507` | `mistral-small-latest` (JSON) | `voxtral-mini-tts-2603` |

The LM stage runs only at intent confidence < 0.7. Models (~2.0 GB) are
not bundled; they are downloaded in-sim from HuggingFace.

German pronunciation is accent-free only in Local mode (Piper
`thorsten`); OpenAI has no German voice, Voxtral no German
preset (Issue #63).

## Build

```bash
make setup     # SDK, ImGui, json, Catch2, spike submodules
make build     # Universal release build -> build/xp_wellys_vfr_atc.xpl
make install   # Code signing + installation into the X-Plane plugins directory
make all       # clean + format + build + lint + test
make repl      # headless atc_repl (no X-Plane / audio / models)
make test      # Catch2 unit + scenario tests
make sanitize  # ASan/UBSan build of the engine OBJECT lib
```

Details, architecture, configuration and development workflow:
[`docs/README.md`](docs/README.md).
Binding guidelines for working on the code: [`CLAUDE.md`](CLAUDE.md).

## Privacy

The plugin has no telemetry or analytics and sends nothing to the author.
What leaves your machine depends on the backend you choose:

- **Local** — speech recognition, language model and speech output all run
  on your machine. The only network access is the one-time model download
  from HuggingFace (~2.0 GB).
- **OpenAI / Mistral** — requests go directly from your machine to the
  selected provider, authenticated with your own API key:
  - STT: your push-to-talk recording, plus airport names and callsigns as
    recognition hints
  - LM: the transcript and the ATC context (only when intent confidence
    is < 0.7)
  - TTS: the ATC reply text

  The provider's privacy policy applies
  ([OpenAI](https://openai.com/policies/privacy-policy/),
  [Mistral](https://mistral.ai/terms/#privacy-policy)).

**API keys** are entered in the plugin settings but never written to the
plugin's config files. They are stored in the operating system's credential
store: macOS Keychain or Windows Credential Manager (one entry per
provider). Deleting the key in the plugin settings removes the entry.

## License

GPL-3.0-or-later (required by espeak-ng, statically linked into `libpiper`).
Third-party breakdown in [`THIRD_PARTY.md`](THIRD_PARTY.md).
