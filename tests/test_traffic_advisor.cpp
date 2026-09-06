#include "atc/atc_state_machine.hpp"
#include "atc/traffic_advisor.hpp"
#include "data/traffic_context.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>

using traffic_advisor::AdvisoryHistory;
using traffic_advisor::evaluate;
using traffic_advisor::mark_emitted;
using traffic_advisor::UserState;
using traffic_context::TrafficContext;
using traffic_context::TrafficTarget;

// ── Fixture builders ────────────────────────────────────────────────────────

namespace {

// User on TOWER frequency in TOWER_CONTACT, ready to receive advisories.
// Default is airborne (on_ground=false) so existing tests keep behaviour.
UserState in_contact_user(double track = 0.0, double alt = 1500.0,
                          double gs = 100.0) {
  UserState u;
  u.atc_state = atc_state_machine::ATCState::TOWER_CONTACT;
  u.on_active_atc_freq = true;
  u.lat = 47.4583;
  u.lon = 8.5483;
  u.alt_msl_ft = alt;
  u.heading_deg = track;
  u.track_deg = track;
  u.groundspeed_kts = gs;
  u.on_ground = false;
  u.target_has_mode_c_default = true;
  return u;
}

// Manually-built target with derived geometry pre-filled. The advisor
// reads only the fields it cares about, so we don't need lat/lon to
// match clock_position — they're consistent in the production runtime
// reader and fixture loader, but for unit tests it's easier to set the
// derived fields directly.
TrafficTarget make_target(uint32_t modeS, double clock, double dist_nm,
                          double alt_diff, double track_deg = 180.0,
                          double gs = 200.0) {
  TrafficTarget t;
  t.modeS_id = modeS;
  t.callsign = "TEST";
  t.clock_position = clock;
  t.distance_to_user_nm = dist_nm;
  t.altitude_diff_ft = alt_diff;
  // Bearing is just clock + heading mod 360. With user heading 0 the
  // bearing is clock_pos * 30 (1->30, 2->60, 12->360 etc.).
  t.bearing_from_user_deg = clock * 30.0;
  if (t.bearing_from_user_deg >= 360.0)
    t.bearing_from_user_deg -= 360.0;
  t.alt_msl_ft = 1500.0 + alt_diff;
  // Default airborne (1500 ft AGL) so the advisor's domain-match filter
  // doesn't suppress every existing test that builds airborne-pilot
  // fixtures. Tests that need a ground-domain target override this.
  t.alt_agl_ft = 1500.0;
  t.track_deg = track_deg;
  t.groundspeed_kts = gs;
  return t;
}

} // namespace

// ── Trigger logic ────────────────────────────────────────────────────────────

TEST_CASE("advisor: one converging target -> advisory triggers",
          "[traffic][advisor]") {
    UserState u = in_contact_user();

    TrafficContext ctx;
    // Target at 1 o'clock, 5 NM, +500 ft, head-on (track 180 -> opposite
    // direction). Closure rate is positive.
    ctx.targets.push_back(make_target(0xABCDE, 1.0, 5.0, 500.0, 180.0));

    AdvisoryHistory history;
    auto adv = evaluate(ctx, u, history, 100.0);
    REQUIRE(adv.has_value());
    REQUIRE(adv->modeS_id == 0xABCDE);
    REQUIRE(adv->vars["clock"] == "1");
    REQUIRE(adv->vars["distance"] == "5");
    REQUIRE(adv->vars["direction"] == "opposite direction");
    REQUIRE(adv->vars["altitude_info"] == "indicating 2000 feet");
}

TEST_CASE("advisor: 60s per-target cooldown blocks duplicate",
          "[traffic][advisor]") {
    UserState u = in_contact_user();
    TrafficContext ctx;
    ctx.targets.push_back(make_target(42, 1.0, 4.0, 0.0, 180.0));

    AdvisoryHistory history;
    auto first = evaluate(ctx, u, history, 100.0);
    REQUIRE(first.has_value());
    mark_emitted(history, first->modeS_id, 100.0);

    // Same target, +30 s — per-target cooldown (60 s) blocks.
    auto second = evaluate(ctx, u, history, 130.0);
    REQUIRE_FALSE(second.has_value());

    // Same target, +61 s — per-target cooldown expired AND global
    // cooldown (20 s) expired. Should fire again.
    auto third = evaluate(ctx, u, history, 161.0);
    REQUIRE(third.has_value());
}

