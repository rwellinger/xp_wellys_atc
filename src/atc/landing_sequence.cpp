/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "atc/landing_sequence.hpp"

#include "data/traffic_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace landing_sequence {

using traffic_context::TrafficPhase;
using traffic_context::TrafficTarget;

namespace {

double distance_to_threshold_nm(double lat, double lon,
                                const ActiveRunway &rwy) {
  return traffic_geometry::distance_nm(rwy.threshold_lat, rwy.threshold_lon,
                                       lat, lon);
}

bool is_ground_phase(TrafficPhase p) {
  return p == TrafficPhase::OnGround || p == TrafficPhase::Takeoff ||
         p == TrafficPhase::Landed;
}

// Signed angular difference normalised to [-180, 180].
double signed_diff(double a, double b) {
  return std::fmod(a - b + 540.0, 360.0) - 180.0;
}

} // namespace

SequenceResult
compute_landing_sequence(const traffic_context::TrafficContext &traffic,
                         const UserPosition &user, const ActiveRunway &rwy) {
  SequenceResult out;

  // 1. Targets on Final, sorted ascending by distance to threshold.
  // Indexed by source-vector position so the sort key is a deterministic
  // (distance, index) pair instead of a pointer — keeps clang-tidy's
  // nondeterministic-pointer-iteration-order check happy.
  struct FinalEntry {
    double dist_nm;
    std::size_t source_idx;
  };
  std::vector<FinalEntry> finals;
  finals.reserve(traffic.targets.size());
  for (std::size_t i = 0; i < traffic.targets.size(); ++i) {
    const auto &t = traffic.targets[i];
    if (t.phase != TrafficPhase::Final)
      continue;
    finals.push_back({distance_to_threshold_nm(t.lat, t.lon, rwy), i});
  }
  std::sort(finals.begin(), finals.end(),
            [](const FinalEntry &a, const FinalEntry &b) {
              if (a.dist_nm != b.dist_nm)
                return a.dist_nm < b.dist_nm;
              return a.source_idx < b.source_idx;
            });

  // 2. Insert user by distance to threshold.
  const double user_dist_nm = distance_to_threshold_nm(user.lat, user.lon, rwy);
  std::size_t user_idx = 0;
  for (; user_idx < finals.size(); ++user_idx) {
    if (finals[user_idx].dist_nm > user_dist_nm)
      break;
  }
  out.user_position = static_cast<int>(user_idx + 1);

  // 3. follow_target = target directly ahead.
  if (user_idx > 0)
    out.follow_target = traffic.targets[finals[user_idx - 1].source_idx];

  // 4. Runway-occupancy scan: any ground-phase target on the active
  //    runway's centerline within reach of the threshold.
  for (const auto &t : traffic.targets) {
    if (!is_ground_phase(t.phase))
      continue;
    if (!traffic_geometry::is_on_runway_centerline(
            t.lat, t.lon, rwy.threshold_lat, rwy.threshold_lon, rwy.heading_deg,
            kRunwayOccupiedReachM, kRunwayOccupiedLateralM))
      continue;
    out.runway_occupied = true;
    out.occupant = t;
    break;
  }

  return out;
}

std::string leg_label(const TrafficTarget &follow, const ActiveRunway &rwy,
                      const std::string &pattern_direction) {
  // Project follow-target onto the runway axis. `along` < 0 means
  // approach side of the threshold; > 0 means past it (departure
  // side). Lateral side is determined by the signed cross-track
  // angle: positive sin(brg-heading) → right of the runway,
  // negative → left.
  const double dist_nm = traffic_geometry::distance_nm(
      rwy.threshold_lat, rwy.threshold_lon, follow.lat, follow.lon);
  const double dist_m = dist_nm * 1852.0;
  const double brg = traffic_geometry::bearing_deg(
      rwy.threshold_lat, rwy.threshold_lon, follow.lat, follow.lon);
  const double rel_rad = (brg - rwy.heading_deg) * M_PI / 180.0;
  const double along_m = dist_m * std::cos(rel_rad);
  const double cross_m = dist_m * std::sin(rel_rad); // signed
  const bool target_right_of_runway = cross_m > 0.0;

  // Pattern direction defaults to "left" if the caller didn't supply
  // one — matches the EU default. The pattern-direction label always
  // describes which side of the runway downwind/base lives on; it
  // doesn't change with the side the follow-target actually flies.
  const std::string side =
      pattern_direction.empty() ? std::string{"left"} : pattern_direction;

  // Leg classification, same heuristics as the classifier:
  //   - heading aligned (~0°) with runway -> final
  //   - heading opposite (~180°) -> downwind
  //   - heading perpendicular (~90°) AND along < 0 -> base
  //   - heading perpendicular AND along > 0 -> crosswind
  const double diff = std::fabs(signed_diff(follow.track_deg, rwy.heading_deg));

  // Final: ±20° tolerance (wider than the classifier's 10° because we
  // want to label legs even for targets the classifier put in Pattern
  // but that look very near-aligned).
  if (diff <= 20.0)
    return "final";

  // Downwind: ~180°. Use the *actual* side the target flies, not the
  // declared pattern direction — gives the pilot a true geometric cue
  // ("left downwind" vs. "right downwind") even at airports where the
  // database disagrees with what the LiveTraffic AI is doing.
  if (std::fabs(diff - 180.0) <= 30.0) {
    return std::string{target_right_of_runway ? "right" : "left"} + " downwind";
  }

  // Perpendicular -> base or crosswind.
  if (std::fabs(diff - 90.0) <= 40.0) {
    const std::string lr = target_right_of_runway ? "right" : "left";
    if (along_m <= 0.0)
      return lr + " base";
    return lr + " crosswind";
  }

  // Fallback — the database side label is the safe default.
  return side + " base";
}

} // namespace landing_sequence
