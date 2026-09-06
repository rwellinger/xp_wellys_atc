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

#include "atc/flows/crosscountry_flow.hpp"

#include "atc/flows/state_storage.hpp"
#include "core/logging.hpp"

#include <string>

namespace crosscountry_flow {

using atc_state_machine::ATCState;
namespace internal = atc_state_machine::internal;

// Last airport observed while EN_ROUTE. Pre-step-4 this lived in
// atc_state_machine.cpp; it is XC-specific (only consulted while
// EN_ROUTE), so it belongs here under the Schicht-1 ownership rule.
static std::string last_airport_id_;

const char *state_name(State s) {
  switch (s) {
  case State::DEPARTURE_CLEARED:
    return "XC/DEPARTURE_CLEARED";
  case State::EN_ROUTE:
    return "XC/EN_ROUTE";
  }
  return "XC/UNKNOWN";
}

State from_atc_state(ATCState s) {
  switch (s) {
  case ATCState::DEPARTURE_CLEARED:
    return State::DEPARTURE_CLEARED;
  case ATCState::EN_ROUTE:
    return State::EN_ROUTE;
  default:
    return State::EN_ROUTE;
  }
}

ATCState to_atc_state(State s) {
  switch (s) {
  case State::DEPARTURE_CLEARED:
    return ATCState::DEPARTURE_CLEARED;
  case State::EN_ROUTE:
    return ATCState::EN_ROUTE;
  }
  return ATCState::EN_ROUTE;
}

bool is_xc_state(ATCState s) {
  switch (s) {
  case ATCState::EN_ROUTE:
    return true;
  // DEPARTURE_CLEARED is shared between Pattern and XC — the
  // discriminator is internal::departure_type(). is_xc_state() means
  // "lives exclusively on the XC side"; DEPARTURE_CLEARED does not.
  default:
    return false;
  }
}

void check_airport_change(const xplane_context::XPlaneContext &ctx,
                          double now_secs) {
  internal::set_last_now_secs(now_secs);
  // Per-frame airport tracker. While not EN_ROUTE, just shadow the
  // current nearest airport so the moment the pilot enters EN_ROUTE we
  // have a valid baseline. While EN_ROUTE and the airport changes, drop
  // to IDLE so the UI hint pipeline (and the next pilot call) treat
  // this as a fresh inbound contact for the new airport. This unlocks
  // INITIAL_CALL_INBOUND hints as soon as the airport lock changes —
  // important for VFR arrivals at small airports without an Approach
  // controller (e.g. LSZG), where staying EN_ROUTE silences the hints.
  if (ctx.nearest_airport_id.empty())
    return;

  if (internal::get_state_ref() != ATCState::EN_ROUTE) {
    last_airport_id_ = ctx.nearest_airport_id;
    return;
  }

  if (last_airport_id_.empty()) {
    last_airport_id_ = ctx.nearest_airport_id;
    return;
  }

  if (ctx.nearest_airport_id == last_airport_id_)
    return;

  logging::info("ATC: airport changed %s -> %s while EN_ROUTE, resetting",
                last_airport_id_.c_str(), ctx.nearest_airport_id.c_str());
  internal::transition_to(ATCState::IDLE, "airport_change_en_route");
  // The previous airport's ATC is no longer talking to us — any pending
  // readback context dies with the handoff. Without this, the UI hint
  // pipeline keeps showing only READBACK at the new airport because
  // readback_override stays armed.
  internal::set_readback_pending(false);
  internal::clear_assigned_runway();
  internal::set_departure_type(internal::DepartureType::PATTERN);
  last_airport_id_ = ctx.nearest_airport_id;
}

void reset() { last_airport_id_.clear(); }

void apply_landing_sequence(const intent_parser::PilotMessage &msg,
                            const xplane_context::XPlaneContext &ctx,
                            const traffic_context::TrafficContext &traffic,
                            std::map<std::string, std::string> &vars,
                            atc_state_machine::ATCResponse &resp) {
  // Phase-4 no-op. The pattern-side overlay owns sequencing; XC hands
  // off to Pattern at the moment of clearance. Kept here as a
  // structural symmetry placeholder so Phase 5 can wire "expect number
  // N to land" into the Approach reply without re-plumbing the
  // process() dispatcher.
  (void)msg;
  (void)ctx;
  (void)traffic;
  (void)vars;
  (void)resp;
}

} // namespace crosscountry_flow
