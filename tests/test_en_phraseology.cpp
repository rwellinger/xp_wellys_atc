#include "atc/en_phraseology.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

using en_phraseology::expand_callsign_phonetic;
using en_phraseology::normalize_for_speech;
using en_phraseology::parse_spoken_icao;
using en_phraseology::parse_spoken_number;

// ── Runways ──────────────────────────────────────────────────────────

TEST_CASE("Runway: two-digit", "[en_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Runway 25") == "Runway two fife");
}

TEST_CASE("Runway: leading-zero", "[en_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Runway 07") == "Runway zero seven");
}

TEST_CASE("Runway: suffix L -> left", "[en_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Runway 36L") == "Runway tree six left");
}

TEST_CASE("Runway: suffix C -> center", "[en_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Runway 14C") == "Runway one four center");
}

TEST_CASE("Runway: lower-case anchor keeps its case",
          "[en_phraseology][runway]") {
    REQUIRE(normalize_for_speech("runway 25 left") == "runway two fife left");
}

// ── QNH (no unit, unlike German Hektopascal) ─────────────────────────

TEST_CASE("QNH: standard 4-digit", "[en_phraseology][qnh]") {
    REQUIRE(normalize_for_speech("QNH 1013") == "QNH one zero one tree");
}

TEST_CASE("QNH: non-standard 1020", "[en_phraseology][qnh]") {
    REQUIRE(normalize_for_speech("QNH 1020") == "QNH one zero two zero");
}

// ── Heading (padded to 3) ────────────────────────────────────────────

TEST_CASE("heading: already 3-digit", "[en_phraseology][heading]") {
    REQUIRE(normalize_for_speech("heading 050") == "heading zero fife zero");
}

TEST_CASE("heading: two-digit padded", "[en_phraseology][heading]") {
    REQUIRE(normalize_for_speech("heading 50") == "heading zero fife zero");
}

TEST_CASE("heading: 360", "[en_phraseology][heading]") {
    REQUIRE(normalize_for_speech("heading 360") == "heading tree six zero");
}

// ── Frequencies (ICAO "decimal") ─────────────────────────────────────

TEST_CASE("Frequency: 118.300", "[en_phraseology][frequency]") {
    REQUIRE(normalize_for_speech("118.300") ==
            "one one eight decimal tree zero zero");
}

TEST_CASE("Frequency: 119.450 in context", "[en_phraseology][frequency]") {
    REQUIRE(normalize_for_speech("contact tower on 119.450") ==
            "contact tower on one one niner decimal four fife zero");
}

// ── Altitudes ────────────────────────────────────────────────────────

TEST_CASE("Altitude: 3500 feet uses thousand/hundred",
          "[en_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("3500 feet") ==
            "tree thousand fife hundred feet");
}

TEST_CASE("Altitude: 1000 feet", "[en_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("1000 feet") == "one thousand feet");
}

TEST_CASE("Altitude: below 1000 falls back to digit-by-digit",
          "[en_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("500 feet") == "fife zero zero feet");
}

// ── Wind ─────────────────────────────────────────────────────────────

TEST_CASE("Wind: 250 degrees 15 knots", "[en_phraseology][wind]") {
    REQUIRE(normalize_for_speech("wind 250 degrees 15 knots") ==
            "wind two fife zero degrees one fife knots");
}

TEST_CASE("Wind: single-digit speed", "[en_phraseology][wind]") {
    REQUIRE(normalize_for_speech("wind 240 degrees 8 knots") ==
            "wind two four zero degrees eight knots");
}

// ── Clock / number ───────────────────────────────────────────────────

TEST_CASE("Clock: 2 o'clock", "[en_phraseology][clock]") {
    REQUIRE(normalize_for_speech("traffic 2 o'clock") ==
            "traffic two o'clock");
}

TEST_CASE("Sequence number", "[en_phraseology][sequence]") {
    REQUIRE(normalize_for_speech("number 2") == "number two");
}

// ── Composite ────────────────────────────────────────────────────────

TEST_CASE("Digit 4: spoken as 'four', not the ICAO respelling 'fower'",
          "[en_phraseology][digit4]") {
    // Issue #63: "fower" is a human pronunciation hint, not a real word;
    // TTS engines say it literally. "four" is intelligible everywhere.
    REQUIRE(normalize_for_speech("Runway 24") == "Runway two four");
    REQUIRE(normalize_for_speech("Runway 24").find("fower") ==
            std::string::npos);
    // 3/5/9 keep their ICAO forms (real words / clean homophones).
    REQUIRE(normalize_for_speech("Runway 35") == "Runway tree fife");
    // Reverse parser still round-trips "four" back to the digit.
    REQUIRE(parse_spoken_number("Runway two four") == "Runway 24");
}

