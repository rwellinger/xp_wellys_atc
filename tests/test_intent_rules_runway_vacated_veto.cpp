/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under GPL-3.0-or-later. See LICENSE.
 */

// RUNWAY_VACATED impossibility-veto adjustments. Covers the EDNY
// deadlock fix: a Whisper-mangled "Piste 06 startfrei" (the departure
// readback) becomes "Piste 06 frei" and matches the RUNWAY_VACATED
// rule. The DE-profile adjustments demote that to READBACK when a
// readback is pending, or to confidence 0.0 (LM-repair fallback)
// otherwise — but ONLY while the session has not been airborne yet.
//
// Also covers Bullet F.2: the existing REQUEST_TAXI ->
// REQUEST_TAXI_PARKING adjustment switched from the 120 s just_landed
// window to the timeless at_airport_after_landing window.

#include "atc/atc_state_machine.hpp"
#include "atc/flows/state_storage.hpp"
#include "atc/intent_parser.hpp"
#include "atc/intent_rules.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

using intent_parser::parse;
using intent_parser::PilotIntent;
using atc_state_machine::ATCState;

namespace {

struct DeRegionGuard {
    std::string saved_region;
    std::string saved_callsign;
    DeRegionGuard()
        : saved_region(settings::atc_profile()),
          saved_callsign(settings::pilot_callsign()) {
        settings::set_atc_profile("DE");
        settings::set_pilot_callsign_raw("Hotel Bravo Delta Sierra Victor");
        intent_rules::reload();
    }
    ~DeRegionGuard() {
        settings::set_atc_profile(saved_region);
        settings::set_pilot_callsign_raw(saved_callsign);
        intent_rules::reload();
    }
};

xplane_context::XPlaneContext ground_ctx() {
    xplane_context::XPlaneContext ctx;
    ctx.on_ground = true;
    ctx.facility_type = xplane_context::FacilityType::TOWERED;
    return ctx;
}

xplane_context::XPlaneContext airborne_ctx() {
    xplane_context::XPlaneContext ctx;
    ctx.on_ground = false;
    ctx.facility_type = xplane_context::FacilityType::TOWERED;
    return ctx;
}

} // namespace

// ── 1. The EDNY deadlock case ───────────────────────────────────────

TEST_CASE("vacated-veto: 'Piste 06 frei' am Rollhalt mit readback_pending "
          "wird zu READBACK demotiert",
          "[intent][de][runway_vacated][veto]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_state(ATCState::DEPARTURE_CLEARED);
    atc_state_machine::internal::set_readback_pending(true);
    // was_airborne_ left at its init() default of false.
    REQUIRE_FALSE(atc_state_machine::was_airborne());
    REQUIRE(atc_state_machine::is_readback_pending());

    auto ctx = ground_ctx();
    ctx.now_secs = 1.0;
    auto m = parse("Piste 06 frei, Hotel Bravo Delta Sierra Victor.", ctx);

    REQUIRE(m.intent == PilotIntent::READBACK);
}

// ── 2. No readback pending → confidence demotion ───────────────────

TEST_CASE("vacated-veto: 'Piste 06 frei' ohne readback_pending "
          "wird auf confidence 0.0 gedaempft (LM-Repair-Pfad)",
          "[intent][de][runway_vacated][veto]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_state(ATCState::DEPARTURE_CLEARED);
    atc_state_machine::internal::set_readback_pending(false);
    REQUIRE_FALSE(atc_state_machine::was_airborne());
    REQUIRE_FALSE(atc_state_machine::is_readback_pending());

    auto ctx = ground_ctx();
    ctx.now_secs = 1.0;
    auto m = parse("Piste 06 frei, Hotel Bravo Delta Sierra Victor.", ctx);

    // Intent stays RUNWAY_VACATED but confidence drops so engine.cpp:382
    // routes through the LM repair path.
    REQUIRE(m.intent == PilotIntent::RUNWAY_VACATED);
    REQUIRE(m.confidence == 0.0f);
}

// ── 3. Real vacated (regression) ───────────────────────────────────

TEST_CASE("vacated-veto: echter Vacated nach Landung bleibt RUNWAY_VACATED",
          "[intent][de][runway_vacated][veto][regression]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_was_airborne(true);
    atc_state_machine::set_state(ATCState::LANDING_CLEARED);
    REQUIRE(atc_state_machine::was_airborne());

    auto ctx = ground_ctx();
    ctx.now_secs = 30.0;
    auto m = parse("Piste 06 frei, Hotel Bravo Delta Sierra Victor.", ctx);

    REQUIRE(m.intent == PilotIntent::RUNWAY_VACATED);
    REQUIRE(m.confidence >= 0.85f);
}

// ── 4. Touch-and-Go keeps was_airborne true ────────────────────────

