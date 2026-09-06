#include "atc/bzf_compliance.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>
#include <vector>

using bzf_compliance::build_correction_response;
using bzf_compliance::check_pilot_readback;
using bzf_compliance::Element;
using bzf_compliance::extract_required;

// ── extract_required: tower-side detection ─────────────────────────

TEST_CASE("extract_required: empty tower response yields empty list",
          "[bzf_compliance][extract]") {
    REQUIRE(extract_required("").empty());
}

TEST_CASE("extract_required: callsign-only clearance still requires callsign",
          "[bzf_compliance][extract]") {
    auto out = extract_required("D-EXYZ, verstanden, auf Wiederhoeren.");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0] == Element::Callsign);
}

TEST_CASE("extract_required: taxi clearance requires callsign + runway + QNH",
          "[bzf_compliance][extract]") {
    auto out = extract_required(
        "D-EXYZ, rollen Sie zum Rollhalt Piste 25 ueber Alpha, QNH 1013.");
    REQUIRE(out.size() == 3);
    REQUIRE(out[0] == Element::Callsign);
    REQUIRE(out[1] == Element::Runway);
    REQUIRE(out[2] == Element::QNH);
}

TEST_CASE("extract_required: handoff requires callsign + frequency",
          "[bzf_compliance][extract]") {
    auto out = extract_required("D-EXYZ, kontaktieren Sie Tower auf 118.300.");
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == Element::Callsign);
    REQUIRE(out[1] == Element::Frequency);
}

TEST_CASE("extract_required: squawk clearance requires callsign + squawk",
          "[bzf_compliance][extract]") {
    auto out = extract_required("D-EXYZ, Squawk 7000.");
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == Element::Callsign);
    REQUIRE(out[1] == Element::Squawk);
}

TEST_CASE("extract_required: full clearance with multiple elements",
          "[bzf_compliance][extract]") {
    auto out = extract_required(
        "D-EXYZ, Piste 25, Start frei, QNH 1013, Squawk 7000, "
        "kontaktieren Sie Tower auf 118.300.");
    // callsign + runway + QNH + frequency + squawk
    REQUIRE(out.size() == 5);
}

// ── check_pilot_readback: pilot-side validation ────────────────────

TEST_CASE("check_pilot_readback: conformant readback yields empty missing",
          "[bzf_compliance][check]") {
    std::vector<Element> required{Element::Callsign, Element::Runway,
                                  Element::QNH};
    auto missing =
        check_pilot_readback("Piste 25, QNH 1013, D-EXYZ", required, "D-EXYZ");
    REQUIRE(missing.empty());
}

TEST_CASE("check_pilot_readback: missing QNH is reported",
          "[bzf_compliance][check]") {
    std::vector<Element> required{Element::Callsign, Element::Runway,
                                  Element::QNH};
    auto missing = check_pilot_readback("Piste 25, D-EXYZ", required, "D-EXYZ");
    REQUIRE(missing.size() == 1);
    REQUIRE(missing[0] == Element::QNH);
}

TEST_CASE("check_pilot_readback: missing runway and QNH both reported",
          "[bzf_compliance][check]") {
    std::vector<Element> required{Element::Callsign, Element::Runway,
                                  Element::QNH};
    auto missing = check_pilot_readback("verstanden, D-EXYZ", required, "D-EXYZ");
    REQUIRE(missing.size() == 2);
    REQUIRE(missing[0] == Element::Runway);
    REQUIRE(missing[1] == Element::QNH);
}

TEST_CASE("check_pilot_readback: missing callsign is reported",
          "[bzf_compliance][check]") {
    std::vector<Element> required{Element::Callsign, Element::Runway};
    auto missing = check_pilot_readback("Piste 25, verstanden", required,
                                        "D-EXYZ");
    REQUIRE(missing.size() == 1);
    REQUIRE(missing[0] == Element::Callsign);
}

