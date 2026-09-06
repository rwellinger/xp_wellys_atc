#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/flight_phase.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

// Closes the last open flank from the readback-matching change: when the
// engine's _INVALID safety net fires under BZF strict, it must validate a
// content-incomplete readback against the LIVE stored clearance
// components and fire a TARGETED correction — never silently absorb it.
//
// The net is a pure pass-through: it calls run_state_machine(READBACK),
// i.e. atc_state_machine::process() with a READBACK intent, which runs
// apply_bzf_strict_check() against g_state.last_clearance_components_.
// So the failure mode the net could hide — a dead/empty component snapshot
// validating against nothing and degrading to a silent ack — is exactly
// what these tests force through process() directly. Driven by hand-built
// PilotMessages (no rule parser, no LM, no threading) so the assertion is
// deterministic.

namespace {

// Arms a readback-demanding departure clearance from TOWER_CONTACT and
// returns the resulting tower response. After this, readback_pending is
// true and last_clearance_components() carries {Callsign, Runway}.
xplane_context::XPlaneContext primed_ctx() {
    xplane_context::XPlaneContext ctx{};
    ctx.facility_type = xplane_context::FacilityType::TOWERED;
    ctx.on_ground = true;
    ctx.frequency_type = xplane_context::FrequencyType::TOWER;
    ctx.active_runway = "06";
    ctx.nearest_airport_id = "EDNY";
    return ctx;
}

void load_de_profile() {
    settings::set_atc_profile("DE");
    settings::set_pilot_callsign_raw("Hotel Bravo Whiskey Romeo Oscar");
    atc_templates::init();
    flight_phase::reload();
    atc_state_machine::init();
}

} // namespace

TEST_CASE("BZF strict: incomplete readback fires a targeted correction "
          "against live clearance components",
          "[readback][bzf_strict]") {
    load_de_profile();
    settings::set_bzf_strict_mode(true);

    auto ctx = primed_ctx();
    atc_state_machine::set_state(atc_state_machine::ATCState::TOWER_CONTACT);

    // Arm the departure clearance (callsign + runway).
    intent_parser::PilotMessage ready;
    ready.intent = intent_parser::PilotIntent::READY_FOR_DEPARTURE;
    ready.confidence = 0.9f;
    ready.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    ready.runway = "06";
    ready.raw_transcript =
        "Hotel Bravo Whiskey Romeo Oscar Piste 06 abflugbereit";
    auto cleared = atc_state_machine::process(ready, ctx, 100.0);
    REQUIRE(cleared.requires_readback);
    REQUIRE(atc_state_machine::is_readback_pending());

    // The structured components must be live and value-precise.
    auto comp = atc_state_machine::last_clearance_components();
    REQUIRE_FALSE(comp.required.empty());
    REQUIRE(comp.runway == "06");

    // Incomplete readback: callsign present, runway omitted. Under strict
    // this MUST be corrected, not silently accepted.
    intent_parser::PilotMessage incomplete;
    incomplete.intent = intent_parser::PilotIntent::READBACK;
    incomplete.confidence = 0.9f;
    incomplete.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    incomplete.raw_transcript = "Start frei Hotel Bravo Whiskey Romeo Oscar";
    auto corr = atc_state_machine::process(incomplete, ctx, 101.0);

    REQUIRE_FALSE(corr.text.empty()); // a correction was issued, not silent
    REQUIRE(corr.requires_readback);  // still owes a readback
    REQUIRE(atc_state_machine::is_readback_pending());
    // Incomplete (nothing wrong, only omitted) -> NfL READ BACK wording,
    // and the tower must NOT re-state the runway value.
    REQUIRE(corr.text.find("WIEDERHOLEN SIE WOERTLICH") != std::string::npos);
    REQUIRE(corr.text.find("NEGATIV") == std::string::npos);
    // State must not advance on a non-conformant readback.
    REQUIRE(atc_state_machine::get_state() ==
            atc_state_machine::state_from_name("Pattern/DEPARTURE_CLEARED"));
}