TEST_CASE("vacated-veto: was_airborne bleibt true ueber Touch-and-Go-Sequenz",
          "[intent][de][was_airborne]") {
    DeRegionGuard g;
    atc_state_machine::init();
    REQUIRE_FALSE(atc_state_machine::was_airborne());

    // First takeoff — flight_phase::update() would call this when
    // crossing into CLIMB. We simulate the setter directly since the
    // unit test does not run the flight_phase frame loop.
    atc_state_machine::set_was_airborne(true);
    REQUIRE(atc_state_machine::was_airborne());

    // Touch-and-Go: landing roll + immediate takeoff. We model the
    // ATC dialog with a TOUCH_AND_GO_CLEARED transition and back to
    // PATTERN_ENTRY (no IDLE in between).
    atc_state_machine::set_state(ATCState::TOUCH_AND_GO_CLEARED);
    atc_state_machine::set_state(ATCState::PATTERN_ENTRY);

    // Flag must NOT have been reset — the reset hook only fires on
    // ground-departure intents processed via process(), and we never
    // hit IDLE.
    REQUIRE(atc_state_machine::was_airborne());
}

// ── 5. Full-stop + REQUEST_TAXI resets was_airborne ────────────────

TEST_CASE("vacated-veto: REQUEST_TAXI nach Full-Stop am Boden resettet "
          "was_airborne (neuer Departure-Zyklus)",
          "[intent][de][was_airborne][reset]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_was_airborne(true);
    atc_state_machine::set_state(ATCState::IDLE);
    REQUIRE(atc_state_machine::was_airborne());

    auto ctx = ground_ctx();
    ctx.now_secs = 200.0; // well past any post-landing window
    // Tuned to GROUND for a taxi request: with the default UNKNOWN freq the
    // wrong-frequency hint guard intercepts before the reset hook, but only
    // when flight_phase config happens to be loaded by a prior test --
    // an order-dependent trap (Issue #3). GROUND is the realistic frequency
    // for REQUEST_TAXI and makes the test order-independent.
    ctx.frequency_type = xplane_context::FrequencyType::GROUND;
    // Drive a REQUEST_TAXI through process(). With at_airport_after_landing
    // false (no landing in history), apply_adjustments leaves the intent
    // intact, and the reset hook in process() fires.
    intent_parser::PilotMessage msg;
    msg.intent = PilotIntent::REQUEST_TAXI;
    msg.callsign = "Hotel Bravo Delta Sierra Victor";
    msg.confidence = 0.9f;
    atc_state_machine::process(msg, ctx, ctx.now_secs);

    REQUIRE_FALSE(atc_state_machine::was_airborne());
}

// ── 5a. INITIAL_CALL_INBOUND airborne does NOT reset ───────────────

TEST_CASE("vacated-veto: INITIAL_CALL_INBOUND airborne haelt was_airborne "
          "auf true (Intent nicht in der Reset-Liste)",
          "[intent][de][was_airborne][reset]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_was_airborne(true);
    atc_state_machine::set_state(ATCState::EN_ROUTE);

    auto ctx = airborne_ctx();
    ctx.now_secs = 500.0;
    intent_parser::PilotMessage msg;
    msg.intent = PilotIntent::INITIAL_CALL_INBOUND;
    msg.callsign = "Hotel Bravo Delta Sierra Victor";
    msg.confidence = 0.9f;
    atc_state_machine::process(msg, ctx, ctx.now_secs);

    REQUIRE(atc_state_machine::was_airborne());
}

// ── 5b. Generic INITIAL_CALL on the ground resets ──────────────────

TEST_CASE("vacated-veto: generisches INITIAL_CALL am Boden resettet "
          "was_airborne (Low-Confidence-Fallback)",
          "[intent][de][was_airborne][reset]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_was_airborne(true);
    atc_state_machine::set_state(ATCState::IDLE);

    auto ctx = ground_ctx();
    ctx.now_secs = 200.0;
    // TOWER freq: a generic INITIAL_CALL maps to the INITIAL_CALL_TOWER
    // template key (intent_parser fallback), whose frequency precondition
    // allows only TOWER. The default UNKNOWN freq otherwise triggers the
    // wrong-frequency hint guard (order-dependent on flight_phase being
    // loaded) and on GROUND the freq-precondition guard rejects -- both
    // early-return before the reset hook. See Issue #3.
    ctx.frequency_type = xplane_context::FrequencyType::TOWER;
    intent_parser::PilotMessage msg;
    msg.intent = PilotIntent::INITIAL_CALL;
    msg.callsign = "Hotel Bravo Delta Sierra Victor";
    msg.confidence = 0.8f;
    atc_state_machine::process(msg, ctx, ctx.now_secs);

    REQUIRE_FALSE(atc_state_machine::was_airborne());
}

