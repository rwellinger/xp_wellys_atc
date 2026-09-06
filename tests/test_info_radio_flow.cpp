/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under GPL-3.0-or-later. See LICENSE.
 */

// AFIS/Info and Radio facilities at uncontrolled fields. apt.dat encodes both
// under the Tower row code (1054); the parser reclassifies the name suffix to
// FrequencyType::INFO / RADIO (see test_frequency_classification.cpp). INFO is
// served by the advisory handle_info_flow() (no clearance, no readback, call
// "<Platz> Information"); RADIO routes through handle_unicom_flow() with the
// "<Platz> Radio" call. NfL 2024 §34 b) / §35.

#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/flows/ground_operations.hpp"
#include "atc/intent_parser.hpp"
#include "atc/intent_rules.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

using atc_state_machine::ATCResponse;
using atc_state_machine::ATCState;
using intent_parser::PilotIntent;
using intent_parser::PilotMessage;
using FT = xplane_context::FrequencyType;

namespace {

struct DeProfileGuard {
    std::string saved_region;
    DeProfileGuard() : saved_region(settings::atc_profile()) {
        settings::set_atc_profile("DE");
    }
    ~DeProfileGuard() { settings::set_atc_profile(saved_region); }
};

// Airborne pilot inbound to an uncontrolled field, tuned to the named facility.
xplane_context::XPlaneContext info_ctx(FT freq) {
    xplane_context::XPlaneContext ctx;
    ctx.on_ground = false;
    ctx.facility_type = xplane_context::FacilityType::AFIS; // INFO/RADIO facility (1054 named Info/Radio)
    ctx.frequency_type = freq;
    ctx.nearest_airport_id = "EDTY";
    ctx.nearest_airport_name = "Schwaebisch Hall";
    ctx.active_runway = "10";
    return ctx;
}

bool has(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}

PilotMessage inbound_msg() {
    PilotMessage msg;
    msg.intent = PilotIntent::INITIAL_CALL_INBOUND;
    return msg;
}

PilotMessage msg_with(PilotIntent intent) {
    PilotMessage msg;
    msg.intent = intent;
    return msg;
}

} // namespace

TEST_CASE("info-flow: INFO frequency routes to handle_info_flow, advisory call",
          "[info][de][flow]") {
    DeProfileGuard g;
    atc_state_machine::init();
    atc_templates::reload();

    auto ctx = info_ctx(FT::INFO);
    ATCResponse resp;
    REQUIRE(ground_ops::handle_info_flow(inbound_msg(), ctx, resp));

    // Advisory: addresses "<Platz> Information", never a clearance/readback.
    REQUIRE(has(resp.text, "Schwaebisch Hall Information"));
    REQUIRE(resp.next_state == ATCState::IDLE);
    REQUIRE_FALSE(resp.requires_readback);
}

TEST_CASE("info-flow: SELF_ANNOUNCE_DEPARTURE is silent on INFO and RADIO",
          "[info][de][flow][self_announce]") {
    DeProfileGuard g;
    atc_state_machine::init();
    atc_templates::reload();

    // AFIS does not acknowledge departure self-announces: empty response_text
    // -> the session pipeline speaks nothing (same pattern as READBACK).
    auto msg = msg_with(PilotIntent::SELF_ANNOUNCE_DEPARTURE);

    ATCResponse info_resp;
    REQUIRE(ground_ops::handle_info_flow(msg, info_ctx(FT::INFO), info_resp));
    REQUIRE(info_resp.text.empty());
    REQUIRE(info_resp.next_state == ATCState::IDLE);
    REQUIRE_FALSE(info_resp.requires_readback);

    ATCResponse radio_resp;
    REQUIRE(
        ground_ops::handle_unicom_flow(msg, info_ctx(FT::RADIO), radio_resp));
    REQUIRE(radio_resp.text.empty());
    REQUIRE(radio_resp.next_state == ATCState::IDLE);
    REQUIRE_FALSE(radio_resp.requires_readback);
}

TEST_CASE("info-flow: handle_info_flow ignores non-INFO frequencies",
          "[info][de][flow]") {
    DeProfileGuard g;
    atc_state_machine::init();
    atc_templates::reload();

    ATCResponse resp;
    REQUIRE_FALSE(
        ground_ops::handle_info_flow(inbound_msg(), info_ctx(FT::RADIO), resp));
    REQUIRE_FALSE(
        ground_ops::handle_info_flow(inbound_msg(), info_ctx(FT::UNICOM), resp));
}

TEST_CASE("info-flow: RADIO frequency routes through unicom flow with Radio call",
          "[info][de][flow]") {
    DeProfileGuard g;
    atc_state_machine::init();
    atc_templates::reload();

    auto ctx = info_ctx(FT::RADIO);
    ATCResponse resp;
    REQUIRE(ground_ops::handle_unicom_flow(inbound_msg(), ctx, resp));

    REQUIRE(has(resp.text, "Schwaebisch Hall Radio"));
    REQUIRE(resp.next_state == ATCState::IDLE);
    REQUIRE_FALSE(resp.requires_readback);
}

