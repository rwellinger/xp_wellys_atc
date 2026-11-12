#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/engine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/flows/pattern_flow.hpp"
#include "atc/intent_parser.hpp"
#include "core/cross_country_log.hpp"
#include "core/xplane_context.hpp"
#include "data/traffic_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <filesystem>
#include <map>
#include <string>

// Issue #1 - helicopter tolerance: hover is a ground-class movement, helipad
// approaches are finals, and runway overlays stay out of the way.

namespace {

using flight_phase::FlightPhase;
using xplane_context::FrequencyType;
using xplane_context::XPlaneContext;

XPlaneContext hover_ctx() {
  XPlaneContext ctx;
  ctx.is_helicopter = true;
  ctx.on_ground = false;
  ctx.height_agl_ft = 5.0f;
  ctx.groundspeed_kts = 8.0f;
  ctx.latitude = 47.67;
  ctx.longitude = 9.51;
  ctx.airport_lat = 47.67;
  ctx.airport_lon = 9.51;
  ctx.nearest_airport_id = "EDNY";
  ctx.nearest_airport_name = "Friedrichshafen";
  ctx.facility_type = xplane_context::FacilityType::TOWERED;
  ctx.frequency_type = FrequencyType::GROUND;
  ctx.active_runway = "24";
  ctx.com_radio_powered = true;
  ctx.avionics_on = true;
  ctx.aircraft_icao = "R22";
  return ctx;
}

xplane_context::RunwayInfo runway_24() {
  xplane_context::RunwayInfo rwy;
  rwy.end1.number = "24";
  rwy.end1.heading_deg = 240.0f;
  rwy.end1.lat = 47.67;
  rwy.end1.lon = 9.51;
  rwy.end2.number = "06";
  rwy.end2.heading_deg = 60.0f;
  return rwy;
}

void load_profile(const std::string &profile) {
  settings::set_atc_profile(profile);
  flight_phase::init();
  atc_templates::init();
}

// The profile tables are module globals: a test that loads EN must put DE back
// itself, the settings reset does not touch them.
struct RestoreGerman {
  ~RestoreGerman() {
    load_profile("DE");
    intent_parser::init();
  }
};

void tick(XPlaneContext &ctx, int seconds) {
  for (int i = 0; i < seconds; ++i) {
    flight_phase::classify_ground(ctx);
    flight_phase::update(ctx, 1.0f);
  }
}

} // namespace

TEST_CASE("heli: resolve_is_helicopter precedence", "[helicopter]") {
  using xplane_context::resolve_is_helicopter;
  REQUIRE(resolve_is_helicopter("auto", true, "XXXX"));
  REQUIRE(resolve_is_helicopter("auto", false, "R22"));
  REQUIRE(resolve_is_helicopter("auto", false, "ec35"));
  REQUIRE_FALSE(resolve_is_helicopter("auto", false, "C172"));
  REQUIRE_FALSE(resolve_is_helicopter("auto", false, ""));
  REQUIRE(resolve_is_helicopter("helicopter", false, "C172"));
  REQUIRE_FALSE(resolve_is_helicopter("airplane", true, "R22"));
}

TEST_CASE("heli: hover and hover-taxi are ground-class TAXI", "[helicopter]") {
  load_profile("DE");
  auto ctx = hover_ctx();
  tick(ctx, 5);
  REQUIRE(ctx.on_ground);
  REQUIRE(ctx.hovering);
  REQUIRE(flight_phase::get() == FlightPhase::TAXI);

  ctx.groundspeed_kts = 0.0f; // stationary hover
  tick(ctx, 5);
  REQUIRE(flight_phase::get() == FlightPhase::TAXI);
}

TEST_CASE("heli: identical geometry stays airborne for fixed-wing",
          "[helicopter]") {
  load_profile("DE");
  auto ctx = hover_ctx();
  ctx.is_helicopter = false;
  tick(ctx, 5);
  REQUIRE_FALSE(ctx.on_ground);
  REQUIRE_FALSE(ctx.hovering);
  REQUIRE(flight_phase::get() == FlightPhase::PATTERN);
}