TEST_CASE("BZF strict: wrong readback value fires NEGATIV correction naming "
          "the soll value (NfL §25 b) Nr. 3)",
          "[readback][bzf_strict]") {
    load_de_profile();
    settings::set_bzf_strict_mode(true);

    auto ctx = primed_ctx();
    atc_state_machine::set_state(atc_state_machine::ATCState::TOWER_CONTACT);

    // Arm the departure clearance for runway 06.
    intent_parser::PilotMessage ready;
    ready.intent = intent_parser::PilotIntent::READY_FOR_DEPARTURE;
    ready.confidence = 0.9f;
    ready.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    ready.runway = "06";
    ready.raw_transcript =
        "Hotel Bravo Whiskey Romeo Oscar Piste 06 abflugbereit";
    REQUIRE(atc_state_machine::process(ready, ctx, 100.0).requires_readback);

    // Pilot reads back the WRONG runway (25 instead of 06). This is a
    // discrepancy the controller must actively berichtigen with the soll.
    intent_parser::PilotMessage wrong;
    wrong.intent = intent_parser::PilotIntent::READBACK;
    wrong.confidence = 0.9f;
    wrong.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    wrong.raw_transcript = "Piste 25 Start frei Hotel Bravo Whiskey Romeo Oscar";
    auto corr = atc_state_machine::process(wrong, ctx, 101.0);

    REQUIRE_FALSE(corr.text.empty());
    REQUIRE(corr.text.find("NEGATIV") != std::string::npos);
    REQUIRE(corr.text.find("Piste 06") != std::string::npos);       // soll named
    REQUIRE(corr.text.find("WIEDERHOLEN SIE WOERTLICH") != std::string::npos);
    REQUIRE(corr.requires_readback);
    REQUIRE(atc_state_machine::is_readback_pending());
    REQUIRE(atc_state_machine::get_state() ==
            atc_state_machine::state_from_name("Pattern/DEPARTURE_CLEARED"));
}

TEST_CASE("BZF strict: complete welded readback is silently accepted",
          "[readback][bzf_strict]") {
    load_de_profile();
    settings::set_bzf_strict_mode(true);

    auto ctx = primed_ctx();
    atc_state_machine::set_state(atc_state_machine::ATCState::TOWER_CONTACT);

    intent_parser::PilotMessage ready;
    ready.intent = intent_parser::PilotIntent::READY_FOR_DEPARTURE;
    ready.confidence = 0.9f;
    ready.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    ready.runway = "06";
    ready.raw_transcript =
        "Hotel Bravo Whiskey Romeo Oscar Piste 06 abflugbereit";
    REQUIRE(atc_state_machine::process(ready, ctx, 100.0).requires_readback);

    // Complete readback with Whisper-welded "startfrei" — runway + callsign
    // both covered -> silent ack, readback resolved.
    intent_parser::PilotMessage complete;
    complete.intent = intent_parser::PilotIntent::READBACK;
    complete.confidence = 0.9f;
    complete.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    complete.raw_transcript = "Piste 06 startfrei Hotel Bravo Whiskey Romeo Oscar";
    auto resp = atc_state_machine::process(complete, ctx, 101.0);

    REQUIRE(resp.text.empty()); // silent
    REQUIRE_FALSE(atc_state_machine::is_readback_pending());
}

// The literal "dead snapshot" guard: last_clearance_components_ must ride
// inside the AtcMachineState snapshot used by the TTS revert guard. If a
// future change made capture_snapshot copy fields selectively and dropped
// the components, a post-revert strict check would validate against an
// empty set and silently absorb every readback.
TEST_CASE("BZF strict: clearance components survive snapshot/restore",
          "[readback][bzf_strict][snapshot]") {
    load_de_profile();
    settings::set_bzf_strict_mode(true);

    auto ctx = primed_ctx();
    atc_state_machine::set_state(atc_state_machine::ATCState::TOWER_CONTACT);

    intent_parser::PilotMessage ready;
    ready.intent = intent_parser::PilotIntent::READY_FOR_DEPARTURE;
    ready.confidence = 0.9f;
    ready.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    ready.runway = "06";
    ready.raw_transcript =
        "Hotel Bravo Whiskey Romeo Oscar Piste 06 abflugbereit";
    REQUIRE(atc_state_machine::process(ready, ctx, 100.0).requires_readback);

    auto before = atc_state_machine::last_clearance_components();
    REQUIRE_FALSE(before.required.empty());

    auto snap = atc_state_machine::capture_snapshot();

    // Wipe the live components (reset clears them).
    atc_state_machine::reset();
    REQUIRE(atc_state_machine::last_clearance_components().required.empty());

    // Restore the armed snapshot — components must come back intact.
    REQUIRE(atc_state_machine::restore_snapshot_if_gen(
        snap, atc_state_machine::current_gen()));
    auto after = atc_state_machine::last_clearance_components();
    REQUIRE(after.required == before.required);
    REQUIRE(after.runway == before.runway);
    REQUIRE(after.callsign == before.callsign);
}