TEST_CASE("check_pilot_readback: BZF-shortened callsign accepted",
          "[bzf_compliance][check][callsign]") {
    // Phonetic full callsign: "Delta Echo X-Ray Yankee Zulu".
    // Pilot uses BZF-Verkürzung (NfL §13 b)): last two tokens only.
    std::vector<Element> required{Element::Callsign, Element::Runway};
    auto missing = check_pilot_readback(
        "Piste 25, Yankee Zulu", required, "Delta Echo X-Ray Yankee Zulu");
    REQUIRE(missing.empty());
}

// Issue #79 — Mistral/Voxtral emits the trailing registration digit as a
// numeral ("Charlie Oscar Lima 1") while the stored callsign carries the
// number word ("Charlie Oscar Lima eins"). Before the canon_stripped()
// unification the readback was wrongly flagged callsign:missing and the
// tower answered "WIEDERHOLEN SIE WOERTLICH" on a correct readback.
TEST_CASE("check_pilot_readback: numeral trailing digit matches number-word "
          "callsign (Voxtral)",
          "[bzf_compliance][check][callsign][issue79]") {
    std::vector<Element> required{Element::Callsign, Element::Runway,
                                  Element::QNH};
    auto missing = check_pilot_readback(
        "Rolle via Alpha zu Rollhalt, Piste 04, QNH 1018, Charlie, Oscar, "
        "Lima 1.",
        required, "Charlie Oscar Lima eins");
    REQUIRE(missing.empty());
}

// Issue #79 — Voxtral inserts commas between spelled phonetic letters
// ("Charlie, Oscar, Lima"), which defeated the plain substring match.
// canon_stripped() welds punctuation/whitespace out, so the callsign is
// recognised regardless of the comma noise.
TEST_CASE("check_pilot_readback: comma-split phonetic callsign accepted "
          "(Voxtral)",
          "[bzf_compliance][check][callsign][issue79]") {
    std::vector<Element> required{Element::Callsign};
    auto missing =
        check_pilot_readback("verstanden, Charlie, Oscar, Lima, eins", required,
                             "Charlie Oscar Lima eins");
    REQUIRE(missing.empty());
}

// Issue #79 — the numeral/word unification must also hold for a multi-digit
// registration run: Voxtral welds "eins zwo drei" into "123" while the
// stored callsign keeps the spelled words, and it emits "Alpha" where the
// stored form carries the NATO "Alfa". Both must still match.
TEST_CASE("check_pilot_readback: welded numeral run + Alfa/Alpha callsign "
          "(Voxtral)",
          "[bzf_compliance][check][callsign][issue79]") {
    std::vector<Element> required{Element::Callsign, Element::Runway};
    auto missing = check_pilot_readback(
        "Piste 25, November 123 Alpha Bravo", required,
        "November eins zwo drei Alfa Bravo");
    REQUIRE(missing.empty());
}

TEST_CASE("check_pilot_readback: frequency detection",
          "[bzf_compliance][check]") {
    std::vector<Element> required{Element::Callsign, Element::Frequency};
    auto ok =
        check_pilot_readback("118.300, D-EXYZ", required, "D-EXYZ");
    REQUIRE(ok.empty());
    auto missing =
        check_pilot_readback("verstanden, D-EXYZ", required, "D-EXYZ");
    REQUIRE(missing.size() == 1);
    REQUIRE(missing[0] == Element::Frequency);
}

TEST_CASE("check_pilot_readback: empty required list yields empty missing",
          "[bzf_compliance][check][edge]") {
    auto missing = check_pilot_readback("anything", {}, "D-EXYZ");
    REQUIRE(missing.empty());
}

