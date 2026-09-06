#include "data/traffic_context.hpp"
#include "../tools/atc_repl/traffic_fixture.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <cstddef>
#include <string>

// ── trim_callsign ───────────────────────────────────────────────────────────

TEST_CASE("trim_callsign: strips trailing nulls + whitespace",
          "[traffic][callsign]") {
    // 8-byte slot: "DLH123\x00\x00 " — null-terminated, trailing space ignored.
    const char raw[8] = {'D', 'L', 'H', '1', '2', '3', '\0', ' '};
    REQUIRE(traffic_context::trim_callsign(raw, sizeof(raw)) == "DLH123");
}

TEST_CASE("trim_callsign: strips leading + trailing whitespace",
          "[traffic][callsign]") {
    const char raw[8] = {' ', ' ', 'S', 'W', 'R', '4', '2', ' '};
    REQUIRE(traffic_context::trim_callsign(raw, sizeof(raw)) == "SWR42");
}

TEST_CASE("trim_callsign: empty buffer", "[traffic][callsign]") {
    const char raw[8] = {'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'};
    REQUIRE(traffic_context::trim_callsign(raw, sizeof(raw)) == "");
}

TEST_CASE("trim_callsign: full 8-byte non-terminated callsign",
          "[traffic][callsign]") {
    const char raw[8] = {'A', 'B', 'C', '1', '2', '3', '4', '5'};
    REQUIRE(traffic_context::trim_callsign(raw, sizeof(raw)) == "ABC12345");
}

// ── Fixture loader (atc_repl + tests share this) ────────────────────────────

#ifndef XP_WELLYS_ATC_TEST_FIXTURES_DIR
#error "XP_WELLYS_ATC_TEST_FIXTURES_DIR must be defined by tests CMake"
#endif

static std::string fixture_path(const char *name) {
    return std::string(XP_WELLYS_ATC_TEST_FIXTURES_DIR) + "/" + name;
}

TEST_CASE("traffic fixture: loader filters > 40 NM cutoff",
          "[traffic][fixture]") {
    auto loaded = traffic_fixture::load(fixture_path("traffic_lszh_basic.json"));
    // Fixture has 5 raw targets; FAR123 sits ~50 NM W and must be dropped.
    REQUIRE(loaded.snapshot.targets.size() == 4);
    for (const auto &t : loaded.snapshot.targets) {
        REQUIRE(t.distance_to_user_nm <= 40.0);
        REQUIRE(t.callsign != "FAR123");
    }
}

TEST_CASE("traffic fixture: targets sorted ascending by distance",
          "[traffic][fixture]") {
    auto loaded = traffic_fixture::load(fixture_path("traffic_lszh_basic.json"));
    for (std::size_t i = 1; i < loaded.snapshot.targets.size(); ++i) {
        REQUIRE(loaded.snapshot.targets[i - 1].distance_to_user_nm <=
                loaded.snapshot.targets[i].distance_to_user_nm);
    }
}

TEST_CASE("traffic fixture: derived fields are populated",
          "[traffic][fixture]") {
    auto loaded = traffic_fixture::load(fixture_path("traffic_lszh_basic.json"));
    bool found_dlh = false;
    for (const auto &t : loaded.snapshot.targets) {
        if (t.callsign == "DLH123") {
            found_dlh = true;
            // DLH123 sits 5 NM due north of LSZH at 4500 ft. User is at
            // 1500 ft heading 280 — so the target is ~80° to the right
            // of the nose, putting it at 3 o'clock.
            REQUIRE(t.distance_to_user_nm == Catch::Approx(5.0).margin(0.5));
            REQUIRE(t.bearing_from_user_deg == Catch::Approx(0.0).margin(2.0));
            REQUIRE(t.altitude_diff_ft == Catch::Approx(3000.0).margin(1.0));
            REQUIRE(t.alt_agl_ft == Catch::Approx(4500.0 - 1416.0).margin(1.0));
            REQUIRE(t.clock_position == Catch::Approx(3.0));
        }
    }
    REQUIRE(found_dlh);
}

TEST_CASE("traffic_context::set_for_test round-trips through current()",
          "[traffic][context]") {
    auto loaded = traffic_fixture::load(fixture_path("traffic_lszh_basic.json"));
    traffic_context::set_for_test(loaded.snapshot);

    const auto &snap = traffic_context::current();
    REQUIRE(snap.targets.size() == loaded.snapshot.targets.size());
    if (!snap.targets.empty()) {
        REQUIRE(snap.targets.front().callsign ==
                loaded.snapshot.targets.front().callsign);
    }

    traffic_context::set_for_test(traffic_context::TrafficContext{});
    REQUIRE(traffic_context::current().targets.empty());
}
