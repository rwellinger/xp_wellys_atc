# Testing — scenario suite under `testscripts/`

`make test` builds `build/atc_repl` and replays every JSON scenario through the
real engine (intent_parser, atc_state_machine, atc_templates, flight_phase). GPT
fallback is disabled for tests, so all coverage exercises the rule-based path
end-to-end.

## How the test engine works

The headless CLI (`tools/atc_repl/`) is the test harness. It links the same
engine objects the plugin uses (`xp_atc_engine` CMake OBJECT library) but
replaces the X-Plane / Keychain dependencies with thin stubs so it can run on
any machine without X-Plane installed.

### Components

| Path                                         | Role                                                                                  |
|----------------------------------------------|---------------------------------------------------------------------------------------|
| `tools/atc_repl/main.cpp`                    | Entry point. Three modes: batch run, REPL seeded from a scenario, default LSZH REPL.  |
| `tools/atc_repl/scenario.{hpp,cpp}`          | JSON loader and runner. Parses `testscripts/*.json`, drives the engine, asserts.      |
| `tools/atc_repl/repl.{hpp,cpp}`              | Interactive REPL. Reads pilot transcripts on stdin, prints ATC responses on stdout.   |
| `tools/atc_repl/xplane_context_stub.cpp`     | Replaces the plugin's X-Plane DataRef reader with a single mutable `g_cli_ctx`.       |
| `tools/atc_repl/settings_stub.cpp`           | Replaces Keychain / plugin settings with env vars (`XP_ATC_DEBUG`, `XP_ATC_REGION`).  |

The engine itself (`src/engine/`, `src/intent_parser`, `src/atc_state_machine`,
`src/atc_templates`, `src/flight_phase`) is reused unmodified. No test-only
code paths exist in the engine — what the harness exercises is what the plugin
ships.

### What runs per scenario

For each `testscripts/*.json` file, `scenario::run` does the following:

1. **Region select.** Reads `region` from JSON (`EU` or `US`, default `EU`),
   updates `settings::set_flow_region`, then calls `atc_templates::reload()` and
   `flight_phase::reload()` so the next steps see the right
   `data/regions/<region>/{atc_templates,flight_rules}.json`.
2. **Callsign prime.** `settings::set_pilot_callsign_raw` is set to the
   scenario callsign so `intent_parser::matches_configured_callsign` accepts
   the transcript.
3. **Context load.** The JSON `context` block populates `g_cli_ctx`
   (airport, frequency, on-ground state, etc.). Defaults match a parked C172
   at LSZG on Ground 121.800.
4. **Engine reset.** `atc_state_machine::reset()` and `engine::reset()` zero
   out state and the profanity counter so scenarios cannot leak state between
   each other.
5. **Phase prime.** `flight_phase::update` is called 30 times with `dt=1.0s`
   to push past the ground-hysteresis window. Without this, intents like
   `REQUEST_TAXI` get rejected as "engines off / parked" on step 1.
6. **Step loop.** Each step:
   - applies any `set` field changes (with another 30 phase ticks to settle),
   - prints the optional `note`,
   - if `wait_sec` is set, ticks `flight_phase::update` **and**
     `atc_state_machine::check_auto_correction` once per simulated second,
   - if `text` is non-empty, builds an `engine::Input` (with `quality` from the
     step or default `1.0f`) and calls `engine::process_transcript` synchronously
     (GPT fallback off → engine returns immediately),
   - asserts `expect` (case-insensitive substring match against the response),
     `expect_not` (same match, must be absent), and `expect_state` (equality
     against `state_name(get_state())`).
7. **Result.** The `RunResult` (steps, assertions, mismatches) is returned.
   `main.cpp` aggregates across files into the JUnit-style summary.

### Differences from the plugin path

These are the only things the harness does not exercise:

- **GPT classification & fallback.** The CLI hard-codes
  `gpt_fallback_enabled=false`, so low-confidence intents go through the
  state machine's `_INVALID` template fallback. GPT call paths are not tested.
- **Whisper / TTS / audio.** No microphone capture, no MP3 playback. The
  pilot's transcript is taken straight from the JSON `text` field.
- **Real-time timing.** `flight_phase::update` and
  `atc_state_machine::check_auto_correction` are called with synthetic 1.0s
  ticks. Auto-correction delays are driven exclusively by `wait_sec` steps —
  wall-clock time does not advance.
- **X-Plane DataRefs.** No nav-aid lookups, no apt.dat parsing. Airport
  frequencies / runway selection are taken from the JSON `context` directly.

These trade-offs are deliberate: the suite is meant to be a fast, deterministic
regression net for the rule-based engine and the JSON-driven phraseology.
Anything that needs the network (GPT, Whisper, TTS) or hardware (audio, X-Plane)
is integration testing and lives outside `make test`.

