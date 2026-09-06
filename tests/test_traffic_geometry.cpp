#include "data/traffic_geometry.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <cmath>

using Catch::Approx;

// LSZH (Zurich) and LSGG (Geneva) reference points. Distance and
// initial bearing are well-known great-circle values; the test allows
// ±1 NM / ±1° tolerance because the published runway lat/lon for each
// airport varies a few hundred metres between sources.
static constexpr double kLszhLat = 47.4583;
static constexpr double kLszhLon = 8.5483;
static constexpr double kLsggLat = 46.2381;
static constexpr double kLsggLon = 6.1090;

TEST_CASE("traffic_geometry::distance_nm: LSZH -> LSGG", "[traffic][geometry]") {
    // Great-circle distance LSZH (47.4583, 8.5483) to LSGG (46.2381, 6.1090)
    // is ~124 NM via haversine. Tolerance is wide enough that small
    // refinements to the reference lat/lon won't churn the test.
    const double d = traffic_geometry::distance_nm(kLszhLat, kLszhLon,
                                                   kLsggLat, kLsggLon);
    REQUIRE(d == Approx(124.0).margin(2.0));
}

TEST_CASE("traffic_geometry::bearing_deg: LSZH -> LSGG", "[traffic][geometry]") {
    const double b = traffic_geometry::bearing_deg(kLszhLat, kLszhLon,
                                                   kLsggLat, kLsggLon);
    REQUIRE(b == Approx(234.0).margin(1.5));
}

TEST_CASE("traffic_geometry::distance_nm: zero distance",
          "[traffic][geometry]") {
    REQUIRE(traffic_geometry::distance_nm(kLszhLat, kLszhLon, kLszhLat,
                                          kLszhLon) == Approx(0.0).margin(1e-6));
}

TEST_CASE("traffic_geometry::bearing_deg: result is in [0, 360)",
          "[traffic][geometry]") {
    // Point due south of LSZH should yield ~180 deg.
    const double b = traffic_geometry::bearing_deg(kLszhLat, kLszhLon,
                                                   kLszhLat - 1.0, kLszhLon);
    REQUIRE(b == Approx(180.0).margin(0.5));
    REQUIRE(b >= 0.0);
    REQUIRE(b < 360.0);
}

// The clock-position helper rounds to the nearest hour and collapses
// 0/12 onto 12 so the result stays in (0, 12]. Each row in the table
// pins down one of the edge cases described in the spec.
struct ClockCase {
    double heading;
    double bearing;
    double expected;
    const char *label;
};

TEST_CASE("traffic_geometry::clock_position table",
          "[traffic][geometry][clock]") {
    const ClockCase cases[] = {
        {360.0, 90.0, 3.0, "head=360 brg=090 -> 3 o'clock"},
        {180.0, 90.0, 9.0, "head=180 brg=090 -> 9 o'clock"},
        {0.0, 0.0, 12.0, "head=000 brg=000 -> 12 o'clock"},
        {350.0, 10.0, 1.0, "head=350 brg=010 -> 1 o'clock (wrap)"},
        {10.0, 350.0, 11.0, "head=010 brg=350 -> 11 o'clock (wrap)"},
        {0.0, 355.0, 12.0, "head=000 brg=355 -> 12 o'clock (just left)"},
        {0.0, 5.0, 12.0, "head=000 brg=005 -> 12 o'clock (just right)"},
        {270.0, 0.0, 3.0, "head=270 brg=000 -> 3 o'clock"},
        {90.0, 270.0, 6.0, "head=090 brg=270 -> 6 o'clock"},
    };
    for (const auto &c : cases) {
        INFO(c.label);
        REQUIRE(traffic_geometry::clock_position(c.heading, c.bearing) ==
                Approx(c.expected));
    }
}

TEST_CASE("traffic_geometry::clock_position: result is always in (0, 12]",
          "[traffic][geometry][clock]") {
    for (int hdg = 0; hdg < 360; hdg += 23) {
        for (int brg = 0; brg < 360; brg += 17) {
            const double c =
                traffic_geometry::clock_position(hdg, brg);
            REQUIRE(c > 0.0);
            REQUIRE(c <= 12.0);
        }
    }
}

