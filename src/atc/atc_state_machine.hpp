/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef ATC_STATE_MACHINE_HPP
#define ATC_STATE_MACHINE_HPP

#include "atc/bzf_compliance.hpp"
#include "atc/flight_phase.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>

// Template variable building moved to ground_ops::build_vars in
// src/atc/flows/ground_operations.hpp as part of the A1 flow-split
// refactor (step 1). External callers include that header directly.

namespace atc_state_machine {

enum class ATCState {
  IDLE,
  GROUND_CONTACT,
  TAXI_CLEARED,
  TOWER_CONTACT,
  DEPARTURE_CLEARED,
  PATTERN_ENTRY,
  LANDING_CLEARED,
  TOUCH_AND_GO_CLEARED,
  UNICOM_ACTIVE,
  EN_ROUTE,
};

struct ATCResponse {
  std::string text;
  ATCState next_state = ATCState::IDLE;
  bool requires_readback = false;
};

// Bounded log of past ATC states. Pushed every time the state machine
// transitions to a different state, in chronological order: the front
// is the oldest, the back is the most recent past state (the entry
// just before the current state_). Capped to keep memory bounded —
// see kHistoryCap in the implementation. Cleared by init/stop/reset
// (fresh session). Disregard does NOT clear it: post-Disregard
// classifiers still need to know "we were in LANDING_CLEARED a moment
// ago" to disambiguate post-landing intents.
struct StateHistoryEntry {
  ATCState state;
  std::string reason;    // e.g. "process:RUNWAY_VACATED", "disregard_on_ground"
  double timestamp_secs; // monotonic, sourced from caller (engine: in.now_secs)
};

void init();
void stop();
void reset();

// Guarded between-flights reset. Called from XPluginReceiveMessage on
// XPLM_MSG_AIRPORT_LOADED / XPLM_MSG_PLANE_LOADED (user repositioned or
// reloaded the aircraft). Performs the FULL reset() ONLY when safely
// between flights: on_ground == true AND state_ in { IDLE, GROUND_CONTACT }.
// Otherwise a strict no-op: writes nothing and does NOT bump the
// revert-guard generation counter, so a spurious (non-reposition) message
// can never wipe a flight that is taxiing, departing, in the pattern, or
// on final. on_ground is supplied by the caller from the latest
// XPlaneContext snapshot; at message time it reflects the previous frame
// and may read stale-airborne (false) when repositioning out of flight —
// the guard intentionally treats that as "do not reset".
void begin_fresh_flight(bool on_ground);

// Pilot-driven "Disregard" — drops the current ATC dialog and lands on
// a flow-appropriate state instead of blind IDLE: airborne pilots near
// their last airport keep PATTERN_ENTRY; airborne pilots away from any
// airport return to EN_ROUTE; pilots on the ground go all the way to
// IDLE. Always preserves the runway lock when staying airborne so the
// pilot doesn't have to re-negotiate it.
void disregard(const xplane_context::XPlaneContext &ctx,
               flight_phase::FlightPhase phase, double now_secs);

ATCState get_state();
const char *state_name(ATCState state);
bool is_readback_pending();

// Structured snapshot of the most recent readback-demanding clearance:
// the exact values the controller passed plus which Kat-1 elements
// (NfL §25 b) Nr. 1) THIS transmission obligated the pilot to read back.
// Armed alongside is_readback_pending(); empty `required` when no
// readback is outstanding. Consumed by the deterministic readback
// recognition in engine::process_transcript (matching, not classifying)
// and by the BZF-strict completeness check.
bzf_compliance::ClearanceComponents last_clearance_components();

// Session-lifecycle flag, paired with just_landed() (see table near
// the bottom of this header). True iff the aircraft has been airborne
// at any point since the last init/stop/reset OR the last new
// departure cycle. Survives Touch-and-Go and arbitrarily long roll-
// outs / taxi-backs — the "session" here is the flight, not the time
// window.
bool was_airborne();

// Idempotent setter for the session-lifecycle flag. Called from
// flight_phase::update() every frame the aircraft is airborne — the
// idempotency guard inside the implementation prevents frame-spam
// invalidation of the revert-guard generation counter. Symmetric to
// set_state() / set_readback_pending() but exposed as a public function
// so flight_phase (which lives outside the flows/ bridge) can drive it
// without including the internal flow storage header.
void set_was_airborne(bool v);

// The legacy get_departure_type_name() public accessor was removed in
// step 5 of the A1 flow-split refactor — state_name() already carries
// the flow qualifier ("Pattern/" vs "XC/") after step 3b, and
// flow_coordinator::active_flow_name(flow_coordinator::active()) is
// the canonical source for callers that need the bare flow tag.

ATCState state_from_name(const std::string &name);
void set_state(ATCState state);

// Returns the runway that ATC has cleared the pilot for (set on first
// clearance, held until the dialog returns to IDLE). Empty otherwise.
const std::string &assigned_runway();

// Returns the pilot callsign that the tower locked onto at the first
// non-IDLE transition of the current session. Empty when the dialog
// is still in IDLE or the parser never extracted one. Use this in
// preference to the per-utterance msg.callsign once a session has
// been established — otherwise a single mistranscribed word ("Delta")
// can hijack the tower's salutation mid-session.
const std::string &session_callsign();

// ── Declared VFR flight intent (NfL 2024 1.4.7) ──────────────────────
// The pilot states destination/intent ONCE on the taxi / initial-ground
// call, NOT on every follow-up transmission. The tower must remember it
// for the rest of the flight, so build_vars() reads this session state
// before falling back to the pre-flight UI setting (settings::vfr_*).
// Without it, a READY_FOR_DEPARTURE that omits the destination would fall
// back to "pattern" and the tower would wrongly answer "Melden Sie
// Gegenanflug". Set from speech in engine.cpp on a successful
// extract_destination()/"Platzrunde" match. Per-flight session state:
// cleared on init/stop/reset (hence begin_fresh_flight + the new-flight
// button), never persisted across flights. Captured/restored by the
// snapshot guard alongside the rest of g_state.
void set_flight_intent_cross_country(const std::string &destination);
void set_flight_intent_pattern();
// True once the pilot has stated either intent this flight. While false,
// build_vars() falls through to settings::vfr_flight_type().
bool flight_intent_declared();
// Meaningful only when flight_intent_declared(): true => cross-country.
bool flight_intent_cross_country();
// Spoken ICAO destination ("EDMA"); empty for a pattern flight or a
// destination-less cross-country declaration.
const std::string &flight_intent_destination();

// Returns assigned_runway() if non-empty, else ctx.active_runway. Use this
// for any "what runway are we operating to" question outside the spoken
// ATC response itself (UI hints, ATIS, STT bias, phase detection).
std::string effective_runway(const xplane_context::XPlaneContext &ctx);

ATCResponse process(const intent_parser::PilotMessage &msg,
                    const xplane_context::XPlaneContext &ctx, double now_secs);

// Check and apply auto-corrections based on flight phase mismatches.
// Call every frame from atc_session::update(). Uses dt for delay timers
// and now_secs as the timestamp written to history when a correction
// fires.
void check_auto_correction(flight_phase::FlightPhase phase, float dt,
                           double now_secs);

// Per-frame airport-change reset. When the pilot is EN_ROUTE and the
// nearest airport changes (e.g. crossing into a new control zone), drop
// to IDLE so the UI hint pipeline reflects the new airport's options
// (INITIAL_CALL_INBOUND etc.) instead of remaining silent on EN_ROUTE.
// Call from atc_session::update() after check_auto_correction().
void check_airport_change(const xplane_context::XPlaneContext &ctx,
                          double now_secs);

// Render a controller-issued traffic advisory through the standard
// template path WITHOUT changing ATCState. The traffic dialog runs
// parallel to the main flow (see traffic_dialog.hpp). The
// `template_key` selects which entry inside the TRAFFIC_DIALOG block
// is rendered — Phase-2 airborne callouts use "traffic_advisory",
// Phase-3 ground conflicts use "taxi_hold_position" /
// "taxi_caution" / "taxi_give_way". Defaults to the airborne entry
// for backwards compatibility. Returns the rendered text.
std::string
render_traffic_advisory(const std::map<std::string, std::string> &advisory_vars,
                        const xplane_context::XPlaneContext &ctx,
                        const std::string &template_key = "traffic_advisory");

// ── State history accessors ─────────────────────────────────────────
// Read-only views into the bounded history deque maintained by
// transition_to(). Useful for downstream consumers (LM-classify
// prompt, hint filter, intent-rule adjustments) that need to
// disambiguate situations where the current state alone is not
// enough.

// Most-recent past state. Returns IDLE when the history is empty
// (e.g. right after init/stop/reset, or before the first transition).
ATCState previous_state();

// True iff `s` was visited within the last `max_age_secs`. The
// caller's clock (now_secs) must use the same monotonic source as
// the timestamps fed to process()/disregard()/check_*. Current state
// counts: a pilot still mid-LANDING_CLEARED is "recently in"
// LANDING_CLEARED.
bool was_recently_in(ATCState s, double max_age_secs, double now_secs);

// ── Session-lifecycle flags ─────────────────────────────────────────
//
// Two complementary "did this happen this flight" predicates. They
// answer different questions and live in different places — keep both
// in mind, and document any new sibling flag in the table below.
//
//   +------------------+--------------------------------+--------------------------------------+
//   | Flag             | Set                            | Reset |
//   +------------------+--------------------------------+--------------------------------------+
//   | just_landed      | transition_to(LANDING_CLEARED  | implicit: 120 s
//   window expires       | | (history-derived)| or TOUCH_AND_GO_CLEARED) — via
//   | (time-based), or init/stop/reset     | |                  | the bounded
//   state history.     | clears the history.                  |
//   +------------------+--------------------------------+--------------------------------------+
//   | was_airborne     | flight_phase::update() crosses | process() of
//   REQUEST_TAXI /          | | (AtcMachineState | into an airborne phase
//   (CLIMB, | INITIAL_CALL_{GROUND,TOWER} /        | |  field, in       |
//   PATTERN, FINAL_APPROACH,       | generic INITIAL_CALL, WHILE          | |
//   snapshot)       | CRUISE) and pushes the flag    | ctx.on_ground=true (new
//   departure    | |                  | via set_was_airborne(true).    |
//   cycle). INITIAL_CALL_INBOUND is      | |                  | Idempotent —
//   frame-spam safe.  | excluded from the intent list;       | | | | the
//   on_ground gate also catches      | |                  | | low-confidence
//   INITIAL_CALL          | |                  | | misclassifications mid-air.
//   | |                  |                                | Also:
//   init/stop/reset.               |
//   +------------------+--------------------------------+--------------------------------------+
//
// Adding a third lifecycle flag? Extend this table — leaving the set
// or reset row blank is a review red flag.

// Domain-specific convenience helper. True when the pilot recently
// touched down: LANDING_CLEARED or TOUCH_AND_GO_CLEARED was visited
// within `window_secs` (default 120 s). Caller must additionally
// check ctx.on_ground if "still rolling out" matters.
bool just_landed(double now_secs, double window_secs = 120.0);

// Time-independent post-landing check. True when the pilot is on the
// ground AND the most recent landing in history (or current state)
// has not been followed by a new DEPARTURE_CLEARED. Survives long
// stand times — REQUEST_TAXI_PARKING stays a valid hint while the
// pilot is still taxiing in, regardless of how many minutes elapsed.
bool at_airport_after_landing(const xplane_context::XPlaneContext &ctx);

// Read-only access to the full history deque (oldest at front).
// Stable across calls until the next transition.
const std::deque<StateHistoryEntry> &get_history();

// Comma-separated history of state names for prompt embedding,
// chronological order. Includes the current state as the last
// element. Example: "PATTERN_ENTRY,LANDING_CLEARED,IDLE".
std::string history_csv();

// ── Snapshot / Restore (TTS revert guard) ───────────────────────────
//
// Used by atc_session's speak_with_revert_guard helper to undo a
// process()-driven state transition when the TTS playback that would
// have spoken the response fails. Semantics:
//
//   auto snap = capture_snapshot();        // BEFORE process()
//   auto resp = process(msg, ctx, t);
//   auto expected = current_gen();         // AFTER process()
//   tts::synthesize_async(resp.text, [snap, expected](Audio, bool ok) {
//     if (ok) return;
//     if (restore_snapshot_if_gen(snap, expected)) {
//       // Restore branch: state rolled back, pilot reissues request.
//     } else {
//       // Stale branch: a later mutation (auto-correction, another
//       // process()) bumped gen since the snapshot was taken — the
//       // unsent clearance text still lives in last_tower_response_text_
//       // so REQUEST_REPEAT can replay it.
//     }
//   });
//
// Threading: all three functions assert they run on the flight-loop
// thread (captured by init()). The TTS callback is marshaled there by
// backends::manager::enqueue_callback() — see manager.cpp::synthesize_async.

// Opaque snapshot type. Holds a copy of g_state including its gen
// value at the moment of capture. Layout is intentionally hidden.
class AtcStateSnapshot {
public:
  AtcStateSnapshot();
  AtcStateSnapshot(const AtcStateSnapshot &);
  AtcStateSnapshot(AtcStateSnapshot &&) noexcept;
  AtcStateSnapshot &operator=(const AtcStateSnapshot &);
  AtcStateSnapshot &operator=(AtcStateSnapshot &&) noexcept;
  ~AtcStateSnapshot();

private:
  friend AtcStateSnapshot capture_snapshot();
  friend bool restore_snapshot_if_gen(const AtcStateSnapshot &snap,
                                      uint64_t expected_gen);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Copy the full AtcMachineState into an opaque snapshot.
AtcStateSnapshot capture_snapshot();

// Current monotonic generation counter. Bumped by every semantic
// mutation (state transitions, readback flag, runway lock, departure
// type, history push, clearance/tower text). NOT bumped by per-frame
// heartbeats (last_now_secs, auto-correction timer). Use the value
// captured directly after process() as `expected_gen` for the revert
// guard.
uint64_t current_gen();

// If current_gen() == expected_gen, replace g_state with `snap` and
// bump gen ONCE more (so the restored gen is strictly larger than any
// gen the caller already observed — any still-pending guard with an
// older expected_gen will see a mismatch and bail). Returns true on
// successful restore. Returns false when a later mutation has already
// advanced gen past expected_gen — caller is in the "stale" branch and
// must surface a Repeat-friendly system message instead.
bool restore_snapshot_if_gen(const AtcStateSnapshot &snap,
                             uint64_t expected_gen);

} // namespace atc_state_machine

#endif // ATC_STATE_MACHINE_HPP