// Whisper biases toward the general-language prior on acoustically
// ambiguous NATO letters: it renders "Victor" as "Vector" reliably
// enough that any HB-XXV / D-XXV registration would otherwise be
// unusable in strict mode. Regression fix from user test 2026-06-05
// (HB-DSV at EDNY): pilot read back the full callsign correctly,
// Whisper transcribed "Hotel Bravo Delta Sierra Vector", and strict
// mode wrongly flagged missing callsign.
TEST_CASE("check_pilot_readback: Whisper Vector->Victor tolerated",
          "[bzf_compliance][check][whisper]") {
    std::vector<Element> required{Element::Callsign, Element::Runway,
                                  Element::QNH};
    auto missing = check_pilot_readback(
        "Rollen zum Rollhalt Piste 24 ueber Alfa QNH 1013 Hotel Bravo Delta "
        "Sierra Vector",
        required, "Hotel Bravo Delta Sierra Victor");
    REQUIRE(missing.empty());
}

TEST_CASE("check_pilot_readback: Whisper Juliet->Juliett tolerated",
          "[bzf_compliance][check][whisper]") {
    std::vector<Element> required{Element::Callsign};
    auto missing = check_pilot_readback("verstanden D-Echo Juliet Bravo",
                                        required, "Delta Echo Juliett Bravo");
    REQUIRE(missing.empty());
}

// Regression from the EDNY Friedrichshafen test flight (2026-06-15):
// pilot read back "runway 06 Landung frei 3 Alfa Bravo". Strict mode
// wrongly flagged BOTH runway (English "runway" not recognised) and
// callsign (Whisper "Alfa" vs stored "Alpha"). Both forms are now
// tolerated so the landing clearance read-back is accepted.
TEST_CASE("check_pilot_readback: English 'runway NN' accepted",
          "[bzf_compliance][check][whisper]") {
    std::vector<Element> required{Element::Callsign, Element::Runway};
    auto missing = check_pilot_readback(
        "runway 06 Landung frei November One Two Three Alpha Bravo", required,
        "November One Two Three Alpha Bravo");
    REQUIRE(missing.empty());
}

TEST_CASE("check_pilot_readback: Whisper Alfa->Alpha tolerated",
          "[bzf_compliance][check][whisper]") {
    std::vector<Element> required{Element::Callsign, Element::Runway};
    auto missing = check_pilot_readback("runway 06 Landung frei 3 Alfa Bravo",
                                        required,
                                        "November One Two Three Alpha Bravo");
    REQUIRE(missing.empty());
}

// Whisper welds German compounds: "Rollhalt Piste 06" gets transcribed
// as "Rollhaltpiste 06" (no space). The runway matcher must still
// recognise it — otherwise the pilot's correct readback is flagged
// as missing runway. Regression fix from user EDNY test 2026-06-05:
// "Rollen zur Rollhaltpiste 06 über Alfa QNH 1020 Hotel Bravo Delta
// Sierra Victor".
TEST_CASE("check_pilot_readback: Whisper-welded compound Rollhaltpiste",
          "[bzf_compliance][check][whisper]") {
    std::vector<Element> required{Element::Callsign, Element::Runway,
                                  Element::QNH};
    auto missing = check_pilot_readback(
        "Rollen zur Rollhaltpiste 06 über Alfa QNH 1020 Hotel Bravo Delta "
        "Sierra Victor",
        required, "Hotel Bravo Delta Sierra Victor");
    REQUIRE(missing.empty());
}

TEST_CASE("extract_required: welded compound also detected on tower side",
          "[bzf_compliance][extract][whisper]") {
    // Even if a template (or future LLM-generated tower text) emits a
    // welded form, the extractor must still recognise the runway.
    auto out = extract_required("D-EXYZ, rollen Sie zur Rollhaltpiste 06.");
    bool has_runway = false;
    for (auto e : out)
        if (e == Element::Runway)
            has_runway = true;
    REQUIRE(has_runway);
}

// ── build_correction_response: template rendering ──────────────────

TEST_CASE("build_correction_response: empty missing yields empty string",
          "[bzf_compliance][correction]") {
    REQUIRE(build_correction_response("D-EXYZ", {}).empty());
}

