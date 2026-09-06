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

#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/bzf_compliance.hpp"
#include "atc/flight_phase.hpp"
#include "atc/flows/crosscountry_flow.hpp"
#include "atc/flows/ground_operations.hpp"
#include "atc/flows/pattern_flow.hpp"
#include "atc/flows/state_storage.hpp"
#include "atc/traffic_dialog.hpp"
#include "core/logging.hpp"
#include "data/traffic_context.hpp"
#include "data/traffic_geometry.hpp"
#include "persistence/settings.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <deque>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

namespace atc_state_machine {

// ──────────────────────────────────────────────────────────────────────
//   ATC MACHINE STATE — SINGLE OWNERSHIP, GEN-COUNTER DISCIPLINE
// ──────────────────────────────────────────────────────────────────────
//
// All semantic state lives in `g_state` below. This struct is
// file-local (anonymous namespace, no header export) so external TUs
// must go through the atc_state_machine::internal::* bridge — direct
// field writes from outside this TU are compile-errors by design.
//
// Snapshot/Restore semantics (consumed by atc_session's TTS revert
// guard): a snapshot copies the WHOLE struct; restore overwrites the
// whole struct AND bumps `gen` once. This keeps `gen` strictly
// monotonic forever, so any in-flight callback holding an old
// `expected_gen` is invalidated the instant a restore happens.
//
// gen-bump rule:
//   * EVERY function that mutates a SEMANTIC field MUST call
//     bump_gen() on entry (or at the end if it may early-return without
//     mutating). Semantic fields:
//       state_, history_, readback_pending_, assigned_runway_,
//       session_callsign_, departure_type_, last_clearance_text_,
//       last_tower_response_text_, was_airborne_
//   * Heartbeat-only fields DO NOT bump gen — they tick every frame
//     and would render the counter useless for the revert guard:
//       last_now_secs_, active_correction_key_, correction_timer_
//   * Lifecycle resets (init/stop/reset) bump gen exactly once even
//     though they clear many fields at once.
//
// Adding a new field? Decide: semantic or heartbeat. If semantic, every
// new mutator-call-site MUST bump_gen(). Add a Catch2 monotonicity test
// in tests/test_state_revert_guard.cpp to catch silent omissions.
//
// Threading: every mutator must run on the X-Plane flight-loop thread.
// `g_flight_loop_thread_id` is captured by init() and verified via
// `assert_flight_loop_thread()` in the snapshot/restore API. Worker
// threads (curl, llama, whisper) marshal their results through
// backends::manager::enqueue_callback() which drains in the flight
// loop — see manager.cpp::drain_callback_queue() / main.cpp:101.

namespace {

constexpr size_t kHistoryCap = 8;

struct AtcMachineState {
  // Monotonic generation counter. Bumped by every semantic mutation
  // (see rule above). Never decreases — restore() bumps it again
  // after copying the snapshot back, so a stale snapshot can't
  // resurrect an old gen value.
  uint64_t gen = 0;

  ATCState state_ = ATCState::IDLE;
  bool readback_pending_ = false;

  // Session-lifecycle flag: true once the aircraft has been airborne at
  // any point since the last init/stop/reset or the last new-departure-
  // cycle reset. Survives Touch-and-Go (no reset on landing) and is
  // explicitly cleared when a new departure cycle starts on the ground
  // (REQUEST_TAXI / INITIAL_CALL_GROUND/TOWER / generic INITIAL_CALL
  // processed while ctx.on_ground=true). INITIAL_CALL_INBOUND does NOT
  // reset — an inbound call is the opposite of a departure cycle.
  //
  // Drives the RUNWAY_VACATED-impossibility veto: vacated is physically
  // impossible without a prior airborne phase, so a "piste frei" heard
  // before the first takeoff is interpreted as a malformed Start-frei
  // readback (when readback_pending) or routed through the LM repair
  // path (otherwise). See data/atc_profiles/de/intent_rules.json
  // adjustments and src/atc/atc_state_machine.hpp's session-lifecycle
  // table for the documented set/reset matrix.
  bool was_airborne_ = false;

  std::string assigned_runway_; // locked once ATC assigns a runway

  // Pilot callsign locked at the first non-IDLE transition with a
  // non-empty msg.callsign. Holds for the remainder of the session so
  // a later mid-session utterance with a garbled callsign (STT
  // mishears "HB-DSV" as "Delta") cannot make the tower address the
  // pilot by the fragment. Cleared on every IDLE return alongside
  // assigned_runway_ — same lifecycle, same release points.
  std::string session_callsign_;

  // Most recent tower clearance text that demanded a readback. Set by
  // apply_post_transition_hooks() whenever resp.requires_readback is
  // true, cleared on init/stop/reset/airport-change and whenever the
  // readback expectation resolves. Consumed by the BZF-strict
  // conformance check on the next READBACK intent.
  std::string last_clearance_text_;

  // Structured form of last_clearance_text_: the exact values the
  // controller passed plus which Kat-1 elements THIS transmission
  // obligated the pilot to read back (NfL §25 b) Nr. 1, fixed
  // phraseological table per clearance type, gated by what was issued).
  // Set/cleared in lockstep with last_clearance_text_. Drives the
  // deterministic readback recognition in the engine (a readback is a
  // soll-ist match against this, not an intent-classification problem)
  // and the BZF-strict completeness check.
  bzf_compliance::ClearanceComponents last_clearance_components_;

  // Most recent NON-corrective tower utterance — used by
  // REQUEST_REPEAT to replay "the last real clearance". Set in
  // apply_post_transition_hooks (NOT in apply_bzf_strict_check), so a
  // strict-mode corrective response like "wiederholen Sie mit QNH"
  // never overwrites the real clearance. That is what the pilot
  // wants when forgetting the QNH and asking the tower to repeat —
  // they need the original numbers, not the scolding.
  std::string last_tower_response_text_;

  internal::DepartureType departure_type_ = internal::DepartureType::PATTERN;

  // Declared VFR flight intent (NfL 2024 1.4.7) — see header. The pilot
  // names destination/intent once on the taxi/initial-ground call;
  // build_vars() reads this for the rest of the flight instead of
  // re-deriving it from every utterance. flight_intent_declared_ stays
  // false until stated on the radio, so the build_vars fallback to the
  // pre-flight settings::vfr_* field is untouched for silent flights.
  // Distinct from departure_type_, which discriminates the POST-takeoff
  // Pattern/XC flow after DEPARTURE_CLEARED. Cleared in init/stop/reset.
  bool flight_intent_declared_ = false;
  bool flight_intent_cross_country_ = false;
  std::string flight_intent_destination_;

