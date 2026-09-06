# Phase 2 — VFR En-Route Traffic Advisories: Smoke Test

This is the live in-sim verification step for Phase 2. The unit + scenario tests
cover every deterministic branch (`make test`); this doc covers what `make test`
cannot — the X-Plane ↔ LiveTraffic ↔ plugin loop and the audio path.

Branch under test: `feat/traffic-phase-2-enroute-advisories`.

## Prerequisites

- `make all` clean on the branch (engine OBJECT lib SDK-free, all tests green).
- LiveTraffic installed and active (other providers untested in Phase 2).
- All three local models downloaded (Whisper, Llama, Piper) — check the in-sim
  Models tab. Without TTS the controller can't speak the advisory.
- Audio output routed to your headset (Settings → Sound → ATIS / ATC bus).
- A test aircraft of your choice. Cessna 172 default scenery is fine.

## Scenario 1 — LSZH transit, head-on advisory

Goal: a real LiveTraffic target appears in the forward arc, advisor fires once,
pilot acknowledges, state restores.

1. Set X-Plane location to ~5 NM south of LSZH at 1500 ft MSL, heading 360.
2. Start the engine, taxi/take off is not required — set the aircraft airborne
   in flight or use Custom Scenery `Set Position`.
3. Tune COM1 to **Zurich Tower 118.100**. Confirm the plugin's Frequencies panel
   shows `freq_type = TOWER`. Confirm Status panel ATCState advances past `IDLE`
   on first call (e.g. *"Zurich Tower, Hotel Bravo X-ray Yankee Zulu, transit
   from south."*).
4. Wait for LiveTraffic to inject a target inbound from the north. Watch the
   `xp_wellys_vfr_atc` log lines:
   - `traffic_context update: <N> targets`
   - on trigger: `Engine emitted traffic advisory (target_id=...)`
5. Expected speech (TTS): something like *"X-ray Yankee Zulu, traffic, 12
   o'clock, 5 miles, opposite direction, indicating 4500 feet, type unknown."*
6. Reply with **PTT: "Traffic in sight, HB-XYZ"**.
7. Expected speech: *"X-ray Yankee Zulu, roger, maintain visual separation."*
8. Confirm Status panel ATCState restores to whatever it was before the
   advisory (probably `TOWER_CONTACT` or `EN_ROUTE`).

## Scenario 2 — same target, pilot does not see traffic

Repeat Scenario 1 steps 1–5, then:

6. Reply with **PTT: "Negative contact, HB-XYZ"**.
7. Expected speech: *"X-ray Yankee Zulu, roger, traffic now N o'clock, M
   miles."* — clock + distance reflect the *current* geometry of the same
   target, not the geometry at first issue.
8. Confirm ATCState restored.
9. Wait ~30 s. Confirm the advisor does **not** re-fire on the same target —
   per-target cooldown is 60 s.

## Scenario 3 — cooldown sanity

1. Repeat Scenario 1 to fire one advisory.
2. Within 20 s, deliberately position another LiveTraffic target into the
   forward arc (or just wait for one).
3. Confirm the advisor stays silent — global 20 s cooldown is in force.

## What to log if anything misfires

When something looks wrong, capture:

- The plugin log lines around the time of the misfire. The advisor is verbose
  on emit (target_id, geometry vars).
- The TCAS slot dump (atc_repl `--traffic-fixture` round-tripped against a
  saved snapshot is the offline equivalent — log lines `traffic_context init`
  + `update` show what the runtime reader saw).
- The ATCState transitions printed by `atc_state_machine` (every transition
  logs `ATC state: <prev> -> <next>`).

If the advisor does **not** fire when you expect one:
- Confirm `atc_state != IDLE` — the gate refuses to fire while the pilot has
  not made first contact.
- Confirm `frequency_type` is one of `TOWER / GROUND / APPROACH`. UNKNOWN /
  UNICOM / CTAF / ATIS all gate out by design.
- Confirm the target's `clock_position` is in the forward arc (1, 2, 3, 9, 10,
  11, 12). 4–8 are deliberately suppressed.
- Confirm `2.0 ≤ distance_nm ≤ 8.0` and `|altitude_diff_ft| ≤ 1500`.

## Out of scope for this smoke test

- Multiple simultaneous advisories (Phase 2 emits the nearest qualifying
  target; chains are deferred).
