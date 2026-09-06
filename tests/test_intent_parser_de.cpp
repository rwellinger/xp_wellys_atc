// M7 DoD: 20 typical BZF (Beschraenkt Zugeteiltes Sprechfunkzeugnis)
// transmissions from AIP Germany / training material classify
// correctly via the rule parser at high confidence. The reverse
// normalizer (de_phraseology::parse_spoken_number) runs first inside
// intent_parser::parse() so ziffernweise pronunciation ("eins null
// eins drei") is collapsed to raw digits before rule matching.

#include "atc/atc_state_machine.hpp"
#include "atc/flows/ground_operations.hpp"
#include "atc/intent_parser.hpp"
#include "atc/intent_rules.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

using intent_parser::parse;
using intent_parser::PilotIntent;

namespace {

// RAII guard: switches flow_region to "DE" + sets a German-style
// pilot callsign + forces intent_rules to reload from the DE JSON
// for the duration of the test case, then restores both so
// neighbouring TUs (test_intent_parser.cpp) are not contaminated.
// intent_rules caches the rule table on first parse() call; without
// the explicit reload, EU/US rules loaded by earlier tests would
// stick around and DE tests would mis-classify.
struct DeRegionGuard {
    std::string saved_region;
    std::string saved_callsign;
    DeRegionGuard()
        : saved_region(settings::atc_profile()),
          saved_callsign(settings::pilot_callsign()) {
        settings::set_atc_profile("DE");
        settings::set_pilot_callsign_raw("Delta Echo Whiskey Lima Yankee");
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

// On-ground pilot at an AFIS/Info field. on_ground=true is essential: the
// ground-only INITIAL_CALL_GROUND is demoted to 0.3 while airborne.
xplane_context::XPlaneContext afis_ground_ctx() {
    xplane_context::XPlaneContext ctx;
    ctx.on_ground = true;
    ctx.facility_type = xplane_context::FacilityType::AFIS;
    ctx.frequency_type = xplane_context::FrequencyType::INFO;
    ctx.nearest_airport_id = "EDTY";
    ctx.active_runway = "10";
    return ctx;
}

} // namespace

// ── Departure phase (ground) ─────────────────────────────────────────

TEST_CASE("DE: VFR Abflug 'abflugbereit nach Osten' -> READY_FOR_DEPARTURE_VFR",
          "[intent][de][departure]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Friedrichshafen Turm, Delta Echo Whiskey Lima Yankee, "
                   "am Rollhalt Piste zwo vier, abflugbereit, nach Osten.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::READY_FOR_DEPARTURE_VFR);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: spoken ICAO destination extracted into msg.destination",
          "[intent][de][destination]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Augsburg Boden, Delta Echo Whiskey Lima Yankee, am Vorfeld, "
                   "VFR nach Echo Delta Mike Alfa, Information Alfa.",
                   ctx);
    REQUIRE(m.destination == "EDMA");
}

TEST_CASE("DE: directional 'nach Osten' is not a destination",
          "[intent][de][destination]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, am Rollhalt Piste zwo vier, "
                   "abflugbereit, nach Osten.",
                   ctx);
    REQUIRE(m.destination.empty());
}

TEST_CASE("DE: 'abflugbereit Piste 24' -> READY_FOR_DEPARTURE",
          "[intent][de][departure]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, am Rollhalt Piste 24, "
                   "abflugbereit.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::READY_FOR_DEPARTURE);
    REQUIRE(m.confidence >= 0.85f);
}