  // Bounded chronological log of state transitions. Front = oldest,
  // back = most recent past state. Filled by transition_to(); read
  // by downstream consumers (LM-classify prompt, hint filter, intent
  // rules) to disambiguate situations where the current state alone
  // is insufficient (e.g. post-landing IDLE is not the same as
  // cold-start IDLE). Cap chosen for typical inbound sequence
  // EN_ROUTE -> PATTERN_ENTRY -> LANDING_CLEARED -> IDLE ->
  // GROUND_CONTACT -> TAXI_CLEARED -> IDLE plus a couple of
  // disregards/auto-corrections.
  std::deque<StateHistoryEntry> history_;

  // ── Heartbeat fields (DO NOT bump gen) ────────────────────────────
  // Last timestamp seen by a public mutating entry point (process,
  // disregard, check_auto_correction, check_airport_change). Internal
  // helpers calling transition_to() inside one of those flows pick
  // this up so every History entry has a sensible timestamp without
  // having to thread the value through every helper signature.
  double last_now_secs_ = 0.0;
  std::string active_correction_key_;
  float correction_timer_ = 0.0f;
};

static AtcMachineState g_state;

// Flight-loop thread fingerprint. Captured by init(), checked by
// assert_flight_loop_thread() on snapshot/restore. A null-id (no
// init() called yet — only happens in tests or pre-XPluginStart) is
// accepted to keep unit tests trivial.
static std::atomic<std::thread::id> g_flight_loop_thread_id{};

inline void bump_gen() { ++g_state.gen; }

inline void assert_flight_loop_thread() {
  auto expected = g_flight_loop_thread_id.load();
  if (expected == std::thread::id{})
    return; // not yet captured — pre-init / test context
  assert(std::this_thread::get_id() == expected &&
         "atc_state_machine mutated off the flight-loop thread");
}

} // namespace

// ── State name <-> string ───────────────────────────────────────────
//
// Step 3b of the A1 flow-split refactor qualifies state names with a
// flow prefix:
//   - GroundOps states (IDLE, GROUND_CONTACT, TAXI_CLEARED, TOWER_CONTACT,
//     UNICOM_ACTIVE) keep their raw name.
//   - Pattern states (PATTERN_ENTRY, LANDING_CLEARED, TOUCH_AND_GO_CLEARED)
//     gain the "Pattern/" prefix.
//   - CrossCountry states (EN_ROUTE) gain the "XC/" prefix.
//   - DEPARTURE_CLEARED is the only state that can sit in either flow.
//     Its qualified name is selected from departure_type_ — Pattern when
//     READY_FOR_DEPARTURE was the trigger, CrossCountry when
//     READY_FOR_DEPARTURE_VFR was the trigger.
//
// state_from_name() accepts both qualified ("Pattern/PATTERN_ENTRY") and
// raw ("PATTERN_ENTRY") forms during the migration window so external
// callers (scenario JSONs, log scrapers) don't have to flip in lock-step
// with the C++ change.

const char *state_name(ATCState state) {
  switch (state) {
  case ATCState::IDLE:
    return "IDLE";
  case ATCState::GROUND_CONTACT:
    return "GROUND_CONTACT";
  case ATCState::TAXI_CLEARED:
    return "TAXI_CLEARED";
  case ATCState::TOWER_CONTACT:
    return "TOWER_CONTACT";
  case ATCState::DEPARTURE_CLEARED:
    // Pattern/XC discrimination via the legacy departure_type_ flag —
    // disappears in step 4 when each flow owns its own state enum.
    return g_state.departure_type_ == internal::DepartureType::CROSS_COUNTRY
               ? "XC/DEPARTURE_CLEARED"
               : "Pattern/DEPARTURE_CLEARED";
  case ATCState::PATTERN_ENTRY:
    return "Pattern/PATTERN_ENTRY";
  case ATCState::LANDING_CLEARED:
    return "Pattern/LANDING_CLEARED";
  case ATCState::TOUCH_AND_GO_CLEARED:
    return "Pattern/TOUCH_AND_GO_CLEARED";
  case ATCState::UNICOM_ACTIVE:
    return "UNICOM_ACTIVE";
  case ATCState::EN_ROUTE:
    return "XC/EN_ROUTE";
  }
  return "UNKNOWN";
}

ATCState state_from_name(const std::string &name) {
  // Map both qualified ("Pattern/PATTERN_ENTRY") and raw ("PATTERN_ENTRY")
  // forms. The qualified form is the post-step-3b canonical; raw is kept
  // for backwards compatibility with set_state() callers and any log
  // scrapers that pre-date the rename.
  static const std::unordered_map<std::string, ATCState> kMap = {
      {"IDLE", ATCState::IDLE},
      {"GROUND_CONTACT", ATCState::GROUND_CONTACT},
      {"TAXI_CLEARED", ATCState::TAXI_CLEARED},
      {"TOWER_CONTACT", ATCState::TOWER_CONTACT},
      {"UNICOM_ACTIVE", ATCState::UNICOM_ACTIVE},
      {"DEPARTURE_CLEARED", ATCState::DEPARTURE_CLEARED},
      {"Pattern/DEPARTURE_CLEARED", ATCState::DEPARTURE_CLEARED},
      {"XC/DEPARTURE_CLEARED", ATCState::DEPARTURE_CLEARED},
      {"PATTERN_ENTRY", ATCState::PATTERN_ENTRY},
      {"Pattern/PATTERN_ENTRY", ATCState::PATTERN_ENTRY},
      {"LANDING_CLEARED", ATCState::LANDING_CLEARED},
      {"Pattern/LANDING_CLEARED", ATCState::LANDING_CLEARED},
      {"TOUCH_AND_GO_CLEARED", ATCState::TOUCH_AND_GO_CLEARED},
      {"Pattern/TOUCH_AND_GO_CLEARED", ATCState::TOUCH_AND_GO_CLEARED},
      {"EN_ROUTE", ATCState::EN_ROUTE},
      {"XC/EN_ROUTE", ATCState::EN_ROUTE},
  };
  auto it = kMap.find(name);
  return it != kMap.end() ? it->second : ATCState::IDLE;
}

// ── internal:: bridge implementation (private API for ground_ops) ───

namespace internal {

const char *departure_type_name(DepartureType t) {
  return t == DepartureType::CROSS_COUNTRY ? "CROSS_COUNTRY" : "PATTERN";
}

ATCState get_state_ref() { return g_state.state_; }
bool readback_pending() { return g_state.readback_pending_; }
const std::string &assigned_runway_ref() { return g_state.assigned_runway_; }
const std::string &session_callsign_ref() { return g_state.session_callsign_; }
DepartureType departure_type() { return g_state.departure_type_; }

// Single source of truth for state mutations. Pushes the previous
// state into history_ (capped at kHistoryCap, oldest dropped first),
// switches state_ to next, and emits a uniform log line. No-op when
// next == state_ (avoids polluting history with self-transitions).
void transition_to(ATCState next, const char *reason) {
  if (next == g_state.state_)
    return;
  assert_flight_loop_thread();
  bump_gen();
  g_state.history_.push_back(StateHistoryEntry{
      g_state.state_, reason ? reason : "", g_state.last_now_secs_});
  while (g_state.history_.size() > kHistoryCap)
    g_state.history_.pop_front();
  logging::info("ATC state: %s -> %s (%s)", state_name(g_state.state_),
                state_name(next), reason ? reason : "");
  g_state.state_ = next;
}

void set_readback_pending(bool v) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.readback_pending_ = v;
}
void set_was_airborne(bool v) {
  // Idempotent — flight_phase::update() calls this every frame the
  // aircraft is airborne, so a no-op write must not bump the
  // revert-guard gen counter (would invalidate every in-flight
  // snapshot).
  if (g_state.was_airborne_ == v)
    return;
  assert_flight_loop_thread();
  bump_gen();
  g_state.was_airborne_ = v;
}
void set_assigned_runway(const std::string &rwy) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.assigned_runway_ = rwy;
}
void clear_assigned_runway() {
  assert_flight_loop_thread();
  bump_gen();
  g_state.assigned_runway_.clear();
}
void set_session_callsign(const std::string &cs) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.session_callsign_ = cs;
}
void clear_session_callsign() {
  assert_flight_loop_thread();
  bump_gen();
  g_state.session_callsign_.clear();
}
void set_departure_type(DepartureType t) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.departure_type_ = t;
}
// Heartbeat — does NOT bump gen (every frame would invalidate the
// revert guard's expected_gen).
void set_last_now_secs(double t) { g_state.last_now_secs_ = t; }
double last_now_secs() { return g_state.last_now_secs_; }

} // namespace internal