## File-name convention

| Prefix       | Purpose                                                              |
|--------------|----------------------------------------------------------------------|
| `flow_NN_*`  | Happy-flow scenarios — pilot does the right thing, ATC responds.     |
| `bad_NN_*`   | Bad-case / error-handling scenarios — pilot makes a mistake.         |
| (legacy)     | A handful of unprefixed scripts predate the convention; kept for now.|

The runner enumerates files alphabetically, so `bad_*` rows appear above
`flow_*` rows in the summary.

## Scenario schema (relevant subset)

Each step in the `say` array supports:

- `text` — pilot transcript. Empty (omit) for set-only or wait-only steps.
- `expect` — case-insensitive substring assertion against the ATC response.
- `expect_state` — assertion against `atc_state_machine::state_name(get_state())`
  after processing the step. Useful when the bad-case behaviour is "no state
  change" rather than a specific phrase.
- `expect_not` — case-insensitive substring that **must not** appear in the
  response. Counterpart to `expect`. Only valid together with `expect` or
  `expect_state` on the same step — otherwise an empty/silent response would
  pass vacuously (the loader raises `runtime_error`).
- `quality` — float, mapped to `engine::Input.quality`. Defaults to `1.0f`.
  Values below `0.3f` trigger the `"say again"` short-circuit in
  `engine::process_transcript`.
- `set` — object of context fields applied **before** processing `text`. Common
  fields: `com`, `freq_type`, `on_ground`, `agl_ft`, `altitude_ft`,
  `groundspeed_kt`, `airport`, `runway`.
- `wait_sec` — integer number of seconds. Drives `flight_phase::update` **and**
  `atc_state_machine::check_auto_correction` at `dt=1.0s` each, exactly once
  per scenario step. Prints one `WAIT` line with the before/after phase +
  state. See "Time simulation & auto-corrections" below.
- `note` — printed before the step for log readability.

## Time simulation & auto-corrections

`wait_sec` is the only place in the runner that ticks
`atc_state_machine::check_auto_correction`. The 30-tick priming loops at
scenario start and after every `set` intentionally remain
`flight_phase::update`-only so existing happy-flow scenarios cannot silently
turn red when a corner case in auto-corrections shifts.

The nine auto-corrections currently configured in
`data/regions/<eu|us>/flight_rules.json` are:

| ATC state             | Condition    | Phases triggering it                                        | Next state    | Delay |
|-----------------------|--------------|-------------------------------------------------------------|---------------|-------|
| `DEPARTURE_CLEARED`   | `on_airborne`| `CLIMB`, `PATTERN`, `FINAL_APPROACH`, `CRUISE`              | `PATTERN_ENTRY` | 5 s |
| `DEPARTURE_CLEARED`   | `on_parked`  | `PARKED`                                                    | `IDLE`        | 10 s |
| `PATTERN_ENTRY`       | `on_ground`  | `LANDING_ROLL`, `TAXI`, `PARKED`                            | `IDLE`        | 3 s  |
| `LANDING_CLEARED`     | `on_ground`  | `TAXI`, `PARKED`                                            | `IDLE`        | 90 s |
| `TOUCH_AND_GO_CLEARED`| `on_airborne`| `CLIMB`, `PATTERN`                                          | `PATTERN_ENTRY` | 3 s |
| `TOUCH_AND_GO_CLEARED`| `on_ground`  | `TAXI`, `PARKED`                                            | `IDLE`        | 3 s  |
| `GROUND_CONTACT`      | `on_airborne`| `CLIMB`, `PATTERN`, `FINAL_APPROACH`, `CRUISE`, `TAKEOFF_ROLL` | `IDLE`     | 5 s  |
| `TAXI_CLEARED`        | `on_airborne`| `CLIMB`, `PATTERN`, `FINAL_APPROACH`, `CRUISE`, `TAKEOFF_ROLL` | `IDLE`     | 5 s  |
| `EN_ROUTE`            | `on_ground`  | `TAXI`, `PARKED`                                            | `IDLE`        | 3 s  |

The state machine suppresses the `DEPARTURE_CLEARED → PATTERN_ENTRY`
correction when `departure_type == CROSS_COUNTRY` (the pilot will leave the
frequency explicitly). `bad_12a` + `bad_12b` cover both branches.

Example:

```json
{ "text": "November One Two Three Alpha Bravo ready for departure",
  "expect": "cleared for takeoff",
  "expect_state": "DEPARTURE_CLEARED" },
{ "set": {"on_ground": false, "agl_ft": 500, "altitude_ft": 1000, "groundspeed_kt": 75} },
{ "wait_sec": 6, "expect_state": "PATTERN_ENTRY" }
```

## Frequency guards