// Issue #30: the collision-free non-word mishearing "Bisten" -> "piste" is
// fixed deterministically by the normalize[] map (before rule matching), so
// the rule parser resolves runway + intent without an LM round-trip.
TEST_CASE("DE: Whisper mishearing 'Bisten 24' normalises to 'Piste 24'",
          "[intent][de][departure][normalize]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, am Rollhalt Bisten 24, "
                   "abflugbereit.",
                   ctx);
    REQUIRE(m.runway == "24");
    REQUIRE(m.intent == PilotIntent::READY_FOR_DEPARTURE);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Boden initial call -> INITIAL_CALL_GROUND",
          "[intent][de][initial_call]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    // Pure initial call: facility ('Boden') + position, no taxi
    // request keyword. With 'rollen' the rule parser correctly
    // prefers REQUEST_TAXI -- see separate test below.
    auto m = parse("Friedrichshafen Boden, Delta Echo Whiskey Lima Yankee, "
                   "an der Tankstelle, Information Alfa.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::INITIAL_CALL_GROUND);
    REQUIRE(m.confidence >= 0.80f);
}

TEST_CASE("DE: AFIS Information initial call -> INITIAL_CALL_GROUND",
          "[intent][de][initial_call][info]") {
    DeRegionGuard g;
    auto ctx = afis_ground_ctx();
    // AFIS field: pilot addresses '<Platz> Information' instead of 'Boden'.
    // The transcript carries 'Information' twice (station address + ATIS
    // letter); the standalone-word has_facility matcher catches it on ground.
    auto m = parse("Schwaebisch Hall Information, Delta Echo Romeo Kilo Lima, "
                   "BN2P, am Vorfeld, VFR Platzrunde, Information Alpha.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::INITIAL_CALL_GROUND);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: AFIS final-approach report with ATIS letter stays position report",
          "[intent][de][info][regression]") {
    DeRegionGuard g;
    auto ctx = afis_ground_ctx();
    ctx.on_ground = false; // airborne
    // Regression: REPORT_POSITION_FINAL precedes INITIAL_CALL_GROUND in
    // rule order and match() returns at the first hit, so the new
    // 'information' matcher never hijacks an 'Endanflug' position report.
    auto m = parse("Schwaebisch Hall Information, im Endanflug, "
                   "Information Bravo.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::REPORT_POSITION_FINAL);
}

TEST_CASE("DE: AFIS departure self-announce -> SELF_ANNOUNCE_DEPARTURE",
          "[intent][de][info][self_announce]") {
    DeRegionGuard g;
    auto ctx = afis_ground_ctx();
    // AFIS field has no takeoff clearance: pilot self-announces the departure.
    auto m = parse("Schwaebisch Hall D-ERKL, Piste 10, ich starte.", ctx);
    REQUIRE(m.intent == PilotIntent::SELF_ANNOUNCE_DEPARTURE);
    REQUIRE(m.confidence >= 0.88f);
}

TEST_CASE("DE: 'erbitte rollen Piste 10' stays REQUEST_TAXI (not self-announce)",
          "[intent][de][taxi][self_announce]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    // Collision guard: the none-clause [erbitte/rollfreigabe/rollen] keeps a
    // taxi request out of SELF_ANNOUNCE_DEPARTURE.
    auto m = parse("erbitte rollen Piste 10.", ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_TAXI);
}

TEST_CASE("DE: towered 'Piste 10, abflugbereit' stays READY_FOR_DEPARTURE",
          "[intent][de][departure][self_announce]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    // Collision guard: READY_FOR_DEPARTURE precedes SELF_ANNOUNCE_DEPARTURE in
    // rule order, so 'abflugbereit' at a towered field wins.
    auto m = parse("Schwaebisch Hall Tower, D-ERKL, Piste 10, abflugbereit.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::READY_FOR_DEPARTURE);
}

TEST_CASE("DE: Boden + erbitte Rollen -> REQUEST_TAXI",
          "[intent][de][taxi]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Friedrichshafen Boden, Delta Echo Whiskey Lima Yankee, "
                   "an der Tankstelle, erbitte Rollen zum Rollhalt Piste 24.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_TAXI);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Rollfreigabe readback 'rollen Sie ueber Charlie' -> READBACK",
          "[intent][de][readback]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, rollen Sie zum Rollhalt "
                   "Piste zwo vier ueber Charlie, QNH eins null eins drei.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::READBACK);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Funkpruefung -> RADIO_CHECK",
          "[intent][de][radio_check]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Funkpruefung, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::RADIO_CHECK);
    REQUIRE(m.confidence >= 0.85f);
}

// "Wiederholen Sie" / "Say again" — pilot asks tower to repeat the
// last clearance (NfL §18 c) Nr. 4). Critical UX for strict-mode:
// when the pilot forgets the QNH and can't read it back, they must
// be able to ask for a replay without falling into the corrective
// loop. Added 2026-06-05 (user EDNY test).
TEST_CASE("DE: 'Wiederholen Sie' -> REQUEST_REPEAT",
          "[intent][de][repeat]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse(
        "Wiederholen Sie, Hotel Bravo Delta Sierra Victor.", ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_REPEAT);
    REQUIRE(m.confidence >= 0.9f);
}

TEST_CASE("DE: 'Sagen Sie nochmals' -> REQUEST_REPEAT",
          "[intent][de][repeat]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Sagen Sie nochmals.", ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_REPEAT);
}

TEST_CASE("DE: 'Say again' -> REQUEST_REPEAT (English variant accepted)",
          "[intent][de][repeat]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Say again, HB-DSV.", ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_REPEAT);
}