// ── Lifecycle ───────────────────────────────────────────────────────

void init() {
  // Capture flight-loop thread fingerprint for assert_flight_loop_thread.
  // Allowed to run multiple times. In the live plugin main.cpp calls init()
  // only on XPluginStart; settings reload does NOT re-init the state machine.
  // The session-lifecycle was_airborne flag is therefore reset here only at
  // startup; airport/plane reload disarm it via set_was_airborne(false) in
  // XPluginReceiveMessage, and a post-landing new departure via line ~905.
  g_flight_loop_thread_id.store(std::this_thread::get_id());

  // Full reset is one semantic transition — single bump_gen() covers
  // the whole field sweep below.
  bump_gen();
  g_state.state_ = ATCState::IDLE;
  g_state.readback_pending_ = false;
  g_state.was_airborne_ = false;
  g_state.assigned_runway_.clear();
  g_state.session_callsign_.clear();
  g_state.departure_type_ = internal::DepartureType::PATTERN;
  g_state.flight_intent_declared_ = false;
  g_state.flight_intent_cross_country_ = false;
  g_state.flight_intent_destination_.clear();
  g_state.history_.clear();
  g_state.last_now_secs_ = 0.0;
  g_state.last_clearance_text_.clear();
  g_state.last_clearance_components_ = {};

  // Honor the user's "where am I starting" setting. The default
  // engines_running keeps the IDLE start above; ready_for_takeoff
  // jumps straight to TOWER_CONTACT so a pilot spawning on the
  // runway sees READY_FOR_DEPARTURE hints immediately.
  const std::string mode = settings::start_mode();
  if (mode == "ready_for_takeoff") {
    g_state.state_ = ATCState::TOWER_CONTACT;
    logging::info(
        "Initial state: TOWER_CONTACT (start_mode=ready_for_takeoff)");
  }
}

void stop() {
  bump_gen();
  g_state.state_ = ATCState::IDLE;
  g_state.readback_pending_ = false;
  g_state.was_airborne_ = false;
  g_state.assigned_runway_.clear();
  g_state.session_callsign_.clear();
  g_state.departure_type_ = internal::DepartureType::PATTERN;
  g_state.flight_intent_declared_ = false;
  g_state.flight_intent_cross_country_ = false;
  g_state.flight_intent_destination_.clear();
  g_state.history_.clear();
  g_state.last_now_secs_ = 0.0;
  g_state.last_clearance_text_.clear();
  g_state.last_clearance_components_ = {};
  g_state.last_tower_response_text_.clear();
}

void reset() {
  assert_flight_loop_thread();
  bump_gen();
  g_state.state_ = ATCState::IDLE;
  g_state.readback_pending_ = false;
  g_state.was_airborne_ = false;
  g_state.assigned_runway_.clear();
  g_state.session_callsign_.clear();
  g_state.departure_type_ = internal::DepartureType::PATTERN;
  g_state.flight_intent_declared_ = false;
  g_state.flight_intent_cross_country_ = false;
  g_state.flight_intent_destination_.clear();
  crosscountry_flow::reset();
  g_state.history_.clear();
  g_state.last_now_secs_ = 0.0;
  g_state.last_clearance_text_.clear();
  g_state.last_clearance_components_ = {};
  g_state.last_tower_response_text_.clear();
  logging::info("ATC state machine reset to IDLE");
}

void begin_fresh_flight(bool on_ground) {
  // Strict no-op unless we are confidently between flights. Reading
  // g_state.state_ directly is fine — we are inside the namespace. The
  // guard MUST short-circuit before any mutation so the no-op path bumps
  // no gen and writes nothing (in-flight revert-guard snapshots stay
  // valid, and a spurious non-reposition message can never wipe a flight
  // that is taxiing, departing, in the pattern, or on final).
  const bool between_flights =
      on_ground && (g_state.state_ == ATCState::IDLE ||
                    g_state.state_ == ATCState::GROUND_CONTACT);
  if (!between_flights) {
    logging::info("begin_fresh_flight: guard blocked (on_ground=%d state=%s) "
                  "- no reset",
                  on_ground ? 1 : 0, state_name(g_state.state_));
    return;
  }
  reset(); // reuses the full field sweep + single bump_gen() + log line
}

