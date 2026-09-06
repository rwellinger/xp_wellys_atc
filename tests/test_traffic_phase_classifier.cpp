#include "data/traffic_context.hpp"
#include "data/traffic_phase_classifier.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <cmath>

using traffic_context::TrafficPhase;
using traffic_context::TrafficTarget;
using traffic_phase_classifier::AirportRunwayHints;
using traffic_phase_classifier::classify;

namespace {

// Minimal builder — the classifier only reads alt_agl / groundspeed /
// vertical_speed, so everything else stays at its struct default.
TrafficTarget make(double agl, double gs, double vs) {
  TrafficTarget t;
  t.alt_agl_ft = agl;
  t.groundspeed_kts = gs;
  t.vertical_speed_fpm = vs;
  return t;
}

} // namespace

TEST_CASE("classifier: OnGround at idle thresholds", "[traffic][classifier]") {
  REQUIRE(classify(make(0.0, 0.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::OnGround);
  REQUIRE(classify(make(20.0, 4.9, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::OnGround);
  // 5 kts is the OnGround/Taxi boundary, classified as Taxi.
  REQUIRE(classify(make(20.0, 5.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Taxi);
}

TEST_CASE("classifier: Taxi between 5 and 40 kts", "[traffic][classifier]") {
  REQUIRE(classify(make(20.0, 5.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Taxi);
  REQUIRE(classify(make(20.0, 25.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Taxi);
  REQUIRE(classify(make(20.0, 39.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Taxi);
  // 40 kts on ground without climb -> Unknown (not Takeoff: no vs > 200)
  REQUIRE(classify(make(20.0, 40.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
}

TEST_CASE("classifier: Takeoff requires speed + climb",
          "[traffic][classifier]") {
  // 50 kts + climbing at 500 fpm under 200 ft AGL -> Takeoff.
  REQUIRE(classify(make(100.0, 50.0, 500.0), TrafficPhase::Unknown) ==
          TrafficPhase::Takeoff);
  REQUIRE(classify(make(180.0, 80.0, 300.0), TrafficPhase::Unknown) ==
          TrafficPhase::Takeoff);
  // Same speed but level -> Unknown (vs not > 200)
  REQUIRE(classify(make(100.0, 50.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
  // Same speed but already above 200 ft AGL -> Unknown (Phase 4 will refine)
  REQUIRE(classify(make(200.0, 50.0, 500.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
}

TEST_CASE("classifier: Landed needs prior airborne phase",
          "[traffic][classifier]") {
  // Came from Final, now on the rollout under 80 kts -> Landed.
  REQUIRE(classify(make(20.0, 60.0, 0.0), TrafficPhase::Final) ==
          TrafficPhase::Landed);
  REQUIRE(classify(make(20.0, 60.0, 0.0), TrafficPhase::Pattern) ==
          TrafficPhase::Landed);
  REQUIRE(classify(make(20.0, 60.0, 0.0), TrafficPhase::Cruise) ==
          TrafficPhase::Landed);
  REQUIRE(classify(make(20.0, 60.0, 0.0), TrafficPhase::Descend) ==
          TrafficPhase::Landed);
  REQUIRE(classify(make(20.0, 60.0, 0.0), TrafficPhase::Climb) ==
          TrafficPhase::Landed);
  // Came from OnGround (just sitting), now Taxi-speed -> Taxi, not Landed.
  REQUIRE(classify(make(20.0, 10.0, 0.0), TrafficPhase::OnGround) ==
          TrafficPhase::Taxi);
  // Came from Taxi at slow speed -> still Taxi.
  REQUIRE(classify(make(20.0, 10.0, 0.0), TrafficPhase::Taxi) ==
          TrafficPhase::Taxi);
}

TEST_CASE("classifier: Landed bounded by 80 kts ceiling",
          "[traffic][classifier]") {
  REQUIRE(classify(make(20.0, 79.0, 0.0), TrafficPhase::Final) ==
          TrafficPhase::Landed);
  // 80 kts on the rollout would still be a roll, but the rule's
  // strict-less-than makes 80 fall through to Unknown until Phase 4
  // refines the airborne-state classifiers.
  REQUIRE(classify(make(20.0, 80.0, 0.0), TrafficPhase::Final) ==
          TrafficPhase::Unknown);
}

TEST_CASE("classifier: AGL ceiling at 50 ft for ground phases",
          "[traffic][classifier]") {
  // 49 AGL + 10 kts -> Taxi
  REQUIRE(classify(make(49.0, 10.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Taxi);
  // 50 AGL + 10 kts -> Unknown (no longer a ground phase)
  REQUIRE(classify(make(50.0, 10.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
}

TEST_CASE("classifier: airborne-pending phases stay Unknown for now",
          "[traffic][classifier]") {
  // Cruise altitude, level, fast — Phase 4 refines this to Cruise.
  // For Phase 3 the classifier returns Unknown.
  REQUIRE(classify(make(5000.0, 120.0, 0.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
  // Descending below pattern but still above 200 ft -> Unknown.
  REQUIRE(classify(make(800.0, 90.0, -500.0), TrafficPhase::Unknown) ==
          TrafficPhase::Unknown);
}

// ── Phase-4: Pattern / Final refinement ───────────────────────────────────

namespace {

// LSZH RWY 14 reference: threshold ≈ (47.4794, 8.5253), heading 137° true.
constexpr double kLszhAirportLat = 47.4583;
constexpr double kLszhAirportLon = 8.5483;
constexpr double kLszh14ThrLat = 47.4794;
constexpr double kLszh14ThrLon = 8.5253;
constexpr double kLszh14Heading = 137.0;

AirportRunwayHints lszh14_hints() {
  AirportRunwayHints h;
  h.airport_lat = kLszhAirportLat;
  h.airport_lon = kLszhAirportLon;
  h.threshold_lat = kLszh14ThrLat;
  h.threshold_lon = kLszh14ThrLon;
  h.runway_heading_deg = kLszh14Heading;
  h.pattern_direction = "left";
  return h;
}

// Offset (base_lat, base_lon) by `dist_m` along `track_deg`. Flat-earth
// — accurate within a few cm at the distances we use.
void offset_meters(double base_lat, double base_lon, double track_deg,
                   double dist_m, double &out_lat, double &out_lon) {
  constexpr double kDeg2Rad = M_PI / 180.0;
  constexpr double kEarthRadiusM = 6371000.0;
  const double dlat_m = dist_m * std::cos(track_deg * kDeg2Rad);
  const double dlon_m = dist_m * std::sin(track_deg * kDeg2Rad);
  out_lat = base_lat + (dlat_m / kEarthRadiusM) * (180.0 / M_PI);
  out_lon = base_lon +
            (dlon_m / (kEarthRadiusM *
                       std::cos(base_lat * kDeg2Rad))) *
                (180.0 / M_PI);
}

// Pattern/Final builder: positions the target relative to the LSZH
// RWY 14 threshold/airport reference, then fills the dynamics the
// classifier reads.
TrafficTarget make_airborne(double lat, double lon, double agl, double gs,
                            double vs, double track) {
  TrafficTarget t;
  t.lat = lat;
  t.lon = lon;
  t.alt_agl_ft = agl;
  t.groundspeed_kts = gs;
  t.vertical_speed_fpm = vs;
  t.track_deg = track;
  return t;
}

} // namespace

TEST_CASE("classifier: Final — aligned, descending, short of threshold",
          "[traffic][classifier][phase4]") {
  // 3 NM short of the RWY 14 threshold on the extended centerline.
  // Reciprocal of 137° is 317° — moving 3 NM on 317° from the threshold
  // puts the target right where a final-approach target would sit.
  constexpr double kNmToM = 1852.0;
  double lat = 0.0;
  double lon = 0.0;
  offset_meters(kLszh14ThrLat, kLszh14ThrLon, 317.0, 3.0 * kNmToM, lat, lon);
  // Target heading down the runway (137°), 800 fpm down.
  auto t = make_airborne(lat, lon, 1500.0, 90.0, -800.0, kLszh14Heading);
  REQUIRE(classify(t, TrafficPhase::Unknown, lszh14_hints()) ==
          TrafficPhase::Final);
}

TEST_CASE("classifier: Final requires descent — level flight stays Unknown",
          "[traffic][classifier][phase4]") {
  constexpr double kNmToM = 1852.0;
  double lat = 0.0;
  double lon = 0.0;
  offset_meters(kLszh14ThrLat, kLszh14ThrLon, 317.0, 3.0 * kNmToM, lat, lon);
  // Aligned but level. Classifier picks Pattern (downwind/base/cross
  // don't match either since track is aligned, not perpendicular/
  // opposite) → falls through to Unknown.
  auto t = make_airborne(lat, lon, 1500.0, 90.0, 0.0, kLszh14Heading);
  REQUIRE(classify(t, TrafficPhase::Unknown, lszh14_hints()) ==
          TrafficPhase::Unknown);
}

TEST_CASE("classifier: Pattern — downwind (opposite heading)",
          "[traffic][classifier][phase4]") {
  // Target abeam the threshold, ~2 NM out, on the left side, heading
  // ~opposite of runway (137°+180° = 317°).
  constexpr double kNmToM = 1852.0;
  // 90° to the left of the runway heading from the threshold:
  // 137° - 90° = 47° (north-east of threshold). Walk 2 NM that way.
  double lat = 0.0;
  double lon = 0.0;
  offset_meters(kLszh14ThrLat, kLszh14ThrLon, 47.0, 2.0 * kNmToM, lat, lon);
  auto t = make_airborne(lat, lon, 1500.0, 90.0, 0.0, 317.0);
  REQUIRE(classify(t, TrafficPhase::Unknown, lszh14_hints()) ==
          TrafficPhase::Pattern);
}

TEST_CASE("classifier: Pattern — base (perpendicular heading)",
          "[traffic][classifier][phase4]") {
  // Target 2 NM short of the threshold, turning base — heading
  // perpendicular to runway. We pick 137°-90° = 47° (left base).
  constexpr double kNmToM = 1852.0;
  double lat = 0.0;
  double lon = 0.0;
  offset_meters(kLszh14ThrLat, kLszh14ThrLon, 317.0, 2.0 * kNmToM, lat, lon);
  auto t = make_airborne(lat, lon, 1200.0, 85.0, -300.0, 47.0);
  REQUIRE(classify(t, TrafficPhase::Unknown, lszh14_hints()) ==
          TrafficPhase::Pattern);
}

TEST_CASE("classifier: Pattern/Final gated by airport vicinity",
          "[traffic][classifier][phase4]") {
  // Target aligned + descending but 12 NM away from the threshold —
  // outside the 8 NM airport-vicinity envelope. Stays Unknown.
  constexpr double kNmToM = 1852.0;
  double lat = 0.0;
  double lon = 0.0;
  offset_meters(kLszh14ThrLat, kLszh14ThrLon, 317.0, 12.0 * kNmToM, lat, lon);
  auto t = make_airborne(lat, lon, 2500.0, 100.0, -500.0, kLszh14Heading);
  REQUIRE(classify(t, TrafficPhase::Unknown, lszh14_hints()) ==
          TrafficPhase::Unknown);
}

TEST_CASE("classifier: Pattern/Final never returned without hints",
          "[traffic][classifier][phase4]") {
  // Same geometry that produces Final with hints, but no hints passed.
  constexpr double kNmToM = 1852.0;
  double lat = 0.0;
  double lon = 0.0;
  offset_meters(kLszh14ThrLat, kLszh14ThrLon, 317.0, 3.0 * kNmToM, lat, lon);
  auto t = make_airborne(lat, lon, 1500.0, 90.0, -800.0, kLszh14Heading);
  REQUIRE(classify(t, TrafficPhase::Unknown) == TrafficPhase::Unknown);
}