TEST_CASE("advisor: 20s global cooldown blocks a different target",
          "[traffic][advisor]") {
    UserState u = in_contact_user();
    AdvisoryHistory history;

    TrafficContext ctx_a;
    ctx_a.targets.push_back(make_target(100, 1.0, 4.0, 0.0, 180.0));
    auto first = evaluate(ctx_a, u, history, 0.0);
    REQUIRE(first.has_value());
    mark_emitted(history, first->modeS_id, 0.0);

    // Different target (different modeS), only 5 s later. Per-target
    // cooldown wouldn't apply — it's a new target — but the global 20 s
    // cooldown blocks any advisory.
    TrafficContext ctx_b;
    ctx_b.targets.push_back(make_target(200, 2.0, 3.0, 200.0, 180.0));
    auto second = evaluate(ctx_b, u, history, 5.0);
    REQUIRE_FALSE(second.has_value());

    // 21 s after the first emit, the global cooldown is clear.
    auto third = evaluate(ctx_b, u, history, 21.0);
    REQUIRE(third.has_value());
    REQUIRE(third->modeS_id == 200);
}

TEST_CASE("advisor: user not in contact -> no advisory",
          "[traffic][advisor]") {
    UserState u = in_contact_user();
    u.atc_state = atc_state_machine::ATCState::IDLE; // gating fails

    TrafficContext ctx;
    ctx.targets.push_back(make_target(1, 1.0, 4.0, 0.0, 180.0));

    AdvisoryHistory history;
    REQUIRE_FALSE(evaluate(ctx, u, history, 100.0).has_value());

    // Right state but on UNKNOWN frequency -> still gated out.
    u.atc_state = atc_state_machine::ATCState::TOWER_CONTACT;
    u.on_active_atc_freq = false;
    REQUIRE_FALSE(evaluate(ctx, u, history, 100.0).has_value());
}

TEST_CASE("advisor: target behind user (clock 4-8) -> no advisory",
          "[traffic][advisor]") {
    UserState u = in_contact_user();
    AdvisoryHistory history;

    for (double clock : {4.0, 5.0, 6.0, 7.0, 8.0}) {
        TrafficContext ctx;
        ctx.targets.push_back(
            make_target(static_cast<uint32_t>(clock), clock, 4.0, 0.0, 180.0));
        INFO("clock=" << clock);
        REQUIRE_FALSE(evaluate(ctx, u, history, 100.0).has_value());
    }
}

TEST_CASE("advisor: nearest qualifying target wins",
          "[traffic][advisor]") {
    UserState u = in_contact_user();
    AdvisoryHistory history;

    TrafficContext ctx;
    // Sorted by distance (the runtime reader / fixture loader sort
    // ascending by distance — the advisor relies on that ordering).
    ctx.targets.push_back(make_target(200, 2.0, 3.0, 0.0, 180.0));
    ctx.targets.push_back(make_target(300, 2.0, 6.0, 0.0, 180.0));

    auto adv = evaluate(ctx, u, history, 100.0);
    REQUIRE(adv.has_value());
    REQUIRE(adv->modeS_id == 200);
}

TEST_CASE("advisor: out-of-range targets are skipped",
          "[traffic][advisor]") {
    UserState u = in_contact_user();
    AdvisoryHistory history;

    TrafficContext ctx;
    // Too close (< 2 NM).
    ctx.targets.push_back(make_target(1, 1.0, 1.5, 0.0, 180.0));
    // Too far (> 8 NM).
    ctx.targets.push_back(make_target(2, 2.0, 9.0, 0.0, 180.0));
    // Altitude difference > 1500 ft.
    ctx.targets.push_back(make_target(3, 2.0, 4.0, 2000.0, 180.0));
    // Diverging (target moving away — same track as user, faster).
    ctx.targets.push_back(make_target(4, 12.0, 5.0, 0.0, 0.0, 200.0));

    REQUIRE_FALSE(evaluate(ctx, u, history, 100.0).has_value());
}