// "Funkprobe" is the colloquial BZF variant of "Funkpruefung". Whisper
// reliably transcribes it that way and pilots in real DACH operation
// say it interchangeably — must classify as RADIO_CHECK and not fall
// through to INITIAL_CALL_TOWER via the facility-keyword "Tower".
// Regression fix from user test 2026-06-05 (EDNY tower_only).
TEST_CASE("DE: Funkprobe -> RADIO_CHECK (not INITIAL_CALL_TOWER)",
          "[intent][de][radio_check]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse(
        "Friedrichshafen Tower, Hotel Bravo Delta Sierra Victor, Funkprobe 120,075.",
        ctx);
    REQUIRE(m.intent == PilotIntent::RADIO_CHECK);
    REQUIRE(m.confidence >= 0.85f);
}

// ── Pattern phase (airborne) ─────────────────────────────────────────

TEST_CASE("DE: Im Gegenanflug -> REPORT_POSITION_DOWNWIND",
          "[intent][de][position]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, im Gegenanflug Piste 24.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::REPORT_POSITION_DOWNWIND);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Im Queranflug -> REPORT_POSITION_BASE",
          "[intent][de][position]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, im Queranflug Piste 24.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::REPORT_POSITION_BASE);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Im Endanflug -> REPORT_POSITION_FINAL",
          "[intent][de][position]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, im Endanflug Piste zwo vier.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::REPORT_POSITION_FINAL);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Durchstarten -> GO_AROUND",
          "[intent][de][go_around]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Durchstarten, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::GO_AROUND);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Friedrichshafen Turm zur Landung -> INITIAL_CALL_INBOUND",
          "[intent][de][inbound]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Friedrichshafen Turm, Delta Echo Whiskey Lima Yankee, "
                   "zur Landung, Piste 24.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::INITIAL_CALL_INBOUND);
    REQUIRE(m.confidence >= 0.80f);
}

// ── AFIS/Info inbound initial call (Issue 13) ────────────────────────
// NfL ANLAGE 1.4.13 a)/e): the inbound first call '... ZUR LANDUNG'
// applies at AFIS fields too, where the pilot addresses 'Information'/
// 'Radio' instead of 'Turm'. Before the fix REQUEST_LANDING (no turm in
// its none-guard) swallowed these into an _INVALID dead end.