The engine enforces `intent_frequency` rules from `flight_rules.json` directly
in `atc_state_machine::process()`, right after the flight-phase precondition
guard. Each intent entry in the new object schema has the form:

```json
"REQUEST_FLIGHT_FOLLOWING": {
  "allowed": ["APPROACH"],
  "rejection": "{callsign}, contact Approach for flight following."
}
```

If the current `ctx.frequency_type` is not in `allowed`, the rejection
template is rendered via `atc_templates::fill` and the state machine leaves
`state_` unchanged. Region-specific rejections come for free because
`flight_rules.json` is loaded per region — the EU file sets
`REQUEST_FLIGHT_FOLLOWING.allowed` to `[]` with the rejection "flight
following service is not available in this region".

Tower-only airports (no separate Ground frequency, e.g. LSZB) are exempt from
the guard on the TOWER frequency — Ground-class intents route to the Tower
controller. The UI filter in `atc_ui.cpp` applies the same exception and now
only acts as defense-in-depth for the menu.

## Bad-case taxonomy (M5)

Each bad-case scenario is structured in two halves:

1. **Part A** — the pilot makes the mistake. Assertion verifies the controller
   reaction (and that no state corruption occurred via `expect_state`).
2. **Part B** — the pilot recovers (switches frequency, climbs out, retransmits
   clearly, etc.). Assertion verifies the flow continues normally.

| File                                              | Category                       | Engine path under test                                              |
|---------------------------------------------------|--------------------------------|---------------------------------------------------------------------|
| `bad_01_taxi_on_tower_eu.json`                    | Wrong frequency                | IDLE-state freq routing: REQUEST_TAXI on TOWER → "contact ground"   |
| `bad_02_ready_for_departure_on_ground_us.json`    | Wrong frequency (US)           | TAXI_CLEARED + READY_FOR_DEPARTURE on GROUND → "contact Tower"      |
| `bad_03_no_position_on_initial_call.json`         | Missing position phrase        | `position_remark` variable inserted, taxi still cleared             |
| `bad_04_inbound_while_parked.json`                | Wrong flight phase             | `flight_phase::check_precondition` rejects INITIAL_CALL_INBOUND     |
| `bad_05_runway_vacated_while_airborne.json`       | Wrong flight phase             | precondition rejects RUNWAY_VACATED while airborne, state preserved |
| `bad_06_profanity_escalation.json`                | Radio discipline               | engine `profanity_warnings_` counter, no state mutation             |
| `bad_07_low_quality_transcript.json`              | Low Whisper quality            | `Input.quality < 0.3f` short-circuit to "say again"                 |
| `bad_08_self_correction.json`                     | ICAO "correction" phraseology  | intent_parser drops everything before the last "correction"         |
| `bad_09_flight_following_on_tower_us.json`        | Wrong frequency (US)           | Engine freq guard rejects REQUEST_FLIGHT_FOLLOWING on TOWER         |
| `bad_10_eu_vfr_no_squawk.json`                    | Region discipline (EU)         | `expect_not: "squawk"` on every response of a full EU VFR flow      |
| `bad_11_eu_flight_following_not_available.json`   | Region-specific rejection      | EU has `allowed: []` for REQUEST_FLIGHT_FOLLOWING                   |
| `bad_12_*.json`, `bad_12b_*.json`                 | Auto-correction: airborne      | `DEPARTURE_CLEARED → PATTERN_ENTRY` vs. cross-country suppression   |
| `bad_13_landing_cleared_on_ground_90s.json`       | Auto-correction: slow timer    | `LANDING_CLEARED → IDLE` after 90 s on ground                       |
| `bad_14_pattern_entry_after_landing.json`         | Auto-correction: fast timer    | `PATTERN_ENTRY → IDLE` after 3 s on ground                          |

## Authoring rules

These are non-negotiable — they exist to keep the suite a real safety net:

1. **Never weaken an assertion to make it pass.** If `expect` mismatches the
   actual response, either fix the engine in a separate commit or document the
   gap. See M5 §4 for the original wording.
2. **Never bypass the engine in the harness.** No `reset:true` flags, no magic
   context shortcuts. The runner already calls `engine::reset()` between
   scenarios.
3. **Keep `expect_state` on bad cases that should NOT mutate state.** A passing
   substring match isn't enough proof when the correct behaviour is silence.
4. **Document deferred bad cases.** If an engine gap blocks a scenario, capture
   it here under "Known limitations" rather than deleting the test idea.

## Running the suite

```bash
make test                                # full suite
./build/atc_repl run testscripts/bad_*.json   # bad cases only
./build/atc_repl run testscripts/bad_07_low_quality_transcript.json   # one file
./build/atc_repl repl testscripts/flow_01_eu_pattern_grenchen.json    # interactive REPL seeded from a scenario
```