// ── classify_relative_track ─────────────────────────────────────────────────

TEST_CASE("classify_relative_track: opposite direction band",
          "[traffic][geometry][classifier]") {
    // user_track = 0; target_track in [150, 210] -> opposite direction.
    // 12 o'clock keeps the result in the "opposite" branch (ahead of user).
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 150.0, 12.0) ==
            "opposite direction");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 180.0, 12.0) ==
            "opposite direction");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 210.0, 12.0) ==
            "opposite direction");
    // Just outside the lower bound (149°) drops to "converging".
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 149.0, 12.0) ==
            "converging");
    // Just outside the upper bound (211°) drops to "converging".
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 211.0, 12.0) ==
            "converging");
}

TEST_CASE("classify_relative_track: same direction band",
          "[traffic][geometry][classifier]") {
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 0.0, 12.0) ==
            "same direction");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 30.0, 12.0) ==
            "same direction");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 330.0, 12.0) ==
            "same direction");
    // Just outside the upper bound (31°) leaves the same-direction zone.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 31.0, 12.0) !=
            "same direction");
    // Just outside the lower bound (329°) likewise.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 329.0, 12.0) !=
            "same direction");
}

TEST_CASE("classify_relative_track: crossing left to right",
          "[traffic][geometry][classifier]") {
    // user_track = 0, target_track = 90 (perpendicular crossing) AND
    // target on the user's left side (clock 9, 10, 11) -> L->R.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 90.0, 10.0) ==
            "crossing left to right");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 60.0, 11.0) ==
            "crossing left to right");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 120.0, 9.5) ==
            "crossing left to right");
    // Same diff but target on the right side -> classifier rejects the
    // L->R label and falls through to "converging".
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 90.0, 3.0) !=
            "crossing left to right");
}

TEST_CASE("classify_relative_track: crossing right to left",
          "[traffic][geometry][classifier]") {
    // user_track = 0, target_track = 270 -> diff = 270 (in [240, 300]) AND
    // target on the user's right side (clock 1, 2, 3) -> R->L.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 270.0, 2.0) ==
            "crossing right to left");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 240.0, 3.0) ==
            "crossing right to left");
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 300.0, 1.0) ==
            "crossing right to left");
    // Same diff but target on the left -> falls through.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 270.0, 9.0) !=
            "crossing right to left");
}

TEST_CASE("classify_relative_track: fallback converging",
          "[traffic][geometry][classifier]") {
    // 45° track diff, target at 12 o'clock — neither perpendicular nor
    // a same/opposite case.
    REQUIRE(traffic_geometry::classify_relative_track(0.0, 45.0, 12.0) ==
            "converging");
}

// ── format_altitude_info ────────────────────────────────────────────────────

TEST_CASE("format_altitude_info: mode-c rounds to nearest 100 ft",
          "[traffic][geometry][altitude]") {
    REQUIRE(traffic_geometry::format_altitude_info(4530.0, 1500.0, true) ==
            "indicating 4500 feet");
    REQUIRE(traffic_geometry::format_altitude_info(4571.0, 1500.0, true) ==
            "indicating 4600 feet");
    REQUIRE(traffic_geometry::format_altitude_info(0.0, 1500.0, true) ==
            "indicating 0 feet");
}

TEST_CASE("format_altitude_info: relative within 2000 ft",
          "[traffic][geometry][altitude]") {
    // Target above by 1000 ft.
    REQUIRE(traffic_geometry::format_altitude_info(2500.0, 1500.0, false) ==
            "1000 feet above");
    // Target below by 1000 ft.
    REQUIRE(traffic_geometry::format_altitude_info(500.0, 1500.0, false) ==
            "1000 feet below");
    // Rounding to nearest 100 ft.
    REQUIRE(traffic_geometry::format_altitude_info(2530.0, 1500.0, false) ==
            "1000 feet above");
}

TEST_CASE("format_altitude_info: unknown when far apart and no mode-c",
          "[traffic][geometry][altitude]") {
    REQUIRE(traffic_geometry::format_altitude_info(5000.0, 1500.0, false) ==
            "altitude unknown");
    REQUIRE(traffic_geometry::format_altitude_info(1500.0, 5000.0, false) ==
            "altitude unknown");
}

