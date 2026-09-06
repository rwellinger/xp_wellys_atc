// Catch2 unit tests for the phraseology-hints matrix lookup.
//
// Drives the JSON-backed matrix in data/atc_profiles/<region>/
// phraseology_hints.json. Each TEST_CASE represents one row of the
// approved State x Phase x Facility x Frequency matrix and pins the
// expected hint set so future JSON edits surface as test failures.

#include "atc/flight_phase.hpp"
#include "atc/phraseology_hints.hpp"
#include "core/xplane_context.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>

using atc_state_machine::ATCState;
using flight_phase::FlightPhase;
using xplane_context::FacilityType;
using xplane_context::FrequencyType;

namespace {

// settings::atc_profile_data_dir() in the test stub resolves XP_ATC_DATA_DIR
// or falls back to ./data — both point at the live region JSON files,
// so ::init() loads the same matrix the plugin uses.
struct LoadGuard {
  // Load BOTH stages of the live hint pipeline: the matrix (phraseology_hints)
  // and the intent_frequency allowlist (flight_phase) the UI's defense filter
  // consults. Tests that only loaded the matrix masked the AFIS regression
  // below — the matrix was correct, the allowlist was not.
  LoadGuard() {
    phraseology_hints::init();
    flight_phase::init();
  }
  ~LoadGuard() {
    flight_phase::stop();
    phraseology_hints::stop();
  }
};

bool contains(const std::vector<std::string> &v, const std::string &needle) {
  return std::find(v.begin(), v.end(), needle) != v.end();
}

// Replicates the UI's two-stage pipeline (src/ui/atc_ui.cpp): matrix lookup,
// then the defense-in-depth frequency filter. A hint only reaches the panel
// if it survives BOTH. Mirrors the tower_only carve-out so towered cases are
// faithful too.
std::vector<std::string> pipeline(const phraseology_hints::HintQuery &q) {
  std::vector<std::string> v = phraseology_hints::lookup(q);
  v.erase(std::remove_if(v.begin(), v.end(),
                         [&](const std::string &key) {
                           if (flight_phase::is_intent_valid_for_frequency(
                                   key, q.frequency_type))
                             return false;
                           return !(q.tower_only &&
                                    q.frequency_type ==
                                        FrequencyType::TOWER);
                         }),
          v.end());
  return v;
}

phraseology_hints::HintQuery make_query(ATCState state, FlightPhase phase,
                                        FacilityType facility, FrequencyType freq,
                                        bool tower_only = false,
                                        bool post_landing = false) {
  phraseology_hints::HintQuery q{};
  q.state = state;
  q.phase = phase;
  q.facility = facility;
  q.frequency_type = freq;
  q.tower_only = tower_only;
  q.post_landing = post_landing;
  return q;
}

} // namespace

TEST_CASE("phraseology_hints: cold start IDLE+PARKED on Ground",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*facility=*/FacilityType::TOWERED,
                      FrequencyType::GROUND);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "INITIAL_CALL_GROUND"));
  REQUIRE(contains(hints, "RADIO_CHECK"));
  REQUIRE_FALSE(contains(hints, "READY_FOR_DEPARTURE"));
  REQUIRE_FALSE(contains(hints, "REQUEST_TAXI_PARKING"));
}

TEST_CASE("phraseology_hints: tower-only IDLE+PARKED on Tower",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*facility=*/FacilityType::TOWERED,
                      FrequencyType::TOWER, /*tower_only=*/true);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "INITIAL_CALL_GROUND"));
  REQUIRE_FALSE(contains(hints, "READY_FOR_DEPARTURE"));
}

TEST_CASE("phraseology_hints: 2-tier IDLE+PARKED on Tower yields no hints",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*facility=*/FacilityType::TOWERED,
                      FrequencyType::TOWER, /*tower_only=*/false);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(hints.empty());
}

TEST_CASE("phraseology_hints: post-landing on Ground -> taxi to parking",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::TAXI, /*facility=*/FacilityType::TOWERED,
                      FrequencyType::GROUND, /*tower_only=*/false,
                      /*post_landing=*/true);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "REQUEST_TAXI_PARKING"));
  REQUIRE_FALSE(contains(hints, "INITIAL_CALL_GROUND"));
  REQUIRE_FALSE(contains(hints, "REQUEST_TAXI"));
}

