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

#ifndef ATC_FLOWS_CROSSCOUNTRY_FLOW_HPP
#define ATC_FLOWS_CROSSCOUNTRY_FLOW_HPP

// Cross-country flow. Step 4 of the A1 flow-split refactor introduces
// the disjoint enum that mirrors the XC-side ATCState entries and
// hosts the XC-specific airport-change logic that used to live in
// atc_state_machine.cpp.
//
// Today the enum mirrors the three XC-side ATCState values 1:1 — once
// the facade is gone (step 5), this becomes the canonical type. The
// qualified state strings ("XC/EN_ROUTE" etc.) are produced
// exclusively by state_name(State) below.

#include "atc/atc_state_machine.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"
#include "data/traffic_context.hpp"

#include <map>
#include <string>

namespace crosscountry_flow {

enum class State {
  DEPARTURE_CLEARED,
  EN_ROUTE,
};

// Qualified state name. Single source of truth for the "XC/" prefix
// convention.
const char *state_name(State s);

// Map between the legacy ATCState enum and the disjoint XC enum.
// Returns DEPARTURE_CLEARED for non-XC inputs — callers must gate
// with is_xc_state().
State from_atc_state(atc_state_machine::ATCState s);
atc_state_machine::ATCState to_atc_state(State s);

// True iff `s` is an XC-side ATCState (mirror of the from_atc_state
// fallthrough set).
bool is_xc_state(atc_state_machine::ATCState s);

// Per-frame airport-change reset. When the pilot is EN_ROUTE and the
// nearest airport changes (e.g. crossing into a new control zone),
// drops to IDLE so the UI hint pipeline (and the next pilot call)
// treats this as a fresh inbound contact for the new airport. Owned
// by CrossCountryFlow because EN_ROUTE is the only state this
// triggers from. The reset clears readback_pending, assigned_runway,
// and departure_type via the internal:: bridge — those statics
// belong to atc_state_machine today, will move to per-flow ownership
// alongside the facade tear-down in step 5.
void check_airport_change(const xplane_context::XPlaneContext &ctx,
                          double now_secs);

// Reset internal state (currently: last-seen airport id). Called when
// the active flow switches away from CrossCountry.
void reset();

// Phase-4 landing-sequence overlay for the cross-country side. Called
// after the regular template lookup for users on the cross-country side.
// In Phase 4 this is a deliberate no-op: the approach handoff is
// already complete by the time landing sequencing matters, and the
// pattern-side overlay takes over once the user lands on
// Pattern/PATTERN_ENTRY. Reserved for Phase 5 — when Approach starts
// issuing "expect number N" prefixes ahead of the Tower handoff.
void apply_landing_sequence(const intent_parser::PilotMessage &msg,
                            const xplane_context::XPlaneContext &ctx,
                            const traffic_context::TrafficContext &traffic,
                            std::map<std::string, std::string> &vars,
                            atc_state_machine::ATCResponse &resp);

} // namespace crosscountry_flow

#endif // ATC_FLOWS_CROSSCOUNTRY_FLOW_HPP