// ── Disregard ───────────────────────────────────────────────────────

// "Disregard" radius: airborne pilots within this distance from their
// nearest airport are treated as still in the pattern flow; outside,
// they land in EN_ROUTE so an INITIAL_CALL_INBOUND can re-establish.
constexpr double kDisregardPatternRadiusNm = 5.0;

void disregard(const xplane_context::XPlaneContext &ctx,
               flight_phase::FlightPhase phase, double now_secs) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.last_now_secs_ = now_secs;
  // Always clear the side-channel — a Disregard mid-advisory should
  // also drop the pending traffic ack.
  traffic_dialog::reset();
  g_state.readback_pending_ = false;
  g_state.last_clearance_text_.clear();
  g_state.last_clearance_components_ = {};
  g_state.last_tower_response_text_.clear();

  if (!flight_phase::is_airborne(phase)) {
    internal::transition_to(ATCState::IDLE, "disregard_on_ground");
    g_state.assigned_runway_.clear();
    g_state.session_callsign_.clear();
    g_state.departure_type_ = internal::DepartureType::PATTERN;
    return;
  }

  // Airborne. Decide between PATTERN_ENTRY (still over the home
  // pattern) and EN_ROUTE (transit). The user's last airport ICAO is
  // tracked in ctx; if we don't have a position fix yet, fall back to
  // PATTERN_ENTRY since the pilot was just talking to that tower.
  bool near_airport = true;
  if (ctx.airport_lat != 0.0 || ctx.airport_lon != 0.0) {
    double d_nm = traffic_geometry::distance_nm(
        ctx.latitude, ctx.longitude, ctx.airport_lat, ctx.airport_lon);
    near_airport = d_nm <= kDisregardPatternRadiusNm;
  }

  // Keep assigned_runway_ — pilot is still in the same airspace.
  internal::transition_to(near_airport ? ATCState::PATTERN_ENTRY
                                       : ATCState::EN_ROUTE,
                          near_airport ? "disregard_airborne_near_airport"
                                       : "disregard_airborne_transit");
}

// ── Public read accessors ───────────────────────────────────────────

ATCState get_state() { return g_state.state_; }

bool is_readback_pending() { return g_state.readback_pending_; }

bzf_compliance::ClearanceComponents last_clearance_components() {
  return g_state.last_clearance_components_;
}

bool was_airborne() { return g_state.was_airborne_; }

void set_was_airborne(bool v) { internal::set_was_airborne(v); }

void set_state(ATCState state) {
  internal::transition_to(state, "external_set_state");
  if (state == ATCState::IDLE) {
    if (!g_state.assigned_runway_.empty()) {
      logging::info("Runway lock released");
      bump_gen();
      g_state.assigned_runway_.clear();
    }
    if (!g_state.session_callsign_.empty()) {
      bump_gen();
      g_state.session_callsign_.clear();
    }
  }
}

const std::string &assigned_runway() { return g_state.assigned_runway_; }

const std::string &session_callsign() { return g_state.session_callsign_; }

void set_flight_intent_cross_country(const std::string &destination) {
  assert_flight_loop_thread();
  bump_gen();
  g_state.flight_intent_declared_ = true;
  g_state.flight_intent_cross_country_ = true;
  g_state.flight_intent_destination_ = destination;
}

void set_flight_intent_pattern() {
  assert_flight_loop_thread();
  bump_gen();
  g_state.flight_intent_declared_ = true;
  g_state.flight_intent_cross_country_ = false;
  g_state.flight_intent_destination_.clear();
}

bool flight_intent_declared() { return g_state.flight_intent_declared_; }

bool flight_intent_cross_country() {
  return g_state.flight_intent_cross_country_;
}

const std::string &flight_intent_destination() {
  return g_state.flight_intent_destination_;
}

std::string effective_runway(const xplane_context::XPlaneContext &ctx) {
  return g_state.assigned_runway_.empty() ? ctx.active_runway
                                          : g_state.assigned_runway_;
}

// ── Readback obligation table (NfL §25 b) Nr. 1) ────────────────────

// Which Kat-1 elements a clearance type obligates the pilot to read back,
// keyed on the pilot intent that triggered the clearance. Callsign is
// mandatory on every clearance (§14 c) Nr. 2). QNH/Frequency appear as
// candidates but only become required when actually ISSUED in this
// transmission (gated below by template-placeholder presence) — §25 b) 1
// iii) ties the QNH readback to the controller passing it, not to ambient
// ATIS. Wind is NfL convention, never readback-mandatory, so never a
// candidate. Kat-2 elements (taxi route "ueber Alpha") are never counted.
static std::vector<bzf_compliance::Element>
clearance_candidates(intent_parser::PilotIntent trigger) {
  using PI = intent_parser::PilotIntent;
  using E = bzf_compliance::Element;
  switch (trigger) {
  case PI::REQUEST_TAXI:
  case PI::INITIAL_CALL_INBOUND:
    return {E::Callsign, E::Runway, E::QNH};
  case PI::READY_FOR_DEPARTURE:
  case PI::READY_FOR_DEPARTURE_VFR:
  case PI::REQUEST_LANDING:
  case PI::REQUEST_TOUCH_AND_GO:
    return {E::Callsign, E::Runway};
  case PI::REQUEST_FREQUENCY:
    return {E::Callsign, E::Frequency};
  default:
    // Any other readback-demanding clearance: at least the callsign.
    return {E::Callsign};
  }
}