TEST_CASE("Composite: landing clearance", "[en_phraseology][composite]") {
    const std::string in =
        "November one two tree Alfa Bravo, Runway 24 cleared to land, "
        "wind 240 degrees 8 knots, QNH 1013.";
    const std::string want =
        "November one two tree Alfa Bravo, Runway two four cleared to "
        "land, wind two four zero degrees eight knots, QNH one zero one "
        "tree.";
    REQUIRE(normalize_for_speech(in) == want);
}

// ── Idempotency / edges ──────────────────────────────────────────────

TEST_CASE("Idempotency: applying twice yields same output",
          "[en_phraseology][idempotent]") {
    const std::string in =
        "Runway 24, QNH 1013, wind 250 degrees 15 knots, 3500 feet, "
        "heading 050, 119.450.";
    const std::string once = normalize_for_speech(in);
    REQUIRE(normalize_for_speech(once) == once);
}

TEST_CASE("Edge: empty string", "[en_phraseology][edge]") {
    REQUIRE(normalize_for_speech("").empty());
}

TEST_CASE("Edge: non-aviation text untouched", "[en_phraseology][edge]") {
    const std::string in = "good day, frequency change approved.";
    REQUIRE(normalize_for_speech(in) == in);
}

// ── Reverse normalizer ───────────────────────────────────────────────

TEST_CASE("parse_spoken_number: QNH four-digit run",
          "[en_phraseology][parse][qnh]") {
    REQUIRE(parse_spoken_number("QNH one zero one tree") == "QNH 1013");
}

TEST_CASE("parse_spoken_number: Runway two-digit run with suffix",
          "[en_phraseology][parse][runway]") {
    REQUIRE(parse_spoken_number("Runway two fife left") == "Runway 25 left");
}

TEST_CASE("parse_spoken_number: tolerant plain-English 'five'",
          "[en_phraseology][parse][runway]") {
    REQUIRE(parse_spoken_number("Runway two five") == "Runway 25");
}

TEST_CASE("parse_spoken_number: heading three-digit run",
          "[en_phraseology][parse][heading]") {
    REQUIRE(parse_spoken_number("heading zero fife zero") == "heading 050");
}

TEST_CASE("parse_spoken_number: frequency with 'decimal'",
          "[en_phraseology][parse][frequency]") {
    REQUIRE(parse_spoken_number("one one eight decimal tree zero zero") ==
            "118.300");
}

TEST_CASE("parse_spoken_number: frequency with colloquial 'point'",
          "[en_phraseology][parse][frequency]") {
    REQUIRE(parse_spoken_number("one one eight point three") == "118.3");
}

TEST_CASE("parse_spoken_number: wind direction and speed",
          "[en_phraseology][parse][wind]") {
    REQUIRE(parse_spoken_number(
                "wind two fife zero degrees one fife knots") ==
            "wind 250 degrees 15 knots");
}

TEST_CASE("parse_spoken_number: idempotency on numeric input",
          "[en_phraseology][parse][idempotent]") {
    REQUIRE(parse_spoken_number("Runway 25") == "Runway 25");
    REQUIRE(parse_spoken_number("QNH 1013") == "QNH 1013");
    REQUIRE(parse_spoken_number("118.300") == "118.300");
}

TEST_CASE("parse_spoken_number: single isolated digit word unchanged",
          "[en_phraseology][parse][safety]") {
    REQUIRE(parse_spoken_number("just one moment please") ==
            "just one moment please");
}

TEST_CASE("parse_spoken_number: anchor keyword permits single digit",
          "[en_phraseology][parse][runway]") {
    REQUIRE(parse_spoken_number("Runway seven left") == "Runway 7 left");
}

TEST_CASE("parse_spoken_number: empty string",
          "[en_phraseology][parse][edge]") {
    REQUIRE(parse_spoken_number("").empty());
}

// ── Callsign phonetic expansion (ICAO Annex 10 Vol II §5.2.1.3/.4) ────
//
// settings::pilot_callsign() routes through this when atc_language()=="en".
// docs/icao/icao_coverage.md §11.1 (ICAO digits tree/fower/fife/niner) and
// §11.3 (Alfa, Juliett double-t, X-ray) are the wording anchors.