TEST_CASE("DE: AFIS Information inbound 'zur Landung' -> INITIAL_CALL_INBOUND",
          "[intent][de][inbound][info]") {
    DeRegionGuard g;
    auto ctx = afis_ground_ctx();
    ctx.on_ground = false; // airborne, joining the pattern inbound
    // No pattern-leg keyword and no VRP: a bare inbound first call. With a
    // leg word ('Endanflug') REPORT_POSITION_FINAL would win; with a VRP
    // the upgrade rule would lift this to INITIAL_CALL_INBOUND_VRP.
    auto m = parse("Schwaebisch Hall Information, Delta Echo Romeo Kilo Lima, "
                   "zur Landung.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::INITIAL_CALL_INBOUND);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: AFIS Information cross-country inbound -> INITIAL_CALL_INBOUND",
          "[intent][de][inbound][info]") {
    DeRegionGuard g;
    auto ctx = afis_ground_ctx();
    ctx.on_ground = false; // airborne, en route to the field
    auto m = parse("Schwaebisch Hall Information, Delta Echo Romeo Kilo Lima, "
                   "10 Kilometer suedlich, 1500 Fuss, zur Landung.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::INITIAL_CALL_INBOUND);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: AFIS Radio inbound 'zur Landung' -> INITIAL_CALL_INBOUND",
          "[intent][de][inbound][radio]") {
    DeRegionGuard g;
    auto ctx = afis_ground_ctx();
    ctx.on_ground = false;
    ctx.frequency_type = xplane_context::FrequencyType::RADIO;
    auto m = parse("Hamburg Radio, Delta Echo Romeo Kilo Lima, "
                   "zur Landung.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::INITIAL_CALL_INBOUND);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: AFIS Information ground call without landing phrase stays GROUND",
          "[intent][de][inbound][info][collision]") {
    DeRegionGuard g;
    auto ctx = afis_ground_ctx(); // on_ground=true
    // No 'zur Landung'/'voller Halt' -> the inbound rule must not fire;
    // this is a ground initial call, not an inbound one.
    auto m = parse("Schwaebisch Hall Information, Delta Echo Romeo Kilo Lima, "
                   "am Vorfeld, VFR Platzrunde, Information Alpha.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::INITIAL_CALL_GROUND);
}

TEST_CASE("DE: AFIS Information final report with landing phrase stays FINAL",
          "[intent][de][inbound][info][regression]") {
    DeRegionGuard g;
    auto ctx = afis_ground_ctx();
    ctx.on_ground = false; // airborne
    // REPORT_POSITION_FINAL ('Endanflug', conf 0.93) precedes the inbound
    // rule: the new 'information' matcher must not hijack a leg report even
    // when 'zur Landung' is also present.
    auto m = parse("Schwaebisch Hall Information, im Endanflug, "
                   "zur Landung, Information Bravo.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::REPORT_POSITION_FINAL);
}

TEST_CASE("DE: Verkehr in Sicht -> TRAFFIC_IN_SIGHT",
          "[intent][de][traffic]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Verkehr in Sicht, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::TRAFFIC_IN_SIGHT);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Negativer Kontakt -> TRAFFIC_NEGATIVE_CONTACT",
          "[intent][de][traffic]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Negativer Kontakt, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::TRAFFIC_NEGATIVE_CONTACT);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Halte Ausschau -> TRAFFIC_LOOKING",
          "[intent][de][traffic]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Halte Ausschau, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::TRAFFIC_LOOKING);
}

// ── Landing phase ────────────────────────────────────────────────────

TEST_CASE("DE: Piste frei -> RUNWAY_VACATED",
          "[intent][de][vacated]") {
    // RUNWAY_VACATED is physically impossible without a prior airborne
    // phase. The new was_airborne-veto adjustment demotes the bare
    // match to confidence 0.0 (LM repair path) otherwise. Realistic
    // scenario for this rule: pilot has been airborne and just rolled
    // out, so set the session-lifecycle flag here.
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_was_airborne(true);
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, Piste frei.", ctx);
    REQUIRE(m.intent == PilotIntent::RUNWAY_VACATED);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Piste verlassen -> RUNWAY_VACATED",
          "[intent][de][vacated]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_was_airborne(true);
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, habe die Piste verlassen.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::RUNWAY_VACATED);
    REQUIRE(m.confidence >= 0.85f);
}

// ── Readback / control flow ──────────────────────────────────────────

TEST_CASE("DE: Verstanden -> READBACK",
          "[intent][de][readback]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Verstanden, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::READBACK);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: QNH readback with ziffernweise -> READBACK",
          "[intent][de][readback][bzf]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse(
        "Information Alfa, QNH eins null eins drei, Delta Echo Whiskey Lima Yankee.",
        ctx);
    REQUIRE(m.intent == PilotIntent::READBACK);
}

TEST_CASE("DE: Frequenzwechsel announcement -> LEAVING_FREQUENCY",
          "[intent][de][frequency]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse(
        "Frequenzwechsel zu eins eins acht Komma drei null null, "
        "Delta Echo Whiskey Lima Yankee.",
        ctx);
    REQUIRE(m.intent == PilotIntent::LEAVING_FREQUENCY);
    REQUIRE(m.confidence >= 0.80f);
}

TEST_CASE("DE: Negativ correction -> NEGATIVE_CORRECTION",
          "[intent][de][correction]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Negativ, Delta Echo Whiskey Lima Yankee, "
                   "Piste 25 war falsch.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::NEGATIVE_CORRECTION);
    REQUIRE(m.confidence >= 0.85f);
}

// ── Feature extractors (runway + callsign) ───────────────────────────

TEST_CASE("DE: extract_runway from 'Piste zwo fuenf links'",
          "[intent][de][runway]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, am Rollhalt Piste zwo fuenf "
                   "links, abflugbereit.",
                   ctx);
    REQUIRE(m.runway == "25L");
}

TEST_CASE("DE: extract_callsign from D-EWLY spoken sequence",
          "[intent][de][callsign]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Friedrichshafen Boden, Delta Echo Whiskey Lima Yankee, "
                   "an der Tankstelle, erbitte Rollen.",
                   ctx);
    REQUIRE(m.callsign == "Delta Echo Whiskey Lima Yankee");
}

