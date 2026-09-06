/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "data/traffic_phase_classifier.hpp"

#include "data/traffic_geometry.hpp"

#include <cmath>

namespace traffic_phase_classifier {

using traffic_context::TrafficPhase;

namespace {

// "Was the target airborne the last time we classified it?" — the only
// trigger for the Landed branch. Pattern and Final are airborne phases
// too, so they participate.
bool was_airborne(TrafficPhase prev) {
  switch (prev) {
  case TrafficPhase::Climb:
  case TrafficPhase::Cruise:
  case TrafficPhase::Descend:
  case TrafficPhase::Final:
  case TrafficPhase::Pattern:
    return true;
  default:
    return false;
  }
}

// Phase-4 thresholds — heuristic, intentionally loose to absorb GPS
// noise and the variability of pilot pattern flying.
constexpr double kAirportVicinityNm = 8.0;
constexpr double kPatternRadiusNm = 5.0;
constexpr double kFinalRadiusNm = 5.0;
constexpr double kAirportVicinityAglFt = 3000.0;
constexpr double kFinalAlignmentDeg = 10.0;
constexpr double kFinalDescentFpm = -200.0;
constexpr double kLegSlopDeg = 30.0; // tolerance around 90°/180° leg angles

// Signed angular difference normalised to [-180, 180].
double signed_diff(double a, double b) {
  double d = std::fmod(a - b + 540.0, 360.0) - 180.0;
  return d;
}

// True if target heading is within `tolerance` of being parallel to
// `ref_heading` (regardless of sign of along/anti).
bool near_heading(double track, double ref, double tolerance) {
  return std::fabs(signed_diff(track, ref)) <= tolerance;
}

// True if target heading is within `tolerance` of being anti-parallel
// to `ref_heading` (i.e. ~180° opposed).
bool near_opposite(double track, double ref, double tolerance) {
  double diff = std::fabs(signed_diff(track, ref));
  return std::fabs(diff - 180.0) <= tolerance;
}

// True if target heading is within `tolerance` of being perpendicular
// to `ref_heading` (i.e. ~±90°).
bool near_perpendicular(double track, double ref, double tolerance) {
  double diff = std::fabs(signed_diff(track, ref));
  return std::fabs(diff - 90.0) <= tolerance;
}

TrafficPhase
classify_airborne_near_airport(const traffic_context::TrafficTarget &t,
                               const AirportRunwayHints &hints) {
  // Distance target → threshold, and along/lateral offsets relative to
  // the active-runway centerline (positive `along` = past the threshold
  // in the landing direction, so for an approaching target it is
  // negative).
  double along_m = 0.0;
  double lateral_m = 0.0;
  traffic_geometry::runway_axis_offsets(
      t.lat, t.lon, hints.threshold_lat, hints.threshold_lon,
      hints.runway_heading_deg, along_m, lateral_m);
  const double dist_thresh_nm = traffic_geometry::distance_nm(
      hints.threshold_lat, hints.threshold_lon, t.lat, t.lon);

  // Final: aligned with the landing heading, descending, on the
  // approach side of the threshold (i.e. before touchdown), within
  // 5 NM. The track tolerance is ±10°; descent is ≥ 200 fpm down.
  if (near_heading(t.track_deg, hints.runway_heading_deg, kFinalAlignmentDeg) &&
      t.vertical_speed_fpm < kFinalDescentFpm &&
      dist_thresh_nm <= kFinalRadiusNm && along_m <= 0.0) {
    return TrafficPhase::Final;
  }

  // Pattern legs are gated on a wider airport-radius (5 NM from the
  // airport reference point). Inside that radius, the leg classifier
  // looks at heading alignment vs the active runway:
  //   downwind  — opposite (~180°) to runway heading
  //   base      — perpendicular (~90°) AND on the approach side
  //   crosswind — perpendicular AND on the departure side
  const double dist_airport_nm = traffic_geometry::distance_nm(
      hints.airport_lat, hints.airport_lon, t.lat, t.lon);
  if (dist_airport_nm > kPatternRadiusNm)
    return TrafficPhase::Unknown;

  if (near_opposite(t.track_deg, hints.runway_heading_deg, kLegSlopDeg))
    return TrafficPhase::Pattern; // downwind
  if (near_perpendicular(t.track_deg, hints.runway_heading_deg, kLegSlopDeg)) {
    // Either base (along < 0, descending toward runway) or crosswind
    // (along > 0, past the threshold). Both qualify as Pattern at this
    // resolution — the leg label is recomputed at sequencing time.
    return TrafficPhase::Pattern;
  }

  return TrafficPhase::Unknown;
}

} // namespace

TrafficPhase classify(const traffic_context::TrafficTarget &target,
                      TrafficPhase prev_phase,
                      const std::optional<AirportRunwayHints> &hints) {
  const double agl = target.alt_agl_ft;
  const double gs = target.groundspeed_kts;
  const double vs = target.vertical_speed_fpm;

  // Landed is order-sensitive: a target that was airborne on the prior
  // tick and is now on the ground stays "Landed" rather than collapsing
  // to OnGround/Taxi, so downstream advisories can treat it as a fresh
  // arrival.
  if (was_airborne(prev_phase) && agl < 50.0 && gs < 80.0)
    return TrafficPhase::Landed;

  if (agl < 50.0 && gs < 5.0)
    return TrafficPhase::OnGround;

  if (agl < 50.0 && gs >= 5.0 && gs < 40.0)
    return TrafficPhase::Taxi;

  if (agl < 200.0 && gs >= 40.0 && vs > 200.0)
    return TrafficPhase::Takeoff;

  // Phase-4 refinement. Only when the caller supplied airport hints
  // AND the target is in the airport-vicinity envelope (≤ 8 NM, below
  // 3000 ft AGL). Outside that envelope we still return Unknown for
  // airborne targets — Climb/Cruise/Descend remain out of scope.
  if (hints.has_value() && agl < kAirportVicinityAglFt) {
    const double airport_dist_nm = traffic_geometry::distance_nm(
        hints->airport_lat, hints->airport_lon, target.lat, target.lon);
    if (airport_dist_nm <= kAirportVicinityNm) {
      TrafficPhase refined = classify_airborne_near_airport(target, *hints);
      if (refined != TrafficPhase::Unknown)
        return refined;
    }
  }

  return TrafficPhase::Unknown;
}

} // namespace traffic_phase_classifier
