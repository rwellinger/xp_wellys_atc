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

#ifndef ATC_FLOWS_FLOW_COORDINATOR_HPP
#define ATC_FLOWS_FLOW_COORDINATOR_HPP

// Coordinator scaffolding for the A1 flow-split refactor. Step 2
// introduces the data shapes; step 4 wires them up to actual per-flow
// implementations. Today these types are derived from
// atc_state_machine's state_ + departure_type_, so they read like a
// thin view over the existing state machine. Once PatternFlow and
// CrossCountryFlow own their own state in step 4, the derivation
// flips: the coordinator becomes authoritative and the legacy flag
// disappears.

#include "atc/atc_state_machine.hpp"

#include <string>

namespace flow_coordinator {

// Which sub-flow currently owns the dialog. Today (step 2) it is
// derived from atc_state_machine's state_ + departure_type_:
//   - GroundOps   when the dialog is in a ground-side state (IDLE,
//                 GROUND_CONTACT, TAXI_CLEARED, TOWER_CONTACT, UNICOM_ACTIVE)
//                 OR airborne but with no active departure
//                 (EN_ROUTE — transitions out of the XC flow in step 4).
//   - Pattern     when state_ ∈ {DEPARTURE_CLEARED with
//                 departure_type_ == PATTERN, PATTERN_ENTRY,
//                 LANDING_CLEARED, TOUCH_AND_GO_CLEARED}.
//   - CrossCountry when state_ == DEPARTURE_CLEARED and
//                  departure_type_ == CROSS_COUNTRY, plus EN_ROUTE
//                  on a fresh XC departure (step 4 makes this
//                  exclusive — today EN_ROUTE is treated as GroundOps
//                  for backwards-compatibility with the existing
//                  state machine).
enum class ActiveFlow {
  GroundOps,
  Pattern,
  CrossCountry,
};

const char *active_flow_name(ActiveFlow f);

// Returns the active flow derived from atc_state_machine state.
// Pure read; safe to call from any thread that already serialises
// state mutations through the engine's flight-loop callback.
ActiveFlow active();

// Qualified state name in the form expected once the flows split.
// In step 2 this still returns the raw ATCState name (e.g.
// "PATTERN_ENTRY") so existing log + scenario-test asserts keep
// matching. Step 3b prefixes it with "Pattern/" or "XC/".
std::string state_name();

// Pre-allocated result shape for step 4's process() dispatch. The
// sub-flow returns a FlowResult; the coordinator inspects
// `transition` to decide whether to switch the active flow. Defined
// here so the type is stable across the migration; not yet produced
// by atc_state_machine::process (that wiring happens in step 4).
struct FlowResult {
  atc_state_machine::ATCResponse response;
  enum class Transition {
    Stay,                  // remain in the current flow
    HandoffToPattern,      // GroundOps -> PatternFlow (READY_FOR_DEPARTURE)
    HandoffToCrossCountry, // GroundOps -> CrossCountryFlow (READY_FOR_DEPARTURE_VFR)
    CompletedToGroundOps,  // PatternFlow / CrossCountryFlow -> GroundOps
  } transition = Transition::Stay;
  std::string assigned_runway; // populated on Handoff-* by GroundOps
};

} // namespace flow_coordinator

#endif // ATC_FLOWS_FLOW_COORDINATOR_HPP