// ── Landefreigabe-Readback vs. Vacated-Mishearing ───────────────────
// Original-Bug (EDNY-Rundflug 2026-06-06): Pilot liest die
// Landefreigabe als "Landung frei, Piste 06, ..." zurueck, Whisper
// transkribiert sauber, der Rule-Parser kannte das Token aber nicht,
// LM-Repair-Pfad reparierte falsch zu "Piste 06 frei" und
// klassifizierte RUNWAY_VACATED. Phase-Guard schlug zu und Tower
// antwortete mit dem englischen "unable, you appear to still be
// airborne." aus flight_rules.json. Fix: READBACK-Rule um
// "landung frei" / "start frei" erweitern; gleichzeitig den
// Spiegel-Fall (Whisper verhoert ein korrekt gefunktes "Piste frei"
// als "Landung frei" im Post-Landing-Audio-Kontext) mit einem
// Adjustment auf require_context_flag=at_airport_after_landing
// zurueck demoten. Diese fuenf Faelle nageln das zusammengesetzte
// Rule + Adjustment Verhalten fest.

TEST_CASE("DE: 'Landung frei' airborne im LANDING_CLEARED -> READBACK",
          "[intent][de][readback][landung_frei]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_state(atc_state_machine::ATCState::LANDING_CLEARED);
    auto ctx = airborne_ctx();
    ctx.now_secs = 1.0;
    auto m = parse("Landung frei, Piste 06, Hotel Bravo Delta Sierra Victor.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::READBACK);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: 'Landung frei' post-landing kurz nach Touchdown -> "
          "RUNWAY_VACATED via Adjustment",
          "[intent][de][readback][landung_frei]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_state(atc_state_machine::ATCState::LANDING_CLEARED);
    atc_state_machine::set_state(atc_state_machine::ATCState::IDLE);
    auto ctx = ground_ctx();
    ctx.now_secs = 30.0; // just_landed window still active
    REQUIRE(atc_state_machine::just_landed(ctx.now_secs));
    REQUIRE(atc_state_machine::at_airport_after_landing(ctx));
    auto m = parse("Landung frei, Piste 06, Delta Sierra Victor.", ctx);
    REQUIRE(m.intent == PilotIntent::RUNWAY_VACATED);
}

TEST_CASE("DE: 'Landung frei' 2:30 nach Touchdown -> RUNWAY_VACATED "
          "(just_landed expired, at_airport_after_landing lebt weiter)",
          "[intent][de][readback][landung_frei]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_state(atc_state_machine::ATCState::LANDING_CLEARED);
    atc_state_machine::set_state(atc_state_machine::ATCState::IDLE);
    auto ctx = ground_ctx();
    ctx.now_secs = 150.0; // > 120 s default just_landed window
    REQUIRE_FALSE(atc_state_machine::just_landed(ctx.now_secs));
    REQUIRE(atc_state_machine::at_airport_after_landing(ctx));
    auto m = parse("Landung frei, Piste 06, Delta Sierra Victor.", ctx);
    REQUIRE(m.intent == PilotIntent::RUNWAY_VACATED);
}

TEST_CASE("DE: 'Piste 06 frei' korrekt nach Landung -> RUNWAY_VACATED "
          "(direkter Rule-Match, kein Adjustment)",
          "[intent][de][runway_vacated]") {
    DeRegionGuard g;
    atc_state_machine::init();
    // Realistic post-landing setup: pilot has been airborne, landed,
    // and is now rolling out. was_airborne stays true across the
    // LANDING_CLEARED -> IDLE transition because the new departure
    // cycle hook only fires on REQUEST_TAXI / INITIAL_CALL_*.
    atc_state_machine::set_was_airborne(true);
    atc_state_machine::set_state(atc_state_machine::ATCState::LANDING_CLEARED);
    atc_state_machine::set_state(atc_state_machine::ATCState::IDLE);
    auto ctx = ground_ctx();
    ctx.now_secs = 30.0;
    auto m = parse("Piste 06 frei, Delta Sierra Victor.", ctx);
    REQUIRE(m.intent == PilotIntent::RUNWAY_VACATED);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: 'Start frei' airborne nach Departure -> READBACK",
          "[intent][de][readback][start_frei]") {
    DeRegionGuard g;
    atc_state_machine::init();
    atc_state_machine::set_state(
        atc_state_machine::ATCState::DEPARTURE_CLEARED);
    auto ctx = airborne_ctx();
    ctx.now_secs = 1.0;
    auto m = parse("Start frei, Piste 25, Delta Sierra Victor.", ctx);
    REQUIRE(m.intent == PilotIntent::READBACK);
    REQUIRE(m.confidence >= 0.85f);
}

// ── Flugzeugtyp im Erstanruf-Hint ───────────────────────────────────
// NfL 2024 Anlage 1.4.4 listet "Luftfahrzeugmuster" als typisches
// Element des VFR-Erstanrufs an Tower / Boden. Wir spielen es als
// optionale Variable {aircraft_type_phrase} aus build_vars in die
// pilot_phraseology-Templates ein: gefuellt mit ", DV20" wenn
// ctx.aircraft_icao gesetzt ist, leer wenn nicht (Cold-Start-Race
// oder Payware-Liveries ohne acf_ICAO DataRef).

TEST_CASE("DE: build_vars liefert aircraft_type_phrase wenn acf_ICAO gesetzt",
          "[ground_ops][aircraft_type]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    ctx.aircraft_icao = "DV20";
    intent_parser::PilotMessage msg{};
    auto vars = ground_ops::build_vars(msg, ctx);
    REQUIRE(vars.at("aircraft_type") == "DV20");
    REQUIRE(vars.at("aircraft_type_phrase") == ", DV20");
}

TEST_CASE("DE: build_vars liefert leeres aircraft_type_phrase wenn "
          "acf_ICAO leer ist",
          "[ground_ops][aircraft_type]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    ctx.aircraft_icao.clear();
    intent_parser::PilotMessage msg{};
    auto vars = ground_ops::build_vars(msg, ctx);
    REQUIRE(vars.at("aircraft_type").empty());
    REQUIRE(vars.at("aircraft_type_phrase").empty());
}

// ── DE Position-Marker im Erstanruf ─────────────────────────────────
// User-Log EDNY 2026-06-06: Pilot funkt "in Parkposition, erbitte
// Rollen" und der Tower antwortet trotzdem mit "Sagen Sie Ihre
// Position." weil detect_has_position() nur englische Marker kannte.
// "Parkposition" (zusammengeschrieben) matched keinen englischen
// Substring. Die neue DE-Marker-Liste deckt die typischen BZF-
// Erstanruf-Positionen vor dem Rollen ab.

TEST_CASE("DE: 'in Parkposition' setzt has_position",
          "[intent][de][position_marker]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Friedrichshafen Turm, Hotel Bravo Delta Sierra Victor, "
                   "in Parkposition, erbitte Rollen, Information Alpha.",
                   ctx);
    REQUIRE(m.intent == intent_parser::PilotIntent::REQUEST_TAXI);
    REQUIRE(m.has_position);
}

