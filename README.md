# Welly's VFR ATC — AI Voice ATC for X-Plane 12

![Welly's VFR ATC panel with ATIS broadcast at LSZB Bern-Belp](images/atc-atis-example.jpg)

> **Talk to the tower via push-to-talk — AI-powered VFR radio in German or
> English for X-Plane 12, running locally on your machine or through the
> cloud.**

Welly's VFR ATC turns your VFR flights in X-Plane 12 into a real radio
conversation: you press the push-to-talk key, speak your call into the
microphone, and the tower answers you back by voice — in either **German
(NfL/BZF)** or **English (ICAO)** VFR phraseology, with realistic reactions
even to pilot mistakes.

> ### 🛩️ This is the **VFR** plugin
>
> Pattern work, cross-country, UNICOM/CTAF, AFIS, VFR reporting points,
> traffic advisories — **VFR radio is what this plugin does, and this is
> where all VFR development happens.** It is the home of the German
> NfL/BZF profile and the BZF strict mode.
>
> **Looking for IFR?** Instrument flight is a separate product with its own
> plugin: **[Welly's IFR ATC](https://github.com/rwellinger/xp_welly_llm_atc)**
> — IFR clearance delivery, SID/STAR out of X-Plane's CIFP data, SimBrief
> route integration, en-route sector handoffs, approach and landing.
> English/ICAO only. This plugin has no IFR flow at all — see
> [What it can't do (yet)](#what-it-cant-do-yet).

---

## What the plugin covers

- **🇩🇪 🇬🇧 Two language profiles — DE (NfL/BZF) & EN (ICAO)** — switchable
  at runtime in the settings; **German** stays the default. **German**
  models VFR radio phraseology per **NfL Sprechfunk 2024** (DACH region)
  with an optional **BZF strict mode** that checks your readbacks for
  completeness the way an examiner would. **English** follows the
  self-contained **ICAO VFR phraseology** (ICAO Annex 10 Vol II / Doc 4444 /
  EASA SERA) — not a translation of the German profile, but its own standard
  wording and number/callsign pronunciation. The **operating interface**
  (menus, buttons, labels) has its own language switch, independent of the
  spoken phraseology — so you can run the UI in English while training German
  NfL radio, and the switch applies instantly without a restart.
- **🛬 Traffic pattern** — the full pattern flow at controlled and
  uncontrolled airfields: entry, downwind, base, final, landing,
  touch-and-go, go-around — including landing sequencing ("you are number
  two, follow the traffic").
- **🗺️ Cross country** — the complete cross-country flight: departure
  clearance, en-route frequency changes and the approach flow between
  airfields. The approach controller proactively hands you off to the tower
  with the destination frequency.
- **🗼 Airfield types** — the plugin automatically detects the kind of air
  traffic service at each airfield and adapts phraseology and flow
  accordingly:
  - **Uncontrolled** — self-information via UNICOM/CTAF (self-announcements,
    no clearances).
  - **With tower** — controlled field with clearances over the tower
    frequency.
  - **With tower and ground** — additionally a separate ground control for
    taxi clearances.
  - **With AFIS** — airfield with a flight information service (information
    rather than control): traffic and airfield information without binding
    clearances.
- **🎙️ Voice radio** — natural speech via push-to-talk (keyboard or
  joystick). Context-aware **phraseology hints** show you what to say, and
  the tower politely coaches you on poor radio discipline.
- **🤖 AI support** — speech recognition → intent understanding → speech
  synthesis, plus automatic **ATIS broadcasts** from live weather and
  **traffic advisories** about surrounding aircraft.
- **💻 Local AI & ☁️ Cloud AI** — choose your mode at runtime:
  - **Local** (Apple Silicon; **Windows x64 from v0.8**) — runs **100%
    offline** after a one-time model download. No subscription, no API key, no
    constant internet. GPU-accelerated via **Metal** on Apple Silicon and
    **Vulkan** on Windows.
  - **OpenAI Cloud** (any Mac, any Windows PC) — with your own API key. English
    voices only, so German is spoken with a US accent.
  - **Mistral Cloud** (any Mac, any Windows PC) — with your own API key.
    Multilingual, but Voxtral ships **no German preset voice** (US/GB English +
    French only), so with the default voices German still carries an English
    accent.

  > **For accurate German, use Local mode** (Piper `de_DE-thorsten`, a native
  > German voice). Both cloud providers speak German with an accent — see
  > issue #63.
- **🪟 Windows — all three backends** — X-Plane 12 on Windows is fully
  supported (`win_x64/xp_wellys_vfr_atc.xpl`). The **cloud modes** are verified
  end to end on real Windows 11 hardware (Shadow cloud PC, NVIDIA GPU): a
  complete VFR round trip out of **Friedrichshafen (EDNY)** — plugin loading,
  microphone/PTT, the full STT→ATC→TTS pipeline and the API key in the Windows
  Credential Manager. **Local mode runs on Windows too** (**from v0.8**):
  whisper + llama on the **Vulkan** GPU backend and the accent-free German
  Piper `de_DE-thorsten` voice, with response times comparable to Apple
  Silicon.

  Vulkan rather than CUDA on purpose — no GPU redistributable to ship,
  `vulkan-1.dll` comes with the graphics driver, and it covers AMD/Intel GPUs
  as well. The trade is roughly 10–30 % less throughput than CUDA on the same
  card; note also that the models share VRAM with X-Plane's own Vulkan
  renderer. The artifact ships `piper.dll` + `onnxruntime.dll` alongside the
  `.xpl` (it is not DLL-free); the models download in-sim.

  **Windows prerequisite — Microsoft Visual C++ 2015–2022 Redistributable
  (x64).** The `.xpl` itself is built with the static CRT and needs nothing,
  but the bundled `piper.dll` and `onnxruntime.dll` import `MSVCP140.dll` +
  `VCRUNTIME140.dll`. Most systems already have it (X-Plane 12 and virtually
  every game installs it), so this rarely bites — but when it does, the
  failure is easy to misread: because `piper.dll` is delay-loaded, the plugin
  starts fine and the cloud modes work, and only the **first TTS playback**
  fails with `0xC06D007E` in `Log.txt`. Same error code as a missing DLL next
  to the `.xpl`. If you see it, install the redistributable from Microsoft
  before looking anywhere else.

## A typical VFR flight, end to end

Every one of these calls is spoken — press PTT, talk, listen. The
phraseology hints panel tells you what to say if you are unsure, and the
tower reacts realistically when you get it wrong.

| Phase | What you say | What the tower does |
|---|---|---|
| **Before start-up** | Listen to ATIS | Automatic ATIS broadcast from live sim weather, with information letter |
| **Taxi** | "Ground, D-EABC, at the apron, request taxi for local VFR flight" | Taxi clearance to the holding point, runway and QNH |
| **Departure** | "Ready for departure" | Takeoff clearance with wind — or line-up-and-wait if traffic is in the way |
| **Pattern** | "Downwind runway 14" · "Final" | Pattern acknowledgement, landing sequencing ("you are number two, follow the traffic on left base") |
| **Landing** | "Request touch and go" | Clearance for touch-and-go, full stop, or an unsolicited go-around when the runway is blocked |
| **Cross country** | "Request frequency change" · "Leaving your frequency" | Frequency change approved, en-route traffic advisories, then handoff |
| **Inbound** | "Inbound November, 2000 feet, request joining" | Approach hands you to the tower with the destination frequency; pattern entry instructions |
| **Uncontrolled field** | "Kirchdorf traffic, D-EABC, joining downwind 25" | UNICOM/CTAF self-announcement acknowledged — no clearances, as in reality |

## What Welly's ATC is for

> **This plugin is first and foremost oriented toward REALITY.** The goal is
> to reproduce VFR radio procedures as authentically as possible — in German
> per NfL/BZF, in English per ICAO — so that you can **train and practice for
> future exams and tests, such as the BZF**. We work continuously toward
> bringing the phraseology and the ATC flows even closer to real practice.

**Disclaimer.** Welly's ATC is a practice and training tool for flight
simulation. It is **not an official certification, not an educational
resource in the sense of accredited training, and not a substitute for real
exam preparation**. We accept **no responsibility and give no guarantee
whatsoever regarding passing any test or exam**. Use is at your own risk; no
warranty is given for the correctness of the phraseology shown. Corrections
from BZF holders are expressly welcome.

## What it can't do (yet)

- **No IFR — by design, it lives in its own plugin** — no instrument flight,
  no IFR clearances, no flight planning, no FMS/routing here. That feature
  set is a separate product:
  **[Welly's IFR ATC](https://github.com/rwellinger/xp_welly_llm_atc)**
  (SID/STAR from CIFP, SimBrief routes, sector handoffs, approach — English
  only). This plugin stays focused on VFR.
- **German & English, no FR/IT** — VFR phraseology comes as a German
  (NfL/BZF, default) and an English (ICAO) profile. Further languages — such
  as French or Italian for western Switzerland or Ticino — are not planned.
- **No local mode on Intel Macs** — they can use the plugin, but only in cloud
  mode (OpenAI or Mistral, API key required). Local mode runs on Apple Silicon
  (Metal) and Windows x64 (Vulkan).
- **Windows local mode is younger than the macOS one** — it works, but has far
  fewer flight hours behind it. Vulkan compute is also not guaranteed on
  virtualised GPUs (some cloud-PC vGPU profiles); where it is unavailable, ggml
  falls back to CPU rather than failing.
- **Not modeled yet** — wake-turbulence separation, freely selectable taxi
  routes (currently always "via Alpha"), large hub airports (LSZH, LSGG …)
  with a delivery workflow, and a virtual co-pilot / checklist reader.

> A detailed breakdown of the limitations including effort estimates is in
> the [technical documentation](docs/README.md#known-limitations).

## Related plugins

### 🛫 Welly's IFR ATC — the instrument-flight sibling

**[Welly's IFR ATC](https://github.com/rwellinger/xp_welly_llm_atc)** is the
separate plugin for instrument flight. Same voice pipeline and the same
backend choice (Local / OpenAI / Mistral), but a completely different ATC
flow: IFR clearance with squawk and SID, SID climb and en-route sector
handoffs with real controller names, STAR descent, approach and landing —
driven by your **SimBrief** flight plan and X-Plane's own **CIFP** procedure
data, with readback verification and unprompted deviation calls.

Pick by what you fly: **VFR → this plugin. IFR → that one.** They install
side by side. Two differences worth knowing up front: the IFR plugin is
**English/ICAO only** (no German NfL/BZF profile), and it needs some extra
data installed (SimBrief OFP, an OpenAir airspace file).

### 🎯 Welly's VFR Trainer — optional gamification

**[Welly's VFR Trainer](https://github.com/rwellinger/xp_wellys_vfr_trainer)** is
an optional gamification layer on top of Welly's ATC: it suggests VFR flights
in the DACH region by airfield difficulty and, after landing, rates your
flight by time-correlating this plugin's ATC radio calls with the flight data
and having the LLM judge them. Welly's ATC works fully on its own — the
trainer is purely optional.

## 📖 Technical documentation

Installation, quick start, backend modes, building from source, local
inference models, configuration, architecture and development workflow:

**→ [docs/README.md](docs/README.md)**

## License

[GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html)
— details and the breakdown of third-party dependencies are in the
[technical documentation](docs/README.md#license) and in
[`THIRD_PARTY.md`](THIRD_PARTY.md).
