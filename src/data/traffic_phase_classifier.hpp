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

#ifndef TRAFFIC_PHASE_CLASSIFIER_HPP
#define TRAFFIC_PHASE_CLASSIFIER_HPP

#include "data/traffic_context.hpp"

#include <optional>
#include <string>

// Phase 3/4 traffic-phase classifier. SDK-free, provider-agnostic,
// table-driven heuristic. Independent of the user-aircraft
// flight_phase detector: traffic targets see only their own
// alt_agl / groundspeed / vertical_speed plus the phase we
// classified them as on the previous tick.
//
// Phase 4 adds the Pattern/Final branches: for targets in the vicinity
// of the user's destination airport (≤ 8 NM, < 3000 ft AGL), geometric
// alignment with the active-runway heading + airport pattern direction
// promotes them to Final or Pattern. Climb/Cruise/Descend stay Unknown
// for now — they don't drive sequencing or advisories.
namespace traffic_phase_classifier {

// Optional hints about the user's destination airport + active runway,
// used by the Phase-4 Pattern/Final refinement. When absent, the
// classifier behaves exactly like the Phase-3 version (only OnGround /
// Taxi / Takeoff / Landed are produced).
struct AirportRunwayHints {
  // Airport reference point (used for the "near the destination" gate).
  double airport_lat = 0.0;
  double airport_lon = 0.0;

  // Active landing runway: threshold position + heading the aircraft
  // would fly down on touchdown.
  double threshold_lat = 0.0;
  double threshold_lon = 0.0;
  double runway_heading_deg = 0.0;

  // Published pattern direction for the active runway ("left" / "right").
  // Empty if the airport DB has no value — the side-discriminator falls
  // back to the global default in that case.
  std::string pattern_direction;
};

// Pure function. `prev_phase` is the phase we classified the same
// target as on the previous update tick, or Unknown if this is the
// first time we have seen the modeS_id. The Landed rule is the only
// branch that consults prev_phase — every other branch is a pure
// function of the target's current dynamics + (optionally) the
// airport hints.
traffic_context::TrafficPhase
classify(const traffic_context::TrafficTarget &target,
         traffic_context::TrafficPhase prev_phase,
         const std::optional<AirportRunwayHints> &hints = std::nullopt);

} // namespace traffic_phase_classifier

#endif // TRAFFIC_PHASE_CLASSIFIER_HPP
