/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "data/traffic_geometry.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace traffic_geometry {

namespace {
constexpr double kDeg2Rad = M_PI / 180.0;
constexpr double kEarthRadiusM = 6371000.0;
constexpr double kMetersPerNm = 1852.0;

// Round a double to the nearest 100 ft and return as int.
int round_to_100(double v) {
  double r = std::round(v / 100.0) * 100.0;
  return static_cast<int>(r);
}
} // namespace

double bearing_deg(double lat1, double lon1, double lat2, double lon2) {
  double lat1r = lat1 * kDeg2Rad;
  double lat2r = lat2 * kDeg2Rad;
  double dlon = (lon2 - lon1) * kDeg2Rad;
  double y = std::sin(dlon) * std::cos(lat2r);
  double x = std::cos(lat1r) * std::sin(lat2r) -
             std::sin(lat1r) * std::cos(lat2r) * std::cos(dlon);
  double bearing = std::atan2(y, x) / kDeg2Rad;
  return std::fmod(bearing + 360.0, 360.0);
}

double distance_nm(double lat1, double lon1, double lat2, double lon2) {
  double dlat = (lat2 - lat1) * kDeg2Rad;
  double dlon = (lon2 - lon1) * kDeg2Rad;
  double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
             std::cos(lat1 * kDeg2Rad) * std::cos(lat2 * kDeg2Rad) *
                 std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
  double dist_m =
      kEarthRadiusM * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
  return dist_m / kMetersPerNm;
}

double clock_position(double user_heading_deg, double target_bearing_deg) {
  double rel = std::fmod(target_bearing_deg - user_heading_deg + 720.0, 360.0);
  double hours = std::round(rel / 30.0);
  // 0 and 12 both refer to dead-ahead; collapse onto 12 so the value
  // stays in (0, 12] as documented.
  if (hours <= 0.0 || hours >= 12.0)
    return 12.0;
  return hours;
}

std::string classify_relative_track(double user_track_deg,
                                    double target_track_deg, double clock_pos) {
  // Track diff in [0, 360): 0 = same heading, 180 = opposite.
  double diff = std::fmod(target_track_deg - user_track_deg + 720.0, 360.0);

  // Opposite direction: diff in [150°, 210°].
  if (diff >= 150.0 && diff <= 210.0)
    return "opposite direction";

  // Same direction: diff <= 30° or diff >= 330°.
  if (diff <= 30.0 || diff >= 330.0)
    return "same direction";

  // Crossing left to right: diff in [60°, 120°] AND target on left side
  // (clock 9, 10, 11 — i.e. clock_pos in (6, 12)).
  if (diff >= 60.0 && diff <= 120.0 && clock_pos > 6.0 && clock_pos < 12.0)
    return "crossing left to right";

  // Crossing right to left: diff in [240°, 300°] AND target on right
  // side (clock 1, 2, 3 — i.e. clock_pos in (0, 6)).
  if (diff >= 240.0 && diff <= 300.0 && clock_pos > 0.0 && clock_pos < 6.0)
    return "crossing right to left";

  // Fallback: closure exists but the geometry doesn't fit a clean
  // category.
  return "converging";
}

namespace {

// Offset (target_lat, target_lon) by `distance_m` along
// `track_deg` and return the new position. Flat-earth
// approximation — fine for the < 500 m offsets we sample for
// the ground-conflict cone (errors stay sub-metre).
void offset_position(double lat, double lon, double track_deg,
                     double distance_m, double &out_lat, double &out_lon) {
  const double lat_rad = lat * kDeg2Rad;
  const double track_rad = track_deg * kDeg2Rad;
  const double dlat_m = distance_m * std::cos(track_rad);
  const double dlon_m = distance_m * std::sin(track_rad);
  out_lat = lat + (dlat_m / kEarthRadiusM) / kDeg2Rad;
  out_lon = lon + (dlon_m / (kEarthRadiusM * std::cos(lat_rad))) / kDeg2Rad;
}

// Signed angular difference in degrees, normalised to [-180, 180].
double angle_diff_deg(double a, double b) {
  double d = std::fmod(a - b + 540.0, 360.0) - 180.0;
  return d;
}

} // namespace

bool path_intersects_cone(double user_lat, double user_lon,
                          double user_heading_deg, double cone_half_deg,
                          double cone_dist_m, double target_lat,
                          double target_lon, double target_track_deg,
                          double target_groundspeed_kts,
                          double lookahead_secs) {
  // Total distance the target covers during lookahead. Knots ->
  // metres per second: 1 kt = 0.514444 m/s.
  const double total_m = target_groundspeed_kts * 0.514444 * lookahead_secs;

  constexpr int kSamples = 10;
  for (int i = 0; i <= kSamples; ++i) {
    const double frac = static_cast<double>(i) / kSamples;
    double sample_lat = target_lat;
    double sample_lon = target_lon;
    if (total_m > 0.0)
      offset_position(target_lat, target_lon, target_track_deg, total_m * frac,
                      sample_lat, sample_lon);

    const double dist_nm =
        distance_nm(user_lat, user_lon, sample_lat, sample_lon);
    const double dist_m = dist_nm * kMetersPerNm;
    if (dist_m > cone_dist_m)
      continue;

    const double bearing =
        bearing_deg(user_lat, user_lon, sample_lat, sample_lon);
    const double offset = std::fabs(angle_diff_deg(bearing, user_heading_deg));
    if (offset <= cone_half_deg)
      return true;
  }
  return false;
}

void runway_axis_offsets(double target_lat, double target_lon,
                         double threshold_lat, double threshold_lon,
                         double runway_heading_deg, double &along_m,
                         double &lateral_m) {
  const double dist_nm =
      distance_nm(threshold_lat, threshold_lon, target_lat, target_lon);
  const double dist_m = dist_nm * kMetersPerNm;
  if (dist_m <= 0.0) {
    along_m = 0.0;
    lateral_m = 0.0;
    return;
  }
  const double brg =
      bearing_deg(threshold_lat, threshold_lon, target_lat, target_lon);
  // Angle between runway heading (landing direction) and the bearing
  // from threshold to target. 0 = directly down the runway.
  const double delta_rad = (brg - runway_heading_deg) * kDeg2Rad;
  along_m = dist_m * std::cos(delta_rad);
  lateral_m = std::fabs(dist_m * std::sin(delta_rad));
}

bool is_on_runway_centerline(double target_lat, double target_lon,
                             double threshold_lat, double threshold_lon,
                             double runway_heading_deg, double length_m,
                             double max_lateral_m) {
  double along_m = 0.0;
  double lateral_m = 0.0;
  runway_axis_offsets(target_lat, target_lon, threshold_lat, threshold_lon,
                      runway_heading_deg, along_m, lateral_m);
  if (lateral_m > max_lateral_m)
    return false;
  if (along_m < 0.0 || along_m > length_m)
    return false;
  return true;
}

std::string format_altitude_info(double target_alt_msl_ft,
                                 double user_alt_msl_ft, bool has_mode_c) {
  if (has_mode_c) {
    int alt = round_to_100(target_alt_msl_ft);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "indicating %d feet", alt);
    return buf;
  }

  double diff = target_alt_msl_ft - user_alt_msl_ft;
  if (std::fabs(diff) < 2000.0) {
    int n = round_to_100(std::fabs(diff));
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d feet %s", n,
                  diff >= 0.0 ? "above" : "below");
    return buf;
  }

  return "altitude unknown";
}

} // namespace traffic_geometry
