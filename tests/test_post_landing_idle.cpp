/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under GPL-3.0-or-later. See LICENSE.
 */

// Post-landing-IDLE vs fresh-spawn-IDLE disambiguation via was_airborne.
//
// IDLE is overloaded: a pilot fresh on the apron (never flew) and a pilot
// who just landed and taxied back both sit in IDLE on the ground. Two
// consumers must tell them apart, both keyed on was_airborne:
//   1. The tower-only apron-first-contact collapse (INITIAL_CALL_TOWER ->
//      INITIAL_CALL_GROUND) may only fire for fresh-spawn-IDLE.
//   2. The IDLE intent whitelist offers the apron/inbound first-contact
//      intents to fresh-spawn-IDLE and the sign-off (LEAVING_FREQUENCY) to
//      post-landing-IDLE.
// Regression for the EDNY sign-off bug: "verlasse die Frequenz" after a
// completed flight was collapsed into a brand-new ground first contact.

#include "atc/atc_templates.hpp"
#include "atc/atc_state_machine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/flows/ground_operations.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>

using atc_state_machine::ATCState;
using intent_parser::PilotIntent;
using intent_parser::PilotMessage;

namespace {

struct DeProfileGuard {
    std::string saved_region;
    DeProfileGuard() : saved_region(settings::atc_profile()) {
        settings::set_atc_profile("DE");
    }
    ~DeProfileGuard() { settings::set_atc_profile(saved_region); }
};

xplane_context::XPlaneContext tower_only_ground_ctx() {
    xplane_context::XPlaneContext ctx;
    ctx.on_ground = true;
    ctx.facility_type = xplane_context::FacilityType::TOWERED;
    ctx.tower_only = true;
    return ctx;
}

bool contains(const std::vector<std::string> &v, const std::string &key) {
    return std::find(v.begin(), v.end(), key) != v.end();
}

} // namespace

// ── 1. Collapse fires only for fresh-spawn-IDLE ─────────────────────

TEST_CASE("post-landing-idle: fresh-spawn tower-only INITIAL_CALL_TOWER "
          "collapses to GROUND",
          "[idle][de][collapse][was_airborne]") {
    DeProfileGuard g;
    atc_state_machine::init();
    atc_state_machine::set_state(ATCState::IDLE);
    REQUIRE_FALSE(atc_state_machine::was_airborne());

    auto ctx = tower_only_ground_ctx();
    PilotMessage msg;
    msg.intent = PilotIntent::INITIAL_CALL_TOWER;

    ground_ops::apply_tower_only_initial_collapse(msg, ctx);

    REQUIRE(msg.intent == PilotIntent::INITIAL_CALL_GROUND);
}

TEST_CASE("post-landing-idle: airborne-session tower-only INITIAL_CALL_TOWER "
          "is NOT collapsed (sign-off, not first contact)",
          "[idle][de][collapse][was_airborne]") {
    DeProfileGuard g;
    atc_state_machine::init();
    atc_state_machine::set_state(ATCState::IDLE);
    atc_state_machine::set_was_airborne(true);
    REQUIRE(atc_state_machine::was_airborne());

    auto ctx = tower_only_ground_ctx();
    PilotMessage msg;
    msg.intent = PilotIntent::INITIAL_CALL_TOWER;

    ground_ops::apply_tower_only_initial_collapse(msg, ctx);

    REQUIRE(msg.intent == PilotIntent::INITIAL_CALL_TOWER);
}

// ── 2. IDLE whitelist splits on was_airborne ────────────────────────

TEST_CASE("post-landing-idle: fresh-spawn IDLE whitelist offers first-contact, "
          "not the sign-off",
          "[idle][de][valid_intents][was_airborne]") {
    atc_templates::reload();
    auto valid = atc_templates::valid_intents(/*is_towered=*/true, "IDLE",
                                              /*post_landing=*/false);

    REQUIRE(contains(valid, "INITIAL_CALL_GROUND"));
    REQUIRE(contains(valid, "INITIAL_CALL_TOWER"));
    REQUIRE_FALSE(contains(valid, "LEAVING_FREQUENCY"));
}

