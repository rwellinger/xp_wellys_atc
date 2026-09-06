# CLAUDE.md

Diese Datei gibt Claude Code dauerhafte Leitlinien für die Arbeit in
diesem Repository. Lies sie vollständig, bevor du irgendetwas anderes
tust.

---

## Projektüberblick

**xp_wellys_vfr_atc** ist ein C++17-Plugin für X-Plane 12 (**macOS 13.3+**),
das KI-gestützte ATC-Sprechfunk-Kommunikation für die VFR-Flugsimulation
bereitstellt. Schwerpunkt ist **VFR-Sprechfunk in Deutschland** nach NfL
Sprechfunk 2024 (DACH-VFR) mit optionalem, **DE-only**-**BZF-Strict-Mode**.
Daneben gibt es ein **englisches ICAO-VFR-Profil** (ICAO Doc 4444 /
Annex 10 / SERA — **keine** NfL-Übersetzung). **Kein** IFR. Auslieferung
als **Universal Binary** (`arm64 + x86_64`).

Die **Sprache** ist der einzige maßgebliche Schalter
(`settings::atc_language()`, Issue #36) und liefert `"de"` (Standard) oder
`"en"`. Davon leiten sich `settings::atc_profile()` (`"DE"` / `"EN"`) und
`settings::backend_language()` (gleich `atc_language()`) ab. Die Sprache
ist im **Settings-Tab** wählbar (`settings::set_atc_language()`). Es gibt
zwei Profil-Bundles: `data/atc_profiles/de/` und `data/atc_profiles/en/`.
Strict-/Conformance-Hooks gaten auf `atc_profile()=="DE"` und sind im
EN-Profil inert.

**Triple-Backend-Inferenz** — der Nutzer wählt den Modus zur Laufzeit in
den Einstellungen:

- **Local** (Apple Silicon `arm64` und Windows `x64`): whisper.cpp
  `small-q5_1` (multilinguales STT, Deutsch) + llama.cpp Llama 3.2 3B Instruct
  Q4_K_M (LM, nur für die Absichts-Klassifizierung bei geringer Konfidenz) +
  Piper `de_DE-thorsten-medium` (CPU + onnxruntime TTS) mit gebündelten
  `espeak-ng-data`. Das ggml-GPU-Backend steckt im prebuilt Bundle und ist
  plattformabhängig: **Metal** auf Apple Silicon, **Vulkan** auf Windows (siehe
  Windows-Slice unten). Die Modelle (~2,0 GB) sind NICHT gebündelt — sie werden
  beim ersten Start in-sim von HuggingFace geladen (HTTPS, fortsetzbar,
  SHA256-verifiziert) nach `<plugin>/Resources/models/`.
- **OpenAI Cloud** (jeder Mac, eigener API-Key): Whisper API (STT) + Chat
  Completions `gpt-4o-mini` JSON-Modus (LM) + TTS API (TTS, sechs
  Stimmen). API-Key im macOS Keychain über `Security.framework` unter dem
  Service `com.xp_wellys_devfr_atc.openai`, nie in `settings.json`.
- **Mistral Cloud** (jeder Mac, eigener API-Key): Voxtral STT
  (`voxtral-mini-2507`) + Mistral Chat Completions
  (`mistral-small-latest`, JSON-Modus) + Voxtral TTS
  (`voxtral-mini-tts-2603`). Separater Keychain-Eintrag
  `com.xp_wellys_devfr_atc.mistral`, sodass der OpenAI-Key unberührt bleibt.
  Multilinguales TTS (Voxtral spricht u. a. Deutsch). **Wichtig (Issue
  #63):** Voxtral hat **keine deutsche Preset-Stimme** — die Presets sind
  US-EN (`en_paul_*`), GB-EN (`gb_oliver_*`) und FR. Die Default-Voices
  sind englisch, deutscher Text bekommt damit einen **US-Akzent**. Weniger
  Akzent geht nur über eine französische Preset-Stimme (emergenter
  Cross-Lingual-Effekt) oder Voice-Cloning mit deutscher Referenz — beides
  nicht Default. Für sauberes Deutsch ist **Local** (Piper `thorsten`) das
  einzige Backend; OpenAI hat gar keine deutsche Stimme.

Der `x86_64`-Slice hat **keine** lokalen Backends einkompiliert; **OpenAI**
oder **Mistral** ist die einzige Option auf Intel-Macs (der Loader
schreibt `local` → `openai` beim Start für diesen Slice still um).

**Windows-Slice (`win_x64/xp_wellys_vfr_atc.xpl`, alle drei Backends)** — die
per-arch `.xpl` MUSS den Plugin-Namen tragen (nicht `win.xpl`): X-Plane 12 auf
Windows lädt eine generisch benannte Datei still nicht. Die Cloud-Backends
(OpenAI + Mistral über statisches libcurl aus vcpkg, `x64-windows-static`,
Schannel-TLS) und der **volle lokale Stack** kommen beide mit:
`XPWELLYS_USE_LOCAL_INFERENCE=ON` + `XPWELLYS_USE_LOCAL_TTS=ON` gegen das
prebuilt `xp_wellys_libs`-**win-x64-full-Bundle** (ab v0.4.0) — whisper + llama
auf **ggml/Vulkan**, Piper-TTS auf CPU.

**Vulkan, nicht CUDA** (bewusst): CUDA-Redist-DLLs (`ggml-cuda` 50–160 MB,
cuBLAS/cuBLASLt mehrere hundert MB) sind für ein SkunkCrafts-Updater-Release
untragbar. Vulkan braucht **keine** zusätzliche Runtime-DLL — `vulkan-1.dll`
kommt vom Grafiktreiber, und X-Plane 12 rendert auf Windows ohnehin über
Vulkan. Kosten: grob 10–30 % weniger Durchsatz als CUDA, und die Modelle teilen
sich den VRAM mit X-Planes Renderer.

**Zwei Windows-Load-Time-Fallen — beide tragend, nicht "vereinfachen":**

1. `/DELAYLOAD:piper.dll` + `/DELAYLOAD:vulkan-1.dll`. Cloud-Nutzer mappen
   Piper/onnxruntime damit nie. `piper.dll` stattdessen als Load-Time-Import zu
   linken wurde probiert und ist **schlechter**: X-Plane verweigert dann das
   ganze Plugin (`Error Code = 1114`), der Cloud-Modus geht mit verloren.
2. Der Delay-Load-Helper löst DLLs über den **Prozess**-Suchpfad auf, der
   `win_x64/` NICHT enthält (das gilt nur für Load-Time-Imports der `.xpl`).
   Deshalb lädt `loader.cpp::preload_piper_dll()` die DLL vorab per
   `LoadLibraryExW` + `LOAD_WITH_ALTERED_SEARCH_PATH` mit absolutem Pfad, den
   es aus dem eigenen Modul-Handle ableitet (`GetModuleFileNameW`) — nicht aus
   `model_paths::plugin_root()`, das gemischte Separatoren liefert. Ohne den
   Preload: `0xC06D007E` beim ersten `piper_create`.

Dazu ein **Patch am Bundle** (`xp_wellys_libs` ≥ 0.4.2,
`patches/piper-lazy-ort-env.patch`): libpiper deklariert `Ort::Env` auf
Namespace-Ebene; dessen Konstruktor läuft beim Mappen der DLL unter dem
Windows-Loader-Lock und crasht dort (`0xC0000005`). Der Patch macht ihn zum
function-local static. Beide Fehler traten **gestapelt** auf — jeder Fix
änderte nur den Fehlercode, was wie ein Fehlschlag aussah.

Der `backend_mode` ist eine **Laufzeit**-Einstellung; dasselbe Binary bedient
Local, OpenAI und Mistral. Gebaut mit MSVC (Ninja) via CMake auf
`windows-latest` in CI (**nicht lokal** — es gibt keinen lokalen MSVC-Pfad); das
CI holt das win-x64-Bundle per `PREBUILT_LIBS_VERSION` und verifiziert es gegen
`manifest.txt` (SHA256). Das Bundle ist mit **statischer CRT (`/MT`)** gebaut,
deshalb MUSS der Plugin-Build `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`
setzen — `xp_wellys_libs.cmake` bricht sonst beim Configure ab. Das Artefakt
trägt `piper.dll` + `onnxruntime.dll` (+ `onnxruntime_providers_shared.dll`)
neben der `.xpl` und `espeak-ng-data` unter `Resources/` — **nicht DLL-frei**
(whisper/llama/ggml sind statisch in der `.xpl`, Vulkan bringt nichts mit; die
DLLs sind allein Piper/onnxruntime). Modelle (whisper `small-q5_1`, Llama 3B
Q4, `de_DE-thorsten`) lädt der Nutzer in-sim im Models-Tab. Der
CMake-`elseif(WIN32)`-Zweig setzt `IBM=1 APL=0 LIN=0` und linkt
`XPLM_64.lib`/`XPWidgets_64.lib` + `opengl32 Ws2_32 Crypt32 Bcrypt Advapi32`. Die macOS-`.mm`-Bridges
(`clipboard.mm`, `mic_permission.mm`) sind aus dem Windows-Build
ausgeschlossen und durch echte `*_win.cpp`-Ports ersetzt (Issue #22):
`clipboard_win.cpp` liest/schreibt das System-Clipboard über die
Win32-Clipboard-API (`CF_UNICODETEXT`, UTF-8↔UTF-16), `mic_permission_win.cpp`
ist ein bewusster No-op (`return true` — Windows hat keinen In-Process-
Mic-Prompt; ein System-Privacy-Block äussert sich als leerer Capture-Stream).
Die Windows-**Mic-Capture** (Issue #21) liegt in `audio_recorder.cpp` hinter
`#elif defined(_WIN32)`: **miniaudio** (single-header, WASAPI) resampelt das
Standard-Mic nativ → 16 kHz mono s16 in denselben `buffer_`; der macOS-
Core-Audio-Pfad bleibt unangetastet. miniaudio (`vendor/miniaudio.h`, von
`make setup` / dem CI-Deps-Step geholt) braucht **keine** Extra-Link-Libs —
ole32/WASAPI werden zur Laufzeit dynamisch geladen; `MINIAUDIO_IMPLEMENTATION`
lebt allein in `audio_recorder.cpp`. Die Worker-Priorisierung (`manager`) ist
jetzt auf beiden Plattformen echt: `pthread_set_qos_class_self_np(UTILITY)` auf
Apple, `SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL)` auf Windows (Issue
#23) — der frühere No-op war damit begründet, dass der Slice kein
Local-Inference fuhr; mit whisper/llama auf Vulkan konkurrieren die Worker nun
real mit X-Planes Renderer. Siehe Epic #24.

Alle drei Backend-Familien teilen sich dieselben drei abstrakten
`i_*.hpp`-Strategie-Interfaces. Engine-Code berührt nie ein konkretes
Backend. Siehe den Abschnitt **Backend Adapter Rule** — eine harte
Invariante, erzwungen durch `tests/test_audit_logging.cpp`.

Lizenz: **GPL-3.0-or-later** (verlangt von espeak-ng, statisch in das
gebündelte `libpiper.dylib` des `arm64`-Slice gelinkt).

---

## Build-System

```bash
make setup     # X-Plane SDK, Dear ImGui, nlohmann/json, Catch2, Spike-Submodule
make build     # Universal-Release-Build → build/xp_wellys_vfr_atc.xpl (arm64+x86_64 lipo'd)
make install   # Code-Signing + Installation ins X-Plane-Plugins-Verzeichnis
make all       # clean + format + build + lint + test (volle lokale CI)
make repl      # headless atc_repl-Tool (kein X-Plane / kein Audio / keine Modelle)
make test      # Catch2-Unit-Tests + Szenario-Tests (inkl. Audit-Invariante)
make sanitize  # ASan- + UBSan-Build der Engine-OBJECT-Lib + atc_repl + Tests
```

**Test-Reihenfolge + Isolation (Issue #3, behoben):** `make test` läuft auf
`--order rand --rng-seed 42` — reproduzierbar UND mit aktivem Latent-Bug-Detektor
(zufällige Reihenfolge deckt geteilten globalen Zustand auf). Per-Test-Isolation
erzwingt ein **Catch2-EventListener** (`tests/module_reset_listener.cpp`), der vor
**jedem** Test-Case die Modul-Globals zurücksetzt: `atc_state_machine::init()` +
`reset()` (g_state inkl. `was_airborne_`/`history_`, kaskadiert `crosscountry_flow`),
`flight_phase::reset()` (Laufzeit-Phase `current_phase_`/`candidate_*`, Config bleibt
geladen), `atis_generator::init()`, `settings::reset_for_test()` (Stub:
`bzf_strict_mode`/VFR-Intention/Callsign). Die Modul-Globals liegen hinter
**getrennten Modulgrenzen** — es gibt kein einzelnes Global; der Listener ruft daher
N modul-eigene Resets. Neue Tests dürfen sich NICHT darauf verlassen, dass
`flight_phase` ungeladen ist (frühere Falle: `ground_ctx()` mit `frequency_type=UNKNOWN`
löste je nach Ladezustand den Wrong-Frequency-Hint-Guard aus) — immer eine realistische
`frequency_type` setzen. Lokal neue Leaks jagen: andere Seeds durchprobieren, z. B.
`./build/xp_wellys_vfr_atc_tests --order rand --rng-seed 7`.

`make build` erzeugt stets das Universal Binary: CMake läuft zweimal —
arm64 mit `XPWELLYS_USE_LOCAL_INFERENCE=ON` (build-arm64/), x86_64 mit
demselben Flag `OFF` (build-x86_64/) — und `lipo`-merged die zwei `.xpl`s
zu `build/xp_wellys_vfr_atc.xpl`. Das `libpiper.dylib` und
`libonnxruntime.{1.22.0,}.dylib` des arm64-Slice werden neben das
lipo'd Binary gestaged, damit `make install` sie findet.
`make release-build` ist derselbe Build mit durchgereichtem
`-DRELEASE=ON` (vom GitHub-Actions-Tag-Workflow genutzt).

`make sanitize` instrumentiert nur den SDK-freien Engine-Pfad. Das
`.xpl`-Plugin-Modul wird NICHT sanitisiert — ASan im X-Plane-Prozess ist
auf macOS ARM64 fragil. Nutze Instruments.app (Leaks / Allocations),
angehängt an den X-Plane-Prozess, für Laufzeit-Leak-Jagd im Live-Plugin.

- **CMake 3.26+**, C++17, **macOS 13.3+** (onnxruntime 1.22.0 verlangt das)
- **CMake-Option `XPWELLYS_USE_LOCAL_INFERENCE`** (Standard `ON`) — steuert,
  ob die drei lokalen Backends (`whisper_stt`, `llama_lm`, `piper_tts`)
  und ihre Submodul-Abhängigkeiten (whisper.cpp, llama.cpp, Piper)
  kompiliert und gelinkt werden. Für den x86_64-Slice auf `OFF`; das
  resultierende Binary hat null lokalen Inferenz-Code.
- Toolchain: Homebrew LLVM (`/opt/homebrew/opt/llvm`), `ccache` automatisch erkannt
- Ausgabe: `build/xp_wellys_vfr_atc.xpl`; auf dem arm64-Slice zusätzlich
  gestaged `libpiper.dylib` + `libonnxruntime.{1.22.0,}.dylib` neben dem
  `.xpl`, zur Laufzeit über `@loader_path`-rpath aufgelöst. Der
  x86_64-Slice hat keine dieser dylibs — ein deutlich kleineres Artefakt.
- Compiler-Flags: `-Wall -Wextra -fvisibility=hidden`,
  OpenGL-Deprecation nur in unseren TUs unterdrückt
- Gelinkte System-Frameworks: `AudioToolbox`, `AudioUnit`, `CoreAudio`,
  `AVFoundation`, `CoreFoundation`, `OpenGL`, `Security` (Keychain)
- Netzwerk: System-libcurl über `find_package(CURL)` — vom Modell-Downloader
  (arm64 / Local-Modus) UND von jedem Cloud-HTTPS-Aufruf (OpenAI und
  Mistral) genutzt. Die drei lokalen Backends DÜRFEN libcurl NICHT nutzen;
  siehe Backend Adapter Rule.
- Inferenz-Libs (nur arm64): `whisper`, `llama`, `common` (statisch) +
  `piper` (shared dylib, linkt `libonnxruntime.1.22.0.dylib`). Der
  x86_64-Slice linkt nur libcurl + die System-Frameworks oben.

## Vendor-Abhängigkeiten

Von `make setup` befüllt, nie eingecheckt:

| Pfad | Inhalt |
|---|---|
| `sdk/` | X-Plane-SDK-Header (XPLM/, XPWidgets/) |
| `vendor/imgui/` | Dear ImGui v1.91.x |
| `vendor/json.hpp` | nlohmann/json v3.11.x |
| `vendor/miniaudio.h` | miniaudio v0.11.x (Windows-Mic-Capture, WASAPI) |
| `spikes/spike_whisper/third_party/whisper.cpp/` | whisper.cpp-Submodul |
| `spikes/spike_llama/third_party/llama.cpp/` | llama.cpp-Submodul (liefert `ggml`) |
| `spikes/spike_piper/third_party/piper1-gpl/` | Piper-Submodul (espeak-ng + onnxruntime) |

Der CMake-Build zieht llama.cpp **zuerst**, damit dessen gepinntes
`ggml`-Target gewinnt; whisper.cpp kurzschliesst dann auf dem
bestehenden Target. Inline in `CMakeLists.txt` dokumentiert.

---

## Verzeichnisstruktur

```
xp_wellys_vfr_atc/
├── CLAUDE.md
├── README.md, THIRD_PARTY.md, LICENSE
├── CMakeLists.txt
├── Makefile
├── VERSION.txt
├── src/
│   ├── main.cpp                # XPlugin*-Einstiegspunkte, Menü, Flight-Loop
│   ├── atc/
│   │   ├── atc_session.hpp/.cpp        # PTT-Koordinator + Debug-Text-Injektion (plugin-only)
│   │   ├── engine.hpp/.cpp             # SDK-freier Transcript → Response-Orchestrator
│   │   ├── intent_parser.hpp/.cpp      # Regelbasiert: Transcript → PilotIntent (Lexing + Scoring)
│   │   ├── intent_rules.hpp/.cpp       # Regeltabellen, von intent_parser konsumiert
│   │   ├── atc_state_machine.hpp/.cpp  # VFR-ATC-Logik + template-basierte Responses + State-Revert-Guard
│   │   ├── atc_templates.hpp/.cpp      # JSON-Template-Engine
│   │   ├── atis_generator.hpp/.cpp     # ATIS-Ansage + Informations-Buchstaben
│   │   ├── flight_phase.hpp/.cpp       # Flugphase + Vorbedingungs-Guards
│   │   ├── phraseology_hints.hpp/.cpp  # Kontextbewusster Phraseologie-Spickzettel (SDK-frei)
│   │   ├── traffic_advisor.hpp/.cpp    # En-route-Verkehrshinweis-Generator (SDK-frei)
│   │   ├── traffic_dialog.hpp/.cpp     # Piloten-Antwort-Parser ("in Sicht" / "negativ") (SDK-frei)
│   │   ├── landing_sequence.hpp/.cpp   # Phase-4-Sequenzierung + Runway-Occupancy-Primitive (SDK-frei)
│   │   ├── bzf_compliance.hpp/.cpp     # NfL §25 b) Nr. 1 Readback-Prüfung (SDK-frei)
│   │   ├── initial_call_conformance.hpp/.cpp # BZF-Erstanruf-Pflichtinhalt-Check (SDK-frei, datengetrieben)
│   │   ├── de_phraseology.hpp/.cpp     # Rufzeichen-/Zahlen-Expansion (SDK-frei)
│   │   └── flows/                      # Per-Kontext-Flow-Module (SDK-frei)
│   │       ├── ground_operations.hpp/.cpp   # Taxi / Clearance-Delivery / Engine-Start
│   │       ├── pattern_flow.hpp/.cpp        # Platzrunde + Landing-Sequence-Overlay
│   │       ├── crosscountry_flow.hpp/.cpp   # En-route-Freq-Wechsel / Handoff
│   │       ├── flow_coordinator.hpp/.cpp    # Dispatch nach ATCState
│   │       └── state_storage.hpp            # AtcStateSnapshot + Generations-Zähler
│   ├── audio/
│   │   ├── ptt_input.hpp/.cpp          # Push-to-Talk-Befehlsbindung
│   │   ├── audio_recorder.hpp/.cpp     # Core-Audio-Mic-Capture → 16 kHz PCM
│   │   ├── audio_player.hpp/.cpp       # Core-Audio-PCM-Wiedergabe auf Funkbus + Squelch-Burst
│   │   └── mic_permission.hpp/.mm      # macOS-Mikrofon-Berechtigungs-Prompt
│   ├── backends/
│   │   ├── i_speech_to_text.hpp        # Strategie-Interface — STT
│   │   ├── i_language_model.hpp        # Strategie-Interface — LLM
│   │   ├── i_text_to_speech.hpp        # Strategie-Interface — TTS
│   │   ├── whisper_stt.hpp/.cpp        # whisper.cpp-Wrapper — Local (gated by XPWELLYS_USE_LOCAL_INFERENCE)
│   │   ├── llama_lm.hpp/.cpp           # llama.cpp-Wrapper — Local (gated)
│   │   ├── piper_tts.hpp/.cpp          # Piper-Wrapper — Local (gated)
│   │   ├── openai_common.hpp/.cpp      # Geteilte libcurl- + JSON-Helfer für OpenAI-Clients
│   │   ├── openai_stt.hpp/.cpp         # OpenAI-Whisper-API-Client (beide Slices)
│   │   ├── openai_lm.hpp/.cpp          # OpenAI-Chat-Completions-Client (beide Slices)
│   │   ├── openai_tts.hpp/.cpp         # OpenAI-TTS-API-Client (beide Slices)
│   │   ├── mistral_stt.hpp/.cpp        # Mistral-Voxtral-STT-Client (beide Slices)
│   │   ├── mistral_lm.hpp/.cpp         # Mistral-Chat-Completions-Client (beide Slices)
│   │   ├── mistral_tts.hpp/.cpp        # Mistral-Voxtral-TTS-Client — JSON-Envelope mit base64-WAV (beide Slices)
│   │   ├── manager.hpp/.cpp            # std::thread-Async-Dispatch (SDK-frei)
│   │   ├── loader.hpp/.cpp             # Modus-bewusstes Backend-Bring-up (plugin-only)
│   │   └── downloader.hpp/.cpp         # libcurl + Range-Resume + SHA256 (plugin-only)
│   ├── core/
│   │   ├── logging.hpp/.cpp            # XPLMDebugString + level-basiertes Logging
│   │   ├── cross_country_log.hpp/.cpp  # Per-Flug-JSON-Funk-Logger + ATC-Logbuch (SDK-frei, rein beobachtend)
│   │   ├── xplane_context.hpp/.cpp     # SDK-freier XPlaneContext-Struct + Helfer
│   │   └── xplane_context_runtime.cpp  # SDK-gekoppelter DataRef-Reader (plugin-only)
│   ├── data/
│   │   ├── airport_vrps.hpp/.cpp       # JSON-geladene VFR-Meldepunkte (gebündelt + User-Override)
│   │   ├── airspace_db.hpp/.cpp        # apt.dat-abgeleiteter Airspace/Controller-Index
│   │   ├── openair_db.hpp/.cpp         # Echter 3D-OpenAir-Luftraum-Parser (CTR/RMZ/TMZ/TMA/CTA/ED-R) mit Floor/Ceiling (SDK-frei)
│   │   ├── traffic_context.hpp/.cpp    # SDK-freier TrafficContext-Struct + Helfer
│   │   ├── traffic_context_runtime.cpp # 2-Hz-TCAS-DataRef-Snapshot (plugin-only)
│   │   ├── traffic_geometry.hpp/.cpp   # Relativ-Peilung-/Uhrzeit-Mathematik (SDK-frei)
│   │   └── traffic_phase_classifier.hpp/.cpp # Hebt Airborne-Ziele auf Pattern/Final (SDK-frei)
│   ├── persistence/
│   │   ├── settings.hpp/.cpp           # JSON-Config (plugin-only — hängt an Plugin-Pfaden)
│   │   ├── keychain.hpp/.cpp           # macOS-Security.framework-Wrapper für Cloud-API-Keys (OpenAI + Mistral)
│   │   ├── model_paths.hpp/.cpp        # Löst <plugin>/Resources/models/ via XPLMGetPluginInfo auf
│   │   ├── model_manifest.hpp/.cpp     # Manifest-Einträge + SHA256 (CommonCrypto, SDK-frei)
│   │   └── models_catalog.hpp/.cpp     # Lädt data/models_catalog.json — einzige Quelle für wählbare Modell-Slugs/Voices
│   └── ui/
│       ├── atc_ui.hpp/.cpp             # Dear-ImGui-ATC-Panel + Settings/Models/Traffic-Tabs + Debug-Texteingabe
│       ├── ui_strings.hpp/.cpp         # i18n-String-Lookup (de)
│       └── clipboard.hpp/.mm           # System-Pasteboard-Lese-Helfer (NSPasteboard) — hinter [Paste]-Buttons
├── data/
│   ├── settings.json                   # Laufzeit-Standards (keine Geheimnisse — eingecheckt)
│   ├── models_catalog.json             # Wählbare Modell-Slugs + Voices für alle drei Backends (editierbar, kein Recompile)
│   ├── atc_prompt_templates.json       # whisper_prompt + gpt_classify_prompt_de + gpt_fallback_prompt_de
│   ├── vrps/airport_vrps.json          # VRP-/Platzrunden-DB (durch User-Datei überschreibbar)
│   ├── airspaces/de_airspace.txt       # Gebündeltes DE-Luftraum-Starterset (OpenAir); User-Override <prefs>/airspace.txt
│   └── atc_profiles/
│       └── de/{atc_templates,flight_rules,intent_rules,phraseology_hints,ui_strings,conformance}.json
├── tools/atc_repl/                     # Headless-Dev-Tool (nur Engine-OBJECT-Lib)
├── tests/                              # Catch2-Unit- + Szenario-Tests
├── spikes/                             # Spike-Submodule + Experimente
├── sdk/                                # make setup, nicht eingecheckt
└── vendor/                             # make setup, nicht eingecheckt
```

Jedes `src/`-Unterverzeichnis besitzt eine Aufgabe. Includes nutzen die
subdir-präfixierte Form (z. B. `#include "backends/whisper_stt.hpp"`), so
sind Abhängigkeiten an der Aufrufstelle sichtbar.

Die CMake-**OBJECT**-Bibliothek `xp_atc_engine` kompiliert alle SDK-freien
TUs (engine, intent_parser, intent_rules, State-Machine, Templates,
flight_phase, ATIS, phraseology_hints, traffic_advisor, traffic_dialog,
landing_sequence, flows/*, bzf_compliance, de_phraseology, manager,
Daten-Loader, traffic_context-Struct, traffic_geometry,
traffic_phase_classifier, openair_db, logging, cross_country_log, xplane_context-Struct,
model_manifest, models_catalog, ui_strings). Sowohl das Plugin-Modul als
auch das headless
`atc_repl`-Tool nutzen sie wieder. Das Plugin-Modul ergänzt die
SDK-gekoppelten Einheiten (main, atc_session, audio/*,
xplane_context_runtime, traffic_context_runtime, loader, downloader,
model_paths, settings, keychain, ui/atc_ui, ui/clipboard) und die zwei
Cloud-Backend-Familien: OpenAI (`openai_common`, `openai_stt`,
`openai_lm`, `openai_tts`) und Mistral (`mistral_stt`, `mistral_lm`,
`mistral_tts`). Die drei lokalen Backends (`whisper_stt`, `llama_lm`,
`piper_tts`) werden nur bei `XPWELLYS_USE_LOCAL_INFERENCE=ON` ergänzt.

---

## Architektur

### Modul-Verantwortlichkeiten

Jedes Modul nutzt einen C++-Namespace mit `init()`- und `stop()`-
Lebenszyklus-Funktionen, die `main.cpp` in Abhängigkeitsreihenfolge ruft.

**`main.cpp`** — `XPluginStart`, `XPluginStop`, `XPluginEnable`,
`XPluginDisable`. Registriert den Flight-Loop-Callback. Ruft `init()` /
`stop()` aller Module.

**`xplane_context`** — Liest DataRefs pro Flight-Loop in den
`XPlaneContext`-Struct. Leitet `nearest_airport_id` /
`is_towered_airport` über `XPLMGetNavAidInfo` ab. Parst `apt.dat` beim
Init (Hintergrund-Thread) und baut die Frequenz-DB (`AirportFrequencies`,
Codes 50-55 / 1050-1055: ATIS, UNICOM, Delivery, Ground, Tower, Approach),
den Runway-Cache (`RunwayInfo` pro Flugplatz, Code 100) sowie Name,
Elevation und Referenzposition. Wählt `active_runway` aus dem Wind ~1 Hz
(Flaute < 3 kt → längste Piste, sonst grösster Gegenwind). `frequency_type`
durch Abgleich der aktiven COM gegen die Flugplatz-DB. Unterstützt
`tower_only` (Tower übernimmt Taxi). Bietet `set_standby_freq()` für
ImGui-Frequenz-Klicks.

**`atis_generator`** — Generiert realistische ATIS-Ansagen aus den
Wetterdaten des XPlaneContext. Verwaltet den ATIS-Informations-Buchstaben
(Alpha–Zulu), inkrementiert bei signifikanten Änderungen (aktive Piste,
Windrichtung >30°, QNH >1 hPa, Sicht-Kategorie). Spielt automatisch über
TTS, wenn die COM des Piloten innerhalb ~60 NM auf die ATIS-Frequenz des
Flugplatzes passt, mit Cooldown.

**`settings`** — Lädt/speichert `data/settings.json`. Hält den
`backend_mode`-Toggle (`local` | `openai` | `mistral`), die OpenAI- und
Mistral-Modell-/Voice-IDs, `bzf_strict_mode`, `start_mode`,
`traffic_features_enabled`, `debug_text_input` usw. sowie den
`atc_language`-Schalter (`"de"` | `"en"`), aus dem `atc_profile()`
(`"DE"` / `"EN"`) und `backend_language()` (gleich `atc_language()`)
abgeleitet werden. **Kein
API-Key liegt je in `settings.json`** — nur die Flags `api_key_saved` und
`mistral_api_key_saved` werden persistiert; die echten Geheimnisse liegen
im macOS Keychain unter zwei separaten Services
(`com.xp_wellys_devfr_atc.openai` und `com.xp_wellys_devfr_atc.mistral`, beide mit
Account `default`) via `persistence/keychain`.

**`keychain`** — Plugin-only. Umhüllt macOS `Security.framework`
(`SecItemAdd`, `SecItemCopyMatching`, `SecItemDelete`) als generisches
`(service, account) → secret`-Interface. Zweimal genutzt: für den
OpenAI-Key und für den Mistral-Key, sodass beide koexistieren und ein
Moduswechsel nie erneutes Einfügen verlangt. Loggt nur die letzten 4
Zeichen des Keys; ein vollständiger Key-Wert darf nie in `Log.txt`
erscheinen.

> **Hinweis Runtime-Identifier (`...devfr...` bewusst beibehalten).** Bei
> der Umbenennung des Projekts auf `xp_wellys_vfr_atc` wurden vier Klassen
> persistenter Laufzeit-Identifier **absichtlich** auf dem alten Namen
> belassen, damit Bestandsinstallationen nichts verlieren: (1) die
> Keychain-Services `com.xp_wellys_devfr_atc.{openai,mistral}` (sonst
> müssten Nutzer ihre API-Keys neu eingeben), (2) die Plugin-Signature
> `ch.thWelly.wellys_devfr_atc` (sonst behandelt X-Plane das Plugin als
> neues Plugin), (3) die X-Plane-Command-Namen
> `xp_wellys_devfr_atc/{ptt,atc_panel,transcript}` (sonst brechen
> Tastenbelegungen), und (4) die User-State-Verzeichnisse
> `Output/preferences/xp_wellys_devfr_atc` + `Output/xp_wellys_devfr_atc`
> (halten `airspace.txt`-Override, Custom-VRPs, Flight-Logs — „survives
> plugin re-installs"). Diese Strings NICHT „aufräumen".

**`models_catalog`** — SDK-freier Loader für `data/models_catalog.json`.
Exponiert die Per-Backend-STT/LM/TTS/Voice-Optionen. Die Settings-Dropdowns
und die Download-Liste im Models-Tab werden davon getrieben. Einmal beim
Start gelesen; nach dem Editieren der JSON X-Plane neu starten.

**`ptt_input`** — Erkennt die PTT-Aktivierung über den X-Plane-Befehl
`xp_wellys_devfr_atc/ptt`. Benachrichtigt `atc_session` bei Press/Release.

**`audio_recorder`** — Core-Audio-`AudioUnit` erfasst das Mic mit 16 kHz
mono 16-bit PCM in `std::vector<int16_t>`. Bei PTT-Release übergibt es den
PCM-Puffer an die aktive STT-Strategie via `backends::stt()` — die
konkrete Backend-Wahl ist hier unsichtbar. Kein WAV-Datei-Roundtrip.

**`backends/i_speech_to_text`, `i_language_model`, `i_text_to_speech`** —
Rein virtuelle Strategie-Interfaces. Engine-Code spricht ausschliesslich
mit diesen — siehe **Backend Adapter Rule** unten.

**Lokale Backends** (`whisper_stt`, `llama_lm`, `piper_tts`) — alle
gegated durch `XPWELLYS_USE_LOCAL_INFERENCE`, emittieren
`[STT|LM|TTS]-LOCAL`-Audit-Tags. Modell-Dateien (Pfad + SHA256) kommen aus
`models_catalog` via `model_manifest`; das aktive Whisper-File ist das
multilinguale `de`-Modell, das Llama-File ist multilingual + geteilt.
`whisper_stt` lädt mit Metal-Beschleunigung und dem `whisper_prompt`-
Luftfahrt-Bias. `llama_lm` läuft mit max_tokens ≈ 20, temp 0.0 und nutzt
den `gpt_classify_prompt_de`-System-Prompt. `piper_tts` lädt die
Per-Rolle-Voice-ONNX via Piper + onnxruntime + gebündelte espeak-ng-data;
ATIS nutzt `length_scale=1.18`.

**OpenAI-Backends** (`openai_stt`, `openai_lm`, `openai_tts`) — in beide
Slices gebaut, emittieren `[STT|LM|TTS]-OPENAI`-Audit-Tags. Endpunkte:
`v1/audio/transcriptions` (multipart WAV), `v1/chat/completions`
(JSON-Modus), `v1/audio/speech` (MP3/Opus → PCM). Modell-IDs und
Per-Rolle-Voices kommen aus `settings::openai_*`. `openai_common` liefert
die geteilten libcurl- + JSON-Helfer und kürzt den API-Key für jede
Log-Zeile auf die letzten 4 Zeichen (`sk-...ABCD`).

**Mistral-Backends** (`mistral_stt`, `mistral_lm`, `mistral_tts`) — in
beide Slices gebaut, emittieren `[STT|LM|TTS]-MISTRAL`-Audit-Tags.
Endpunkte: `v1/audio/transcriptions` (Voxtral STT, mit `context_bias[]`-
Flugplatz-Biasing), `v1/chat/completions` (JSON-Modus), `v1/audio/speech`
(Voxtral TTS — Achtung: liefert ein **JSON-Envelope** `{"audio_data":
"<base64 WAV>"}` unabhängig vom angeforderten `response_format`; das
innere WAV ist 24 kHz mono 16-bit, in-process dekodiert). Modell-IDs und
Per-Rolle-Voices aus `settings::mistral_*`. Nutzt `openai_common`s
libcurl-/JSON-Helfer, wo die Form identisch ist; Key auf `...ABCD` gekürzt
(kein `sk-`-Präfix — Mistral-Keys sind nicht OpenAI-formatiert).

**`backends/manager`** — SDK-freier `std::thread`-Dispatch + Status-Atomics.
Lebt in der Engine-OBJECT-Lib, damit das headless `atc_repl` es ohne
registriertes konkretes Backend wiederverwenden kann.

**`backends/loader`** — Plugin-seitig, modus-bewusstes Bring-up auf einem
Worker-Thread, getrieben von `settings::backend_mode()`. `"openai"` →
liest den OpenAI-Keychain-Eintrag (oder meldet einen "no key"-Fehler) und
registriert `OpenAi{Stt,Lm,Tts}`. `"mistral"` → liest den
Mistral-Keychain-Eintrag und registriert `Mistral{Stt,Lm,Tts}`. `"local"`
(verlangt `XPWELLYS_USE_LOCAL_INFERENCE`) → verifiziert SHA256-Hashes via
`model_manifest` und registriert `{WhisperStt,LlamaLm,PiperShim}`. Der
x86_64-Slice schreibt `local` → `openai` still um und persistiert
(`mistral` wird auf beiden Slices honoriert). Loggt ein einzeiliges
`BACKEND MODE: ...`-Banner — der Audit-Anker.

**`backends/downloader`** — Plugin-seitig. libcurl-HTTPS-GET mit
`Range`-Resume, direkt auf das Installationsvolume gestreamt.
SHA256-verifiziert vor dem Umbenennen `<file>.part` → finaler Dateiname.
Nur vom Local-Modus für HuggingFace-Modell-Fetches genutzt; berührt nie
die Cloud-APIs.

**`intent_parser` / `intent_rules`** — Regelbasiertes Keyword-/Pattern-
Matching auf Transcript + `XPlaneContext`. `intent_parser` ist der
Lexer/Scorer; `intent_rules` trägt die Regeltabellen. Liefert
`PilotMessage` (Intent + Konfidenz). Sub-Varianten wie
`INITIAL_CALL_{GROUND,TOWER,INBOUND}`, `REPORT_POSITION_{DOWNWIND,BASE,
FINAL}`, `READY_FOR_DEPARTURE_VFR` sind erstklassig.

**`atc_templates`** — JSON-Template-Engine. Lädt `atc_templates.json` beim
Init. `lookup(is_towered, state, intent_key)` mit `_INVALID`-Fallback;
`fill(template, vars)` zur Substitution. Hot-Reload via `reload()`.

**`flight_phase`** — Geometrische Phasen-Erkennung (`PARKED`, `TAXI`,
`TAKEOFF_ROLL`, `CLIMB`, `PATTERN`, `FINAL_APPROACH`, `LANDING_ROLL`,
`CRUISE`) aus Groundspeed + AGL + Heading; Triebwerkszustand ignoriert.
Schwellwerte + Hysterese aus `flight_rules.json`. Bietet
`check_precondition`, `check_frequency_precondition`,
`get_auto_corrections`. Hot-Reload via `reload()`.

**`atc_state_machine`** — Besitzt den aktuellen `ATCState`. Auf einen
gültigen `PilotIntent` wendet es zwei Vorbedingungs-Guards an
(Flugphase, dann Frequenz) aus `flight_rules.json`, schlägt dann das
Template nach und liefert `ATCResponse`. Tower-only-Flugplätze nehmen
Ground-Klasse-Intents auf der TOWER-Frequenz aus. Zudem:
`check_auto_correction(phase, dt)`, `build_vars()`, `state_from_name()`,
`set_state()`, und der BZF-Strict-Check (`apply_bzf_strict_check()`) beim
READBACK-Intent.

**`initial_call_conformance`** — SDK-freier, datengetriebener
BZF-Erstanruf-Pflichtinhalt-Check. Lädt `data/atc_profiles/de/conformance.json`:
pro Intent **zwei Sets** — `required` (NfL-zwingende Elemente) und `recommended`
(didaktische BZF-Vollform). `bzf_strict_mode` wählt das harte Set: strict=false
erzwingt nur `required`, strict=true `required ∪ recommended`. `evaluate()` ist eine
reine Detektion (position via Parser-Signal, atis_letter via „Information"+NATO-
Buchstabe, intention/aircraft_type via `element_keywords`/Live-acf_ICAO);
`build_request_prompt()` baut die gezielte Nachforderung. Eingehängt als
`ground_ops::apply_initial_call_conformance()`-Guard in `process()` **vor** dem
Template-Lookup. Aktuell verdrahtet/getestet: nur `INITIAL_CALL_GROUND` (dort ist
`required` leer → Simulator-Verhalten unverändert, Nachforderung nur im Trainer); die
Datenstruktur ist generisch für spätere Intents. **Befund (NfL 2024 §1.4.7/§1.4.3,
Primärquelle unter `docs/bzf/`):** im Erstanruf ist nur das Roll-Anliegen (ERBITTE
ROLLEN, separater `REQUEST_TAXI`-Sprechakt) zwingend; Luftfahrzeugmuster/Standort/
Absichten/ATIS sind [optional] — die „5-Pflichtelemente" sind BZF-Didaktik, kein
NfL-Pflichtumfang (eckige Klammern = optional). **H2-Invariante:** der
Erstkontakt-Hint (`flight_rules.json` `pilot_phraseology`) ist Lehrautorität und muss
deckungsgleich mit dem `recommended`-Set sein, sonst wird der Schüler im Trainer für
das verbatim Abgelesene nachgefordert; `tests/test_initial_call_conformance.cpp`
erzwingt das (rendert den Hint → `evaluate` → `missing_recommended` leer). Daher
gehört „ERBITTE ROLLEN" NICHT in den Erstkontakt-Hint, sondern zu `REQUEST_TAXI`.

**`atc_session`** — Besitzt die PTT-State-Machine
(`IDLE → RECORDING → PROCESSING → PLAYING`). Koordiniert die volle
Pipeline mit zweistufiger Absichts-Auflösung: hochkonfidente Intents
(≥0.7) gehen direkt durch die State-Machine; niedrigkonfidente oder
UNKNOWN-Intents laufen über das LM-Strategie-Interface (`backends::lm()`)
zur Klassifizierung — ob das auf lokales Llama, `gpt-4o-mini` oder
`mistral-small` auflöst, ist hier unsichtbar. Blockt neuen PTT-Input
während `PROCESSING` oder `PLAYING`. Exponiert auch `submit_text(text)` —
den Debug-Texteingabe-Einstieg (gegated durch `settings::debug_text_input`),
der STT umgeht und ein getipptes Transcript direkt in
`engine::process_transcript` speist; LM + State-Machine + TTS laufen
identisch zum Sprachpfad. Der State-Revert-/TTS-Fehler-Guard (Snapshot
vor jedem Pilotenzug, Restore-oder-`REQUEST_REPEAT` bei TTS-Fehler mit
Squelch-Burst) lebt ebenfalls hier — siehe README für den
Verhaltensvertrag.

**`cross_country_log`** — SDK-freier, **rein beobachtender** Funk-Logger +
ATC-Flug-Logbuch für die Cross-Country-Messsession. Schreibt **ein
gültiges, hübsch formatiertes JSON-Dokument pro Flug** nach
`<plugin>/data/flightlog/YYYY-MM-DD_HHMM_<AIRPORT>.json` (Verzeichnis via
`set_dir`, vom Plugin in `main.cpp` auf `settings::get_data_dir()`
gesetzt). Das ganze Dokument wird **nach jeder Funke** atomar (Temp-Datei
+ `rename`) neu geschrieben — es ist also stets vollständig und valide
samt aktuellem `summary`; ein „Flug-fertig"-Event ist nicht nötig.
Aufbau (`version` 2): `{ version, flight{started_at, started_at_epoch,
departure_airport, pilot_callsign}, summary{transmissions, classified,
unknown, garbled, lm_fallbacks, readback_issues, phases[]},
transmissions[] }`. `started_at_epoch` (Unix-Epoch-Sekunden, UTC) und das
Per-Funke-Feld `ts` (ebenfalls Epoch, UTC) wurden additiv ergänzt, damit
der Trainer die Funken ohne TZ/DST-Rekonstruktion gegen das xp_pilot-
Flight-Log korrelieren kann (Issue #17); die Lokalzeit-Strings `started_at`
/ `time` bleiben unverändert.
`engine::process_transcript` fädelt die Per-Funke-Felder an jedem
Ausstiegspunkt zusammen, **nachdem** `outcome` feststeht — ohne Eingriff
ins Matching, Routing oder die Klassifikation. Funke-Felder: `time`, `ts`,
`transcript`/`quality`, `intent`/`confidence`, `path` (`rule_skip_lm` |
`lm_fallback` | `clearance_match`), `lm_used` + `lm_backend`/`lm_ready`,
`outcome` (`classified` | `unknown` | `tower_reported_garbled`),
`state`/`flight_phase` (zum Zeitpunkt der Funke gesnapshottet),
`expected_intent` (valid_intents-CSV), `vrp_name_set`/`vrp_name`,
`readback_missing_elements` (nur READBACK) und `failure_locus` — ein
**unverbindlicher** Heuristik-Vorschlag, nur gesetzt bei `outcome !=
classified` oder LM-Fallback. **Flug-Trennung** ist reine Logging-Logik
(kein Verhaltens-Change): airborne gewesen + wieder am Boden in `IDLE` →
neuer Abflug → neue Datei; Touch-and-Go/Platzrunde bleibt ein Flug;
Cross-Country-Flugplatzwechsel rotiert nicht. `begin_new_flight()`
erzwingt eine neue Datei — verdrahtet an den `XPLM_MSG_AIRPORT_LOADED`/
`PLANE_LOADED`-Auto-Reset in `main.cpp` und den „Neuer Flug"-Button im
Settings-Tab. Das Backend-Label setzt der `loader` per
`cross_country_log::set_lm_backend(mode)`; Engine-Code inspiziert
`backend_mode` nie selbst (siehe **Backend Adapter Rule**). Details:
README.

**`audio_player`** — Spielt PCM direkt auf dem X-Plane-Funkbus,
respektiert `settings.volume`.

**Verkehrs-Subsystem** (v2.1 Hinweise + v2.2 Sequenzierung) —
Provider-unabhängiger 2-Hz-`TrafficContext`-Snapshot aus
`sim/cockpit2/tcas/targets/...` (funktioniert mit Stock, LiveTraffic,
xPilot usw.). `traffic_geometry` berechnet Relativ-Peilung /
Uhrzeit-Position / Slant-Range plus die Phase-4-Runway-Centerline-
Projektion (`is_on_runway_centerline`). `traffic_advisor` baut die
Verkehrshinweise (deutsche Phraseologie aus dem DE-Profil) und dispatcht
sie über das TTS-Strategie-Interface auf einem **Seitenkanal**, der den
ATC-Hauptablauf nicht blockt; Cooldown + Dedup eingebaut.
`traffic_dialog` parst die Piloten-Antwort in ein `TrafficReply`-Enum.
`traffic_phase_classifier` hebt Airborne-Ziele auf `Pattern` / `Final`,
wenn die Live-`AirportRunwayHints` (aktive-Pisten-Schwelle + Heading)
passen. `landing_sequence::compute_landing_sequence()` ist eine reine
Funktion, die Final-Phase-Ziele nach Distanz zur Schwelle sortiert, die
1-basierte Sequenzposition des Nutzers ableitet und nach
Ground-Phase-Belegern auf der Runway-Centerline scannt.
`pattern_flow::apply_landing_sequence()` ist das Pattern-Overlay, das
`Pattern/LANDING_CLEARED`-Responses umschreibt, wenn Sequenzierung
greift. `engine::poll_go_around()` ist der frame-getriebene
unaufgeforderte Tower-Call innerhalb 1 NM einer belegten Piste
(render-only, keine State-Änderung, 60 s Cooldown). **Hauptschalter**
`settings::traffic_features_enabled` (Standard `true`) gated das ganze
Subsystem an einer Stelle: `traffic_context::update()` kehrt mit leerem
Snapshot zurück, wenn aus, sodass jeder nachgelagerte Konsument zum No-op
wird.

**`atc_ui`** — Dear-ImGui-Fenster. Status-Panel, Frequenzen-Panel,
Phraseologie-Hinweise, Transkript-Historie mit optionaler
**Debug-Texteingabe**-InputText-Zeile (Toggle:
`settings::debug_text_input`, ruft `atc_session::submit_text`),
**Settings-Tab** (Backend-Mode-Switcher mit separaten `[Paste]` /
`Save Key` / `Delete Key`-Buttons für OpenAI- und Mistral-Key — Cmd+V
wird von X-Plane-Befehlsbindungen abgefangen; Per-Backend-STT/LM/TTS/
Voice-Combos aus `models_catalog`; der BZF-Strict-Mode-Toggle ist nur im
DE-Profil sichtbar — im EN-/ICAO-Profil ist er ausgeblendet, weil die
Strict-/Conformance-Hooks auf `atc_profile()=="DE"` gaten und dort inert
wären), **Models-Tab** (Download / Re-Verify / Fortschritt; relevant
im Local-Modus, in allen Modi sichtbar) und ein optionaler
**Traffic-Tab** (Debug, gegated durch `settings.debug_traffic`, listet
die 10 nächsten Flugzeuge). Alle Strings werden über `ui_strings` gegen
`data/atc_profiles/de/ui_strings.json` aufgelöst. Die `[Paste]`-Buttons
lesen das System-Pasteboard via `ui/clipboard`.

---

## Backend Adapter Rule (HARTE INVARIANTE)

Engine-Code spricht ausschliesslich mit den drei Strategie-Interfaces
`backends/i_{speech_to_text,language_model,text_to_speech}.hpp`. Die
Modus-Entscheidung lebt an **genau einer Stelle**:
`backends/loader.cpp::run_worker()`. Engine-Code DARF
`settings::backend_mode()` NICHT inspizieren oder darauf branchen — wenn
du das willst, fehlt dem Interface eine Abstraktion.

**Quellcode-Invarianten, erzwungen durch `tests/test_audit_logging.cpp`**
(wörtliches `grep` gegen die .cpp-Dateien zur `make test`-Zeit):

- `whisper_stt.cpp` / `llama_lm.cpp` / `piper_tts.cpp` — müssen ihren
  `*-LOCAL`-Audit-Tag tragen; dürfen NICHT `OPENAI`, `MISTRAL`,
  `api.openai.com`, `api.mistral.ai` oder `curl_easy_perform` enthalten.
- `openai_stt.cpp` / `openai_lm.cpp` / `openai_tts.cpp` — müssen ihren
  `*-OPENAI`-Audit-Tag tragen; dürfen NICHT `whisper.h`, `llama.h` oder
  `piper.h` `#include`n; dürfen keinen `-LOCAL]`- oder `-MISTRAL]`-Tag
  enthalten.
- `mistral_stt.cpp` / `mistral_lm.cpp` / `mistral_tts.cpp` — müssen ihren
  `*-MISTRAL`-Audit-Tag tragen; dürfen NICHT `whisper.h`, `llama.h` oder
  `piper.h` `#include`n; dürfen keinen `-LOCAL]`- oder `-OPENAI]`-Tag
  enthalten und nicht `api.openai.com` referenzieren.

Ein versehentlicher Include oder ein kopierter Log-Tag bricht die CI.

**Build-Zeit-Durchsetzung:** die CMake-Option `XPWELLYS_USE_LOCAL_INFERENCE`
steuert, ob die drei lokalen `.cpp`-Dateien überhaupt zum Build
hinzugefügt werden. Der x86_64-Slice wird mit diesem `OFF` gebaut und hat
null whisper/llama/piper-Symbole. Die Header der lokalen Backends DÜRFEN
NICHT ausserhalb eines `#ifdef XPWELLYS_USE_LOCAL_INFERENCE`-Blocks
`#include`d werden — der einzige legitime Konsument ist
`backends/loader.cpp`. Die zwei Cloud-Backend-Familien (OpenAI + Mistral)
werden unbedingt in beide Slices kompiliert.

**Ein backend-berührendes Feature ergänzen:** zuerst das `i_*.hpp`-Interface
erweitern, dann symmetrisch in allen drei Familien implementieren (Local
UND OpenAI UND Mistral). Jeder Inferenz-Aufruf emittiert eine
`[STT|LM|TTS]-[LOCAL|OPENAI|MISTRAL]`-Log-Zeile; das Startup-
`BACKEND MODE: ...`-Banner ist der dauerhafte Audit-Anker.

---

## Wichtige Datenstrukturen

Die massgeblichen Deklarationen liegen in `src/core/xplane_context.hpp`
und `src/atc/{intent_parser,atc_state_machine}.hpp`. Skizze zur schnellen
Orientierung:

```cpp
struct XPlaneContext {                  // src/core/xplane_context.hpp
    // Position + Dynamik
    double latitude, longitude;
    float  altitude_ft_msl, height_agl_ft;
    float  groundspeed_kts, indicated_airspeed_kts, vertical_speed_fpm;
    float  heading_true;
    bool   on_ground;
    // Funkgeräte
    float  com1_freq_mhz, com2_freq_mhz;
    int    active_com;                              // 1 oder 2
    // nächster Flugplatz (pro Frame abgeleitet)
    std::string aircraft_icao, nearest_airport_id;
    bool        is_towered_airport, tower_only;
    FrequencyType frequency_type;                   // gegen airport_freqs gematcht
    AirportFrequencies airport_freqs;               // 50-55 / 1050-1055
    double airport_lat, airport_lon;
    std::vector<RunwayInfo> runways;
    std::string active_runway;
    // Wetter (für ATIS)
    float visibility_m, cloud_base_ft_msl;
    int   cloud_type;                               // 0=clear..4=overcast
    float temperature_c, dewpoint_c, atis_freq_mhz;
};

enum class FlightPhase { PARKED, TAXI, TAKEOFF_ROLL, CLIMB,
                         PATTERN, FINAL_APPROACH, LANDING_ROLL, CRUISE };

enum class PilotIntent { UNKNOWN, RADIO_CHECK,
    INITIAL_CALL, INITIAL_CALL_GROUND, INITIAL_CALL_TOWER, INITIAL_CALL_INBOUND,
    REQUEST_TAXI, READY_FOR_DEPARTURE, READY_FOR_DEPARTURE_VFR,
    REPORT_POSITION, REPORT_POSITION_DOWNWIND, REPORT_POSITION_BASE, REPORT_POSITION_FINAL,
    REQUEST_LANDING, REQUEST_TOUCH_AND_GO, GO_AROUND, RUNWAY_VACATED,
    READBACK, REQUEST_FREQUENCY, LEAVING_FREQUENCY, UNABLE, SELF_ANNOUNCE };

struct PilotMessage { std::string raw_transcript, callsign, runway;
                     PilotIntent intent; float confidence; };
struct ATCResponse  { std::string text; ATCState next_state; bool requires_readback; };
```

`AirportFrequencies` exponiert `has(FrequencyType)`, `first_mhz(...)`,
`lookup(float mhz)` (liefert `UNKNOWN` bei Miss), `has_ground()`.

---

## ATC-State-Machine-Zustände

```
IDLE
GROUND_CONTACT → TAXI_CLEARED → TOWER_CONTACT
TOWER_CONTACT  → DEPARTURE_CLEARED / PATTERN_ENTRY / TOUCH_AND_GO_CLEARED
DEPARTURE_CLEARED (pattern) → PATTERN_ENTRY (Auto-Korrektur nach Start)
DEPARTURE_CLEARED (cross-country) → REQUEST_FREQUENCY / LEAVING_FREQUENCY → EN_ROUTE → (Flugplatzwechsel) → IDLE
PATTERN_ENTRY  → LANDING_CLEARED / TOUCH_AND_GO_CLEARED / GO_AROUND → PATTERN_ENTRY
TOUCH_AND_GO_CLEARED → PATTERN_ENTRY / LANDING_CLEARED / GO_AROUND → PATTERN_ENTRY
LANDING_CLEARED → RUNWAY_VACATED → IDLE / GO_AROUND → PATTERN_ENTRY
EN_ROUTE       → (still, kein ATC-Kontakt) → IDLE bei Flugplatzwechsel
UNICOM_ACTIVE  → IDLE
```

Kontrollierte Flugplätze nutzen den GROUND/TOWER-Ablauf. Unkontrollierte
Flugplätze nutzen `UNICOM_ACTIVE` (nur Selbstansage-Bestätigung, keine
Freigaben).

---

## Inferenz-Pipelines

Drei Backends, dasselbe `i_*.hpp`-Interface. Die LM-Stufe wird **nur**
aufgerufen, wenn `intent_parser` Konfidenz < 0.7 liefert — hochkonfidente
Intents überspringen sie ganz. Jede Inferenz läuft auf `std::thread`s mit
`std::atomic`-Status; der X-Plane-Main-Thread wird nie blockiert.

- **Local** (arm64, `XPWELLYS_USE_LOCAL_INFERENCE=ON`): whisper.cpp →
  llama.cpp (max_tokens ≈ 20, temp 0.0) → Piper. Warme Pipeline ≈ 1,16 s
  auf M4. Modelle in `<plugin>/Resources/models/`, beim ersten Start via
  HuggingFace HTTPS+`Range`+SHA256 geholt. Siehe `README.md` für
  Modell-URLs und Hashes.
- **OpenAI Cloud** (beide Slices): `whisper-1` → `gpt-4o-mini` (JSON-Modus)
  → `tts-1`. Latenz typisch 2–3 s warm. Stimmen je Rolle wählbar.
  API-Key im Keychain, letzte 4 Zeichen geloggt.
- **Mistral Cloud** (beide Slices): `voxtral-mini-2507` →
  `mistral-small-latest` (JSON-Modus) → `voxtral-mini-tts-2603`. Latenz
  typisch 2–3 s warm. Multilinguales TTS (spricht u. a. Deutsch), aber
  **keine deutsche Preset-Stimme** — die Default-Voices sind englisch
  (`en_paul`/`gb_oliver`), deutscher Text klingt daher US-akzentuiert
  (Issue #63). Sauberes Deutsch nur über **Local** (Piper `thorsten`).
  API-Key im Keychain, letzte 4 Zeichen
  geloggt. Achtung: Voxtral TTS liefert ein JSON-Envelope
  `{"audio_data":"<base64 WAV>"}` unabhängig vom `response_format`.

---

## Settings (data/settings.json)

```jsonc
{
  "ptt_key_vk": 49,
  "ptt_joystick_button": -1,
  "pilot_callsign": "November One Two Three Alpha Bravo",
  "active_com": 1,
  "volume": 1.0,

  "atc_profile": "DE",                      // fest "DE" (NfL DACH-VFR)
  "start_mode": "engines_running",          // "engines_running" | "cold_and_dark"

  "pattern_direction": "left",
  "disable_default_atc": false,
  "skip_radio_power_check": false,
  "show_phraseology_hints": true,
  "auto_correction_factor": 1.0,

  "debug_logging": false,
  "debug_traffic": false,
  "debug_text_input": false,                // Status-Tab InputText -> atc_session::submit_text

  "traffic_features_enabled": true,         // Hauptschalter — Advisor / Sequencing / Go-Around
  "bzf_strict_mode": false,                 // NfL §25 b) Nr. 1 Readback-Prüfung

  "backend_mode": "local",                  // "local" | "openai" | "mistral"

  // OpenAI-Key im Keychain @ com.xp_wellys_devfr_atc.openai / default
  "api_key_saved": false,
  "openai_stt_model": "whisper-1",
  "openai_lm_model": "gpt-4o-mini",
  "openai_tts_model": "tts-1",
  "openai_tts_voice_atis": "onyx",
  "openai_tts_voice_tower": "echo",
  "openai_tts_voice_ground": "alloy",

  // Mistral-Key im Keychain @ com.xp_wellys_devfr_atc.mistral / default
  "mistral_api_key_saved": false,
  "mistral_stt_model": "voxtral-mini-2507",
  "mistral_lm_model": "mistral-small-latest",
  "mistral_tts_model": "voxtral-mini-tts-2603",
  "mistral_tts_voice_atis": "gb_oliver_neutral",
  "mistral_tts_voice_tower": "en_paul_confident",
  "mistral_tts_voice_ground": "en_paul_neutral"
}
```

`settings.json` **ist eingecheckt** mit sinnvollen Standards und enthält
in keiner Revision Geheimnisse. **Kein API-Key wird je hier persistiert**
— sowohl der OpenAI- als auch der Mistral-Key liegen im macOS Keychain
unter separaten Services (`com.xp_wellys_devfr_atc.openai` und
`com.xp_wellys_devfr_atc.mistral`, Account `default`) und werden über die
`[Paste]` / `Save Key` / `Delete Key`-Buttons im Settings-Tab verwaltet.
Push-to-Talk ist über den X-Plane-Befehl `xp_wellys_devfr_atc/ptt` (Tastatur
oder Joystick) gebunden.

Die Modell- und Voice-Dropdowns im Settings-Tab werden aus
`data/models_catalog.json` befüllt — editiere diese Datei (nicht
`settings.json`), um einen neuen OpenAI-/Mistral-Slug oder eine neue
Piper-Voice zu ergänzen. Nach dem Editieren X-Plane neu starten; der
Katalog wird einmal beim Start gelesen.

Der x86_64-Slice schreibt `backend_mode: "local"` beim Start automatisch
auf `"openai"` um (der cloud-only Slice hat keine lokalen Backends);
`mistral` wird auf beiden Slices honoriert. Siehe
`backends/loader::run_worker()`.

---

## Coding-Konventionen

- C++17, keine Exceptions über die Plugin-Grenze — alles in `main.cpp` fangen
- Alle X-Plane-API-Aufrufe nur auf dem Main-Thread
- Alle Inferenz-/Netzwerk-/Schwerarbeit auf `std::thread` — `std::atomic`-
  Flags für Status; nie den X-Plane-Main-Thread blockieren
- `XPLMDebugString` für alles Logging (Ausgabe → X-Plane `Log.txt`).
  **Nur reines ASCII (0x20–0x7E)** — sowohl `XPLMDebugString` als auch die
  In-Sim-ImGui-Schrift rendern UTF-8-Sonderzeichen als `?`
- `nlohmann::json` für alles JSON-Parsing
- clang-format + clang-tidy erzwungen (`make format`, `make lint`)
- Keine Exceptions in Destruktoren
- Jeder Modul-Header ist eigenständig — keine zirkulären Includes
- `make` für Build, Lint, Release nutzen
- Clean-Code-Best-Practice — einfach lesbar halten
- Tiefe `if`/`switch`-Verschachtelung vermeiden — Helfer extrahieren, wenn es lang wird
- Die Engine-OBJECT-Bibliothek muss SDK-frei bleiben — jede TU, die
  `<XPLM*.h>` zieht, gehört ins Plugin-Modul
- Bei Backend-Code ausschliesslich über `i_speech_to_text` /
  `i_language_model` / `i_text_to_speech` gehen. Nie aus Engine-Code in ein
  konkretes Backend greifen und nie ausserhalb `backends/loader.cpp` auf
  `backend_mode` branchen. Neue backend-berührende Features symmetrisch in
  allen drei Familien (Local, OpenAI, Mistral) implementieren. Siehe
  **Backend Adapter Rule** — `tests/test_audit_logging.cpp` bricht die CI
  bei einem Cross-Include oder einem versehentlichen Log-Tag.
