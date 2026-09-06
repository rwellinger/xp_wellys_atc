#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/flight_phase.hpp"
#include "atc/initial_call_conformance.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

// Issue #4 — the session-lifecycle was_airborne reset must run BEFORE the
// conformance guard's early return. A strict-mode INITIAL_CALL_GROUND
// re-request is conceptually the start of a new ground departure cycle, so
// it must clear was_airborne even though it early-returns before the
// template lookup. The reset moved ahead of apply_initial_call_conformance()
// in atc_state_machine::process(); this pins that ordering.

namespace {

using intent_parser::PilotIntent;
using intent_parser::PilotMessage;

void load_de_profile() {
    settings::set_atc_profile("DE");
    settings::set_pilot_callsign_raw("Hotel Bravo Whiskey Romeo Oscar");
    settings::set_vfr_flight_type("pattern");
    settings::set_vfr_destination("");
    atc_templates::init();
    initial_call_conformance::init();
    flight_phase::reload();
    atc_state_machine::init();
}

// IDLE / towered / on GROUND frequency — an INITIAL_CALL_GROUND clears the
// phase + frequency guards and reaches the conformance check here.
xplane_context::XPlaneContext ground_ctx() {
    xplane_context::XPlaneContext ctx{};
    ctx.facility_type = xplane_context::FacilityType::TOWERED;
    ctx.on_ground = true;
    ctx.frequency_type = xplane_context::FrequencyType::GROUND;
    ctx.nearest_airport_id = "EDNY";
    ctx.aircraft_icao = "DV20";
    return ctx;
}

// Missing aircraft_type + intention -> strict mode re-requests.
PilotMessage partial_call() {
    PilotMessage m;
    m.intent = PilotIntent::INITIAL_CALL_GROUND;
    m.confidence = 0.85f;
    m.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    m.raw_transcript = "Friedrichshafen Boden, Hotel Bravo Whiskey Romeo Oscar, "
                       "am Vorfeld, Information Alpha";
    m.has_position = true;
    return m;
}

} // namespace

TEST_CASE("was_airborne: strict re-request still resets the flag",
          "[was_airborne][conformance][bzf_strict]") {
    load_de_profile();
    settings::set_bzf_strict_mode(true);
    auto ctx = ground_ctx();

    // Arm the stale flag, then issue an incomplete first contact on the
    // ground. Before the fix the conformance early-return skipped the reset.
    atc_state_machine::set_was_airborne(true);
    atc_state_machine::set_state(atc_state_machine::ATCState::IDLE);
    auto corr = atc_state_machine::process(partial_call(), ctx, 100.0);

    // The targeted re-request was issued and the state did NOT advance.
    REQUIRE_FALSE(corr.text.empty());
    REQUIRE(corr.text.find("Luftfahrzeugmuster") != std::string::npos);
    REQUIRE(atc_state_machine::get_state() == atc_state_machine::ATCState::IDLE);

    // ...and the new-departure-cycle reset still fired despite the early return.
    REQUIRE_FALSE(atc_state_machine::was_airborne());

    // Shared global state: leave strict mode off for the rest of the suite.
    settings::set_bzf_strict_mode(false);
}