TEST_CASE("phraseology_hints: GROUND_CONTACT awaiting taxi clearance",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::GROUND_CONTACT, FlightPhase::PARKED,
                      /*facility=*/FacilityType::TOWERED, FrequencyType::GROUND);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "REQUEST_TAXI"));
}

TEST_CASE("phraseology_hints: TOWER_CONTACT at the holding point",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::TOWER_CONTACT, FlightPhase::TAXI,
                      /*facility=*/FacilityType::TOWERED, FrequencyType::TOWER);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "READY_FOR_DEPARTURE"));
  REQUIRE(contains(hints, "READY_FOR_DEPARTURE_VFR"));
}

TEST_CASE("phraseology_hints: PATTERN_ENTRY in the pattern",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::PATTERN_ENTRY, FlightPhase::PATTERN,
                      /*facility=*/FacilityType::TOWERED, FrequencyType::TOWER);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "REPORT_POSITION_DOWNWIND"));
  REQUIRE(contains(hints, "REQUEST_LANDING"));
  REQUIRE(contains(hints, "GO_AROUND"));
}

TEST_CASE("phraseology_hints: tower-only post-landing offers RUNWAY_VACATED + REQUEST_TAXI_PARKING",
          "[phraseology_hints]") {
  LoadGuard g;
  // LSZB-style: tower_only field, pilot just touched down and is
  // taxiing clear on the Tower freq (no Ground exists). The same
  // controller handles taxi-in, so REQUEST_TAXI_PARKING must stay
  // available alongside RUNWAY_VACATED.
  auto q = make_query(ATCState::IDLE, FlightPhase::TAXI, /*facility=*/FacilityType::TOWERED,
                      FrequencyType::TOWER, /*tower_only=*/true,
                      /*post_landing=*/true);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "RUNWAY_VACATED"));
  REQUIRE(contains(hints, "REQUEST_TAXI_PARKING"));
}

TEST_CASE("phraseology_hints: 2-tier post-landing on Tower only offers RUNWAY_VACATED",
          "[phraseology_hints]") {
  LoadGuard g;
  // LSZG-style: 2-tier field, pilot still on Tower freq after touchdown.
  // Pilot must vacate, then retune to Ground for taxi-in — taxi
  // hints belong to the Ground rule, not here.
  auto q = make_query(ATCState::IDLE, FlightPhase::TAXI, /*facility=*/FacilityType::TOWERED,
                      FrequencyType::TOWER, /*tower_only=*/false,
                      /*post_landing=*/true);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "RUNWAY_VACATED"));
  REQUIRE_FALSE(contains(hints, "REQUEST_TAXI_PARKING"));
}

TEST_CASE("phraseology_hints: LANDING_CLEARED rolling out -> RUNWAY_VACATED",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::LANDING_CLEARED, FlightPhase::LANDING_ROLL,
                      /*facility=*/FacilityType::TOWERED, FrequencyType::TOWER);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "RUNWAY_VACATED"));
}

TEST_CASE("phraseology_hints: uncontrolled IDLE on CTAF -> self announce",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*facility=*/FacilityType::UNCONTROLLED,
                      FrequencyType::CTAF);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "SELF_ANNOUNCE"));
  REQUIRE_FALSE(contains(hints, "INITIAL_CALL_GROUND"));
}

TEST_CASE("phraseology_hints: AFIS/Info on the ground -> first call (not self-announce)",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*facility=*/FacilityType::AFIS,
                      FrequencyType::INFO);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "INITIAL_CALL_GROUND"));
  // AFIS must NOT offer the uncontrolled self-announce — distinct from CTAF.
  REQUIRE_FALSE(contains(hints, "SELF_ANNOUNCE"));
}

TEST_CASE("phraseology_hints: AFIS/Radio inbound -> inbound + positions",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PATTERN, /*facility=*/FacilityType::AFIS,
                      FrequencyType::RADIO);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(contains(hints, "INITIAL_CALL_INBOUND"));
  REQUIRE(contains(hints, "LEAVING_FREQUENCY"));
}

// Issue #14: the cross-country inbound to a towered field reaches IDLE while
// still at CRUISE phase. The Tower IDLE-airborne rule must surface the inbound
// first call there, symmetric to the AFIS rule which already covered CRUISE.
TEST_CASE("phraseology_hints: Tower IDLE cruise inbound -> INITIAL_CALL_INBOUND",
          "[phraseology_hints][inbound]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::CRUISE,
                      /*facility=*/FacilityType::TOWERED, FrequencyType::TOWER);
  auto hints = pipeline(q);
  REQUIRE_FALSE(hints.empty());
  REQUIRE(contains(hints, "INITIAL_CALL_INBOUND"));
  REQUIRE(contains(hints, "INITIAL_CALL_INBOUND_VRP"));
}

