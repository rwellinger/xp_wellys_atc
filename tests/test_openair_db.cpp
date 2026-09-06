// Catch2 unit tests for the OpenAir 3-D airspace DB (openair_db).
// Pins the parser (DP polygons + DC circles + AN name-based class upgrade)
// and the 3-D containment queries (find_enclosing / find_all_enclosing)
// against a hand-built fixture whose geometry is reasoned about exactly in
// tests/fixtures/openair_sample.txt. This is the geometric foundation the
// RMZ_* intents will hang off of (issue #29).

#include "data/openair_db.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace {

// RAII guard: loads the fixture on init (init parses on a worker thread, so
// spin until ready()) and tears the DB down on scope exit. openair_db is not
// part of the module-reset listener, so every test owns its own load/stop.
struct FixtureGuard {
    FixtureGuard() {
        const std::string path =
            std::string(XP_WELLYS_ATC_TEST_FIXTURES_DIR) + "/openair_sample.txt";
        openair_db::init(path, "");
        while (!openair_db::ready())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ~FixtureGuard() { openair_db::stop(); }
};

} // namespace

TEST_CASE("openair_db indexes only awareness-relevant classes",
          "[openair_db]") {
    FixtureGuard guard;
    // box CTR, circle RMZ, TMA, small CTR, letter-upgraded CTR = 5.
    // The AC Q danger area is parsed but not indexed.
    REQUIRE(openair_db::entry_count() == 5);
}

TEST_CASE("openair_db find_enclosing — DP polygon CTR", "[openair_db]") {
    FixtureGuard guard;
    // Inside the box (52:18:30 N 010:33:00 E) below the 2500 ft ceiling.
    auto e = openair_db::find_enclosing(52.308333, 10.55, 1000);
    REQUIRE(e.ac_class == openair_db::AirspaceClass::CTR);
    REQUIRE(e.name == "TEST CTR");
    REQUIRE(e.floor_ft == 0);
    REQUIRE(e.ceiling_ft == 2500);

    // North of the box — outside all zones.
    auto out = openair_db::find_enclosing(52.50, 10.55, 1000);
    REQUIRE(out.name.empty());
    REQUIRE(out.ac_class == openair_db::AirspaceClass::OTHER);
}

TEST_CASE("openair_db find_enclosing — DC circle RMZ + altitude band",
          "[openair_db]") {
    FixtureGuard guard;
    // Centre of the 5 NM RMZ, inside the GND-4500 band.
    auto in = openair_db::find_enclosing(50.0, 10.0, 1000);
    REQUIRE(in.ac_class == openair_db::AirspaceClass::RMZ);
    REQUIRE(in.name == "TEST RMZ");

    // Same spot but above the 4500 ft ceiling — outside.
    auto above = openair_db::find_enclosing(50.0, 10.0, 5000);
    REQUIRE(above.name.empty());

    // ~10 NM north of centre — outside the 5 NM circle.
    auto far = openair_db::find_enclosing(50.166667, 10.0, 1000);
    REQUIRE(far.name.empty());
}

TEST_CASE("openair_db find_enclosing picks the innermost nested zone",
          "[openair_db]") {
    FixtureGuard guard;
    // Centre of both MUC TMA (r30) and MUC CTR (r5) at 2000 ft: both contain
    // the point, innermost (smaller bbox) wins.
    auto inner = openair_db::find_enclosing(48.0, 11.0, 2000);
    REQUIRE(inner.ac_class == openair_db::AirspaceClass::CTR);
    REQUIRE(inner.name == "MUC CTR");

    // Above the CTR ceiling (5000) but inside the TMA band → TMA.
    auto outer = openair_db::find_enclosing(48.0, 11.0, 8000);
    REQUIRE(outer.ac_class == openair_db::AirspaceClass::TMA);
    REQUIRE(outer.name == "MUC TMA");

    // 20 NM north: inside the TMA (r30) only.
    auto tma_only = openair_db::find_enclosing(48.333333, 11.0, 2000);
    REQUIRE(tma_only.ac_class == openair_db::AirspaceClass::TMA);
    REQUIRE(tma_only.name == "MUC TMA");
}

TEST_CASE("openair_db find_all_enclosing returns every containing zone",
          "[openair_db]") {
    FixtureGuard guard;
    auto all = openair_db::find_all_enclosing(48.0, 11.0, 2000);
    REQUIRE(all.size() == 2);
    bool has_ctr = false, has_tma = false;
    for (const auto &e : all) {
        if (e.ac_class == openair_db::AirspaceClass::CTR)
            has_ctr = true;
        if (e.ac_class == openair_db::AirspaceClass::TMA)
            has_tma = true;
    }
    REQUIRE(has_ctr);
    REQUIRE(has_tma);
}

TEST_CASE("openair_db upgrades letter-class AC via the AN name",
          "[openair_db]") {
    FixtureGuard guard;
    // "AC D" with name "LETTER CTR" must classify as CTR.
    auto e = openair_db::find_enclosing(47.083333, 8.083333, 1000);
    REQUIRE(e.ac_class == openair_db::AirspaceClass::CTR);
    REQUIRE(e.name == "LETTER CTR");
}

TEST_CASE("openair_db skips danger/prohibited areas", "[openair_db]") {
    FixtureGuard guard;
    // Inside the AC Q danger circle — must not match (not indexed).
    auto e = openair_db::find_enclosing(45.0, 9.0, 1000);
    REQUIRE(e.name.empty());
    REQUIRE(e.ac_class == openair_db::AirspaceClass::OTHER);
}

TEST_CASE("openair_db class_name is stable", "[openair_db]") {
    REQUIRE(std::string(openair_db::class_name(
                openair_db::AirspaceClass::RMZ)) == "RMZ");
    REQUIRE(std::string(openair_db::class_name(
                openair_db::AirspaceClass::ED_R)) == "ED-R");
    REQUIRE(std::string(openair_db::class_name(
                openair_db::AirspaceClass::OTHER)) == "OTHER");
}
