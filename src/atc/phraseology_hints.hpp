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

#ifndef PHRASEOLOGY_HINTS_HPP
#define PHRASEOLOGY_HINTS_HPP

#include "atc/atc_state_machine.hpp"
#include "atc/flight_phase.hpp"
#include "core/xplane_context.hpp"

#include <string>
#include <vector>

namespace phraseology_hints {

// Inputs to the matrix lookup. UI builds one of these per frame from
// the live context and asks for the legitimate intent menu.
struct HintQuery {
  atc_state_machine::ATCState state = atc_state_machine::ATCState::IDLE;
  flight_phase::FlightPhase phase = flight_phase::FlightPhase::PARKED;
  xplane_context::FacilityType facility = xplane_context::FacilityType::UNKNOWN;
  xplane_context::FrequencyType frequency_type =
      xplane_context::FrequencyType::UNKNOWN;
  bool tower_only = false;
  bool post_landing = false;
};

void init();
void stop();
void reload();

// Top-down matrix walk. First rule whose constraints all match the
// query wins; its intents are returned (may be empty). Returns an
// empty vector if the JSON failed to load.
std::vector<std::string> lookup(const HintQuery &q);

} // namespace phraseology_hints

#endif // PHRASEOLOGY_HINTS_HPP