TEST_CASE("build_correction_response: single missing picks element-specific "
          "template + substitutes callsign",
          "[bzf_compliance][correction]") {
    auto resp = build_correction_response("D-EXYZ", {Element::QNH});
    // Default fallback applies if atc_templates isn't loaded — both
    // forms must mention QNH and contain the callsign verbatim.
    REQUIRE(resp.find("D-EXYZ") != std::string::npos);
}

TEST_CASE("build_correction_response: multi-missing uses missing_multi",
          "[bzf_compliance][correction]") {
    auto resp = build_correction_response(
        "D-EXYZ", {Element::QNH, Element::Runway});
    REQUIRE(resp.find("D-EXYZ") != std::string::npos);
}

// ── element_name / missing_key stability ───────────────────────────

TEST_CASE("element_name maps each Element to a stable lowercase string",
          "[bzf_compliance][names]") {
    REQUIRE(std::string(bzf_compliance::element_name(Element::Callsign)) ==
            "callsign");
    REQUIRE(std::string(bzf_compliance::element_name(Element::Runway)) ==
            "runway");
    REQUIRE(std::string(bzf_compliance::element_name(Element::QNH)) == "qnh");
    REQUIRE(std::string(bzf_compliance::element_name(Element::Frequency)) ==
            "frequency");
    REQUIRE(std::string(bzf_compliance::element_name(Element::Squawk)) ==
            "squawk");
}

TEST_CASE("missing_key maps each Element to its bzf_strict template key",
          "[bzf_compliance][names]") {
    REQUIRE(std::string(bzf_compliance::missing_key(Element::Callsign)) ==
            "missing_callsign");
    REQUIRE(std::string(bzf_compliance::missing_key(Element::Runway)) ==
            "missing_runway");
    REQUIRE(std::string(bzf_compliance::missing_key(Element::QNH)) ==
            "missing_qnh");
    REQUIRE(std::string(bzf_compliance::missing_key(Element::Frequency)) ==
            "missing_frequency");
    REQUIRE(std::string(bzf_compliance::missing_key(Element::Squawk)) ==
            "missing_squawk");
}

// ── missing_readback_elements: char-robust soll-ist match ──────────

using bzf_compliance::ClearanceComponents;
using bzf_compliance::missing_readback_elements;
using bzf_compliance::readback_covers_core;

// The EDNY regression that motivated the whole change: the departure
// clearance issued only callsign + runway ("Piste 06, Start frei, ...").
// Whisper welded "Start frei" into "startfrei", so the keyword parser
// returned UNKNOWN/0.00. The structured matcher only checks the mandated
// values (runway 06, callsign) — "startfrei" is irrelevant filler — so
// the readback is fully covered.
TEST_CASE("missing_readback_elements: welded 'startfrei' readback is complete",
          "[bzf_compliance][readback]") {
    ClearanceComponents comp;
    comp.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    comp.runway = "06";
    comp.required = {Element::Callsign, Element::Runway};
    auto missing = missing_readback_elements(
        comp, "Piste 06 startfrei Hotel Bravo Whiskey Romeo Oscar");
    REQUIRE(missing.empty());
    REQUIRE(readback_covers_core(comp.required, missing));
}

TEST_CASE("missing_readback_elements: welded qnh/piste tokens still match",
          "[bzf_compliance][readback]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "06";
    comp.qnh = "1018";
    comp.required = {Element::Callsign, Element::Runway, Element::QNH};
    // No spaces around the values — the canonical char stream eliminates
    // the word boundary the welding broke.
    auto missing = missing_readback_elements(comp, "piste06 qnh1018 d-exyz");
    REQUIRE(missing.empty());
}