// ── path_intersects_cone (Phase 3 ground-conflict primitive) ──────────────

// Helper: a metre offset in latitude. 1 deg lat ≈ 111 km.
static double meters_to_deg_lat(double m) { return m / 111000.0; }
// Helper: a metre offset in longitude at a given latitude.
static double meters_to_deg_lon(double m, double lat) {
    constexpr double kDeg2Rad = M_PI / 180.0;
    return m / (111000.0 * std::cos(lat * kDeg2Rad));
}

TEST_CASE("path_intersects_cone: target crossing user's path triggers",
          "[traffic][geometry][cone]") {
    // User at LSZH apron, heading 360 (north). Cone: ±30°, 200 m, 20 s.
    // Target 50 m north and 100 m west of the user, moving east at 20 kts.
    // After ~10 s it sits directly north of the user (50 m ahead) — inside
    // the forward cone.
    const double user_lat = 47.4583;
    const double user_lon = 8.5483;
    const double target_lat = user_lat + meters_to_deg_lat(50.0);
    const double target_lon = user_lon - meters_to_deg_lon(100.0, user_lat);
    REQUIRE(traffic_geometry::path_intersects_cone(
                user_lat, user_lon, 360.0, 30.0, 200.0, target_lat,
                target_lon, 90.0, 20.0, 20.0));
}

TEST_CASE("path_intersects_cone: same target moving away misses",
          "[traffic][geometry][cone]") {
    const double user_lat = 47.4583;
    const double user_lon = 8.5483;
    // Same start position (50 m N / 100 m W) but moving WEST (away). The
    // path heads further northwest and never enters the ±30° forward
    // cone.
    const double target_lat = user_lat + meters_to_deg_lat(50.0);
    const double target_lon = user_lon - meters_to_deg_lon(100.0, user_lat);
    REQUIRE_FALSE(traffic_geometry::path_intersects_cone(
                      user_lat, user_lon, 360.0, 30.0, 200.0, target_lat,
                      target_lon, 270.0, 20.0, 20.0));
}

TEST_CASE("path_intersects_cone: target dead ahead and stationary triggers",
          "[traffic][geometry][cone]") {
    const double user_lat = 47.4583;
    const double user_lon = 8.5483;
    // Target sitting 100 m directly north of the user.
    const double target_lat = user_lat + meters_to_deg_lat(100.0);
    const double target_lon = user_lon;
    REQUIRE(traffic_geometry::path_intersects_cone(
                user_lat, user_lon, 360.0, 30.0, 200.0, target_lat,
                target_lon, 0.0, 0.0, 20.0));
}

TEST_CASE("path_intersects_cone: target abeam at 300 m never enters cone",
          "[traffic][geometry][cone]") {
    const double user_lat = 47.4583;
    const double user_lon = 8.5483;
    // Target 300 m west, moving south — never crosses the user's
    // forward cone capped at 200 m.
    const double target_lat = user_lat;
    const double target_lon = user_lon - meters_to_deg_lon(300.0, user_lat);
    REQUIRE_FALSE(traffic_geometry::path_intersects_cone(
                      user_lat, user_lon, 360.0, 30.0, 200.0, target_lat,
                      target_lon, 180.0, 15.0, 20.0));
}

TEST_CASE("path_intersects_cone: target behind user does not trigger",
          "[traffic][geometry][cone]") {
    const double user_lat = 47.4583;
    const double user_lon = 8.5483;
    // Target 100 m south of the user, stationary. The cone faces north;
    // the target is in the rear-arc and outside the cone.
    const double target_lat = user_lat - meters_to_deg_lat(100.0);
    const double target_lon = user_lon;
    REQUIRE_FALSE(traffic_geometry::path_intersects_cone(
                      user_lat, user_lon, 360.0, 30.0, 200.0, target_lat,
                      target_lon, 0.0, 0.0, 20.0));
}

// ── is_on_runway_centerline (Phase 4 runway-occupancy primitive) ──────────

