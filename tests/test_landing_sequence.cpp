/*
 * xp_wellys_vfr_atc - Phase-4 landing-sequence unit tests.
 */

#include "atc/landing_sequence.hpp"
#include "data/traffic_context.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <cmath>

using landing_sequence::ActiveRunway;
using landing_sequence::compute_landing_sequence;
using landing_sequence::leg_label;
using landing_sequence::SequenceResult;
using landing_sequence::UserPosition;
using traffic_context::TrafficContext;
using traffic_context::TrafficPhase;
using traffic_context::TrafficTarget;

namespace {

// LSZH RWY 14 reference geometry. Same constants as the classifier
// tests for cross-test consistency.
constexpr double kThrLat = 47.4794;
constexpr double kThrLon = 8.5253;
constexpr double kRwyHeading = 137.0;
constexpr double kRwyLengthM = 3700.0;

ActiveRunway rwy14() {
  ActiveRunway r;
  r.threshold_lat = kThrLat;
  r.threshold_lon = kThrLon;
  r.heading_deg = kRwyHeading;
  r.length_m = kRwyLengthM;
  r.designator = "14";
  return r;
}

// Offset (base_lat, base_lon) by `dist_m` along `track_deg`.
void offset_meters(double base_lat, double base_lon, double track_deg,
                   double dist_m, double &out_lat, double &out_lon) {
  constexpr double kDeg2Rad = M_PI / 180.0;
  constexpr double kEarthRadiusM = 6371000.0;
  const double dlat_m = dist_m * std::cos(track_deg * kDeg2Rad);
  const double dlon_m = dist_m * std::sin(track_deg * kDeg2Rad);
  out_lat = base_lat + (dlat_m / kEarthRadiusM) * (180.0 / M_PI);
  out_lon = base_lon + (dlon_m / (kEarthRadiusM *
                                  std::cos(base_lat * kDeg2Rad))) *
                          (180.0 / M_PI);
}

// Build a target on the approach side of the threshold (reciprocal of
// the runway heading), `dist_nm` short of the threshold.
TrafficTarget final_target(uint32_t id, double dist_nm,
                           const char *callsign = "TGT",
                           const char *icao_type = "C172") {
  constexpr double kNmToM = 1852.0;
  TrafficTarget t;
  t.modeS_id = id;
  t.callsign = callsign;
  t.icao_type = icao_type;
  const double reciprocal = std::fmod(kRwyHeading + 180.0, 360.0);
  offset_meters(kThrLat, kThrLon, reciprocal, dist_nm * kNmToM, t.lat, t.lon);
  t.alt_agl_ft = 1000.0;
  t.alt_msl_ft = 2500.0;
  t.groundspeed_kts = 80.0;
  t.vertical_speed_fpm = -600.0;
  t.track_deg = kRwyHeading;
  t.phase = TrafficPhase::Final;
  return t;
}

// Place the user `dist_nm` short of the threshold along the centerline.
UserPosition user_short_of_threshold(double dist_nm) {
  constexpr double kNmToM = 1852.0;
  UserPosition u;
  const double reciprocal = std::fmod(kRwyHeading + 180.0, 360.0);
  offset_meters(kThrLat, kThrLon, reciprocal, dist_nm * kNmToM, u.lat, u.lon);
  return u;
}

// Build a ground-phase target sitting on the centerline `dist_m` past
// the threshold (positive along the runway axis).
TrafficTarget runway_occupant(uint32_t id, double along_m,
                              TrafficPhase phase = TrafficPhase::Landed) {
  TrafficTarget t;
  t.modeS_id = id;
  t.callsign = "OCC";
  t.icao_type = "C172";
  offset_meters(kThrLat, kThrLon, kRwyHeading, along_m, t.lat, t.lon);
  t.alt_agl_ft = 0.0;
  t.alt_msl_ft = 0.0;
  t.groundspeed_kts = 20.0;
  t.vertical_speed_fpm = 0.0;
  t.track_deg = kRwyHeading;
  t.phase = phase;
  return t;
}

} // namespace

// ── Sequencing position ────────────────────────────────────────────────────

TEST_CASE("landing_sequence: user first with no traffic — user_position == 1",
          "[landing_sequence]") {
  TrafficContext traffic;
  UserPosition user = user_short_of_threshold(1.0);
  SequenceResult seq = compute_landing_sequence(traffic, user, rwy14());
  REQUIRE(seq.user_position == 1);
  REQUIRE_FALSE(seq.follow_target.has_value());
  REQUIRE_FALSE(seq.runway_occupied);
}

