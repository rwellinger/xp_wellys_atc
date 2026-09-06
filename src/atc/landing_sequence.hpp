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

#ifndef ATC_LANDING_SEQUENCE_HPP
#define ATC_LANDING_SEQUENCE_HPP

// Phase-4 landing-sequencing primitives. SDK-free pure functions —
// inputs are a TrafficContext snapshot, the user's lat/lon, and the
// active-runway geometry. Outputs are the sequence position + the
// aircraft directly ahead (if any) + runway-occupancy info for the
// go-around trigger.

#include "data/traffic_context.hpp"

#include <optional>
#include <string>

namespace landing_sequence {

// Active-runway descriptor for sequencing.
struct ActiveRunway {
  double threshold_lat = 0.0;
  double threshold_lon = 0.0;
  // Direction the aircraft flies down the runway on touchdown
  // (i.e. the landing heading, not the reciprocal).
  double heading_deg = 0.0;
  // Physical runway length in metres. Used as the upper bound of the
  // runway-centerline projection.
  double length_m = 2500.0;
  // The runway designator string used in spoken phraseology
  // ("28", "14L"). Sequencing itself doesn't read this; included so
  // callers can pass it through into template variables without
  // having to track it separately.
  std::string designator;
};

// User position for sequencing. We need lat/lon to project onto the
// final-approach axis and to compute distance-to-threshold.
struct UserPosition {
  double lat = 0.0;
  double lon = 0.0;
};

// Result of compute_landing_sequence().
//
// `user_position` is 1-based. The user is "first to land" when no
// Final-phase traffic is ahead — user_position then equals 1 and
// follow_target is nullopt. When sequencing applies (user_position
// >= 2), follow_target carries the aircraft directly ahead of the
// user.
//
// `runway_occupied` is true if at least one target on the ground
// (OnGround / Takeoff / Landed) projects onto the active-runway
// centerline within `kRunwayOccupiedReachM` of the threshold. The
// occupier itself is exposed in `occupant` so callers can populate
// the "traffic on the runway, {type}" placeholder if needed.
struct SequenceResult {
  int user_position = 1;
  std::optional<traffic_context::TrafficTarget> follow_target;
  bool runway_occupied = false;
  std::optional<traffic_context::TrafficTarget> occupant;
};

// Pure function. Build the landing-sequence snapshot from the current
// traffic snapshot, the user's position, and the active runway.
//
// Algorithm:
//   1. Collect targets with phase == Final, sort by distance-to-threshold.
//   2. Insert the user into the same ordering.
//   3. user_position = index in the merged list + 1.
//   4. follow_target = target directly ahead, if any.
//   5. Runway occupancy: any target with phase in {OnGround, Takeoff,
//      Landed} whose lat/lon projects onto the active-runway centerline
//      within ±kRunwayOccupiedLateralM and 0..kRunwayOccupiedReachM of
//      the threshold.
SequenceResult
compute_landing_sequence(const traffic_context::TrafficContext &traffic,
                         const UserPosition &user, const ActiveRunway &rwy);

// Phraseology helper. Compute the leg label for a follow-target as
// seen from the user's active runway: "left base" / "right base" /
// "left downwind" / "right downwind" / "final" / "left crosswind" /
// "right crosswind". `pattern_direction` ("left"/"right") drives the
// side label; an empty string falls back to "left" (the EU pattern
// default).
std::string leg_label(const traffic_context::TrafficTarget &follow,
                      const ActiveRunway &rwy,
                      const std::string &pattern_direction);

// Constants exposed for testing / documentation.
constexpr double kRunwayOccupiedLateralM = 30.0;
constexpr double kRunwayOccupiedReachM = 1500.0;

} // namespace landing_sequence

#endif // ATC_LANDING_SEQUENCE_HPP