TEST_CASE("heli: classify_ground re-evaluates after leaving the hover band",
          "[helicopter]") {
  load_profile("DE");
  auto ctx = hover_ctx();
  flight_phase::classify_ground(ctx);
  REQUIRE(ctx.hovering);
  ctx.height_agl_ft = 200.0f;
  flight_phase::classify_ground(ctx);
  REQUIRE_FALSE(ctx.hovering);
  REQUIRE_FALSE(ctx.on_ground);
}

TEST_CASE("heli: touching the surface is not hovering", "[helicopter]") {
  load_profile("DE");
  auto ctx = hover_ctx();
  ctx.on_ground = true;
  ctx.height_agl_ft = 0.0f;
  ctx.groundspeed_kts = 0.0f;
  tick(ctx, 5);
  REQUIRE_FALSE(ctx.hovering);
  REQUIRE(flight_phase::get() == FlightPhase::PARKED);
}

TEST_CASE("heli: short excursion above the band does not go airborne",
          "[helicopter]") {
  load_profile("DE");
  auto ctx = hover_ctx();
  tick(ctx, 5);

  ctx.height_agl_ft = 70.0f;
  tick(ctx, 2);
  REQUIRE(flight_phase::get() == FlightPhase::TAXI);
  REQUIRE_FALSE(atc_state_machine::was_airborne());

  ctx.height_agl_ft = 5.0f;
  tick(ctx, 2);
  REQUIRE(flight_phase::get() == FlightPhase::TAXI);

  ctx.height_agl_ft = 300.0f; // real departure
  ctx.groundspeed_kts = 50.0f;
  tick(ctx, 5);
  REQUIRE(flight_phase::is_airborne(flight_phase::get()));
  REQUIRE(atc_state_machine::was_airborne());
}

TEST_CASE("heli: approach off the runway axis is a final", "[helicopter]") {
  load_profile("DE");
  auto ctx = hover_ctx();
  ctx.runways.push_back(runway_24());
  ctx.height_agl_ft = 300.0f;
  ctx.groundspeed_kts = 50.0f;
  ctx.vertical_speed_fpm = -400.0f;
  ctx.heading_true = 150.0f; // 90 degrees off the runway
  tick(ctx, 10);
  REQUIRE(flight_phase::get() == FlightPhase::FINAL_APPROACH);

  SECTION("fixed-wing with the same geometry stays in the pattern") {
    ctx.is_helicopter = false;
    tick(ctx, 10);
    REQUIRE(flight_phase::get() == FlightPhase::PATTERN);
  }
}

TEST_CASE("heli: final works at a heliport without runways", "[helicopter]") {
  load_profile("DE");
  auto ctx = hover_ctx();
  ctx.active_runway.clear();
  ctx.height_agl_ft = 300.0f;
  ctx.groundspeed_kts = 50.0f;
  ctx.vertical_speed_fpm = -400.0f;
  tick(ctx, 10);
  REQUIRE(flight_phase::get() == FlightPhase::FINAL_APPROACH);
}

TEST_CASE("heli: high descent near the field is not yet a final",
          "[helicopter]") {
  load_profile("DE");
  auto ctx = hover_ctx();
  ctx.height_agl_ft = 1500.0f;
  ctx.groundspeed_kts = 80.0f;
  ctx.vertical_speed_fpm = -400.0f;
  tick(ctx, 10);
  REQUIRE(flight_phase::get() == FlightPhase::PATTERN);
}

TEST_CASE("heli: ground intents pass the preconditions in hover",
          "[helicopter]") {
  RestoreGerman restore;
  const std::string profile = GENERATE("DE", "EN");
  load_profile(profile);
  auto ctx = hover_ctx();
  tick(ctx, 5);
  const auto phase = flight_phase::get();
  for (const char *intent : {"INITIAL_CALL_GROUND", "REQUEST_TAXI",
                             "READY_FOR_DEPARTURE", "READY_FOR_DEPARTURE_VFR"})
    REQUIRE(flight_phase::check_precondition(intent, phase).empty());
}

