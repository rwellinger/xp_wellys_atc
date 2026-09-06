// Catch2 unit tests for the VRP database loader. Pins the DE-region
// JSON-backed VRP set against the lookup API used by intent_parser
// (find_in_transcript) and the pattern-flow / ground-operations modules
// (get_pattern_direction). The EDNY "Sierra" path is the M9 DoD anchor:
// if these tests start failing, the deutsche VRP-Anflug-Flow breaks.

#include "data/airport_vrps.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <cstdlib>
#include <string>

namespace {

// RAII guard: switches flow_region to "DE" + reloads the VRP database
// from data/atc_profiles/de/airport_vrps.json, then restores the previous
// region so the test order does not contaminate neighbouring TUs.
struct DeVrpGuard {
    std::string saved_region;
    DeVrpGuard() : saved_region(settings::atc_profile()) {
        settings::set_atc_profile("DE");
        airport_vrps::init();
    }
    ~DeVrpGuard() {
        airport_vrps::stop();
        settings::set_atc_profile(saved_region);
    }
};

// Like DeVrpGuard but also points XP_ATC_USER_PREFS_DIR at the fixtures
// directory so the loader picks up tests/fixtures/user_prefs/airport_vrps_de.json
// as a user override on top of the plugin defaults.
struct DeVrpUserOverrideGuard {
    std::string saved_region;
    bool had_env;
    std::string saved_env;
    DeVrpUserOverrideGuard() : saved_region(settings::atc_profile()) {
        const char *prev = std::getenv("XP_ATC_USER_PREFS_DIR");
        had_env = (prev != nullptr);
        if (had_env)
            saved_env = prev;
        std::string fixtures = std::string(XP_WELLYS_ATC_TEST_FIXTURES_DIR) +
                               "/user_prefs";
        setenv("XP_ATC_USER_PREFS_DIR", fixtures.c_str(), 1);
        settings::set_atc_profile("DE");
        airport_vrps::init();
    }
    ~DeVrpUserOverrideGuard() {
        airport_vrps::stop();
        settings::set_atc_profile(saved_region);
        if (had_env)
            setenv("XP_ATC_USER_PREFS_DIR", saved_env.c_str(), 1);
        else
            unsetenv("XP_ATC_USER_PREFS_DIR");
    }
};

} // namespace

TEST_CASE("airport_vrps: EDNY loads with Sierra (M9 DoD anchor)",
          "[airport_vrps][de]") {
    DeVrpGuard g;
    const auto *ad = airport_vrps::get("EDNY");
    REQUIRE(ad != nullptr);
    REQUIRE(ad->name == "Friedrichshafen");
    REQUIRE(ad->vrps.size() >= 3);

    bool has_sierra = false;
    for (const auto &v : ad->vrps) {
        if (v.name == "Sierra") {
            has_sierra = true;
            break;
        }
    }
    REQUIRE(has_sierra);
}

TEST_CASE("airport_vrps: unknown ICAO returns nullptr",
          "[airport_vrps]") {
    DeVrpGuard g;
    REQUIRE(airport_vrps::get("XXXX") == nullptr);
}

TEST_CASE("airport_vrps: find_in_transcript matches with 'approaching' marker",
          "[airport_vrps][de]") {
    DeVrpGuard g;
    auto match = airport_vrps::find_in_transcript(
        "EDNY", "approaching sierra at 2000 feet");
    REQUIRE(match == "Sierra");
}

TEST_CASE("airport_vrps: find_in_transcript matches with 'passing' marker",
          "[airport_vrps][de]") {
    DeVrpGuard g;
    auto match = airport_vrps::find_in_transcript(
        "EDNY", "passing november inbound for landing");
    REQUIRE(match == "November");
}

TEST_CASE("airport_vrps: find_in_transcript matches with 'via' marker",
          "[airport_vrps][de]") {
    DeVrpGuard g;
    auto match = airport_vrps::find_in_transcript(
        "EDNY", "inbound via oscar at 3000 feet");
    REQUIRE(match == "Oscar");
}

TEST_CASE("airport_vrps: find_in_transcript rejects naked VRP name without marker",
          "[airport_vrps][de]") {
    // Without a position marker the parser must NOT treat phonetic
    // letters as VRPs — otherwise registration numbers like "Sierra
    // Tango" or callsign chunks would false-trigger the VRP path.
    DeVrpGuard g;
    auto match = airport_vrps::find_in_transcript(
        "EDNY", "sierra and november in sight");
    REQUIRE(match.empty());
}

