#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/engine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/intent_parser.hpp"
#include "atc/phraseology_hints.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <string>

// A runtime language switch must not leave the loaded tables on the old
// language: that produced mixed radio calls such as "good day, Sagen Sie Ihre
// Position" until the next plugin start.

namespace {

struct RestoreGerman {
  ~RestoreGerman() {
    settings::set_atc_profile("DE");
    engine::reload_language_profile();
  }
};

std::string say_again() {
  return atc_templates::lookup_fallback("say_again", "<missing>");
}

} // namespace

TEST_CASE("language switch: every loaded table follows", "[language]") {
  RestoreGerman restore;
  settings::set_atc_profile("DE");
  engine::reload_language_profile();
  REQUIRE(say_again() == "wiederholen Sie.");
  const std::string de_rejection = flight_phase::check_precondition(
      "REQUEST_TAXI", flight_phase::FlightPhase::CRUISE);
  REQUIRE(de_rejection.find("negativ") != std::string::npos);

  settings::set_atc_profile("EN");
  engine::reload_language_profile();
  REQUIRE(say_again() == "say again.");
  REQUIRE(flight_phase::check_precondition(
              "REQUEST_TAXI", flight_phase::FlightPhase::CRUISE) !=
          de_rejection);

  xplane_context::XPlaneContext ctx;
  ctx.frequency_type = xplane_context::FrequencyType::GROUND;
  ctx.facility_type = xplane_context::FacilityType::TOWERED;
  auto msg = intent_parser::parse("Grenchen Ground request taxi", ctx);
  REQUIRE(msg.intent == intent_parser::PilotIntent::REQUEST_TAXI);

  settings::set_atc_profile("DE");
  engine::reload_language_profile();
  REQUIRE(say_again() == "wiederholen Sie.");
  msg = intent_parser::parse("Grenchen Rollkontrolle erbitte Rollen", ctx);
  REQUIRE(msg.intent == intent_parser::PilotIntent::REQUEST_TAXI);
}

TEST_CASE("EN: a tower call is no cross-country marker", "[language][en]") {
  RestoreGerman restore;
  settings::set_atc_profile("EN");
  engine::reload_language_profile();
  xplane_context::XPlaneContext ctx;
  ctx.frequency_type = xplane_context::FrequencyType::TOWER;
  ctx.facility_type = xplane_context::FacilityType::TOWERED;

  auto msg = intent_parser::parse(
      "Grenchen Tower, Delta Romeo Uniform, short runway 06, ready for "
      "departure.",
      ctx);
  REQUIRE(msg.intent == intent_parser::PilotIntent::READY_FOR_DEPARTURE);

  msg = intent_parser::parse(
      "Grenchen Tower, Delta Romeo Uniform, ready for departure to the east",
      ctx);
  REQUIRE(msg.intent == intent_parser::PilotIntent::READY_FOR_DEPARTURE_VFR);

  msg = intent_parser::parse(
      "Grenchen Tower, Delta Romeo Uniform, ready for departure to Bern", ctx);
  REQUIRE(msg.intent == intent_parser::PilotIntent::READY_FOR_DEPARTURE_VFR);
}

TEST_CASE("EN: change frequency signs off", "[language][en]") {
  RestoreGerman restore;
  settings::set_atc_profile("EN");
  engine::reload_language_profile();
  xplane_context::XPlaneContext ctx;
  ctx.on_ground = false;
  ctx.frequency_type = xplane_context::FrequencyType::TOWER;
  ctx.facility_type = xplane_context::FacilityType::TOWERED;
  using PI = intent_parser::PilotIntent;

  auto msg = intent_parser::parse(
      "Grenchen Tower, Delta Hotel Delta Romeo Uniform, change frequency, "
      "good day.",
      ctx);
  REQUIRE(msg.intent == PI::LEAVING_FREQUENCY);
  REQUIRE(msg.confidence >= 0.7f);

  msg = intent_parser::parse("Delta Romeo Uniform, request frequency change",
                             ctx);
  REQUIRE(msg.intent != PI::LEAVING_FREQUENCY);
}

TEST_CASE("hints: cross-country departure offers sign-off in the pattern phase",
          "[language][phraseology_hints]") {
  RestoreGerman restore;
  const std::string profile = GENERATE("DE", "EN");
  settings::set_atc_profile(profile);
  engine::reload_language_profile();

  settings::set_vfr_flight_type("cross_country");
  xplane_context::XPlaneContext ctx;
  ctx.nearest_airport_id = "LSZG";
  ctx.facility_type = xplane_context::FacilityType::TOWERED;
  ctx.frequency_type = xplane_context::FrequencyType::TOWER;
  ctx.active_runway = "06";
  atc_state_machine::set_state(atc_state_machine::ATCState::TOWER_CONTACT);

  intent_parser::PilotMessage msg;
  msg.intent = intent_parser::PilotIntent::READY_FOR_DEPARTURE_VFR;
  msg.confidence = 0.92f;
  msg.callsign = "Delta Romeo Uniform";
  msg.runway = "06";
  atc_state_machine::process(msg, ctx, 100.0);
  REQUIRE(atc_state_machine::state_name(atc_state_machine::get_state()) ==
          std::string("XC/DEPARTURE_CLEARED"));

  phraseology_hints::HintQuery q{};
  q.state = atc_state_machine::get_state();
  q.phase = flight_phase::FlightPhase::PATTERN;
  q.facility = xplane_context::FacilityType::TOWERED;
  q.frequency_type = xplane_context::FrequencyType::TOWER;
  const auto hints = phraseology_hints::lookup(q);
  REQUIRE(std::find(hints.begin(), hints.end(), "LEAVING_FREQUENCY") !=
          hints.end());
}