// Build the structured clearance snapshot from the template variable map
// and the raw (pre-fill) template string. Values are the exact tokens the
// controller spoke; `required` is the NfL candidate set filtered to the
// elements THIS transmission actually issued — detected structurally via
// the template placeholder, never re-parsed from rendered free text.
static bzf_compliance::ClearanceComponents
build_clearance_components(const intent_parser::PilotMessage &msg,
                           const std::map<std::string, std::string> &vars,
                           const std::string &response_template) {
  using E = bzf_compliance::Element;
  auto var = [&](const char *k) -> std::string {
    auto it = vars.find(k);
    return it == vars.end() ? std::string{} : it->second;
  };
  auto tmpl_has = [&](const char *placeholder) {
    return response_template.find(placeholder) != std::string::npos;
  };

  bzf_compliance::ClearanceComponents comp;
  comp.callsign = var("callsign");
  comp.runway = var("runway");
  comp.qnh = var("qnh");
  // The taxi / handoff templates use different frequency placeholders;
  // record whichever this template actually rendered.
  if (tmpl_has("{frequency}"))
    comp.frequency = var("frequency");
  else if (tmpl_has("{tower_frequency}"))
    comp.frequency = var("tower_frequency");
  else if (tmpl_has("{ground_frequency}"))
    comp.frequency = var("ground_frequency");
  comp.squawk = var("squawk");

  // required = candidates ∩ {issued this transmission}. Callsign always;
  // a fact element only when its placeholder is in the template AND it
  // carries a value.
  for (E e : clearance_candidates(msg.intent)) {
    bool keep = false;
    switch (e) {
    case E::Callsign:
      keep = true;
      break;
    case E::Runway:
      keep = tmpl_has("{runway}") && !comp.runway.empty();
      break;
    case E::QNH:
      keep = tmpl_has("{qnh}") && !comp.qnh.empty();
      break;
    case E::Frequency:
      keep = !comp.frequency.empty();
      break;
    case E::Squawk:
      keep = tmpl_has("{squawk}") && !comp.squawk.empty();
      break;
    }
    if (keep)
      comp.required.push_back(e);
  }
  return comp;
}

// ── Post-template hooks ─────────────────────────────────────────────

// BZF-Strict-Mode pilot-utterance conformance check. Active only when
// atc_profile() == "DE" AND settings::bzf_strict_mode() == true.
// Returns true if the pilot's readback was non-conformant — in that
// case `resp` is overwritten with a corrective tower response and the
// caller must skip apply_post_transition_hooks() (state must not
// advance, last_clearance_text_ stays armed for the retry).
static bool apply_bzf_strict_check(const intent_parser::PilotMessage &msg,
                                   ATCResponse &resp) {
  if (settings::atc_profile() != "DE")
    return false;
  if (!settings::bzf_strict_mode())
    return false;
  if (msg.intent != intent_parser::PilotIntent::READBACK)
    return false;
  if (g_state.last_clearance_text_.empty())
    return false;

  // Value-precise, weld-robust per-field diff against the stored structured
  // clearance. This is the STRICT threshold: every actually issued Kat-1
  // element must be read back (vs. the lenient recognition threshold in the
  // engine, which only needs callsign + one element). The diff additionally
  // splits each non-conformant element into Missing vs Wrong so the tower can
  // issue the NfL-correct correction (§25 b) Nr. 3: actively berichtigen a
  // wrong value; plain READ BACK for a merely incomplete readback).
  const auto diff = bzf_compliance::diff_readback(
      g_state.last_clearance_components_, msg.raw_transcript);
  const bool conformant =
      std::none_of(diff.begin(), diff.end(), [](const bzf_compliance::FieldDiff &d) {
        return d.status != bzf_compliance::ReadbackStatus::Ok;
      });
  if (conformant)
    return false;

  std::string callsign;
  if (!g_state.session_callsign_.empty())
    callsign = g_state.session_callsign_;
  else if (!msg.callsign.empty())
    callsign = msg.callsign;
  else
    callsign = settings::pilot_callsign();
  resp.text = bzf_compliance::build_correction_response(
      callsign, g_state.last_clearance_components_, diff);
  resp.next_state = g_state.state_;
  resp.requires_readback = true;

  std::string element_list;
  for (const auto &d : diff) {
    if (d.status == bzf_compliance::ReadbackStatus::Ok)
      continue;
    if (!element_list.empty())
      element_list += ",";
    element_list += bzf_compliance::element_name(d.element);
    element_list += (d.status == bzf_compliance::ReadbackStatus::Wrong)
                        ? ":wrong"
                        : ":missing";
  }
  logging::info("BZF strict: readback non-conformant %s", element_list.c_str());
  return true;
}

