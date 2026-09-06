/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under GPL-3.0-or-later. See LICENSE.
 */

// Closes the EDNY taxi-readback deadlock: after a taxi clearance the
// readback is owed, but the pilot's readback ("Rollen zur Rollhaltpiste
// 06 ... <callsign>") shares its keywords with the REQUEST_TAXI clearance
// the tower just issued, so the context-free rule parser scores it
// REQUEST_TAXI at 0.90. Previously the deterministic readback gate in
// engine.cpp only fired below conf 0.7, so this high-conf collision
// slipped past it, re-issued the taxi clearance, and looped forever.
//
// The fix lets the readback gate fire on is_readback_pending() alone,
// with bzf_compliance::readback_covers_core() (callsign + one fact
// element of the stored clearance) as the guard against stealing a
// genuinely different intent. These tests pin both ends: a covering
// high-conf collision becomes READBACK; a non-covering utterance falls
// through untouched.
//
// Driven through engine::process_transcript with no backend registered
// (lm_ready() == false), so the gate is exercised exactly as it runs
// headless / in the brief pre-models-verified window.

#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/engine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/intent_parser.hpp"
#include "atc/intent_rules.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

using intent_parser::PilotIntent;
using atc_state_machine::ATCState;

namespace {

void load_de_profile() {
    settings::set_atc_profile("DE");
    settings::set_pilot_callsign_raw("Hotel Bravo Whiskey Romeo Oscar");
    settings::set_bzf_strict_mode(false);
    atc_templates::init();
    flight_phase::reload();
    intent_rules::reload();
    atc_state_machine::init();
    engine::reset();
}

xplane_context::XPlaneContext ground_ctx() {
    xplane_context::XPlaneContext ctx{};
    ctx.facility_type = xplane_context::FacilityType::TOWERED;
    ctx.on_ground = true;
    ctx.frequency_type = xplane_context::FrequencyType::TOWER;
    ctx.active_runway = "06";
    ctx.nearest_airport_id = "EDNY";
    ctx.tower_only = true;
    return ctx;
}

// Arms a readback-demanding taxi clearance and returns the state the
// machine settled in (TOWER_CONTACT on a tower_only field). After this
// is_readback_pending() is true and last_clearance_components() carries
// {Callsign, Runway, ...}.
ATCState arm_taxi_clearance(const xplane_context::XPlaneContext &ctx) {
    intent_parser::PilotMessage taxi;
    taxi.intent = PilotIntent::REQUEST_TAXI;
    taxi.confidence = 0.9f;
    taxi.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    taxi.runway = "06";
    taxi.raw_transcript =
        "Friedrichshafen Turm Hotel Bravo Whiskey Romeo Oscar am Fahrfeld "
        "erbitte Rollen Piste 06";
    auto cleared = atc_state_machine::process(taxi, ctx, 100.0);
    REQUIRE(cleared.requires_readback);
    REQUIRE(atc_state_machine::is_readback_pending());
    REQUIRE(atc_state_machine::last_clearance_components().runway == "06");
    return atc_state_machine::get_state();
}

engine::Output run(const std::string &transcript,
                   const xplane_context::XPlaneContext &ctx, double now_secs) {
    engine::Input in;
    in.transcript = transcript;
    in.ctx = &ctx;
    in.pilot_callsign = "Hotel Bravo Whiskey Romeo Oscar";
    in.now_secs = now_secs;
    engine::Output got;
    bool called = false;
    engine::process_transcript(in, [&](engine::Output o) {
        got = std::move(o);
        called = true;
    });
    REQUIRE(called);
    return got;
}

} // namespace

// ── 1. The deadlock: high-conf REQUEST_TAXI that is really a readback ──

TEST_CASE("readback-gate: high-conf taxi-readback collision is recognised as "
          "READBACK, not re-issued as REQUEST_TAXI",
          "[readback][engine][gate]") {
    load_de_profile();
    auto ctx = ground_ctx();
    const ATCState armed_state = arm_taxi_clearance(ctx);

    const std::string readback =
        "Rollen zur Rollhaltpiste 06 Hotel Bravo Whiskey Romeo Oscar";

    // Precondition (the collision): the rule parser, blind to the owed
    // readback, scores this utterance as a fresh high-conf taxi request.
    auto raw = intent_parser::parse(readback, ctx);
    REQUIRE(raw.intent == PilotIntent::REQUEST_TAXI);
    REQUIRE(raw.confidence >= 0.7f);

    // The engine must override it: a readback is owed and the utterance
    // covers callsign + runway, so it is a READBACK.
    auto got = run(readback, ctx, 101.0);
    REQUIRE(got.parsed.intent == PilotIntent::READBACK);

    // Readback resolved, no loop: pending cleared, state did not bounce
    // back into the taxi-clearance state.
    REQUIRE_FALSE(atc_state_machine::is_readback_pending());
    REQUIRE(atc_state_machine::get_state() == armed_state);
}

// ── 2. Regression: an ordinary readback still resolves ─────────────────

TEST_CASE("readback-gate: a plain readback still resolves to READBACK",
          "[readback][engine][gate][regression]") {
    load_de_profile();
    auto ctx = ground_ctx();
    const ATCState armed_state = arm_taxi_clearance(ctx);

    auto got = run("Piste 06 ueber Alpha QNH 1018 Hotel Bravo Whiskey Romeo "
                   "Oscar",
                   ctx, 101.0);
    REQUIRE(got.parsed.intent == PilotIntent::READBACK);
    REQUIRE_FALSE(atc_state_machine::is_readback_pending());
    REQUIRE(atc_state_machine::get_state() == armed_state);
}

// ── 3. MANDATORY negative: a non-covering utterance is NOT stolen ──────
//
// With a readback owed, an utterance that carries the callsign but none
// of the clearance's fact values (a query / "say again" / divergent
// request) must fall through to the normal path. readback_covers_core()
// is the boundary that keeps the relaxed gate from absorbing legitimate
// other intents — if this regresses, every callsign-bearing transmission
// while a readback is pending would be swallowed as a phantom readback.

TEST_CASE("readback-gate: an utterance without the owed soll values is not "
          "grabbed as a readback",
          "[readback][engine][gate][negative]") {
    load_de_profile();
    auto ctx = ground_ctx();
    arm_taxi_clearance(ctx);

    auto got = run("Sagen Sie nochmal Hotel Bravo Whiskey Romeo Oscar", ctx,
                   101.0);

    // Not stolen by the readback gate ...
    REQUIRE(got.parsed.intent != PilotIntent::READBACK);
    // ... and the owed readback is still outstanding (not silently
    // absorbed) because the utterance never covered the clearance core.
    REQUIRE(atc_state_machine::is_readback_pending());
}