TEST_CASE("info-flow: UNICOM call is unchanged (still 'Verkehr <Platz>')",
          "[info][de][flow]") {
    DeProfileGuard g;
    atc_state_machine::init();
    atc_templates::reload();

    auto ctx = info_ctx(FT::UNICOM);
    PilotMessage msg;
    msg.intent = PilotIntent::SELF_ANNOUNCE;
    ATCResponse resp;
    REQUIRE(ground_ops::handle_unicom_flow(msg, ctx, resp));

    // Generic uncontrolled/IDLE block: byte-identical "Verkehr <Platz>" call.
    REQUIRE(has(resp.text, "Verkehr Schwaebisch Hall"));
    REQUIRE_FALSE(has(resp.text, "Radio"));
    REQUIRE_FALSE(has(resp.text, "Information"));
}

// ── RMZ (Funkkommunikationspflichtgebiet) — NfL 2024 ANLAGE 7.4 a)/b) ────────
// Testroute EDNY -> EDTY: EDTY hat RMZ + AFIS/Info. INFO ist advisorisch
// (Para 35) -> keine Freigabe, kein Readback. RMZ_ENTER bleibt IDLE,
// RMZ_LEAVE (Transit) -> EN_ROUTE.

TEST_CASE("rmz: RMZ-Einflug-Utterance -> RMZ_ENTER (NfL 7.4 a)",
          "[rmz][info][de][flow]") {
    DeProfileGuard g;
    intent_rules::reload(); // ensure the DE rule table (RMZ rules) is loaded
    atc_state_machine::init();

    auto ctx = info_ctx(FT::INFO);
    auto m = intent_parser::parse(
        "Schwaebisch Hall Information, DA20 Delta Echo Romeo Kilo Lima, "
        "noerdlich Crailsheim, VFR, zwei tausend Fuss, werde in RMZ "
        "einfliegen zur Landung in Schwaebisch Hall.",
        ctx);
    REQUIRE(m.intent == PilotIntent::RMZ_ENTER);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("rmz: RMZ-Verlassen-Utterance -> RMZ_LEAVE (NfL 7.4 b)",
          "[rmz][info][de][flow]") {
    DeProfileGuard g;
    intent_rules::reload(); // ensure the DE rule table (RMZ rules) is loaded
    atc_state_machine::init();

    auto ctx = info_ctx(FT::INFO);
    auto m = intent_parser::parse(
        "Verlasse RMZ, noerdlich Crailsheim, zwei tausend Fuss, "
        "Echo Romeo Kilo Lima.",
        ctx);
    REQUIRE(m.intent == PilotIntent::RMZ_LEAVE);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("rmz: RMZ_ENTER on INFO -> advisory, no clearance, stays IDLE",
          "[rmz][info][de][flow]") {
    DeProfileGuard g;
    atc_state_machine::init();
    atc_templates::reload();

    auto ctx = info_ctx(FT::INFO);
    ATCResponse resp;
    REQUIRE(ground_ops::handle_info_flow(msg_with(PilotIntent::RMZ_ENTER), ctx,
                                         resp));

    REQUIRE(has(resp.text, "Schwaebisch Hall Information"));
    // INFO/AFIS is advisory only: no landing clearance, no readback.
    REQUIRE_FALSE(has(resp.text, "frei zur Landung"));
    REQUIRE(resp.next_state == ATCState::IDLE);
    REQUIRE_FALSE(resp.requires_readback);
}

TEST_CASE("rmz: RMZ_LEAVE on INFO -> acknowledged, transitions to EN_ROUTE",
          "[rmz][info][de][flow]") {
    DeProfileGuard g;
    atc_state_machine::init();
    atc_templates::reload();

    auto ctx = info_ctx(FT::INFO);
    ATCResponse resp;
    REQUIRE(ground_ops::handle_info_flow(msg_with(PilotIntent::RMZ_LEAVE), ctx,
                                         resp));

    REQUIRE(has(resp.text, "Schwaebisch Hall Information"));
    REQUIRE(resp.next_state == ATCState::EN_ROUTE);
    REQUIRE_FALSE(resp.requires_readback);
}

TEST_CASE("rmz: AFIS/INFO never issues a landing clearance (NfL Para 35)",
          "[rmz][info][de][flow]") {
    DeProfileGuard g;
    atc_state_machine::init();
    atc_templates::reload();

    auto ctx = info_ctx(FT::INFO);
    ATCResponse resp;
    // REQUEST_LANDING has no INFO template -> falls to _INVALID, never a
    // clearance. Pilot lands on own responsibility, which is NfL-correct.
    REQUIRE(ground_ops::handle_info_flow(msg_with(PilotIntent::REQUEST_LANDING),
                                         ctx, resp));
    REQUIRE_FALSE(has(resp.text, "frei zur Landung"));
    REQUIRE_FALSE(has(resp.text, "cleared to land"));
    REQUIRE(resp.next_state != ATCState::LANDING_CLEARED);
    REQUIRE_FALSE(resp.requires_readback);
}
