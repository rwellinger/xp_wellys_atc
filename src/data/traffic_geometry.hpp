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

#ifndef TRAFFIC_GEOMETRY_HPP
#define TRAFFIC_GEOMETRY_HPP

#include <string>

namespace traffic_geometry {

// Initial bearing from (lat1,lon1) to (lat2,lon2) in degrees true,
// normalised to [0, 360). Pure haversine math, no SDK calls.
double bearing_deg(double lat1, double lon1, double lat2, double lon2);

// Great-circle distance in nautical miles (haversine, R = 6371 km).
double distance_nm(double lat1, double lon1, double lat2, double lon2);

// Clock position of a target as seen from the user. Result is in
// (0.0, 12.0]: dead-ahead returns 12.0, abeam right 3.0, six o'clock
// 6.0, abeam left 9.0. The result is rounded to the nearest hour clock
// position so callers can use it as a phraseology token directly.
double clock_position(double user_heading_deg, double target_bearing_deg);

// Classify the relative track of a target vs. the user, returning the
// EU phraseology phrase ("opposite direction", "same direction",
// "crossing left to right", "crossing right to left", "converging").
// Inputs are tracks in degrees true; clock_pos is the target's clock
// position as seen from the user (used to disambiguate left- vs.
// right-crossing).
//
// Angular ranges (signed diff, target_track - user_track normalised to
// [0, 360)):
//   - opposite direction: 150° <= diff <= 210°
//   - same direction:     diff <= 30° or diff >= 330°
//   - crossing L->R:      60° <= diff <= 120° AND clock_pos in (6, 12]
//   - crossing R->L:      240° <= diff <= 300° AND clock_pos in (0, 6)
//   - converging:         fallback when none of the above match
std::string classify_relative_track(double user_track_deg,
                                    double target_track_deg, double clock_pos);

// Render the EU "altitude info" phraseology fragment.
//   - has_mode_c true:                  "indicating <alt> feet"
//                                       (alt rounded to nearest 100)
//   - else if |diff| < 2000 ft:         "<n> feet above" / "<n> feet below"
//                                       (n rounded to nearest 100)
//   - else:                             "altitude unknown"
std::string format_altitude_info(double target_alt_msl_ft,
                                 double user_alt_msl_ft, bool has_mode_c);

// Phase-3 ground-conflict primitive. Heuristic: extend the target's
// path along (target_track_deg, target_groundspeed_kts) for
// `lookahead_secs` and return true if any sample point along that
// path falls inside the user's heading cone — a half-angle of
// `cone_half_deg` around `user_heading_deg`, capped at radial
// distance `cone_dist_m`. Pure geometry, no SDK calls. Sampled at
// 10 evenly-spaced points (including endpoints) for cheap O(1)
// behaviour at 2 Hz update rate.
bool path_intersects_cone(double user_lat, double user_lon,
                          double user_heading_deg, double cone_half_deg,
                          double cone_dist_m, double target_lat,
                          double target_lon, double target_track_deg,
                          double target_groundspeed_kts, double lookahead_secs);

// Project (target_lat, target_lon) onto the runway centerline anchored at
// (threshold_lat, threshold_lon) along `runway_heading_deg` (the landing
// direction). `along_m` is the distance from the threshold measured along
// the runway axis (positive = past the threshold toward the far end).
// `lateral_m` is the unsigned perpendicular distance from the centerline.
// Pure geometry, no SDK calls. Flat-earth — accurate within a few cm at
// the distances we use (≤ 2 km from the threshold).
void runway_axis_offsets(double target_lat, double target_lon,
                         double threshold_lat, double threshold_lon,
                         double runway_heading_deg, double &along_m,
                         double &lateral_m);

// True iff (target_lat, target_lon) is on the runway centerline, between
// the threshold and `length_m` past it, with lateral deviation at most
// `max_lateral_m`. Phase-4 runway-occupancy primitive: feeds the
// go-around trigger when a stopped / rolling target is sitting on the
// active runway as the user approaches short final.
bool is_on_runway_centerline(double target_lat, double target_lon,
                             double threshold_lat, double threshold_lon,
                             double runway_heading_deg, double length_m,
                             double max_lateral_m = 30.0);

} // namespace traffic_geometry

#endif // TRAFFIC_GEOMETRY_HPP
