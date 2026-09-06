#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

using intent_parser::intent_from_key;
using intent_parser::intent_name;
using intent_parser::intent_template_key;
using intent_parser::parse;
using intent_parser::PilotIntent;

// ── Enum ↔ string mappings ───────────────────────────────────────────────────

TEST_CASE("intent_name: known intents return their canonical key", "[intent][name]")
{
    REQUIRE(std::string(intent_name(PilotIntent::UNKNOWN)) == "UNKNOWN");
    REQUIRE(std::string(intent_name(PilotIntent::RADIO_CHECK)) == "RADIO_CHECK");
    REQUIRE(std::string(intent_name(PilotIntent::REQUEST_TAXI)) == "REQUEST_TAXI");
    REQUIRE(std::string(intent_name(PilotIntent::REPORT_POSITION_FINAL)) == "REPORT_POSITION_FINAL");
}

// intent_template_key collapses the generic INITIAL_CALL onto the tower variant
// so JSON template lookup always resolves to a concrete template.
TEST_CASE("intent_template_key: generic INITIAL_CALL maps to tower variant", "[intent][template]")
{
    REQUIRE(std::string(intent_template_key(PilotIntent::INITIAL_CALL)) == "INITIAL_CALL_TOWER");
}

TEST_CASE("intent_template_key: sub-variants pass through unchanged", "[intent][template]")
{
    REQUIRE(std::string(intent_template_key(PilotIntent::INITIAL_CALL_GROUND)) == "INITIAL_CALL_GROUND");
    REQUIRE(std::string(intent_template_key(PilotIntent::REPORT_POSITION_DOWNWIND)) == "REPORT_POSITION_DOWNWIND");
}

TEST_CASE("intent_from_key: round-trips intent_name for known keys", "[intent][from_key]")
{
    REQUIRE(intent_from_key("RADIO_CHECK") == PilotIntent::RADIO_CHECK);
    REQUIRE(intent_from_key("REQUEST_TAXI") == PilotIntent::REQUEST_TAXI);
    REQUIRE(intent_from_key("GO_AROUND") == PilotIntent::GO_AROUND);
}

TEST_CASE("intent_from_key: unknown keys fall back to UNKNOWN", "[intent][from_key]")
{
    REQUIRE(intent_from_key("") == PilotIntent::UNKNOWN);
    REQUIRE(intent_from_key("NOT_A_REAL_INTENT") == PilotIntent::UNKNOWN);
}

// Helper: minimal airborne ctx (no airport context required for facility-keyword
// classification tests).
static xplane_context::XPlaneContext airborne_ctx()
{
    xplane_context::XPlaneContext ctx;
    ctx.on_ground = false;
    ctx.facility_type = xplane_context::FacilityType::TOWERED;
    return ctx;
}

// Whisper occasionally prefixes the first utterance with a dash ("-Turm, ...").
// REQUEST_LANDING excludes transcripts that mention the tower facility ("turm")
// so the initial inbound call routes to INITIAL_CALL_INBOUND. The exclusion
// broke when has_facility only handled clean word boundaries — leading
// punctuation made "turm" invisible to the guard, and the call was
// misclassified as REQUEST_LANDING (which then hits _INVALID from IDLE).
TEST_CASE("parse: leading-dash 'Turm' still routes to INITIAL_CALL_INBOUND", "[intent][parse]")
{
    auto ctx = airborne_ctx();

    auto m1 = parse("-Turm, Hotel Bravo Delta Charlie Hotel, Anflug Piste 06, zur Landung.", ctx);
    REQUIRE(m1.intent == PilotIntent::INITIAL_CALL_INBOUND);

    auto m2 = parse("Turm, Hotel Bravo Delta Charlie Hotel, Anflug Piste 06, zur Landung.", ctx);
    REQUIRE(m2.intent == PilotIntent::INITIAL_CALL_INBOUND);

    // Without facility keyword the same body should route to REQUEST_LANDING.
    auto m3 = parse("Hotel Bravo Delta Charlie Hotel, Anflug Piste 06, zur Landung.", ctx);
    REQUIRE(m3.intent == PilotIntent::REQUEST_LANDING);
}

TEST_CASE("parse: facility keyword is punctuation-tolerant", "[intent][parse]")
{
    auto ctx = airborne_ctx();
    ctx.on_ground = true;

    // Leading-comma artifact still detects "boden" as facility.
    auto m = parse(",Boden, Hotel Bravo Delta Charlie Hotel, erbitte Rollen Piste 14.", ctx);
    REQUIRE((m.intent == PilotIntent::INITIAL_CALL_GROUND ||
             m.intent == PilotIntent::REQUEST_TAXI));
}

// Pilot's clearance readback after Tower issues "freigegeben in die
// Kontrollzone, Piste X, ..." must classify as READBACK rather than UNKNOWN.
// Whisper sometimes drops the leading word (logged as "-Kontrollzone Piste 32
// Delta Charlie Hotel"), and the standalone "QNH" readback must also land on
// READBACK rather than be left to the LM (which could misclassify it as a
// position report).
TEST_CASE("parse: control zone clearance readback classifies as READBACK", "[intent][parse][readback]")
{
    auto ctx = airborne_ctx();

    auto m1 = parse("freigegeben in die Kontrollzone Piste 32 Delta Charlie Hotel", ctx);
    REQUIRE(m1.intent == PilotIntent::READBACK);

    auto m2 = parse("-Kontrollzone Piste 32 Delta Charlie Hotel", ctx);
    REQUIRE(m2.intent == PilotIntent::READBACK);

    auto m3 = parse("Piste 32, QNH 1013, Delta Charlie Hotel", ctx);
    REQUIRE(m3.intent == PilotIntent::READBACK);
}
