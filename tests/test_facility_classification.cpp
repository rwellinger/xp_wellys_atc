#include "core/xplane_context.hpp"

#include <catch2/catch_amalgamated.hpp>

// classify_facility derives the operational FacilityType from an airport's
// apt.dat frequency table. It is the single source of truth that replaced the
// former bool is_towered_airport: TOWER -> TOWERED, INFO/RADIO -> AFIS,
// UNICOM/CTAF -> UNCONTROLLED, and nothing usable -> UNKNOWN (the safe default
// that yields an empty hints panel rather than a wrong guess).
using xplane_context::AirportFrequencies;
using xplane_context::classify_facility;
using xplane_context::FacilityType;
using xplane_context::has_contactable_atc;
using FT = xplane_context::FrequencyType;

namespace {
AirportFrequencies with(std::initializer_list<FT> types) {
  AirportFrequencies f;
  uint32_t khz = 118000;
  for (FT t : types)
    f.all.push_back({khz += 100, t});
  return f;
}
} // namespace

TEST_CASE("classify_facility: TOWER -> TOWERED", "[facility]") {
  REQUIRE(classify_facility(with({FT::TOWER})) == FacilityType::TOWERED);
  // Tower wins even when a Ground / ATIS / UNICOM also exist.
  REQUIRE(classify_facility(with({FT::ATIS, FT::GROUND, FT::TOWER})) ==
          FacilityType::TOWERED);
}

TEST_CASE("classify_facility: INFO/RADIO -> AFIS (e.g. EDTY)", "[facility]") {
  // EDTY Schwaebisch Hall: apt.dat 1054 named "... Information" -> INFO.
  REQUIRE(classify_facility(with({FT::INFO})) == FacilityType::AFIS);
  REQUIRE(classify_facility(with({FT::RADIO})) == FacilityType::AFIS);
  // AFIS with an ATIS alongside is still AFIS (no Tower present).
  REQUIRE(classify_facility(with({FT::ATIS, FT::INFO})) == FacilityType::AFIS);
}

TEST_CASE("classify_facility: UNICOM/CTAF -> UNCONTROLLED", "[facility]") {
  REQUIRE(classify_facility(with({FT::UNICOM})) == FacilityType::UNCONTROLLED);
  REQUIRE(classify_facility(with({FT::CTAF})) == FacilityType::UNCONTROLLED);
}

TEST_CASE("classify_facility: no usable contact freq -> UNKNOWN", "[facility]") {
  REQUIRE(classify_facility(AirportFrequencies{}) == FacilityType::UNKNOWN);
  // ATIS/DELIVERY alone is not a contact facility -> still UNKNOWN.
  REQUIRE(classify_facility(with({FT::ATIS, FT::DELIVERY})) ==
          FacilityType::UNKNOWN);
}

TEST_CASE("has_contactable_atc: only contactable freqs count (issue #60)",
          "[facility]") {
  // Radio-less field (glider strip like Olten) -> not contactable.
  REQUIRE_FALSE(has_contactable_atc(AirportFrequencies{}));
  // ATIS-only / Delivery-only -> nothing to talk to -> not contactable.
  REQUIRE_FALSE(has_contactable_atc(with({FT::ATIS})));
  REQUIRE_FALSE(has_contactable_atc(with({FT::DELIVERY})));
  REQUIRE_FALSE(has_contactable_atc(with({FT::ATIS, FT::DELIVERY})));
  // Every facility class with a real contact freq -> contactable.
  REQUIRE(has_contactable_atc(with({FT::TOWER})));
  REQUIRE(has_contactable_atc(with({FT::INFO})));
  REQUIRE(has_contactable_atc(with({FT::RADIO})));
  REQUIRE(has_contactable_atc(with({FT::UNICOM})));
  REQUIRE(has_contactable_atc(with({FT::CTAF})));
  // A contact freq alongside ATIS is still contactable.
  REQUIRE(has_contactable_atc(with({FT::ATIS, FT::TOWER})));
}

TEST_CASE("XPlaneContext::is_towered() derives only from TOWERED", "[facility]") {
  xplane_context::XPlaneContext ctx;
  ctx.facility_type = FacilityType::TOWERED;
  REQUIRE(ctx.is_towered());
  for (FacilityType f : {FacilityType::UNKNOWN, FacilityType::UNCONTROLLED,
                         FacilityType::AFIS}) {
    ctx.facility_type = f;
    REQUIRE_FALSE(ctx.is_towered());
  }
}