TEST_CASE("airport_vrps: find_in_transcript returns empty for unknown airport",
          "[airport_vrps]") {
    DeVrpGuard g;
    auto match = airport_vrps::find_in_transcript(
        "XXXX", "approaching sierra at 2000 feet");
    REQUIRE(match.empty());
}

TEST_CASE("airport_vrps: get_pattern_direction reads per-runway map (EDNY 24)",
          "[airport_vrps][de]") {
    // EDNY pattern is left-hand on both runways (AIP Germany via
    // Navigraph Charts) — both reads must return "left".
    DeVrpGuard g;
    REQUIRE(airport_vrps::get_pattern_direction("EDNY", "24") == "left");
    REQUIRE(airport_vrps::get_pattern_direction("EDNY", "06") == "left");
}

TEST_CASE("airport_vrps: get_pattern_direction falls back via _default (EDMA)",
          "[airport_vrps][de]") {
    // EDMA stores "pattern_direction": "left" as a string in JSON, which
    // the loader normalises to {"_default": "left"}. Any runway should
    // resolve to "left" via the _default fallback.
    DeVrpGuard g;
    REQUIRE(airport_vrps::get_pattern_direction("EDMA", "07") == "left");
    REQUIRE(airport_vrps::get_pattern_direction("EDMA", "25") == "left");
}

TEST_CASE("airport_vrps: get_pattern_direction strips L/R/C suffix (base-runway)",
          "[airport_vrps][de]") {
    // LSGS stores "25": "left" — a query for "25L" must fall back to
    // the base runway "25". This mirrors the lookup contract used by
    // pattern_flow.cpp and ground_operations.cpp.
    DeVrpGuard g;
    REQUIRE(airport_vrps::get_pattern_direction("LSGS", "25L") == "left");
    REQUIRE(airport_vrps::get_pattern_direction("LSGS", "07R") == "right");
}

TEST_CASE("airport_vrps: get_pattern_direction empty for unknown airport",
          "[airport_vrps]") {
    DeVrpGuard g;
    REQUIRE(airport_vrps::get_pattern_direction("XXXX", "25").empty());
}

TEST_CASE("airport_vrps: stop() clears the database",
          "[airport_vrps]") {
    DeVrpGuard g;
    REQUIRE(airport_vrps::get("EDNY") != nullptr);
    airport_vrps::stop();
    REQUIRE(airport_vrps::get("EDNY") == nullptr);
}

// ── User-override mechanism ─────────────────────────────────────────

TEST_CASE("airport_vrps: user override adds new airport (EDDM gets VRPs)",
          "[airport_vrps][override]") {
    // Plugin default for EDDM has no VRPs (pending DFS AIP verification).
    // The fixture user-override supplies "Whiskey" — the override file
    // is loaded on top of the plugin defaults.
    DeVrpUserOverrideGuard g;
    const auto *ad = airport_vrps::get("EDDM");
    REQUIRE(ad != nullptr);
    REQUIRE(ad->vrps.size() == 1);
    REQUIRE(ad->vrps[0].name == "Whiskey");
    REQUIRE(ad->vrps[0].alt_ft == 3500);
}

TEST_CASE("airport_vrps: user override replaces existing entry per ICAO",
          "[airport_vrps][override]") {
    // EDNY's plugin default has 3 VRPs at alt_ft=2000. The fixture
    // override replaces the whole entry with a single Sierra at 2500.
    // Per-ICAO replacement (not field-level merge) — verifies that the
    // user file fully owns the airport it touches.
    DeVrpUserOverrideGuard g;
    const auto *ad = airport_vrps::get("EDNY");
    REQUIRE(ad != nullptr);
    REQUIRE(ad->vrps.size() == 1);
    REQUIRE(ad->vrps[0].name == "Sierra");
    REQUIRE(ad->vrps[0].alt_ft == 2500);
}

TEST_CASE("airport_vrps: airports outside the override file are untouched",
          "[airport_vrps][override]") {
    // EDKB is in the plugin default but not in the override fixture.
    // It must remain at its plugin-default value (3 VRPs).
    DeVrpUserOverrideGuard g;
    const auto *ad = airport_vrps::get("EDKB");
    REQUIRE(ad != nullptr);
    REQUIRE(ad->vrps.size() == 3);
}

TEST_CASE("airport_vrps: missing user override falls back silently",
          "[airport_vrps][override]") {
    // No XP_ATC_USER_PREFS_DIR set — loader must use plugin defaults
    // only, no errors, no spurious entries. EDNY ships with 4 VRPs
    // sourced from Navigraph Charts (AIP Germany VFR).
    DeVrpGuard g;
    const auto *ad = airport_vrps::get("EDNY");
    REQUIRE(ad != nullptr);
    REQUIRE(ad->vrps.size() == 4);
}