// Apply the post-template hooks that mutate persistent state — runway lock,
// readback tracking, departure-type, tower-only auto-advance. Step 4 will
// split these between the per-flow modules; for now they live alongside
// process().
static void
apply_post_transition_hooks(const intent_parser::PilotMessage &msg,
                            const xplane_context::XPlaneContext &ctx,
                            ATCResponse &resp,
                            const bzf_compliance::ClearanceComponents &components) {
  // Track readback state.
  if (msg.intent == intent_parser::PilotIntent::READBACK) {
    bump_gen();
    g_state.readback_pending_ = false;
    g_state.last_clearance_text_.clear();
    g_state.last_clearance_components_ = {};
  } else if (resp.requires_readback) {
    bump_gen();
    g_state.readback_pending_ = true;
    // Snapshot the clearance (text + structured components) so the next
    // pilot utterance can be matched against it by the deterministic
    // readback recognition and the BZF-strict conformance pass.
    g_state.last_clearance_text_ = resp.text;
    g_state.last_clearance_components_ = components;
  }

  // Leaving the controller's frequency or resetting drops stale readback
  // context. Without this, "frequency change good day" after an unread-back
  // takeoff clearance keeps readback_pending armed, and the UI hint pipeline
  // silences every other hint at the next airport.
  if (resp.next_state == ATCState::EN_ROUTE ||
      resp.next_state == ATCState::IDLE) {
    bump_gen();
    g_state.readback_pending_ = false;
    g_state.last_clearance_text_.clear();
    g_state.last_clearance_components_ = {};
  }

  // Lock runway on first clearance that references a runway. Same fallback
  // chain as ground_ops::get_runway() (msg → assigned → active → "28"),
  // inlined here to avoid exposing get_runway() outside ground_operations.
  if (g_state.assigned_runway_.empty() && resp.next_state != ATCState::IDLE) {
    std::string rwy = !msg.runway.empty()          ? msg.runway
                      : !ctx.active_runway.empty() ? ctx.active_runway
                                                   : std::string{"28"};
    if (!rwy.empty()) {
      bump_gen();
      g_state.assigned_runway_ = rwy;
      logging::info("Runway locked: %s", rwy.c_str());
    }
  }

  // Lock pilot callsign on first transition out of IDLE. Captures
  // whatever the parser extracted from the initial-call utterance,
  // when the pilot still speaks deliberately. Held until the dialog
  // returns to IDLE so a mid-session mishear cannot overwrite it.
  if (g_state.session_callsign_.empty() && !msg.callsign.empty() &&
      resp.next_state != ATCState::IDLE) {
    bump_gen();
    g_state.session_callsign_ = msg.callsign;
    logging::info("Session callsign locked: %s", msg.callsign.c_str());
  }

  // Apply state transition if we have a response.
  if (!resp.text.empty()) {
    ATCState prev_state = g_state.state_;
    std::string reason = "process:";
    reason += intent_parser::intent_template_key(msg.intent);
    internal::transition_to(resp.next_state, reason.c_str());

    // Track departure intent: PATTERN (default) vs CROSS_COUNTRY.
    if (resp.next_state == ATCState::DEPARTURE_CLEARED &&
        prev_state != ATCState::DEPARTURE_CLEARED) {
      bump_gen();
      g_state.departure_type_ =
          (msg.intent == intent_parser::PilotIntent::READY_FOR_DEPARTURE_VFR)
              ? internal::DepartureType::CROSS_COUNTRY
              : internal::DepartureType::PATTERN;
      logging::info("Departure type: %s",
                    internal::departure_type_name(g_state.departure_type_));
    } else if (prev_state == ATCState::DEPARTURE_CLEARED &&
               resp.next_state != ATCState::DEPARTURE_CLEARED) {
      bump_gen();
      g_state.departure_type_ = internal::DepartureType::PATTERN;
    }
  }

  // Release runway lock and session callsign when session ends.
  if (resp.next_state == ATCState::IDLE) {
    if (!g_state.assigned_runway_.empty()) {
      bump_gen();
      logging::info("Runway lock released");
      g_state.assigned_runway_.clear();
    }
    if (!g_state.session_callsign_.empty()) {
      bump_gen();
      g_state.session_callsign_.clear();
    }
  }

  // Tower-only airport: skip ground→tower handoff (no freq change needed).
  // Data-driven via flight_rules.tower_only_auto_advance.
  if (ctx.tower_only) {
    std::string current = state_name(g_state.state_);
    std::string next = flight_phase::get_tower_only_auto_advance(current);
    if (!next.empty()) {
      ATCState target = state_from_name(next);
      internal::transition_to(target, "tower_only_auto_advance");
      resp.next_state = target;
    }
  }

  // Snapshot the (non-corrective) tower response so REQUEST_REPEAT can
  // replay it. Strict-mode corrective responses never reach this point
  // because apply_bzf_strict_check() forces an early return — meaning
  // last_tower_response_text_ keeps the REAL last clearance, which is
  // what a pilot who forgot the QNH actually wants to hear again.
  if (!resp.text.empty()) {
    bump_gen();
    g_state.last_tower_response_text_ = resp.text;
  }
}

// ── Main pipeline ───────────────────────────────────────────────────

