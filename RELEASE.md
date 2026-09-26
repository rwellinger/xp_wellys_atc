### Welly's VFR ATC – AI voice ATC plugin for X-Plane 12

Native plugin for **macOS** and **Windows**. Simulates VFR radio communication: you speak, the plugin understands your call and ATC answers with a voice — speech to text, intent recognition and text to speech. German (DACH) and ICAO English phraseology.

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