TEST_CASE("expand_callsign_phonetic: N-number ICAO digits",
          "[en_phraseology][callsign]") {
    REQUIRE(expand_callsign_phonetic("N123AB") ==
            "November one two tree Alfa Bravo");
}

TEST_CASE("expand_callsign_phonetic: G-registration letters only",
          "[en_phraseology][callsign]") {
    REQUIRE(expand_callsign_phonetic("GABCD") ==
            "Golf Alfa Bravo Charlie Delta");
}

TEST_CASE("expand_callsign_phonetic: D-prefix with dash",
          "[en_phraseology][callsign]") {
    REQUIRE(expand_callsign_phonetic("D-EXYZ") ==
            "Delta Echo X-ray Yankee Zulu");
}

TEST_CASE("expand_callsign_phonetic: lowercase input",
          "[en_phraseology][callsign]") {
    REQUIRE(expand_callsign_phonetic("n123ab") ==
            "November one two tree Alfa Bravo");
}

TEST_CASE("expand_callsign_phonetic: empty input",
          "[en_phraseology][callsign][edge]") {
    REQUIRE(expand_callsign_phonetic("").empty());
}

TEST_CASE("expand_callsign_phonetic: pure digits",
          "[en_phraseology][callsign][edge]") {
    REQUIRE(expand_callsign_phonetic("123") == "one two tree");
}

TEST_CASE("expand_callsign_phonetic: dashes and spaces skipped",
          "[en_phraseology][callsign][edge]") {
    REQUIRE(expand_callsign_phonetic("N 1-2-3 AB") ==
            "November one two tree Alfa Bravo");
}

// ── parse_spoken_icao (NATO-reverse destination) ─────────────────────

TEST_CASE("parse_spoken_icao: four NATO letters -> ICAO",
          "[en_phraseology][icao]") {
    REQUIRE(parse_spoken_icao("echo delta mike alfa") == "EDMA");
}

TEST_CASE("parse_spoken_icao: tolerant 'alpha' spelling variant",
          "[en_phraseology][icao]") {
    REQUIRE(parse_spoken_icao("echo delta mike alpha") == "EDMA");
}

TEST_CASE("parse_spoken_icao: stops at first non-NATO word",
          "[en_phraseology][icao]") {
    REQUIRE(parse_spoken_icao("echo delta delta sierra, information alfa") ==
            "EDDS");
}

TEST_CASE("parse_spoken_icao: leading non-NATO word is a miss",
          "[en_phraseology][icao][edge]") {
    REQUIRE(parse_spoken_icao("runway").empty());
}

TEST_CASE("parse_spoken_icao: too few letters is a miss",
          "[en_phraseology][icao][edge]") {
    REQUIRE(parse_spoken_icao("echo delta").empty());
}

TEST_CASE("parse_spoken_icao: empty input", "[en_phraseology][icao][edge]") {
    REQUIRE(parse_spoken_icao("").empty());
}

// ── De-shout (issue #62): ALL-CAPS phraseology must not reach TTS in ──
// uppercase, or a moderation "shouting" heuristic 403-blocks it. ────────

TEST_CASE("de-shout: ICAO SAY AGAIN is lowercased for speech",
          "[en_phraseology][deshout]") {
    REQUIRE(normalize_for_speech("SAY AGAIN") == "say again");
}

TEST_CASE("de-shout: NEGATIVE keyword lowercased",
          "[en_phraseology][deshout]") {
    REQUIRE(normalize_for_speech("NEGATIVE") == "negative");
}

TEST_CASE("de-shout: QNH acronym stays uppercase",
          "[en_phraseology][deshout]") {
    const std::string out = normalize_for_speech("QNH 1013");
    REQUIRE(out.find("QNH") != std::string::npos);
    REQUIRE(out.find("qnh") == std::string::npos);
}

TEST_CASE("de-shout: Title-case words untouched", "[en_phraseology][deshout]") {
    REQUIRE(normalize_for_speech("November One Two Three") ==
            "November One Two Three");
}

TEST_CASE("de-shout: idempotent", "[en_phraseology][deshout]") {
    const std::string once = normalize_for_speech("SAY AGAIN");
    REQUIRE(normalize_for_speech(once) == once);
}