TEST_CASE("post-landing-idle: post-landing IDLE whitelist offers the sign-off, "
          "not the ground first-contact",
          "[idle][de][valid_intents][was_airborne]") {
    atc_templates::reload();
    auto valid = atc_templates::valid_intents(/*is_towered=*/true, "IDLE",
                                              /*post_landing=*/true);

    REQUIRE(contains(valid, "LEAVING_FREQUENCY"));
    REQUIRE_FALSE(contains(valid, "INITIAL_CALL_GROUND"));
    REQUIRE_FALSE(contains(valid, "INITIAL_CALL_TOWER"));
    // INITIAL_CALL_INBOUND/_VRP stay available even with was_airborne=true: a
    // cross-country leg to a new field reaches IDLE airborne and still needs
    // the inbound first contact (Issue #14). Their phase precondition, not the
    // post_landing split, gates the on-ground case.
    REQUIRE(contains(valid, "INITIAL_CALL_INBOUND"));
    REQUIRE(contains(valid, "INITIAL_CALL_INBOUND_VRP"));
}

TEST_CASE("post-landing-idle: non-IDLE state is unaffected by the post_landing "
          "split",
          "[idle][de][valid_intents][was_airborne]") {
    atc_templates::reload();
    auto fresh = atc_templates::valid_intents(true, "GROUND_CONTACT", false);
    auto landed = atc_templates::valid_intents(true, "GROUND_CONTACT", true);
    REQUIRE(fresh == landed);
}

// ── 3. Tower-only: freq auto-correction must NOT skip the taxi step ──
//
// Issue #10: at a tower-only field there is no Ground frequency, so the pilot
// is on TOWER from first contact. The GROUND_CONTACT -> TOWER_CONTACT
// "pilot tuned Tower" auto-correction (flight_rules.json) must stay inert here,
// otherwise the ATIS readback after the collapsed first contact jumps straight
// to TOWER_CONTACT and skips REQUEST_TAXI / TAXI_CLEARED (NfL 1.4.7 a):
// ERBITTE ROLLEN is mandatory before the taxi clearance).

TEST_CASE("tower-only: freq auto-correction does NOT advance GROUND_CONTACT to "
          "TOWER_CONTACT (issue #10)",
          "[idle][de][tower_only][freq_auto_correction]") {
    using FT = xplane_context::FrequencyType;
    DeProfileGuard g;
    atc_state_machine::init();
    flight_phase::reload();
    atc_state_machine::set_state(ATCState::GROUND_CONTACT);

    auto ctx = tower_only_ground_ctx();
    ctx.frequency_type = FT::TOWER; // tower-only: the only controller freq

    ground_ops::apply_frequency_auto_corrections(ctx);

    REQUIRE(atc_state_machine::get_state() == ATCState::GROUND_CONTACT);
}

TEST_CASE("normal towered field: freq auto-correction still advances "
          "GROUND_CONTACT to TOWER_CONTACT (non-regression)",
          "[idle][de][tower_only][freq_auto_correction]") {
    using FT = xplane_context::FrequencyType;
    DeProfileGuard g;
    atc_state_machine::init();
    flight_phase::reload();
    atc_state_machine::set_state(ATCState::GROUND_CONTACT);

    xplane_context::XPlaneContext ctx;
    ctx.on_ground = true;
    ctx.facility_type = xplane_context::FacilityType::TOWERED;
    ctx.tower_only = false; // separate Ground + Tower controllers
    ctx.frequency_type = FT::TOWER;

    ground_ops::apply_frequency_auto_corrections(ctx);

    REQUIRE(atc_state_machine::get_state() == ATCState::TOWER_CONTACT);
}