TEST_CASE("heli: hover keeps ground-intent confidence", "[helicopter]") {
  const auto [profile, transcript] =
      GENERATE(table<std::string, std::string>(
          {{"DE", "Friedrichshafen Rollkontrolle Delta Hotel Alpha Bravo "
                  "Charlie erbitte Rollen"},
           {"EN", "Friedrichshafen Ground Delta Hotel Alpha Bravo Charlie "
                  "request taxi"}}));
  RestoreGerman restore;
  load_profile(profile);
  intent_parser::init();
  auto ctx = hover_ctx();
  tick(ctx, 5);
  auto msg = intent_parser::parse(transcript, ctx);
  REQUIRE(msg.intent == intent_parser::PilotIntent::REQUEST_TAXI);
  REQUIRE(msg.confidence >= 0.7f);
}

TEST_CASE("heli: no go-around for an occupied runway", "[helicopter]") {
  load_profile("DE");
  auto ctx = hover_ctx();
  ctx.runways.push_back(runway_24());
  ctx.on_ground = false;
  atc_state_machine::set_state(atc_state_machine::ATCState::LANDING_CLEARED);

  traffic_context::TrafficContext traffic;
  traffic_context::TrafficTarget occupant;
  occupant.lat = 47.67;
  occupant.lon = 9.51;
  occupant.phase = traffic_context::TrafficPhase::OnGround;
  traffic.targets.push_back(occupant);
  traffic_context::set_for_test(traffic);

  std::string text;
  REQUIRE_FALSE(engine::poll_go_around(ctx, 1000.0, &text));
  REQUIRE(text.empty());

  // Control: the same picture does trigger for fixed-wing.
  ctx.is_helicopter = false;
  REQUIRE(engine::poll_go_around(ctx, 1000.0, &text));
  REQUIRE_FALSE(text.empty());
  traffic_context::set_for_test({});
}

TEST_CASE("heli: landing clearance is not rewritten by sequencing",
          "[helicopter]") {
  load_profile("DE");
  auto ctx = hover_ctx();
  ctx.runways.push_back(runway_24());

  traffic_context::TrafficContext traffic;
  traffic_context::TrafficTarget occupant;
  occupant.lat = 47.67;
  occupant.lon = 9.51;
  occupant.phase = traffic_context::TrafficPhase::OnGround;
  traffic.targets.push_back(occupant);

  intent_parser::PilotMessage msg;
  std::map<std::string, std::string> vars{{"callsign", "DHABC"},
                                          {"runway", "24"}};
  atc_state_machine::ATCResponse resp;
  resp.text = "cleared";
  resp.next_state = atc_state_machine::ATCState::LANDING_CLEARED;

  pattern_flow::apply_landing_sequence(msg, ctx, traffic, vars, resp);
  REQUIRE(resp.text == "cleared");

  // Control: the same picture is rewritten for fixed-wing.
  ctx.is_helicopter = false;
  pattern_flow::apply_landing_sequence(msg, ctx, traffic, vars, resp);
  REQUIRE(resp.text != "cleared");
}

TEST_CASE("heli: hover-taxi sequence stays in one flight log", "[helicopter]") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "xp_atc_heli_log_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  cross_country_log::set_dir(dir.string());
  cross_country_log::begin_new_flight();

  load_profile("DE");
  auto ctx = hover_ctx();
  for (const char *intent : {"INITIAL_CALL_GROUND", "REQUEST_TAXI",
                             "INITIAL_CALL_GROUND"}) {
    ctx.height_agl_ft = 70.0f; // flicker above the band between calls
    tick(ctx, 2);
    ctx.height_agl_ft = 5.0f;
    tick(ctx, 2);

    cross_country_log::Entry e;
    e.transcript = "test";
    e.intent = intent;
    e.outcome = "classified";
    e.state = "IDLE";
    e.flight_phase = flight_phase::phase_name(flight_phase::get());
    e.airport_id = "EDNY";
    e.frequency_type = "GROUND";
    cross_country_log::write(e);
  }

  int files = 0;
  for (const auto &entry : fs::directory_iterator(dir))
    files += entry.path().extension() == ".json" ? 1 : 0;
  REQUIRE(files == 1);
  fs::remove_all(dir);
}