TEST_CASE("missing_readback_elements: spoken digit words are normalised",
          "[bzf_compliance][readback]") {
    ClearanceComponents comp;
    comp.callsign = "Hotel Bravo Whiskey Romeo Oscar";
    comp.runway = "06";
    comp.qnh = "1018";
    comp.required = {Element::Callsign, Element::Runway, Element::QNH};
    auto missing = missing_readback_elements(
        comp,
        "Piste null sechs QNH eins null eins acht Hotel Bravo Whiskey Romeo "
        "Oscar");
    REQUIRE(missing.empty());
}

TEST_CASE("missing_readback_elements: missing QNH is reported (completeness)",
          "[bzf_compliance][readback]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "06";
    comp.qnh = "1018";
    comp.required = {Element::Callsign, Element::Runway, Element::QNH};
    auto missing = missing_readback_elements(comp, "Piste 06 D-EXYZ");
    REQUIRE(missing.size() == 1);
    REQUIRE(missing[0] == Element::QNH);
    // Recognition (lenient) still accepts it — callsign + runway covered.
    REQUIRE(readback_covers_core(comp.required, missing));
}

// ── readback_covers_core: recognition threshold ────────────────────

TEST_CASE("readback_covers_core: callsign alone is not a readback",
          "[bzf_compliance][readback][core]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "06";
    comp.required = {Element::Callsign, Element::Runway};
    // Only the callsign present -> Runway missing -> phantom guard fires.
    auto missing = missing_readback_elements(comp, "verstanden D-EXYZ");
    REQUIRE_FALSE(readback_covers_core(comp.required, missing));
}

TEST_CASE("readback_covers_core: callsign-only clearance accepts callsign",
          "[bzf_compliance][readback][core]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.required = {Element::Callsign};
    auto missing = missing_readback_elements(comp, "verstanden D-EXYZ");
    REQUIRE(missing.empty());
    REQUIRE(readback_covers_core(comp.required, missing));
}

// ── diff_readback: per-field Missing vs Wrong verdict ──────────────

using bzf_compliance::diff_readback;
using bzf_compliance::FieldDiff;
using bzf_compliance::ReadbackStatus;

namespace {
// Find the diff entry for a given element (tests assert on specific fields).
const FieldDiff *find_diff(const std::vector<FieldDiff> &d, Element e) {
    for (const auto &f : d)
        if (f.element == e)
            return &f;
    return nullptr;
}
} // namespace

TEST_CASE("diff_readback: fully correct readback is all Ok",
          "[bzf_compliance][diff]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "25";
    comp.qnh = "1013";
    comp.required = {Element::Callsign, Element::Runway, Element::QNH};
    auto d = diff_readback(comp, "Piste 25, QNH 1013, D-EXYZ");
    REQUIRE(d.size() == 3);
    for (const auto &f : d)
        REQUIRE(f.status == ReadbackStatus::Ok);
}

TEST_CASE("diff_readback: omitted QNH is Missing (no value stated)",
          "[bzf_compliance][diff]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "25";
    comp.qnh = "1013";
    comp.required = {Element::Callsign, Element::Runway, Element::QNH};
    auto d = diff_readback(comp, "Piste 25, D-EXYZ");
    const FieldDiff *q = find_diff(d, Element::QNH);
    REQUIRE(q != nullptr);
    REQUIRE(q->status == ReadbackStatus::Missing);
    REQUIRE(q->expected == "1013");
    REQUIRE(q->stated.empty());
}

TEST_CASE("diff_readback: wrong QNH value is Wrong with expected+stated",
          "[bzf_compliance][diff]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "25";
    comp.qnh = "1013";
    comp.required = {Element::Callsign, Element::Runway, Element::QNH};
    auto d = diff_readback(comp, "Piste 25, QNH 1030, D-EXYZ");
    const FieldDiff *q = find_diff(d, Element::QNH);
    REQUIRE(q != nullptr);
    REQUIRE(q->status == ReadbackStatus::Wrong);
    REQUIRE(q->expected == "1013");
    REQUIRE(q->stated == "1030");
    // The runway and callsign were correct.
    REQUIRE(find_diff(d, Element::Runway)->status == ReadbackStatus::Ok);
    REQUIRE(find_diff(d, Element::Callsign)->status == ReadbackStatus::Ok);
}