TEST_CASE("DE: 'am Vorfeld' setzt has_position",
          "[intent][de][position_marker]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Friedrichshafen Boden, Delta Echo Whiskey Lima Yankee, "
                   "am Vorfeld, erbitte Rollen.",
                   ctx);
    REQUIRE(m.has_position);
}

TEST_CASE("DE: 'GA-Vorfeld' setzt has_position",
          "[intent][de][position_marker]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Tower, HB-DSV, DV20, GA-Vorfeld, Information Alpha, "
                   "erbitte Rollen.",
                   ctx);
    REQUIRE(m.has_position);
}

TEST_CASE("DE: 'an der Tankstelle' setzt has_position",
          "[intent][de][position_marker]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Friedrichshafen Boden, Delta Echo Whiskey Lima Yankee, "
                   "an der Tankstelle, Information Alfa.",
                   ctx);
    REQUIRE(m.has_position);
}

TEST_CASE("DE: Erstanruf OHNE Position laesst has_position=false",
          "[intent][de][position_marker]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Friedrichshafen Boden, Hotel Bravo Delta Sierra Victor, "
                   "erbitte Rollen.",
                   ctx);
    REQUIRE_FALSE(m.has_position);
}

// ── Instruktions-Echo darf nicht als Position-Report durchgehen ─────
// User-Log EDNY 2026-06-06 (Flug 2): Pilot funkt Readback der
// Tower-Anweisung "melden Sie Endanflug" und das matched fälschlich
// REPORT_POSITION_FINAL (conf 0.93) — Tower gab daraufhin die
// Landefreigabe, bevor der Pilot wirklich im Endanflug war. Die
// REPORT_POSITION_*-Rules schliessen jetzt "melden sie" (Sie-Form
// Imperativ) bzw. "report" (EN-Pendant) aus, damit ein
// Instruktions-Echo zu READBACK fällt.

