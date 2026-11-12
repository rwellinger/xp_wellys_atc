# Welly's VFR ATC

**AI voice ATC for VFR flights in X-Plane 12** — native on macOS (Apple Silicon + Intel) and Windows.

Product website, downloads and documentation:
**https://thwelly.ch/xplane-plugins/xp-wellys-vfr-atc/**

Source code and issue tracker: https://github.com/rwellinger/xp_wellys_atc

## What it does

You speak, the plugin understands your call, and ATC answers with a voice: speech to text, intent recognition, text to speech.

- **Two phraseology profiles**, switchable at runtime in the settings: **German** (DACH VFR, NfL Sprechfunk 2024, optional strict BZF mode) and **English** (ICAO VFR).
- **Traffic patterns** — entry, downwind, base, final, landing, touch-and-go, go-around, landing sequence.
- **Cross-country** — departure clearance, frequency changes, handoffs, inbound.
- **Airport types** — uncontrolled (UNICOM/CTAF), Tower, Tower + Ground, AFIS.
- Automatic **ATIS** from the sim weather, **traffic information** from TCAS, **phraseology hints** in the UI.

VFR only — no IFR, no flight plans, no FMS routing.

## Installation

1. Unzip `xp_wellys_vfr_atc.zip`.
2. Copy the whole `xp_wellys_vfr_atc` folder into your X-Plane plugins directory:

```
X-Plane 12/Resources/plugins/xp_wellys_vfr_atc/
├── mac_x64/                ← macOS (Apple Silicon + Intel universal binary, Piper/ONNX Runtime libraries)
├── win_x64/                ← Windows x64 (plugin + piper.dll / onnxruntime.dll)
├── Resources/              ← eSpeak NG data; downloaded models go to Resources/models/
├── data/                   ← settings, phraseology profiles, VRPs, airspaces
└── docs/                   ← documentation
```

3. Start X-Plane. It loads the right binary for your platform automatically.
4. Bind **Push-to-talk** in X-Plane's keyboard or joystick settings to the command `xp_wellys_devfr_atc/ptt`.
5. Open the panel via **Plugins → Welly's ATC → Open / Close** and choose a backend in the **Settings** tab.

To update, replace the folder with the one from a newer release, or use the **SkunkCrafts Updater** — the plugin ships its configuration. The updater keeps your `data/settings.json` and your downloaded models.

**Requirements:** X-Plane 12 · macOS 13.3+ (Apple Silicon or Intel) or Windows 11 x64. On Windows the Microsoft Visual C++ 2015–2022 Redistributable (x64) is required; X-Plane systems usually have it.

## Backends

| Mode | Platforms | What you need |
|---|---|---|
| **Local** | macOS Apple Silicon (Metal), Windows (Vulkan) | ~2.5 GB disk for the models, downloaded in-sim from the **Models** tab; 32 GB RAM recommended |
| **OpenAI** | all | your own OpenAI API key |
| **Mistral** | all | your own Mistral API key |

Intel Macs support the cloud backends only. Accent-free German speech is available in Local mode only (Piper voice `thorsten`).

## Where your data lives

```
<plugin>/data/settings.json                          ← settings (kept by the updater)
<plugin>/Resources/models/                           ← downloaded models (kept by the updater)
<X-Plane>/Output/xp_wellys_devfr_atc/flightlog/      ← per-flight radio logs (JSON)
<X-Plane>/Output/preferences/xp_wellys_devfr_atc/    ← optional user overrides (VRPs, airspaces)
```

The `devfr` folder names are intentional — they keep existing installations working.

## Privacy

- **No telemetry, no account.** The plugin sends nothing to the author.
- **Local mode** runs speech recognition, the language model and speech output on your machine. The only network access is the one-time model download from HuggingFace.
- **OpenAI / Mistral mode** sends your push-to-talk recording, the transcript (only when the call is ambiguous) and the ATC reply text directly to the selected provider, authenticated with your own key. The provider's privacy policy applies.
- **API keys** are never written to the plugin's files. They are stored in the macOS Keychain or the Windows Credential Manager.

## License

Welly's VFR ATC is free software, licensed under the **GNU General Public License v3.0 or later** — see `LICENSE`.
Copyright (C) 2026 thWelly.

It includes third-party components under their own licenses (whisper.cpp, llama.cpp, Piper, eSpeak NG, ONNX Runtime, Dear ImGui, nlohmann/json, miniaudio, libcurl, zlib, X-Plane SDK) — see `THIRD_PARTY_LICENSES.md` and `ONNXRUNTIME_THIRD_PARTY_NOTICES.txt`. The AI models are not part of this package; they are downloaded from their publishers and stay under their own licenses.

## What's new

See `RELEASE.md` for the changes in each version.