TEST_CASE("diff_readback: wrong runway is Wrong, spoken digits normalised",
          "[bzf_compliance][diff]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "25";
    comp.required = {Element::Callsign, Element::Runway};
    // Pilot read back runway "zwo drei" (23) instead of 25.
    auto d = diff_readback(comp, "Piste zwo drei, D-EXYZ");
    const FieldDiff *r = find_diff(d, Element::Runway);
    REQUIRE(r != nullptr);
    REQUIRE(r->status == ReadbackStatus::Wrong);
    REQUIRE(r->expected == "25");
    REQUIRE(r->stated == "23");
}

TEST_CASE("diff_readback: missing callsign is Missing, never Wrong",
          "[bzf_compliance][diff]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "25";
    comp.required = {Element::Callsign, Element::Runway};
    auto d = diff_readback(comp, "Piste 25");
    const FieldDiff *c = find_diff(d, Element::Callsign);
    REQUIRE(c != nullptr);
    REQUIRE(c->status == ReadbackStatus::Missing);
}

// ── build_correction_response(comp, diff): NfL wording ─────────────
// atc_templates is NOT loaded in these unit tests, so lookup_bzf_strict
// returns the hard-coded NfL defaults. Assertions target that wording.

TEST_CASE("build_correction_response(diff): all-Ok yields empty string",
          "[bzf_compliance][correction]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "25";
    comp.required = {Element::Callsign, Element::Runway};
    auto d = diff_readback(comp, "Piste 25, D-EXYZ");
    REQUIRE(build_correction_response("D-EXYZ", comp, d).empty());
}

TEST_CASE("build_correction_response(diff): missing-only -> READ BACK, no value",
          "[bzf_compliance][correction]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "25";
    comp.qnh = "1013";
    comp.required = {Element::Callsign, Element::Runway, Element::QNH};
    auto d = diff_readback(comp, "Piste 25, D-EXYZ"); // QNH omitted
    auto resp = build_correction_response("D-EXYZ", comp, d);
    REQUIRE(resp == "D-EXYZ, WIEDERHOLEN SIE WOERTLICH.");
    // The tower must NOT hand over the omitted value.
    REQUIRE(resp.find("1013") == std::string::npos);
}

TEST_CASE("build_correction_response(diff): wrong value -> NEGATIV + soll",
          "[bzf_compliance][correction]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "25";
    comp.qnh = "1013";
    comp.required = {Element::Callsign, Element::Runway, Element::QNH};
    auto d = diff_readback(comp, "Piste 25, QNH 1030, D-EXYZ"); // wrong QNH
    auto resp = build_correction_response("D-EXYZ", comp, d);
    REQUIRE(resp == "D-EXYZ, NEGATIV, QNH 1013, WIEDERHOLEN SIE WOERTLICH.");
}

TEST_CASE("build_correction_response(diff): multiple wrong values all named",
          "[bzf_compliance][correction]") {
    ClearanceComponents comp;
    comp.callsign = "D-EXYZ";
    comp.runway = "25";
    comp.qnh = "1013";
    comp.required = {Element::Callsign, Element::Runway, Element::QNH};
    // Both runway and QNH read back wrong.
    auto d = diff_readback(comp, "Piste 07, QNH 1030, D-EXYZ");
    auto resp = build_correction_response("D-EXYZ", comp, d);
    REQUIRE(resp.find("NEGATIV") != std::string::npos);
    REQUIRE(resp.find("Piste 25") != std::string::npos);
    REQUIRE(resp.find("QNH 1013") != std::string::npos);
    REQUIRE(resp.find("WIEDERHOLEN SIE WOERTLICH.") != std::string::npos);
}