TEST_CASE("advisor: visual-ack lockout suppresses re-advising the same target",
          "[traffic][advisor]") {
    UserState u = in_contact_user();
    TrafficContext ctx;
    ctx.targets.push_back(make_target(7, 1.0, 4.0, 0.0, 180.0));

    AdvisoryHistory history;
    auto first = evaluate(ctx, u, history, 0.0);
    REQUIRE(first.has_value());
    mark_emitted(history, first->modeS_id, 0.0);
    // Pilot acknowledges with positive visual contact — extends the
    // per-target lockout from 60 s baseline to kVisualAckLockoutSec.
    traffic_advisor::mark_acknowledged_visual(history, first->modeS_id, 5.0);

    // Well past the regular 60 s cooldown, but inside the visual-ack
    // lockout — must stay suppressed.
    REQUIRE_FALSE(evaluate(ctx, u, history, 120.0).has_value());
    REQUIRE_FALSE(evaluate(ctx, u, history, 250.0).has_value());

    // Past the visual-ack window — eligible again.
    auto later =
        evaluate(ctx, u, history,
                 5.0 + traffic_advisor::kVisualAckLockoutSec + 1.0);
    REQUIRE(later.has_value());
    REQUIRE(later->modeS_id == 7);
}

TEST_CASE("advisor: ICAO type from provider flows into vars",
          "[traffic][advisor]") {
    UserState u = in_contact_user();
    TrafficContext ctx;
    auto t = make_target(11, 1.0, 5.0, 0.0, 180.0);
    t.icao_type = "C172";
    ctx.targets.push_back(t);

    AdvisoryHistory history;
    auto adv = evaluate(ctx, u, history, 0.0);
    REQUIRE(adv.has_value());
    REQUIRE(adv->vars["type"] == "C172");

    // Empty icao_type falls back to the EU phraseology default.
    TrafficContext ctx2;
    ctx2.targets.push_back(make_target(12, 1.0, 5.0, 0.0, 180.0));
    AdvisoryHistory h2;
    auto adv2 = evaluate(ctx2, u, h2, 0.0);
    REQUIRE(adv2.has_value());
    REQUIRE(adv2->vars["type"] == "type unknown");
}

// ── Domain-match (ground vs airborne) ─────────────────────────────

TEST_CASE("advisor: airborne user gets no ground-traffic advisory",
          "[traffic][advisor][domain]") {
    UserState u = in_contact_user(); // on_ground=false (default)

    TrafficContext ctx;
    auto t = make_target(0xA1, 1.0, 5.0, -1400.0, 180.0);
    t.alt_agl_ft = 50.0; // ground-domain target (taxiing / liftoff roll)
    ctx.targets.push_back(t);

    AdvisoryHistory history;
    REQUIRE_FALSE(evaluate(ctx, u, history, 0.0).has_value());
}

TEST_CASE("advisor: ground user gets no airborne advisory",
          "[traffic][advisor][domain]") {
    UserState u = in_contact_user();
    u.on_ground = true;
    u.alt_msl_ft = 1700.0; // parked at LSZB
    u.groundspeed_kts = 0.0;

    TrafficContext ctx;
    // Airborne traffic at +1400 ft, 5 NM, 1 o'clock — would qualify
    // geometrically for an airborne pilot but must be suppressed for
    // a ground pilot.
    auto t = make_target(0xB2, 1.0, 5.0, 1400.0, 180.0);
    t.alt_agl_ft = 1400.0;
    ctx.targets.push_back(t);

    AdvisoryHistory history;
    REQUIRE_FALSE(evaluate(ctx, u, history, 0.0).has_value());
}

TEST_CASE("advisor: ground user gets ground-domain advisory",
          "[traffic][advisor][domain]") {
    UserState u = in_contact_user();
    u.on_ground = true;
    u.alt_msl_ft = 1700.0;
    u.groundspeed_kts = 5.0; // taxiing slowly

    TrafficContext ctx;
    auto t = make_target(0xC3, 1.0, 3.0, 0.0, 180.0, 10.0);
    t.alt_agl_ft = 20.0; // ground-domain target
    t.alt_msl_ft = 1720.0;
    ctx.targets.push_back(t);

    AdvisoryHistory history;
    auto adv = evaluate(ctx, u, history, 0.0);
    REQUIRE(adv.has_value());
    REQUIRE(adv->modeS_id == 0xC3);
}