TEST_CASE("DE: 'Melden Sie Endanflug' Readback -> READBACK, nicht REPORT_FINAL",
          "[intent][de][readback][instruction_echo]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Melden Sie Endanflug, Hotel Bravo Delta Sierra Victor.",
                   ctx);
    REQUIRE(m.intent != intent_parser::PilotIntent::REPORT_POSITION_FINAL);
    REQUIRE(m.intent == intent_parser::PilotIntent::READBACK);
}

TEST_CASE("DE: 'Melden Sie Gegenanflug' Readback -> READBACK",
          "[intent][de][readback][instruction_echo]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Melden Sie Gegenanflug, Hotel Bravo Delta Sierra Victor.",
                   ctx);
    REQUIRE(m.intent != intent_parser::PilotIntent::REPORT_POSITION_DOWNWIND);
    REQUIRE(m.intent == intent_parser::PilotIntent::READBACK);
}

TEST_CASE("DE: Echte Endanflug-Meldung ohne 'melden sie' bleibt FINAL",
          "[intent][de][position]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Friedrichshafen Turm, Hotel Bravo Delta Sierra Victor, "
                   "Endanflug Piste 24.",
                   ctx);
    REQUIRE(m.intent == intent_parser::PilotIntent::REPORT_POSITION_FINAL);
}

