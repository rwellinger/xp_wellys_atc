# xp_wellys_atc

Internes Entwicklungsprojekt. C++17-Plugin für X-Plane 12, das
VFR-Sprechfunk (STT → Intent → TTS) simuliert.

Kein Release, keine Distribution, keine Nutzer ausser mir.

---

## Scope

- **VFR only.** Kein IFR, kein Flugplan, kein FMS/Routing.
- **Zwei Phraseologie-Profile**, zur Laufzeit umschaltbar
  (`settings::atc_language()`):
  - `de` (Standard) — NfL Sprechfunk 2024, DACH-VFR, optionaler
    BZF-Strict-Mode.
  - `en` — ICAO VFR (Annex 10 Vol II / Doc 4444 / SERA), eigenständig,
    keine Übersetzung des DE-Profils.
- **Abläufe:** Platzrunde (Entry, Downwind, Base, Final, Landing,
  Touch-and-Go, Go-Around, Landing-Sequencing), Cross-Country
  (Departure-Clearance, Frequenzwechsel, Handoff, Inbound),
  Flugplatztypen unkontrolliert (UNICOM/CTAF) / Tower / Tower+Ground /
  AFIS.
- **Nebenläufig:** automatische ATIS-Ansage aus Sim-Wetter,
  Verkehrshinweise aus TCAS-DataRefs, kontextabhängige
  Phraseologie-Hinweise im UI.

## Plattformen

| Slice | Backends | GPU |
|---|---|---|
| macOS arm64 | Local, OpenAI, Mistral | Metal |
| macOS x86_64 | OpenAI, Mistral | — |
| Windows x64 | Local, OpenAI, Mistral | Vulkan |

`backend_mode` ist eine Laufzeit-Einstellung; dasselbe Binary bedient
alle kompilierten Backends. Der x86_64-Slice schreibt `local` → `openai`
beim Start still um.

Windows: Vulkan statt CUDA (keine Redist-DLLs). Der Build braucht die
MSVC-Redistributable nur wegen `piper.dll` / `onnxruntime.dll`; fehlt
sie, schlägt erst die erste TTS-Wiedergabe mit `0xC06D007E` fehl (Piper
ist delay-loaded).

## Backends

| Modus | STT | LM | TTS |
|---|---|---|---|
| Local | whisper.cpp `small-q5_1` | llama.cpp Llama 3.2 3B Q4_K_M | Piper `de_DE-thorsten-medium` |
| OpenAI | `whisper-1` | `gpt-4o-mini` (JSON) | `tts-1` |
| Mistral | `voxtral-mini-2507` | `mistral-small-latest` (JSON) | `voxtral-mini-tts-2603` |

Die LM-Stufe läuft nur bei Intent-Konfidenz < 0.7. Modelle (~2,0 GB) sind
nicht gebündelt, sondern werden in-sim von HuggingFace geladen.

Deutsche Aussprache ist nur im Local-Modus akzentfrei (Piper
`thorsten`); OpenAI hat keine deutsche Stimme, Voxtral kein deutsches
Preset (Issue #63).

## Build

```bash
make setup     # SDK, ImGui, json, Catch2, Spike-Submodule
make build     # Universal-Release-Build -> build/xp_wellys_vfr_atc.xpl
make install   # Code-Signing + Installation ins X-Plane-Plugins-Verzeichnis
make all       # clean + format + build + lint + test
make repl      # headless atc_repl (kein X-Plane / Audio / Modelle)
make test      # Catch2-Unit- + Szenario-Tests
make sanitize  # ASan/UBSan-Build der Engine-OBJECT-Lib
```

Details, Architektur, Konfiguration und Entwicklungs-Workflow:
[`docs/README.md`](docs/README.md).
Verbindliche Leitlinien für die Arbeit am Code: [`CLAUDE.md`](CLAUDE.md).

## Lizenz

GPL-3.0-or-later (verlangt von espeak-ng, statisch in `libpiper` gelinkt).
Third-Party-Aufschlüsselung in [`THIRD_PARTY.md`](THIRD_PARTY.md).