// ── Phase-3 ground-conflict trigger ──────────────────────────────────

namespace {

// Build a taxiing user at LSZH. user_taxiing flag drives the
// ground-conflict path; on_active_atc_freq is left FALSE on purpose
// — surface safety advisories should not need ATC contact.
UserState taxiing_user(double heading = 360.0) {
  UserState u;
  u.atc_state = atc_state_machine::ATCState::IDLE;
  u.on_active_atc_freq = false;
  u.lat = 47.4583;
  u.lon = 8.5483;
  u.alt_msl_ft = 1416.0;
  u.heading_deg = heading;
  u.track_deg = heading;
  u.groundspeed_kts = 10.0;
  u.on_ground = true;
  u.user_taxiing = true;
  return u;
}

// Build a ground target at a metre offset (east, north) from the user
// with a given phase, track, and groundspeed.
TrafficTarget ground_target(uint32_t modeS, double east_m, double north_m,
                            traffic_context::TrafficPhase phase,
                            double track_deg, double gs_kts,
                            double user_lat = 47.4583,
                            double user_lon = 8.5483) {
  TrafficTarget t;
  t.modeS_id = modeS;
  t.callsign = "TAXI";
  constexpr double kDeg2Rad = M_PI / 180.0;
  t.lat = user_lat + (north_m / 111000.0);
  t.lon = user_lon + (east_m / (111000.0 * std::cos(user_lat * kDeg2Rad)));
  t.alt_msl_ft = 1416.0;
  t.alt_agl_ft = 10.0;
  t.track_deg = track_deg;
  t.groundspeed_kts = gs_kts;
  t.phase = phase;
  // Derived geometry — runtime + fixture loader both populate these,
  // and the ground-conflict path consults distance + clock.
  const double dlat_m = north_m;
  const double dlon_m = east_m;
  const double dist_m = std::sqrt(dlat_m * dlat_m + dlon_m * dlon_m);
  t.distance_to_user_nm = dist_m / 1852.0;
  // Bearing from user to target: atan2(dlon_m, dlat_m) gives compass
  // bearing (0 = north, 90 = east).
  double bearing = std::atan2(dlon_m, dlat_m) / kDeg2Rad;
  if (bearing < 0.0)
    bearing += 360.0;
  t.bearing_from_user_deg = bearing;
  // Clock position relative to user heading (track == heading here).
  double rel = std::fmod(bearing - 360.0 + 720.0, 360.0);
  double clock = std::round(rel / 30.0);
  if (clock <= 0.0 || clock >= 12.0)
    clock = 12.0;
  t.clock_position = clock;
  return t;
}

} // namespace

TEST_CASE("advisor: taxiing user, crossing target -> hold_position",
          "[traffic][advisor][ground]") {
  // User heading north. Target 100 m ahead and slightly west, moving
  // east at 15 kts — crosses the user's path.
  UserState u = taxiing_user(360.0);
  TrafficContext ctx;
  ctx.targets.push_back(ground_target(0xA01, -20.0, 100.0,
                                      traffic_context::TrafficPhase::Taxi,
                                      90.0, 15.0));

  AdvisoryHistory history;
  auto adv = evaluate(ctx, u, history, 0.0);
  REQUIRE(adv.has_value());
  REQUIRE(adv->modeS_id == 0xA01);
  REQUIRE(adv->template_key == "taxi_hold_position");
  REQUIRE(adv->requires_ack == false);
}

TEST_CASE("advisor: ground-conflict 30s per-target cooldown",
          "[traffic][advisor][ground]") {
  UserState u = taxiing_user(360.0);
  TrafficContext ctx;
  ctx.targets.push_back(ground_target(
      0xA02, -20.0, 100.0, traffic_context::TrafficPhase::Taxi, 90.0, 15.0));

  AdvisoryHistory history;
  auto first = evaluate(ctx, u, history, 100.0);
  REQUIRE(first.has_value());
  mark_emitted(history, first->modeS_id, 100.0);

  // +15 s — global ground cooldown (15 s) just expired, but per-target
  // ground cooldown (30 s) still blocks the same modeS.
  REQUIRE_FALSE(evaluate(ctx, u, history, 116.0).has_value());

  // +31 s — both cooldowns expired, fires again.
  auto third = evaluate(ctx, u, history, 131.0);
  REQUIRE(third.has_value());
}