// ── 1.-Person-Meldung "melde X" ist Report, kein Readback ────────────
// User-Log EDNY 2026-06-21: Pilot funkt "melde Endanflug Piste 06"; die
// READBACK-Regel fing das mit dem Verb-Set [melden,melde] ab und
// lieferte eine stille Antwort -> keine Landefreigabe. Der Sie-Imperativ
// "melden sie" (Tower-Echo) bleibt READBACK, die Ich-Form "melde" muss
// zu REPORT_POSITION_*/READY_FOR_DEPARTURE durchfallen. Systematisch
// ueber alle drei Positionsmeldungen + die Abflugbereitschaft.

TEST_CASE("DE: 'melde Endanflug' (Ich-Form) -> REPORT_POSITION_FINAL",
          "[intent][de][position]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Friedrichshafen Turm, Delta Whiskey Echo Alfa Victor, "
                   "melde Endanflug Piste 06.",
                   ctx);
    REQUIRE(m.intent == intent_parser::PilotIntent::REPORT_POSITION_FINAL);
}

TEST_CASE("DE: 'melde Gegenanflug' (Ich-Form) -> REPORT_POSITION_DOWNWIND",
          "[intent][de][position]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Friedrichshafen Turm, Delta Whiskey Echo Alfa Victor, "
                   "melde Gegenanflug Piste 06.",
                   ctx);
    REQUIRE(m.intent == intent_parser::PilotIntent::REPORT_POSITION_DOWNWIND);
}

TEST_CASE("DE: 'melde Queranflug' (Ich-Form) -> REPORT_POSITION_BASE",
          "[intent][de][position]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Friedrichshafen Turm, Delta Whiskey Echo Alfa Victor, "
                   "melde Queranflug Piste 06.",
                   ctx);
    REQUIRE(m.intent == intent_parser::PilotIntent::REPORT_POSITION_BASE);
}

TEST_CASE("DE: 'melde abflugbereit' (Ich-Form) -> READY_FOR_DEPARTURE",
          "[intent][de][departure]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Whiskey Echo Alfa Victor, am Rollhalt Piste 06, "
                   "melde abflugbereit.",
                   ctx);
    REQUIRE(m.intent == intent_parser::PilotIntent::READY_FOR_DEPARTURE);
}

// ── DoD aggregate check ──────────────────────────────────────────────
// Sanity that the parser does NOT regress to LM-bound UNKNOWN on the
// 20 BZF cases above; Catch2 counts each TEST_CASE individually, so
// the DoD ">= 70%" is satisfied by having >= 14 of the high-confidence
// tests above pass. This aggregate test_case is a backstop that
// double-checks the most discriminating phrases are not UNKNOWN.
TEST_CASE("DE: aggregate sanity -- core BZF phrases never classify as UNKNOWN",
          "[intent][de][aggregate]") {
    DeRegionGuard g;
    auto ctx_g = ground_ctx();
    auto ctx_a = airborne_ctx();

    REQUIRE(parse("Funkpruefung", ctx_g).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("Durchstarten", ctx_a).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("Verstanden", ctx_g).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("im Gegenanflug", ctx_a).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("im Queranflug", ctx_a).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("im Endanflug", ctx_a).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("Piste frei", ctx_g).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("Verkehr in Sicht", ctx_a).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("abflugbereit Piste 24", ctx_g).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("Friedrichshafen Turm", ctx_g).intent != PilotIntent::UNKNOWN);
}