TEST_CASE("landing_sequence: 3 finals + user on final — user is number 4",
          "[landing_sequence]") {
  TrafficContext traffic;
  // Three Final targets ahead of the user (closer to threshold).
  traffic.targets.push_back(final_target(0xA1, 0.5, "ABC", "C172"));
  traffic.targets.push_back(final_target(0xA2, 1.5, "DEF", "PA28"));
  traffic.targets.push_back(final_target(0xA3, 2.5, "GHI", "C152"));

  UserPosition user = user_short_of_threshold(4.0);
  SequenceResult seq = compute_landing_sequence(traffic, user, rwy14());
  REQUIRE(seq.user_position == 4);
  REQUIRE(seq.follow_target.has_value());
  // Aircraft directly ahead is the one at 2.5 NM (modeS A3).
  REQUIRE(seq.follow_target->modeS_id == 0xA3u);
  REQUIRE_FALSE(seq.runway_occupied);
}

TEST_CASE("landing_sequence: user between two finals — user is number 2",
          "[landing_sequence]") {
  TrafficContext traffic;
  traffic.targets.push_back(final_target(0xB1, 0.5));  // ahead of user
  traffic.targets.push_back(final_target(0xB2, 3.0));  // behind user

  UserPosition user = user_short_of_threshold(1.5);
  SequenceResult seq = compute_landing_sequence(traffic, user, rwy14());
  REQUIRE(seq.user_position == 2);
  REQUIRE(seq.follow_target.has_value());
  REQUIRE(seq.follow_target->modeS_id == 0xB1u);
}

// ── Runway occupancy ───────────────────────────────────────────────────────

TEST_CASE("landing_sequence: ground target on runway -> runway_occupied",
          "[landing_sequence]") {
  TrafficContext traffic;
  // Target sitting 600 m past the threshold, still rolling out (Landed).
  traffic.targets.push_back(runway_occupant(0xC1, 600.0));

  UserPosition user = user_short_of_threshold(0.6);
  SequenceResult seq = compute_landing_sequence(traffic, user, rwy14());
  REQUIRE(seq.runway_occupied);
  REQUIRE(seq.occupant.has_value());
  REQUIRE(seq.occupant->modeS_id == 0xC1u);
}

TEST_CASE("landing_sequence: airborne Final target does not occupy runway",
          "[landing_sequence]") {
  TrafficContext traffic;
  // Single Final target 0.5 NM short of threshold — airborne, on the
  // approach side. Must not register as runway occupant.
  traffic.targets.push_back(final_target(0xD1, 0.5));

  UserPosition user = user_short_of_threshold(2.0);
  SequenceResult seq = compute_landing_sequence(traffic, user, rwy14());
  REQUIRE_FALSE(seq.runway_occupied);
  REQUIRE(seq.user_position == 2);
}

TEST_CASE("landing_sequence: occupant past runway-length is rejected",
          "[landing_sequence]") {
  TrafficContext traffic;
  // 4 km past threshold — beyond a 3.7 km runway.
  traffic.targets.push_back(runway_occupant(0xE1, 4000.0));

  UserPosition user = user_short_of_threshold(0.5);
  SequenceResult seq = compute_landing_sequence(traffic, user, rwy14());
  REQUIRE_FALSE(seq.runway_occupied);
}

// ── Leg label ─────────────────────────────────────────────────────────────

TEST_CASE("leg_label: aligned target → final", "[landing_sequence][leg]") {
  // Build a Final-aligned target 2 NM short of threshold.
  TrafficTarget t = final_target(0xF1, 2.0);
  REQUIRE(leg_label(t, rwy14(), "left") == "final");
}

TEST_CASE("leg_label: opposite-heading target → downwind",
          "[landing_sequence][leg]") {
  // Target abeam threshold on the left, heading opposite runway.
  TrafficTarget t;
  constexpr double kNmToM = 1852.0;
  // 137° - 90° = 47° → left side of runway.
  offset_meters(kThrLat, kThrLon, 47.0, 1.5 * kNmToM, t.lat, t.lon);
  t.track_deg = std::fmod(kRwyHeading + 180.0, 360.0);
  t.alt_agl_ft = 1500.0;
  t.groundspeed_kts = 90.0;
  t.icao_type = "C172";
  // pattern_direction = "left"; the actual geometric side is left.
  REQUIRE(leg_label(t, rwy14(), "left") == "left downwind");
}

TEST_CASE("leg_label: perpendicular short-of-threshold → base",
          "[landing_sequence][leg]") {
  TrafficTarget t;
  constexpr double kNmToM = 1852.0;
  // Position 2 NM short of threshold (reciprocal = 317°).
  const double reciprocal = std::fmod(kRwyHeading + 180.0, 360.0);
  offset_meters(kThrLat, kThrLon, reciprocal, 2.0 * kNmToM, t.lat, t.lon);
  // Heading perpendicular: 137° - 90° = 47° (turning base from left).
  t.track_deg = 47.0;
  t.alt_agl_ft = 1200.0;
  t.icao_type = "PA28";
  std::string label = leg_label(t, rwy14(), "left");
  // Approach side, perpendicular -> base; side depends on actual
  // geometric position (which is on-axis, cross ~ 0). Accept either
  // side label — the test pins down the "base" leg, not the side.
  REQUIRE((label == "left base" || label == "right base"));
}
