### Welly's VFR ATC – AI voice ATC plugin for X-Plane 12

Native plugin for **macOS** and **Windows**. Simulates VFR radio communication: you speak, the plugin understands your call and ATC answers with a voice — speech to text, intent recognition and text to speech. German (DACH) and ICAO English phraseology.

### What's New in the next release (version to be decided)

  - **Helicopters are now tolerated.** Stage 1 of helicopter support; helicopter-specific phraseology and helipads follow later.
    - Detected automatically from the aircraft data, with a new **Aircraft class** setting (Automatic / Airplane / Helicopter) for misdetected add-ons.
    - Hover and hover-taxi near an airport count as ground movement, so ground calls are no longer rejected with "you are airborne".
    - Approaches that are not aligned with the runway are recognised as final, also at heliports without runways.
    - No go-around calls or landing-sequence rewrites because of runway traffic.
  - **Microphone selection.** A new **Audio** tab lets you choose the input device; the default is the system microphone. X-Plane's Radio Device only covers output.
  - **Microphone capture fixed on macOS.** The microphone was captured in a mode that repeated audio chunks, which can garble spoken calls. It now records at the device's own rate.
  - **Silent recordings are reported.** If the microphone delivers no signal, the plugin now says so instead of answering "say again".
  - **Pre-flight flight type is respected.** A pattern flight stays a pattern flight at departure unless you state a destination by its ICAO code. In the English profile, every call to a "Tower" was mistaken for a cross-country departure.
  - **English sign-off.** "Change frequency" is now understood as leaving the frequency, and on a cross-country departure the sign-off hint also appears while still near the airport.
  - **"Rollbereit" is understood.** Answering ATC's "melden Sie sich rollbereit" with that word now requests taxi instead of repeating the prompt.
  - **Phraseology language switches immediately.** No restart needed; the backends reload, which takes a moment. This also fixes the mix of German and English radio calls after switching. The setting is locked during a transmission.

### What's New in v1.0.1

  - **First public release.** `xp_wellys_atc` is the continuation of `xp_wellys_vfr_atc` — same plugin, new repository name. The plugin folder is still called `xp_wellys_vfr_atc`.
  - **VFR only for now.** No IFR, no flight plans, no FMS routing. IFR may follow in a later version.
  - **Two phraseology profiles**, switchable at runtime:
    - **German** (default) — DACH VFR phraseology (NfL Sprechfunk 2024) with an optional strict BZF mode.
    - **English** — ICAO VFR phraseology.
  - **Traffic patterns** — entry, downwind, base, final, landing, touch-and-go, go-around, landing sequence.
  - **Cross-country** — departure clearance, frequency changes, handoffs, inbound.
  - **Airport types** — uncontrolled (UNICOM/CTAF), Tower, Tower + Ground, AFIS.
  - Automatic ATIS from the sim weather, traffic information from TCAS, phraseology hints in the UI.
  - **Backends** — fully offline on your machine (Apple Silicon with Metal, Windows with Vulkan), or OpenAI / Mistral with your own API key.
  - Replaces v1.0.0, whose SkunkCrafts updater still pointed to the old repository URL.
