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

#ifndef ATC_FLOWS_PATTERN_FLOW_HPP
#define ATC_FLOWS_PATTERN_FLOW_HPP

// Pattern-flow state. Step 4 of the A1 flow-split refactor introduces
// the disjoint enum that will replace the relevant ATCState entries in
// step 5. Today the enum mirrors the four Pattern-side ATCState values
// 1:1 — once the facade is gone, this becomes the canonical type.
//
// The qualified state strings ("Pattern/PATTERN_ENTRY" etc.) are
// produced exclusively by state_name(State) below. External consumers
// (templates, flight_rules, scenario tests) consume these strings via
// the existing data files (post step 3b). Helpers that need to detect
// "this string belongs to PatternFlow" should call state_history-style
// predicates (added later), not raw string comparison.

#include "atc/atc_state_machine.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"
#include "data/traffic_context.hpp"

#include <map>
#include <string>

namespace pattern_flow {

enum class State {
  DEPARTURE_CLEARED,
  PATTERN_ENTRY,
  LANDING_CLEARED,
  TOUCH_AND_GO_CLEARED,
};

// Qualified state name. Single source of truth for the "Pattern/" prefix
// convention — any other helper that needs the string consumes it via
// this function, never via inline literals.
const char *state_name(State s);

// Map between the legacy ATCState enum (in atc_state_machine) and the
// disjoint Pattern enum. Returns std::nullopt when the ATCState is not
// a Pattern-side state; the caller must dispatch via flow_coordinator
// before calling these.
State from_atc_state(atc_state_machine::ATCState s);
atc_state_machine::ATCState to_atc_state(State s);

// True iff `s` is a Pattern-side ATCState (mirror of the from_atc_state
// fallthrough set). Used by the coordinator and state_history helpers
// without exposing the enum range to callers.
bool is_pattern_state(atc_state_machine::ATCState s);

// Phase-4 landing-sequence overlay. Called from the main ATC pipeline
// after the regular template lookup has filled `resp`. When the pilot
// is requesting / reaching landing clearance AND there is traffic in
// front, the response is rewritten via the `number_to_land_follow`
// (or `continue_approach_traffic_runway`) template; the placeholders
// {seq_number} / {seq_type} / {seq_position} in `vars` are populated
// from the sequence result.
//
// No-op when:
//   - `resp.next_state` is not Pattern/LANDING_CLEARED, or
//   - the user is first to land AND the runway is not occupied, or
//   - the active runway cannot be resolved from `ctx`.
//
// Mutates `vars` and `resp.text`. Does not change `resp.next_state` —
// the post-transition hook still drives the regular state move.
void apply_landing_sequence(const intent_parser::PilotMessage &msg,
                            const xplane_context::XPlaneContext &ctx,
                            const traffic_context::TrafficContext &traffic,
                            std::map<std::string, std::string> &vars,
                            atc_state_machine::ATCResponse &resp);

} // namespace pattern_flow

#endif // ATC_FLOWS_PATTERN_FLOW_HPP
