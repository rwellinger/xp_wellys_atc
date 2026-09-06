#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/atis_generator.hpp"
#include "atc/flight_phase.hpp"
#include "atc/flows/ground_operations.hpp"
#include "atc/initial_call_conformance.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

// EN (#38) — H2 invariant for the ICAO-VFR profile bundle.
//
// The English profile bundle (data/atc_profiles/en/) mirrors the DE bundle
// with ICAO-VFR content. The H2 invariant must hold here too: the rendered
// INITIAL_CALL_GROUND hint (flight_rules.json :: pilot_phraseology) is
// teaching authority, so a student who reads it back verbatim must NEVER be
// re-requested by the conformance check. This asserts set-equality between
// the hint's rendered elements and the EN recommended set, exactly as the
// DE test does. It breaks if the EN hint wording and the EN detectors /
// element_keywords drift apart — e.g. if the hint loses its position token
// or its {intention} word ("VFR pattern") stops matching en/conformance.json
// element_keywords.intention.

namespace {

using intent_parser::PilotIntent;
using intent_parser::PilotMessage;

void load_en_profile() {
  settings::set_atc_profile("EN");
  settings::set_pilot_callsign_raw("Papa Hotel Alfa Bravo Charlie");
  settings::set_vfr_flight_type("pattern");
  settings::set_vfr_destination("");
  atc_templates::init();
  initial_call_conformance::init();
  flight_phase::reload();
  atc_state_machine::init();
}

xplane_context::XPlaneContext ground_ctx() {
  xplane_context::XPlaneContext ctx{};
  ctx.facility_type = xplane_context::FacilityType::TOWERED;
  ctx.on_ground = true;
  ctx.frequency_type = xplane_context::FrequencyType::GROUND;
  ctx.nearest_airport_id = "EDNY";
  ctx.aircraft_icao = "DV20";
  return ctx;
}

PilotMessage first_call(const std::string &transcript) {
  PilotMessage m;
  m.intent = PilotIntent::INITIAL_CALL_GROUND;
  m.confidence = 0.85f;
  m.callsign = "Papa Hotel Alfa Bravo Charlie";
  m.raw_transcript = transcript;
  return m;
}

} // namespace

TEST_CASE("EN H2: INITIAL_CALL_GROUND hint satisfies every recommended element",
          "[conformance][hint][h2][en]") {
  load_en_profile();
  atis_generator::init();  // deterministic letter 'A' -> "Alpha"
  auto ctx = ground_ctx(); // aircraft_icao == "DV20"

  auto vars = ground_ops::build_vars(first_call(""), ctx);
  std::string tmpl = flight_phase::get_pilot_phraseology("INITIAL_CALL_GROUND");
  REQUIRE_FALSE(tmpl.empty());
  std::string rendered = atc_templates::fill(tmpl, vars);

  // The first-contact hint must NOT teach the taxi request — that belongs
  // to REQUEST_TAXI, not the initial call (ICAO: a separate speech act).
  REQUIRE(rendered.find("request taxi") == std::string::npos);

  auto parsed = intent_parser::parse(rendered, ctx);
  auto r =
      initial_call_conformance::evaluate("INITIAL_CALL_GROUND", parsed, ctx);
  INFO("rendered EN hint: " << rendered);
  REQUIRE(r.missing_recommended.empty());
  REQUIRE(r.missing_required.empty());
}

TEST_CASE("EN vfr intention: pattern renders 'VFR pattern', H2 holds",
          "[conformance][intention][h2][en]") {
  load_en_profile();
  atis_generator::init();
  settings::set_vfr_flight_type("pattern");
  auto ctx = ground_ctx();

  auto vars = ground_ops::build_vars(first_call(""), ctx);
  REQUIRE(vars["intention"] == "VFR pattern");

  std::string rendered = atc_templates::fill(
      flight_phase::get_pilot_phraseology("INITIAL_CALL_GROUND"), vars);
  REQUIRE(rendered.find("VFR pattern") != std::string::npos);
  auto parsed = intent_parser::parse(rendered, ctx);
  auto r =
      initial_call_conformance::evaluate("INITIAL_CALL_GROUND", parsed, ctx);
  INFO("rendered EN hint: " << rendered);
  REQUIRE(r.missing_recommended.empty());
  REQUIRE(r.missing_required.empty());
}

TEST_CASE("EN vfr intention: cross-country with destination renders 'VFR to X'",
          "[conformance][intention][en]") {
  load_en_profile();
  settings::set_vfr_flight_type("cross_country");
  settings::set_vfr_destination("EDDS");
  auto ctx = ground_ctx();

  // The ICAO field is expanded phonetically so TTS speaks it letter by
  // letter ("EDDS" -> "Echo Delta Delta Sierra").
  auto vars = ground_ops::build_vars(first_call(""), ctx);
  REQUIRE(vars["intention"] == "VFR to Echo Delta Delta Sierra");
  REQUIRE(vars["vfr_course_phrase"] == ", course to Echo Delta Delta Sierra");

  // REQUEST_TAXI carries the intention; READY_FOR_DEPARTURE_VFR carries the
  // course tail.
  std::string taxi = atc_templates::fill(
      flight_phase::get_pilot_phraseology("REQUEST_TAXI"), vars);
  REQUIRE(taxi.find("VFR to Echo Delta Delta Sierra") != std::string::npos);
  std::string dep = atc_templates::fill(
      flight_phase::get_pilot_phraseology("READY_FOR_DEPARTURE_VFR"), vars);
  REQUIRE(dep.find("course to Echo Delta Delta Sierra") != std::string::npos);

  // H2 still holds — "vfr"/"to" are intention keywords.
  std::string rendered = atc_templates::fill(
      flight_phase::get_pilot_phraseology("INITIAL_CALL_GROUND"), vars);
  auto parsed = intent_parser::parse(rendered, ctx);
  auto r =
      initial_call_conformance::evaluate("INITIAL_CALL_GROUND", parsed, ctx);
  INFO("rendered EN hint: " << rendered);
  REQUIRE(r.missing_recommended.empty());
  REQUIRE(r.missing_required.empty());
}
