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

#ifndef FLIGHT_PHASE_HPP
#define FLIGHT_PHASE_HPP

#include "core/xplane_context.hpp"

#include <map>
#include <string>
#include <vector>

namespace flight_phase {

enum class FlightPhase {
  PARKED,
  TAXI,
  TAKEOFF_ROLL,
  CLIMB,
  PATTERN,
  FINAL_APPROACH,
  LANDING_ROLL,
  CRUISE,
};

struct IntentPrecondition {
  std::vector<FlightPhase> allowed_phases;
  std::string rejection_parked;
  std::string rejection_airborne;
  std::string rejection_ground;
};

struct AutoCorrection {
  std::vector<FlightPhase> phases;
  std::string next_state;
  float delay_sec = 3.0f;
};

struct FrequencyAutoCorrection {
  std::vector<xplane_context::FrequencyType> frequencies;
  std::string next_state;
  std::string log;
};

struct FrequencyRule {
  std::vector<std::string> allowed; // FrequencyType names: GROUND, TOWER, etc.
  std::string rejection;            // rendered when current freq not allowed
};

// IDLE-state intent redirect (e.g. "REQUEST_TAXI on Tower freq → contact
// ground"). When `state_ == IDLE` and the pilot's intent + active frequency
// match an IdleRedirect, the configured response is rendered and the state
// stays IDLE — no template lookup happens. `unless_flag == "tower_only"`
// suppresses the redirect at airports that have only a Tower controller.
struct IdleRedirect {
  std::vector<std::string> intent_in;
  xplane_context::FrequencyType freq_type =
      xplane_context::FrequencyType::UNKNOWN;
  std::string unless_flag; // "tower_only" or empty
  std::string response;    // template (filled via atc_templates::fill)
  std::string log;
};

// One-shot pre-template state revert. When the pilot re-issues an intent
// while already in `in_state`, revert to `revert_to` so the same intent can
// be re-processed from a state where the rule actually applies. Used for
// "RE-CLEARANCE": pilot re-says READY_FOR_DEPARTURE while already in
// DEPARTURE_CLEARED → revert to TOWER_CONTACT so the new request reroutes
// (pattern vs. cross-country).
struct StateRevert {
  std::vector<std::string> on_intent_in;
  std::string in_state;
  std::string revert_to;
  bool reset_departure_type = false;
  std::string log;
};

// "Wrong frequency at a towered airport" hint. Triggered when freq_type
// is UNKNOWN but the airport is towered, and the pilot did not just send
// a READBACK. The response template is selected by ground- vs.
// tower-class intent — at tower-only airports the tower response is
// always used.
struct FrequencyHint {
  std::vector<std::string> ground_intents; // intent keys
  std::string ground_response;
  std::string tower_response;
};

void init();
void stop();
void update(const xplane_context::XPlaneContext &ctx, float dt);
void reload();

// Reset only the runtime phase state (current/candidate phase, hysteresis
// timer, airborne latch) back to a fresh PARKED baseline, leaving the
// loaded config (thresholds, preconditions, ...) intact. Used by the
// Catch2 module-reset listener so an airborne test's leftover phase cannot
// make a later phase-precondition guard early-return. See Issue #3.
void reset();

FlightPhase get();
const char *phase_name(FlightPhase phase);
bool is_airborne(FlightPhase phase);
bool is_on_ground(FlightPhase phase);

// Precondition check: returns empty string if allowed, rejection message if not
std::string check_precondition(const std::string &intent_key,
                               FlightPhase phase);

// Auto-correction lookup for a given ATC state
const std::map<std::string, AutoCorrection> *
get_auto_corrections(const std::string &atc_state);

// Frequency-driven forward-progression lookup for a given ATC state.
// First-match semantics. Used inline in atc_state_machine::process()
// to advance state when the pilot is already on a frequency further
// along the expected flow than the current state implies.
const std::map<std::string, FrequencyAutoCorrection> *
get_frequency_auto_corrections(const std::string &atc_state);

FlightPhase phase_from_name(const std::string &name);

// Check if intent is valid for the current COM frequency type
bool is_intent_valid_for_frequency(const std::string &intent_key,
                                   xplane_context::FrequencyType freq_type);

// Frequency-precondition check: returns empty string if allowed, rejection
// message template if the intent is not permitted on the current frequency.
std::string
check_frequency_precondition(const std::string &intent_key,
                             xplane_context::FrequencyType freq_type);

// Get pilot phraseology template for an intent (for UI helper text)
std::string get_pilot_phraseology(const std::string &intent_key);

// State-vs-frequency validity table. Returns the list of ATCState names
// that are still valid when the pilot is on the given frequency type. If
// the current state is not in the list, atc_state_machine resets to
// IDLE before continuing. Returns nullptr when no rule is configured for
// that frequency type (then no validation is applied).
const std::vector<std::string> *
get_state_frequency_validity(xplane_context::FrequencyType freq_type);

// IDLE-state intent redirects, evaluated in array order. First match
// wins. Empty list when none configured.
const std::vector<IdleRedirect> &get_idle_redirects();

// Pre-template state reverts (e.g. RE-CLEARANCE). Empty list when none.
const std::vector<StateRevert> &get_state_reverts();

// Tower-only airport auto-advance: returns the next ATCState name for
// the given current state, or empty string when no rule applies.
std::string get_tower_only_auto_advance(const std::string &state);

// Wrong-frequency hint configuration (UNKNOWN freq at towered airport).
// Returns nullptr when not configured.
const FrequencyHint *get_frequency_hint();

} // namespace flight_phase

#endif // FLIGHT_PHASE_HPP