// ── Two-stage pipeline regression (AFIS/Info hints reaching the panel) ──────
// The matrix surfaced the AFIS intents (covered above), but the UI's
// defense-in-depth frequency filter dropped every one of them because the
// intent_frequency allowlist in flight_rules.json listed no INFO/RADIO entry.
// Result: empty hint panel on the Info frequency. These pin the FULL pipeline.

TEST_CASE("phraseology_hints: is_intent_valid_for_frequency accepts INFO/RADIO",
          "[phraseology_hints][afis]") {
  LoadGuard g;
  using flight_phase::is_intent_valid_for_frequency;
  for (FrequencyType freq : {FrequencyType::INFO, FrequencyType::RADIO}) {
    REQUIRE(is_intent_valid_for_frequency("INITIAL_CALL_GROUND", freq));
    REQUIRE(is_intent_valid_for_frequency("RADIO_CHECK", freq));
    REQUIRE(is_intent_valid_for_frequency("INITIAL_CALL_INBOUND", freq));
    REQUIRE(is_intent_valid_for_frequency("REPORT_POSITION_DOWNWIND", freq));
    REQUIRE(is_intent_valid_for_frequency("REPORT_POSITION_BASE", freq));
    REQUIRE(is_intent_valid_for_frequency("REPORT_POSITION_FINAL", freq));
    REQUIRE(is_intent_valid_for_frequency("LEAVING_FREQUENCY", freq));
  }
}

TEST_CASE("phraseology_hints: AFIS/Info ground pipeline is non-empty (Sch. Hall)",
          "[phraseology_hints][afis]") {
  LoadGuard g;
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED,
                      /*facility=*/FacilityType::AFIS, FrequencyType::INFO);
  auto hints = pipeline(q);
  REQUIRE_FALSE(hints.empty());
  REQUIRE(contains(hints, "INITIAL_CALL_GROUND"));
  REQUIRE(contains(hints, "RADIO_CHECK"));
}

TEST_CASE("phraseology_hints: AFIS airborne pipeline keeps position reports",
          "[phraseology_hints][afis]") {
  LoadGuard g;
  for (FrequencyType freq : {FrequencyType::INFO, FrequencyType::RADIO}) {
    auto q = make_query(ATCState::IDLE, FlightPhase::PATTERN,
                        /*facility=*/FacilityType::AFIS, freq);
    auto hints = pipeline(q);
    REQUIRE_FALSE(hints.empty());
    REQUIRE(contains(hints, "INITIAL_CALL_INBOUND"));
    REQUIRE(contains(hints, "REPORT_POSITION_DOWNWIND"));
    REQUIRE(contains(hints, "LEAVING_FREQUENCY"));
  }
}

TEST_CASE("phraseology_hints: EN_ROUTE without relevant freq is empty",
          "[phraseology_hints]") {
  LoadGuard g;
  auto q = make_query(ATCState::EN_ROUTE, FlightPhase::CRUISE,
                      /*facility=*/FacilityType::TOWERED, FrequencyType::UNKNOWN);
  auto hints = phraseology_hints::lookup(q);
  REQUIRE(hints.empty());
}

TEST_CASE("phraseology_hints: UNKNOWN facility yields empty panel (safe default)",
          "[phraseology_hints]") {
  LoadGuard g;
  // Cache-not-ready / no airport resolved: facility=UNKNOWN must match no rule
  // so the panel is empty (visible + debuggable) rather than silently wrong.
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED,
                      /*facility=*/FacilityType::UNKNOWN, FrequencyType::GROUND);
  REQUIRE(phraseology_hints::lookup(q).empty());
}

TEST_CASE("phraseology_hints: lookup before init returns empty",
          "[phraseology_hints]") {
  // Explicitly stop in case a previous test left rules loaded.
  phraseology_hints::stop();
  auto q = make_query(ATCState::IDLE, FlightPhase::PARKED, /*facility=*/FacilityType::TOWERED,
                      FrequencyType::GROUND);
  REQUIRE(phraseology_hints::lookup(q).empty());
}