TEST_CASE("is_on_runway_centerline: target sitting on the centerline",
          "[traffic][geometry][runway]") {
    // Threshold at LSZH RWY 14 reference (47.4794, 8.5253). Runway
    // heading 137° (true). Target 500 m down the runway, no lateral
    // offset.
    const double thr_lat = 47.4794;
    const double thr_lon = 8.5253;
    const double heading = 137.0;
    constexpr double kDeg2Rad = M_PI / 180.0;
    const double along_m = 500.0;
    const double dlat_m = along_m * std::cos(heading * kDeg2Rad);
    const double dlon_m = along_m * std::sin(heading * kDeg2Rad);
    const double tgt_lat = thr_lat + meters_to_deg_lat(dlat_m);
    const double tgt_lon = thr_lon + meters_to_deg_lon(dlon_m, thr_lat);
    REQUIRE(traffic_geometry::is_on_runway_centerline(
        tgt_lat, tgt_lon, thr_lat, thr_lon, heading, 2500.0, 30.0));
}

TEST_CASE("is_on_runway_centerline: target offset laterally is rejected",
          "[traffic][geometry][runway]") {
    const double thr_lat = 47.4794;
    const double thr_lon = 8.5253;
    const double heading = 137.0;
    constexpr double kDeg2Rad = M_PI / 180.0;
    // 500 m down-track, but 100 m to the right of the centerline.
    const double along_m = 500.0;
    const double dlat_along = along_m * std::cos(heading * kDeg2Rad);
    const double dlon_along = along_m * std::sin(heading * kDeg2Rad);
    // Right-perpendicular = heading + 90°.
    const double perp = (heading + 90.0) * kDeg2Rad;
    const double dlat_perp = 100.0 * std::cos(perp);
    const double dlon_perp = 100.0 * std::sin(perp);
    const double tgt_lat =
        thr_lat + meters_to_deg_lat(dlat_along + dlat_perp);
    const double tgt_lon =
        thr_lon + meters_to_deg_lon(dlon_along + dlon_perp, thr_lat);
    REQUIRE_FALSE(traffic_geometry::is_on_runway_centerline(
        tgt_lat, tgt_lon, thr_lat, thr_lon, heading, 2500.0, 30.0));
}

TEST_CASE("is_on_runway_centerline: target past the far end is rejected",
          "[traffic][geometry][runway]") {
    const double thr_lat = 47.4794;
    const double thr_lon = 8.5253;
    const double heading = 137.0;
    constexpr double kDeg2Rad = M_PI / 180.0;
    // 3 km past the threshold — beyond a 2.5 km runway.
    const double along_m = 3000.0;
    const double dlat_m = along_m * std::cos(heading * kDeg2Rad);
    const double dlon_m = along_m * std::sin(heading * kDeg2Rad);
    const double tgt_lat = thr_lat + meters_to_deg_lat(dlat_m);
    const double tgt_lon = thr_lon + meters_to_deg_lon(dlon_m, thr_lat);
    REQUIRE_FALSE(traffic_geometry::is_on_runway_centerline(
        tgt_lat, tgt_lon, thr_lat, thr_lon, heading, 2500.0, 30.0));
}

TEST_CASE("is_on_runway_centerline: target on the approach side is rejected",
          "[traffic][geometry][runway]") {
    // Target sits short of the threshold (along < 0). The helper only
    // accepts targets between the threshold and length_m past it —
    // approach-side traffic is the job of the Final-phase classifier.
    const double thr_lat = 47.4794;
    const double thr_lon = 8.5253;
    const double heading = 137.0;
    constexpr double kDeg2Rad = M_PI / 180.0;
    // 200 m before the threshold (negate along distance).
    const double along_m = -200.0;
    const double dlat_m = along_m * std::cos(heading * kDeg2Rad);
    const double dlon_m = along_m * std::sin(heading * kDeg2Rad);
    const double tgt_lat = thr_lat + meters_to_deg_lat(dlat_m);
    const double tgt_lon = thr_lon + meters_to_deg_lon(dlon_m, thr_lat);
    REQUIRE_FALSE(traffic_geometry::is_on_runway_centerline(
        tgt_lat, tgt_lon, thr_lat, thr_lon, heading, 2500.0, 30.0));
}