// ── 5c. Generic INITIAL_CALL airborne does NOT reset (gate) ────────

TEST_CASE("vacated-veto: generisches INITIAL_CALL airborne haelt "
          "was_airborne (on_ground-Gate fuer Parser-Misclassifications)",
          "[intent][de][was_airborne][reset]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_was_airborne(true);
    atc_state_machine::set_state(ATCState::EN_ROUTE);

    auto ctx = airborne_ctx();
    ctx.now_secs = 500.0;
    intent_parser::PilotMessage msg;
    msg.intent = PilotIntent::INITIAL_CALL;
    msg.callsign = "Hotel Bravo Delta Sierra Victor";
    msg.confidence = 0.8f;
    atc_state_machine::process(msg, ctx, ctx.now_secs);

    REQUIRE(atc_state_machine::was_airborne());
}

// ── 7. F.2 regression: REQUEST_TAXI -> REQUEST_TAXI_PARKING ─────────
//
// The previous wiring used the 120 s just_landed window — a long
// roll-out plus slow taxi-back would slip past it and the demotion
// stopped firing. We switched to at_airport_after_landing (timeless,
// resets on DEPARTURE_CLEARED). Verify both ends.

TEST_CASE("vacated-veto F.2: REQUEST_TAXI nach langem Taxi-back wird "
          "weiterhin auf REQUEST_TAXI_PARKING gemappt (zeitlos)",
          "[intent][de][adjustment][taxi_parking]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_state(ATCState::LANDING_CLEARED);
    atc_state_machine::set_state(ATCState::IDLE);

    auto ctx = ground_ctx();
    ctx.now_secs = 200.0; // past the old 120 s just_landed window
    REQUIRE_FALSE(atc_state_machine::just_landed(ctx.now_secs));
    REQUIRE(atc_state_machine::at_airport_after_landing(ctx));

    auto m = parse("rollen, Hotel Bravo Delta Sierra Victor.", ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_TAXI_PARKING);
}

TEST_CASE("vacated-veto F.2: REQUEST_TAXI nach neuem Departure-Zyklus "
          "bleibt REQUEST_TAXI (at_airport_after_landing geschlossen)",
          "[intent][de][adjustment][taxi_parking]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_state(ATCState::LANDING_CLEARED);
    atc_state_machine::set_state(ATCState::IDLE);
    // A new departure cycle has started — push DEPARTURE_CLEARED
    // into history so at_airport_after_landing returns false.
    atc_state_machine::set_state(ATCState::GROUND_CONTACT);
    atc_state_machine::set_state(ATCState::TAXI_CLEARED);
    atc_state_machine::set_state(ATCState::TOWER_CONTACT);
    atc_state_machine::set_state(ATCState::DEPARTURE_CLEARED);
    atc_state_machine::set_state(ATCState::IDLE);

    auto ctx = ground_ctx();
    ctx.now_secs = 500.0;
    REQUIRE_FALSE(atc_state_machine::at_airport_after_landing(ctx));

    auto m = parse("rollen, Hotel Bravo Delta Sierra Victor.", ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_TAXI);
}

// ── 6. Snapshot/Revert + monotonicity ───────────────────────────────

TEST_CASE("vacated-veto: was_airborne wird mit-snapshotted und "
          "korrekt restored; gen bleibt strikt monoton",
          "[intent][de][was_airborne][snapshot]") {
    atc_state_machine::init();
    REQUIRE_FALSE(atc_state_machine::was_airborne());

    const uint64_t g0 = atc_state_machine::current_gen();

    // Idempotent: setting the same value (false) must NOT bump gen.
    atc_state_machine::set_was_airborne(false);
    REQUIRE(atc_state_machine::current_gen() == g0);

    // Actual change bumps gen.
    atc_state_machine::set_was_airborne(true);
    REQUIRE(atc_state_machine::current_gen() > g0);
    const uint64_t g1 = atc_state_machine::current_gen();

    // Snapshot now (was_airborne == true).
    auto snap = atc_state_machine::capture_snapshot();

    // Mutate: clear the flag — must bump gen.
    atc_state_machine::set_was_airborne(false);
    REQUIRE(atc_state_machine::current_gen() > g1);
    REQUIRE_FALSE(atc_state_machine::was_airborne());
    const uint64_t g2 = atc_state_machine::current_gen();

    // Restore. gen must end up strictly larger than g2 (strict
    // monotonicity), and was_airborne must be back to true.
    REQUIRE(atc_state_machine::restore_snapshot_if_gen(snap, g2));
    REQUIRE(atc_state_machine::was_airborne());
    REQUIRE(atc_state_machine::current_gen() > g2);
}