- US phraseology (Phase 2 is EU-only).
- CTAF / Unicom advisories (deferred, not in Phase-2 scope).
- Wake-cat phraseology (Phase 5).

## Pass criteria

The phase is signed off when all three scenarios above produce the expected
speech + state transitions on a LiveTraffic-driven flight, and the cooldowns
behave as documented.

---

# Phase 3 — Ground / Taxi Conflict Advisories: Smoke Test

Branch under test: `feat/traffic-phase-3-ground-taxi`.

Phase-3 extends the Phase-2 advisor with a surface-safety side branch. It
fires *only* when the user's flight phase is `TAXI` (see
`src/atc/flight_phase`) and a nearby traffic target is itself taxiing /
taking off / just landed on an intersecting path. Ground-conflict callouts
do **not** require ATC contact (no Tower/Ground frequency needed) and do
**not** expect a voice acknowledgement — the pilot reacts by stopping or
giving way, not by speaking.

## Scenario 1 — taxi conflict, hold position

1. Park at LSZH with engine running. Cold-and-dark is fine after start.
2. Begin taxiing at ~10 kts towards the runway.
3. As another LiveTraffic ground aircraft crosses ahead of you within
   ~550 m and inside ±30° of your nose, expect speech:
   - *"<callsign>, hold position, traffic crossing."*
4. Stop. Wait for the other aircraft to clear. The advisor does NOT
   re-fire on the same target for 30 s.

## Scenario 2 — taxi conflict, side caution

1. Same setup as Scenario 1, but pick a position where another
   aircraft is taxiing abeam your side (e.g. on a parallel taxiway,
   clock 2 or clock 10).
2. Expect:
   - slow target → *"<callsign>, caution, aircraft taxiing on your left."*
     (or *right*)
   - faster target on a converging side path →
     *"<callsign>, give way to the [type] approaching from your right."*

## Scenario 3 — ground-conflict global cooldown

1. Trigger one ground advisory.
2. Within 15 s, deliberately position into another conflict. The advisor
   stays silent — global cooldown is 15 s for ground events (vs 20 s
   for airborne).
3. After 16 s, the next conflict fires.

## Deterministic round-trip via atc_repl

```bash
./build/atc_repl --traffic-fixture tests/fixtures/traffic_taxi_conflict.json
```

Expected: Target A (TAXI1) shown at `clk=12 dist=0.1` with `phase=Taxi`;
Target B (PARK1) shown at `clk=3 dist=0.3` with `phase=OnGround`. The
ground-conflict trigger inside `traffic_advisor::evaluate()` selects
Target A and renders `taxi_hold_position` — covered end-to-end by the
Catch2 test `advisor: taxiing user, crossing target -> hold_position`
inside `tests/test_traffic_advisor.cpp`.

## What to log if anything misfires

- Plugin log lines around the time of the misfire. The advisor logs
  `Engine emitted traffic advisory (target_id=..., template=taxi_*)` on
  every ground fire — confirm the template key matches expectation.
- The per-target `TrafficPhase` field. If a target is `Unknown`, the
  classifier rejected it (alt_agl missing? speed in a no-rule band?).

If the advisor does **not** fire when you expect one:
- Confirm the user's flight phase is `TAXI` — not `PARKED` or
  `TAKEOFF_ROLL`. Phase 3 only triggers in the strict TAXI band.
- Confirm the target's `phase` is `Taxi`, `Takeoff`, or `Landed`. A
  parked `OnGround` target is never a conflict (not moving).
- Confirm the target is within 0.3 NM and inside the ±30° / 200 m cone
  forward of the user's nose.

## Out of scope for this phase

- Specific taxiway names (no taxiway topology — known limitation).
- Runway-incursion detection (Phase 5).
- Refined classifier states `Climb/Cruise/Descend/Final/Pattern` (Phase 4).

## Pass criteria

Three scenarios produce the expected `taxi_*` template responses with no
duplicate fires inside the per-target / global cooldown windows, and the
deterministic `atc_repl` round-trip on `traffic_taxi_conflict.json`
fires exactly once on Target A.

---

# Phase 4 — Landing Sequencing + Go-Around: Smoke Test

Branch under test: `feat/traffic-phase-4-landing-sequencing`.

Phase-4 introduces:

- **Pattern/Final phase refinement** in `traffic_phase_classifier` — needs
  the user's active-runway threshold + heading to promote airborne targets.
- **Sequencing overlay** on `Pattern/LANDING_CLEARED` — when there is
  traffic ahead, "cleared to land" becomes "number N, follow the X on Y,
  cleared to land runway Z".