TEST_CASE("advisor: ground-conflict 15s global cooldown",
          "[traffic][advisor][ground]") {
  UserState u = taxiing_user(360.0);
  AdvisoryHistory history;

  TrafficContext ctx_a;
  ctx_a.targets.push_back(ground_target(
      0xA10, -20.0, 100.0, traffic_context::TrafficPhase::Taxi, 90.0, 15.0));
  auto first = evaluate(ctx_a, u, history, 0.0);
  REQUIRE(first.has_value());
  mark_emitted(history, first->modeS_id, 0.0);

  // Different target, only 10 s later — global ground cooldown blocks.
  TrafficContext ctx_b;
  ctx_b.targets.push_back(ground_target(
      0xA11, 30.0, 100.0, traffic_context::TrafficPhase::Taxi, 270.0, 15.0));
  REQUIRE_FALSE(evaluate(ctx_b, u, history, 10.0).has_value());

  // 16 s after first emit, global cooldown clear.
  auto third = evaluate(ctx_b, u, history, 16.0);
  REQUIRE(third.has_value());
  REQUIRE(third->modeS_id == 0xA11);
}

TEST_CASE("advisor: ground-conflict template picks caution for slow side",
          "[traffic][advisor][ground]") {
  UserState u = taxiing_user(360.0);
  TrafficContext ctx;
  // Target abeam on the user's right (clock ~3), slow taxi (10 kts).
  // Position so the path also intersects the cone — place ahead-right
  // (east + slightly north), moving roughly toward user's path.
  ctx.targets.push_back(ground_target(0xA20, 80.0, 80.0,
                                      traffic_context::TrafficPhase::Taxi,
                                      270.0, 10.0));

  AdvisoryHistory history;
  auto adv = evaluate(ctx, u, history, 0.0);
  REQUIRE(adv.has_value());
  REQUIRE(adv->template_key == "taxi_caution");
  REQUIRE(adv->vars.at("side") == "right");
}

TEST_CASE("advisor: ground-conflict gated off when user not taxiing",
          "[traffic][advisor][ground]") {
  UserState u = taxiing_user(360.0);
  u.user_taxiing = false; // parked
  TrafficContext ctx;
  ctx.targets.push_back(ground_target(
      0xA30, -20.0, 100.0, traffic_context::TrafficPhase::Taxi, 90.0, 15.0));

  AdvisoryHistory history;
  REQUIRE_FALSE(evaluate(ctx, u, history, 0.0).has_value());
}

TEST_CASE("advisor: ground-conflict skips OnGround/Unknown target phases",
          "[traffic][advisor][ground]") {
  UserState u = taxiing_user(360.0);
  AdvisoryHistory history;

  TrafficContext ctx_idle;
  ctx_idle.targets.push_back(ground_target(
      0xA40, -20.0, 100.0, traffic_context::TrafficPhase::OnGround, 0.0, 0.0));
  REQUIRE_FALSE(evaluate(ctx_idle, u, history, 0.0).has_value());

  TrafficContext ctx_unk;
  ctx_unk.targets.push_back(ground_target(
      0xA41, -20.0, 100.0, traffic_context::TrafficPhase::Unknown, 90.0, 15.0));
  REQUIRE_FALSE(evaluate(ctx_unk, u, history, 0.0).has_value());
}

TEST_CASE("advisor: ground-conflict skips out-of-cone target",
          "[traffic][advisor][ground]") {
  UserState u = taxiing_user(360.0);
  TrafficContext ctx;
  // Target 200 m south of user (behind), moving south. Out of forward
  // cone — no trigger.
  ctx.targets.push_back(ground_target(0xA50, 0.0, -200.0,
                                      traffic_context::TrafficPhase::Taxi,
                                      180.0, 10.0));

  AdvisoryHistory history;
  REQUIRE_FALSE(evaluate(ctx, u, history, 0.0).has_value());
}
