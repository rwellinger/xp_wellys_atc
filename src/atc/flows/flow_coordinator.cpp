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

#include "atc/flows/flow_coordinator.hpp"

#include "atc/atc_state_machine.hpp"
#include "atc/flows/state_storage.hpp"

namespace flow_coordinator {

using atc_state_machine::ATCState;
namespace internal = atc_state_machine::internal;

const char *active_flow_name(ActiveFlow f) {
  switch (f) {
  case ActiveFlow::GroundOps:
    return "GroundOps";
  case ActiveFlow::Pattern:
    return "Pattern";
  case ActiveFlow::CrossCountry:
    return "CrossCountry";
  }
  return "Unknown";
}

ActiveFlow active() {
  // Step 4 will move EN_ROUTE into CrossCountryFlow.
  // Today the legacy state machine treats them as terminal states with
  // no departure flag, so they map back to GroundOps for the
  // active-flow view (the dialog is no longer in a clearance / pattern
  // flow). They're grouped with the genuine GroundOps states here.
  ATCState s = internal::get_state_ref();
  switch (s) {
  case ATCState::DEPARTURE_CLEARED:
    return internal::departure_type() == internal::DepartureType::CROSS_COUNTRY
               ? ActiveFlow::CrossCountry
               : ActiveFlow::Pattern;
  case ATCState::PATTERN_ENTRY:
  case ATCState::LANDING_CLEARED:
  case ATCState::TOUCH_AND_GO_CLEARED:
    return ActiveFlow::Pattern;
  case ATCState::IDLE:
  case ATCState::GROUND_CONTACT:
  case ATCState::TAXI_CLEARED:
  case ATCState::TOWER_CONTACT:
  case ATCState::UNICOM_ACTIVE:
  case ATCState::EN_ROUTE:
    return ActiveFlow::GroundOps;
  }
  return ActiveFlow::GroundOps;
}

std::string state_name() {
  // Step 3b will prefix with "Pattern/" or "XC/" based on active().
  // Today: raw ATCState name only, so existing logs + scenario-test
  // asserts keep matching.
  return atc_state_machine::state_name(internal::get_state_ref());
}

} // namespace flow_coordinator