- **"Continue approach, traffic on the runway"** when the active runway
  is occupied — pilot keeps flying the approach while monitoring.
- **Unsolicited go-around** — if the runway stays occupied and the pilot
  is within 1 NM of the threshold, Tower commands the go-around. Pure
  render, no state change, no readback.

## Scenario 1 — three in the pattern, user is number 4

1. Position the aircraft 4 NM short of LSZH RWY 14, 2500 ft MSL, heading
   137°, COM1 on Zurich Tower 118.100.
2. Use LiveTraffic to place at least three aircraft in the pattern (two
   on final, one on downwind). The plugin Traffic tab (debug) should
   show their `phase` as `Final` or `Pattern` once they enter the
   8-NM / 3000-ft AGL envelope.
3. Pilot: **"Zurich Tower, HB-XYZ, request landing runway 14."**
4. Expected speech: *"X-ray Yankee Zulu, number 4, follow the C172 on
   left downwind, cleared to land runway 14."*
   - Number reflects the user's slot in the queue.
   - `{seq_type}` falls back to "traffic" when the provider doesn't
     publish the ICAO type.
   - `{seq_position}` derives from the geometric leg of the
     aircraft directly ahead.
5. Read back: **"Number 4, follow the Cessna, cleared to land 14, HB-XYZ."**
6. State stays `Pattern/LANDING_CLEARED`.

## Scenario 2 — target lands and lingers → go-around

1. Continue from Scenario 1. User now on short final, ~0.8 NM out.
2. Force the leading LiveTraffic target to land and roll out *slowly*
   so it is still on the runway centerline as you cross 1 NM.
   - The plugin Traffic tab shows the laggard with `phase = Landed`.
3. Expected unsolicited Tower call:
   *"X-ray Yankee Zulu, go around, traffic on the runway, climb runway
   heading 3000 feet."*
   - No readback expected. The pilot reacts by flying.
4. Confirm ATCState **does not change** — it stays
   `Pattern/LANDING_CLEARED`. The go-around is a controller command,
   not a dialog turn. The pilot's subsequent "going around" call (if
   they speak it) routes through the regular `GO_AROUND` intent and
   moves to `Pattern/PATTERN_ENTRY` via the existing template.
5. After 60 s the go-around throttle clears — the trigger may fire
   again if the runway is still occupied.

## Scenario 3 — pattern-side label (left vs. right)

1. Repeat Scenario 1 at LSZH (declared pattern direction: `left` in
   `data/regions/eu/airport_vrps.json`).
2. The follow-target on downwind should be labelled `left downwind`,
   not `right downwind`.
3. For comparison: an airport whose `airport_vrps.json` declares
   `right` should yield `right downwind` for an equivalent geometry.

## Deterministic round-trip via atc_repl

```bash
./build/atc_repl --traffic-fixture tests/fixtures/traffic_pattern_lszh_3in_1on.json
```

Expected output: four targets within 40 NM, two classified as `Final`
(IDs 11280001 + 11280002), one as `Pattern` (downwind, ID 11280003),
and one as `Landed` (runway occupant ID 11280004). Unit tests in
`tests/test_landing_sequence.cpp` cover the deterministic position
calculation + go-around trigger; this fixture is the geometric
visual aid.

## What to log if anything misfires

- `traffic_phase_classifier` only fires Pattern/Final when the runtime
  reader populated `rwy_hints` — that requires a non-empty
  `ctx.active_runway` AND a matching `RunwayEnd` in `ctx.runways`. A
  fresh spawn at an airport without `active_runway` resolved (wind
  picker still calm-runway-selecting) returns Unknown for airborne
  targets.
- `Landing sequence: …` log lines are emitted from
  `pattern_flow::apply_landing_sequence` on every overlay rewrite.
- `Engine emitted go-around (user dist=..., occupant id=...)` is the
  audit line for the unsolicited go-around call.

## Out of scope for this phase

- IFR sequencing.
- Wake-turbulence spacing (Phase 5).
- Multi-runway parallel ops.
- Approach-side "expect number N" prefix (XC flow has a structural
  no-op hook reserved for Phase 5).

## Pass criteria

Three scenarios produce the expected sequencing + go-around phraseology,
ATCState stays `Pattern/LANDING_CLEARED` across go-around, and the
deterministic fixture loads with the expected phase classification per
target.