ATCResponse process(const intent_parser::PilotMessage &msg_in,
                    const xplane_context::XPlaneContext &ctx, double now_secs) {
  assert_flight_loop_thread();
  g_state.last_now_secs_ = now_secs; // heartbeat — no gen bump
  ATCResponse resp;

  // Mutable working copy: the tower-only first-contact collapse rewrites the
  // intent in place below (see apply_tower_only_initial_collapse). Every
  // downstream guard, the conformance check and the template lookup then see
  // the effective intent uniformly.
  intent_parser::PilotMessage msg = msg_in;

  // Cross-country departure promotion (NfL 2024 1.4.7 — destination stated
  // once). A pilot who declared a cross-country flight on first contact and
  // now calls a bare "abflugbereit" (no course marker repeated) parses as the
  // pattern READY_FOR_DEPARTURE. Without this the tower issues the pattern
  // departure ("melden Sie im Gegenanflug") and the post-takeoff flow expects
  // a pattern. Rewrite to the VFR variant so the latched session intent drives
  // the clearance template, next_state AND departure_type_ uniformly. A pattern
  // flight (declared or default) is left untouched. Phase/frequency guards are
  // identical for both intents, so the rewrite triggers no different guard.
  if (msg.intent == intent_parser::PilotIntent::READY_FOR_DEPARTURE &&
      g_state.flight_intent_declared_ && g_state.flight_intent_cross_country_) {
    msg.intent = intent_parser::PilotIntent::READY_FOR_DEPARTURE_VFR;
    logging::info("Cross-country flight intent: promoted READY_FOR_DEPARTURE "
                  "-> READY_FOR_DEPARTURE_VFR");
  }

  // Airport-change reset is also done per-frame in check_airport_change();
  // this in-process call is a safety net.
  check_airport_change(ctx, now_secs);

  if (ground_ops::handle_negative_correction(msg, ctx, resp))
    return resp;

  // REQUEST_REPEAT — pilot asked the tower to repeat the last real
  // clearance (NfL §18 c) Nr. 4 "WIEDERHOLEN SIE / SAY AGAIN").
  // Replay last_tower_response_text_ verbatim. State does NOT
  // advance and readback_pending_ is preserved — if the pilot still
  // owed a readback before asking for the repeat, they still do
  // after hearing it again.
  if (msg.intent == intent_parser::PilotIntent::REQUEST_REPEAT) {
    if (!g_state.last_tower_response_text_.empty()) {
      resp.text = g_state.last_tower_response_text_;
    } else {
      std::string cs;
      if (!g_state.session_callsign_.empty())
        cs = g_state.session_callsign_;
      else if (!msg.callsign.empty())
        cs = msg.callsign;
      else
        cs = settings::pilot_callsign();
      resp.text = cs + ", keine vorherige Anweisung zum Wiederholen.";
    }
    resp.next_state = g_state.state_;
    resp.requires_readback = g_state.readback_pending_;
    return resp;
  }

  ground_ops::apply_state_reverts(msg);

  if (ground_ops::handle_info_flow(msg, ctx, resp))
    return resp;

  if (ground_ops::handle_unicom_flow(msg, ctx, resp))
    return resp;

  if (ground_ops::handle_frequency_hint(msg, ctx, resp))
    return resp;

  ground_ops::apply_state_frequency_validity(ctx);
  ground_ops::apply_frequency_auto_corrections(ctx);

  if (ground_ops::handle_idle_redirects(msg, ctx, resp))
    return resp;

  // Tower-only apron first contact collapses INITIAL_CALL_TOWER ->
  // INITIAL_CALL_GROUND so the GROUND conformance check + hint apply. Runs
  // before the guards so phase/freq preconditions, conformance and the
  // template lookup all see the effective intent.
  ground_ops::apply_tower_only_initial_collapse(msg, ctx);

  if (ground_ops::check_phase_precondition(msg, ctx, resp))
    return resp;

  if (ground_ops::check_freq_precondition(msg, ctx, resp))
    return resp;

  // Reset the session-lifecycle was_airborne flag when a new departure
  // cycle starts on the ground. The intent list documents the intent
  // (departure-anmeldung); the on_ground gate is the physical
  // invariant ("a new departure cycle begins on the ground") and
  // catches parser misclassifications mid-air. INITIAL_CALL_INBOUND is
  // deliberately NOT in the list — inbound is the opposite of a
  // departure cycle.
  //
  // Runs BEFORE the conformance guard's early return so a strict-mode
  // INITIAL_CALL_GROUND re-request still resets the flag — a re-request
  // is conceptually the start of a new ground departure cycle, and the
  // invariant must hold independently of the guard's early return.
  // msg.intent is final here (last mutated by the tower-only collapse
  // above); ctx.on_ground is independent of resp, so no new dependency
  // is introduced by hoisting this ahead of the template lookup.
  //
  // F.2 interaction: REQUEST_TAXI gets remapped to REQUEST_TAXI_PARKING
  // by apply_adjustments() upstream when at_airport_after_landing is
  // true; REQUEST_TAXI_PARKING is not in the reset list, so the flag
  // stays armed during the post-landing taxi-back. The reset only
  // fires when REQUEST_TAXI survives the remap — i.e. when the post-
  // landing window has already closed and the pilot really is starting
  // a new flight.
  using PI = intent_parser::PilotIntent;
  if (ctx.on_ground &&
      (msg.intent == PI::REQUEST_TAXI ||
       msg.intent == PI::INITIAL_CALL_GROUND ||
       msg.intent == PI::INITIAL_CALL_TOWER || msg.intent == PI::INITIAL_CALL))
    internal::set_was_airborne(false);

  // BZF first-call conformance (DE, INITIAL_CALL_GROUND only). strict=false
  // only logs missing recommended elements for debrief and proceeds to the
  // normal initial-contact reply; strict=true sends a targeted re-request
  // and holds IDLE until the call is complete. See initial_call_conformance.
  if (ground_ops::apply_initial_call_conformance(msg, ctx, resp))
    return resp;

  // Template-based response lookup.
  auto vars = ground_ops::build_vars(msg, ctx);
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  std::string state_str = state_name(g_state.state_);

  auto tmpl =
      atc_templates::lookup(true, state_str, intent_key, ctx.tower_only);
  resp.text = atc_templates::fill(tmpl.response_template, vars);
  resp.next_state = state_from_name(tmpl.next_state);
  resp.requires_readback = tmpl.requires_readback;

  // Structured snapshot of this clearance for readback matching. Built
  // from the variable map + raw template (the values the controller
  // spoke + which Kat-1 elements this transmission issued), stored by
  // apply_post_transition_hooks() iff requires_readback.
  auto clearance_components =
      build_clearance_components(msg, vars, tmpl.response_template);

  // Phase-4: per-flow sequencing overlay. The Pattern side owns the
  // "number N to land, follow X on Y" / "continue approach, traffic on
  // the runway" rewrites; the XC side is a no-op placeholder for
  // Phase 5 ("expect number N" prefixes from Approach). Both inspect
  // resp + vars and may rewrite resp.text in place.
  const auto &traffic_now = traffic_context::current();
  if (pattern_flow::is_pattern_state(resp.next_state))
    pattern_flow::apply_landing_sequence(msg, ctx, traffic_now, vars, resp);
  else if (crosscountry_flow::is_xc_state(resp.next_state))
    crosscountry_flow::apply_landing_sequence(msg, ctx, traffic_now, vars,
                                              resp);

  // BZF-Strict-Mode conformance check on READBACK intents. Active only
  // when atc_profile() == "DE" AND settings::bzf_strict_mode() == true.
  // When a readback is missing safety-relevant elements (NfL §25 b)
  // Nr. 1), the tower issues a corrective response and the state does
  // NOT advance — the pilot has to read back correctly.
  if (apply_bzf_strict_check(msg, resp)) {
    // Hold readback expectation; don't run apply_post_transition_hooks
    // because the standard path would clear readback_pending_ on a
    // READBACK intent. Leave last_clearance_text_ in place so the
    // pilot can re-attempt against the same clearance.
    bump_gen();
    g_state.readback_pending_ = true;
    return resp;
  }

  apply_post_transition_hooks(msg, ctx, resp, clearance_components);
  return resp;
}

void check_airport_change(const xplane_context::XPlaneContext &ctx,
                          double now_secs) {
  // Owned by crosscountry_flow since step 4 — EN_ROUTE is the only
  // state this reacts to. Facade delegates so external callers
  // (atc_session, process()'s in-process safety net) keep their
  // existing entry point.
  crosscountry_flow::check_airport_change(ctx, now_secs);
}

// ── Auto-correction ─────────────────────────────────────────────────
// State for the per-frame correction timer now lives in
// g_state.active_correction_key_ / g_state.correction_timer_ (heartbeat
// fields — no gen-bump on per-frame ticking).

