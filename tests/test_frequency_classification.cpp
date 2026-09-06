#include "core/xplane_context.hpp"

#include <catch2/catch_amalgamated.hpp>

// apt.dat encodes German AFIS/Info and Radio facilities under the Tower row
// code (1054); the only discriminator is the frequency name. classify_by_name
// refines a TOWER base into INFO/RADIO by the name suffix, leaving real Tower
// (and every other base type) untouched. Names below are verbatim from the
// X-Plane 12 Global Airports apt.dat.
using xplane_context::classify_by_name;
using FT = xplane_context::FrequencyType;

TEST_CASE("classify_by_name: Info suffix -> INFO", "[frequency][classify]") {
    REQUIRE(classify_by_name(FT::TOWER, "Schwaebisch Hall Information") ==
            FT::INFO);
    REQUIRE(classify_by_name(FT::TOWER, "Pattonville Info") == FT::INFO);
    REQUIRE(classify_by_name(FT::TOWER, "Straubing Information") == FT::INFO);
}

TEST_CASE("classify_by_name: Radio suffix -> RADIO", "[frequency][classify]") {
    REQUIRE(classify_by_name(FT::TOWER, "Schweinfurt Radio") == FT::RADIO);
    REQUIRE(classify_by_name(FT::TOWER, "Bad Hersfeld Radio") == FT::RADIO);
}

TEST_CASE("classify_by_name: Tower / unknown / empty stay TOWER",
          "[frequency][classify]") {
    REQUIRE(classify_by_name(FT::TOWER, "Friedrichshafen Tower") == FT::TOWER);
    REQUIRE(classify_by_name(FT::TOWER, "Stuttgart Tower") == FT::TOWER);
    REQUIRE(classify_by_name(FT::TOWER, "Some Glider Strip") == FT::TOWER);
    REQUIRE(classify_by_name(FT::TOWER, "") == FT::TOWER);
    REQUIRE(classify_by_name(FT::TOWER, "   ") == FT::TOWER);
}

TEST_CASE("classify_by_name: case-insensitive matching",
          "[frequency][classify]") {
    REQUIRE(classify_by_name(FT::TOWER, "Egelsbach INFO") == FT::INFO);
    REQUIRE(classify_by_name(FT::TOWER, "Egelsbach info") == FT::INFO);
    REQUIRE(classify_by_name(FT::TOWER, "Somewhere RADIO") == FT::RADIO);
}

TEST_CASE("classify_by_name: non-TOWER base types pass through unchanged",
          "[frequency][classify]") {
    // Only the Tower code is ambiguous; UNICOM/ATIS/GROUND named "... Radio"
    // or "... Information" must not be reclassified.
    REQUIRE(classify_by_name(FT::UNICOM, "Altena Info") == FT::UNICOM);
    REQUIRE(classify_by_name(FT::ATIS, "Straubing ATIS") == FT::ATIS);
    REQUIRE(classify_by_name(FT::GROUND, "Stuttgart Ground") == FT::GROUND);
}