void check_auto_correction(flight_phase::FlightPhase phase, float dt,
                           double now_secs) {
  assert_flight_loop_thread();
  g_state.last_now_secs_ = now_secs; // heartbeat — no gen bump
  if (g_state.state_ == ATCState::IDLE ||
      g_state.state_ == ATCState::UNICOM_ACTIVE)
    return;

  std::string current_state = state_name(g_state.state_);
  auto *corrections = flight_phase::get_auto_corrections(current_state);
  if (!corrections)
    return;

  // Find first matching correction condition
  for (const auto &[cond_name, ac] : *corrections) {
    bool matches = false;
    for (auto p : ac.phases) {
      if (phase == p) {
        matches = true;
        break;
      }
    }

    if (matches) {
      // The legacy CROSS_COUNTRY suppression hack (skip Pattern
      // auto-correction when DEPARTURE_CLEARED + departure_type ==
      // CROSS_COUNTRY → PATTERN_ENTRY) lived here pre-step-4. After
      // step 3b the flight_rules.json on_airborne entry only exists
      // under "Pattern/DEPARTURE_CLEARED" — when the pilot is in
      // XC/DEPARTURE_CLEARED, get_auto_corrections returns nullptr
      // and we never reach this branch. The check was redundant; the
      // structural split in the data layer now does the job.
      std::string key;
      key.reserve(current_state.size() + 1 + cond_name.size());
      key += current_state;
      key += ':';
      key += cond_name;
      if (key != g_state.active_correction_key_) {
        g_state.active_correction_key_ = key;
        g_state.correction_timer_ = 0.0f;
      }
      g_state.correction_timer_ += dt;

      if (g_state.correction_timer_ >=
          ac.delay_sec * settings::auto_correction_factor()) {
        ATCState new_state = state_from_name(ac.next_state);
        logging::info("Auto-correction: phase=%s, condition=%s, after %.1fs",
                      flight_phase::phase_name(phase), cond_name.c_str(),
                      g_state.correction_timer_);
        std::string reason = "auto_correction:";
        reason += cond_name;
        internal::transition_to(new_state, reason.c_str());
        bump_gen();
        g_state.readback_pending_ = false;
        if (new_state == ATCState::IDLE) {
          if (!g_state.assigned_runway_.empty()) {
            logging::info("Runway lock released (auto-correction)");
            g_state.assigned_runway_.clear();
          }
          g_state.session_callsign_.clear();
        }
        g_state.active_correction_key_.clear();
        g_state.correction_timer_ = 0.0f;
      }
      return;
    }
  }

  // No matching condition — reset timer (heartbeat, no gen bump)
  g_state.active_correction_key_.clear();
  g_state.correction_timer_ = 0.0f;
}

// ── State history accessors ─────────────────────────────────────────

ATCState previous_state() {
  if (g_state.history_.empty())
    return ATCState::IDLE;
  return g_state.history_.back().state;
}

bool was_recently_in(ATCState s, double max_age_secs, double now_secs) {
  if (g_state.state_ == s)
    return true;
  for (auto it = g_state.history_.rbegin(); it != g_state.history_.rend();
       ++it) {
    if (now_secs - it->timestamp_secs > max_age_secs)
      return false; // entries beyond this are even older
    if (it->state == s)
      return true;
  }
  return false;
}

bool just_landed(double now_secs, double window_secs) {
  return was_recently_in(ATCState::LANDING_CLEARED, window_secs, now_secs) ||
         was_recently_in(ATCState::TOUCH_AND_GO_CLEARED, window_secs, now_secs);
}

bool at_airport_after_landing(const xplane_context::XPlaneContext &ctx) {
  if (!ctx.on_ground)
    return false;
  auto is_landing = [](ATCState s) {
    return s == ATCState::LANDING_CLEARED ||
           s == ATCState::TOUCH_AND_GO_CLEARED;
  };
  if (is_landing(g_state.state_))
    return true;
  // Walk newest-to-oldest. A DEPARTURE_CLEARED *after* the most
  // recent landing means the pilot is on a new flight (mid-taxi to
  // takeoff or already cleared) — drop out of the post-landing
  // window. Other transitional states (GROUND_CONTACT, TAXI_CLEARED,
  // TOWER_CONTACT) can legitimately appear during taxi-in and don't
  // disqualify.
  for (auto it = g_state.history_.rbegin(); it != g_state.history_.rend();
       ++it) {
    if (it->state == ATCState::DEPARTURE_CLEARED)
      return false;
    if (is_landing(it->state))
      return true;
  }
  return false;
}

const std::deque<StateHistoryEntry> &get_history() { return g_state.history_; }

std::string history_csv() {
  std::string out;
  for (const auto &e : g_state.history_) {
    if (!out.empty())
      out += ',';
    out += state_name(e.state);
  }
  if (!out.empty())
    out += ',';
  out += state_name(g_state.state_);
  return out;
}

// ── Snapshot / Restore (TTS revert guard) ───────────────────────────

struct AtcStateSnapshot::Impl {
  AtcMachineState state;
};

AtcStateSnapshot::AtcStateSnapshot() : impl_(std::make_unique<Impl>()) {}
AtcStateSnapshot::AtcStateSnapshot(const AtcStateSnapshot &other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}
AtcStateSnapshot::AtcStateSnapshot(AtcStateSnapshot &&) noexcept = default;
AtcStateSnapshot &AtcStateSnapshot::operator=(const AtcStateSnapshot &other) {
  if (this != &other)
    impl_ = std::make_unique<Impl>(*other.impl_);
  return *this;
}
AtcStateSnapshot &
AtcStateSnapshot::operator=(AtcStateSnapshot &&) noexcept = default;
AtcStateSnapshot::~AtcStateSnapshot() = default;

AtcStateSnapshot capture_snapshot() {
  assert_flight_loop_thread();
  AtcStateSnapshot snap;
  snap.impl_->state = g_state; // full struct copy, gen included
  return snap;
}

uint64_t current_gen() { return g_state.gen; }

bool restore_snapshot_if_gen(const AtcStateSnapshot &snap,
                             uint64_t expected_gen) {
  assert_flight_loop_thread();
  if (g_state.gen != expected_gen)
    return false; // stale — a later mutation already advanced state
  // Bump the restored gen so it ends up strictly above any value the
  // caller has observed. Any other pending guard with an
  // expected_gen <= current g_state.gen will see a mismatch on its own
  // restore attempt and bail cleanly.
  const uint64_t new_gen = g_state.gen + 1;
  g_state = snap.impl_->state;
  g_state.gen = new_gen;
  logging::info("ATC state restored from snapshot (gen %llu -> %llu)",
                static_cast<unsigned long long>(expected_gen),
                static_cast<unsigned long long>(new_gen));
  return true;
}

// ── Traffic-advisory rendering ──────────────────────────────────────

std::string
render_traffic_advisory(const std::map<std::string, std::string> &advisory_vars,
                        const xplane_context::XPlaneContext &ctx,
                        const std::string &template_key) {
  // Build base vars (callsign, airport, etc.) and merge in the
  // advisor-supplied advisory placeholders. ATCState is intentionally
  // not touched here — the traffic dialog runs parallel to the main
  // flow (see traffic_dialog.{hpp,cpp}).
  intent_parser::PilotMessage synthetic_msg;
  auto vars = ground_ops::build_vars(synthetic_msg, ctx);
  for (const auto &[k, v] : advisory_vars)
    vars[k] = v;

  auto tmpl = atc_templates::lookup(true, "TRAFFIC_DIALOG", template_key);
  return atc_templates::fill(tmpl.response_template, vars);
}

} // namespace atc_state_machine
