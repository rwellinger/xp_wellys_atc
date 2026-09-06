#include "atc/de_phraseology.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <fstream>
#include <sstream>
#include <string>

using de_phraseology::expand_callsign_phonetic;
using de_phraseology::normalize_for_speech;
using de_phraseology::parse_spoken_icao;
using de_phraseology::parse_spoken_number;

// ── Pisten ───────────────────────────────────────────────────────────

TEST_CASE("Piste: two-digit runway", "[de_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Piste 25") == "Piste zwo fünf");
}

TEST_CASE("Piste: leading-zero runway", "[de_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Piste 07") == "Piste null sieben");
}

TEST_CASE("Piste: suffix L translates to links", "[de_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Piste 36L") == "Piste drei sechs links");
}

TEST_CASE("Piste: suffix C translates to mitte", "[de_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Piste 14C") == "Piste eins vier mitte");
}

TEST_CASE("Piste: already-German suffix stays intact",
          "[de_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Piste 25 links") ==
            "Piste zwo fünf links");
}

// ── QNH ──────────────────────────────────────────────────────────────

TEST_CASE("QNH: standard 4-digit value", "[de_phraseology][qnh]") {
    REQUIRE(normalize_for_speech("QNH 1013") ==
            "QNH eins null eins drei Hektopascal");
}

TEST_CASE("QNH: non-standard value 1020", "[de_phraseology][qnh]") {
    REQUIRE(normalize_for_speech("QNH 1020") ==
            "QNH eins null zwo null Hektopascal");
}

TEST_CASE("QNH: pre-existing Hektopascal not duplicated",
          "[de_phraseology][qnh][idempotent]") {
    REQUIRE(normalize_for_speech("QNH 1013 Hektopascal") ==
            "QNH eins null eins drei Hektopascal");
}

// ── Frequenzen ───────────────────────────────────────────────────────

TEST_CASE("Frequency: 118.300", "[de_phraseology][frequency]") {
    REQUIRE(normalize_for_speech("Frequenz 118.300") ==
            "Frequenz eins eins acht Komma drei null null");
}

TEST_CASE("Frequency: 119.450", "[de_phraseology][frequency]") {
    REQUIRE(normalize_for_speech("119.450") ==
            "eins eins neun Komma vier fünf null");
}

TEST_CASE("Frequency: 120.075", "[de_phraseology][frequency]") {
    REQUIRE(normalize_for_speech("auf 120.075") ==
            "auf eins zwo null Komma null sieben fünf");
}

// ── Steuerkurse ──────────────────────────────────────────────────────

TEST_CASE("Steuerkurs: 050 (already 3-digit)", "[de_phraseology][heading]") {
    REQUIRE(normalize_for_speech("Steuerkurs 050") ==
            "Steuerkurs null fünf null");
}

TEST_CASE("Steuerkurs: 180", "[de_phraseology][heading]") {
    REQUIRE(normalize_for_speech("Steuerkurs 180") ==
            "Steuerkurs eins acht null");
}

TEST_CASE("Steuerkurs: 360 full circle", "[de_phraseology][heading]") {
    REQUIRE(normalize_for_speech("Steuerkurs 360") ==
            "Steuerkurs drei sechs null");
}

// ── Callsigns ────────────────────────────────────────────────────────

TEST_CASE("Callsign: D-EXYZ expands fully NATO",
          "[de_phraseology][callsign]") {
    REQUIRE(normalize_for_speech("D-EXYZ") ==
            "Delta Echo X-Ray Yankee Zulu");
}

TEST_CASE("Callsign: D-EWLY", "[de_phraseology][callsign]") {
    REQUIRE(normalize_for_speech("D-EWLY") ==
            "Delta Echo Whiskey Lima Yankee");
}

// ── NATO swap ────────────────────────────────────────────────────────

TEST_CASE("Information letter: Alpha becomes Alfa",
          "[de_phraseology][nato]") {
    REQUIRE(normalize_for_speech("Information Alpha") ==
            "Information Alfa");
}

// ── Hoehen ───────────────────────────────────────────────────────────

TEST_CASE("Altitude: 3500 Fuss uses tausend/hundert",
          "[de_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("3500 Fuss") ==
            "drei tausend fünfhundert Fuß");
}

TEST_CASE("Altitude: 1000 Fuss is einfach tausend",
          "[de_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("1000 Fuss") == "eins tausend Fuß");
}

TEST_CASE("Altitude: 2000 Fuss uses zwo tausend",
          "[de_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("2000 Fuss") == "zwo tausend Fuß");
}

TEST_CASE("Altitude: below 1000 falls back to ziffernweise",
          "[de_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("500 Fuss") == "fünf null null Fuß");
}

// ── Wind ─────────────────────────────────────────────────────────────

TEST_CASE("Wind: 250 Grad 15 Knoten", "[de_phraseology][wind]") {
    REQUIRE(normalize_for_speech("Wind 250 Grad 15 Knoten") ==
            "Wind zwo fünf null Grad eins fünf Knoten");
}

TEST_CASE("Wind: 240 Grad 8 Knoten (single-digit speed)",
          "[de_phraseology][wind]") {
    REQUIRE(normalize_for_speech("Wind 240 Grad 8 Knoten") ==
            "Wind zwo vier null Grad acht Knoten");
}

// ── Clock / Nummer ───────────────────────────────────────────────────

TEST_CASE("Clock: 2 Uhr", "[de_phraseology][clock]") {
    REQUIRE(normalize_for_speech("Verkehr 2 Uhr") == "Verkehr zwo Uhr");
}

TEST_CASE("Sequence number", "[de_phraseology][sequence]") {
    REQUIRE(normalize_for_speech("Nummer 2") == "Nummer zwo");
}

// ── Composite (M2 Worked Examples) ───────────────────────────────────

TEST_CASE("Composite: initial-call-ground full template",
          "[de_phraseology][composite]") {
    const std::string in =
        "D-EWLY, Friedrichshafen Turm, rollen Sie zum Rollhalt Piste 24 "
        "ueber Charlie, QNH 1018.";
    const std::string want =
        "Delta Echo Whiskey Lima Yankee, Friedrichshafen Turm, rollen "
        "Sie zum Rollhalt Piste zwo vier über Charlie, QNH eins null "
        "eins acht Hektopascal.";
    REQUIRE(normalize_for_speech(in) == want);
}

TEST_CASE("Composite: takeoff clearance", "[de_phraseology][composite]") {
    const std::string in =
        "D-EWLY, Wind 240 Grad 8 Knoten, Piste 24, Startfreigabe.";
    const std::string want =
        "Delta Echo Whiskey Lima Yankee, Wind zwo vier null Grad acht "
        "Knoten, Piste zwo vier, Startfreigabe.";
    REQUIRE(normalize_for_speech(in) == want);
}

TEST_CASE("Composite: landing clearance", "[de_phraseology][composite]") {
    const std::string in =
        "D-EWLY, Wind 240 Grad 8 Knoten, Piste 24, Landefreigabe.";
    const std::string want =
        "Delta Echo Whiskey Lima Yankee, Wind zwo vier null Grad acht "
        "Knoten, Piste zwo vier, Landefreigabe.";
    REQUIRE(normalize_for_speech(in) == want);
}

// ── Idempotency ──────────────────────────────────────────────────────

TEST_CASE("Idempotency: applying twice yields same output",
          "[de_phraseology][idempotent]") {
    const std::string in =
        "D-EWLY, Piste 24, QNH 1013, Wind 250 Grad 15 Knoten, "
        "Information Alpha, 3500 Fuss, Steuerkurs 050, 119.450.";
    const std::string once = normalize_for_speech(in);
    const std::string twice = normalize_for_speech(once);
    REQUIRE(once == twice);
}

// ── Edge cases ───────────────────────────────────────────────────────

TEST_CASE("Edge: empty string", "[de_phraseology][edge]") {
    REQUIRE(normalize_for_speech("") == "");
}

TEST_CASE("Edge: text without aviation context untouched",
          "[de_phraseology][edge]") {
    const std::string in = "guten Tag, Frequenzwechsel genehmigt.";
    REQUIRE(normalize_for_speech(in) == in);
}

// ── Reverse normalizer (M7) ──────────────────────────────────────────

TEST_CASE("parse_spoken_number: QNH four-digit run",
          "[de_phraseology][parse][qnh]") {
    REQUIRE(parse_spoken_number("QNH eins null eins drei") == "QNH 1013");
}

TEST_CASE("parse_spoken_number: Piste two-digit run",
          "[de_phraseology][parse][runway]") {
    REQUIRE(parse_spoken_number("Piste zwo fuenf links") ==
            "Piste 25 links");
}

TEST_CASE("parse_spoken_number: Piste with 'zwei' synonym",
          "[de_phraseology][parse][runway]") {
    REQUIRE(parse_spoken_number("Piste zwei fuenf") == "Piste 25");
}

TEST_CASE("parse_spoken_number: Steuerkurs three-digit run",
          "[de_phraseology][parse][heading]") {
    REQUIRE(parse_spoken_number("Steuerkurs null fuenf null") ==
            "Steuerkurs 050");
}

TEST_CASE("parse_spoken_number: frequency with Komma",
          "[de_phraseology][parse][frequency]") {
    REQUIRE(parse_spoken_number("eins eins acht Komma drei null null") ==
            "118.300");
}

TEST_CASE("parse_spoken_number: tower frequency in context",
          "[de_phraseology][parse][frequency]") {
    REQUIRE(parse_spoken_number(
                "Frequenz eins eins neun Komma vier fuenf null") ==
            "Frequenz 119.450");
}

TEST_CASE("parse_spoken_number: wind direction and speed",
          "[de_phraseology][parse][wind]") {
    REQUIRE(parse_spoken_number(
                "Wind zwo fuenf null Grad eins fuenf Knoten") ==
            "Wind 250 Grad 15 Knoten");
}

TEST_CASE("parse_spoken_number: idempotency on numeric input",
          "[de_phraseology][parse][idempotent]") {
    REQUIRE(parse_spoken_number("Piste 25") == "Piste 25");
    REQUIRE(parse_spoken_number("QNH 1013") == "QNH 1013");
    REQUIRE(parse_spoken_number("118.300") == "118.300");
}

TEST_CASE("parse_spoken_number: idempotency double-apply",
          "[de_phraseology][parse][idempotent]") {
    const std::string in = "QNH eins null eins drei, Piste zwo fuenf";
    const std::string once = parse_spoken_number(in);
    const std::string twice = parse_spoken_number(once);
    REQUIRE(once == twice);
}

TEST_CASE("parse_spoken_number: single isolated digit word unchanged",
          "[de_phraseology][parse][safety]") {
    // 'eins' alone (as numerus, not a number) must not be substituted.
    REQUIRE(parse_spoken_number("die eins Achse ist frei") ==
            "die eins Achse ist frei");
}

TEST_CASE("parse_spoken_number: anchor-keyword permits single digit",
          "[de_phraseology][parse][runway]") {
    REQUIRE(parse_spoken_number("Piste sieben links") == "Piste 7 links");
}

TEST_CASE("parse_spoken_number: untouched non-numeric text",
          "[de_phraseology][parse][edge]") {
    const std::string in =
        "Friedrichshafen Turm, guten Tag, Information Alfa.";
    REQUIRE(parse_spoken_number(in) == in);
}

TEST_CASE("parse_spoken_number: empty string",
          "[de_phraseology][parse][edge]") {
    REQUIRE(parse_spoken_number("") == "");
}

TEST_CASE("parse_spoken_number: composite BZF transmission",
          "[de_phraseology][parse][composite]") {
    const std::string in =
        "Friedrichshafen Turm, am Rollhalt Piste zwo vier, "
        "abflugbereit, QNH eins null eins drei.";
    const std::string want =
        "Friedrichshafen Turm, am Rollhalt Piste 24, "
        "abflugbereit, QNH 1013.";
    REQUIRE(parse_spoken_number(in) == want);
}

// ── Template invariant ───────────────────────────────────────────────

// Templates must keep raw-digit placeholders ({runway}, {qnh}) so that
// the normalizer is the single source of ziffernweise expansion. If a
// template ever ships with literal "zwo "/"null sieben"/"fuenf"
// fragments, double-normalization risk is back on the table and the
// reverse-normalizer (M7) has nothing stable to map.
TEST_CASE("DE templates contain no pre-expanded BZF digit words",
          "[de_phraseology][templates][invariant]") {
    const std::string path =
        std::string(XP_WELLYS_ATC_SOURCE_DIR) +
        "/data/atc_profiles/de/atc_templates.json";
    std::ifstream f(path);
    REQUIRE(f.is_open());
    std::stringstream buf;
    buf << f.rdbuf();
    const std::string body = buf.str();

    // Per-token guards: every match indicates a template author wrote
    // out the BZF spelling instead of leaving the placeholder for the
    // normalizer. (Note: "eins" appears legitimately in standalone
    // phrases like "Nummer eins"; we deliberately do not assert
    // against that token.)
    REQUIRE(body.find("zwo ") == std::string::npos);
    REQUIRE(body.find("null sieben") == std::string::npos);
    REQUIRE(body.find("fuenf ") == std::string::npos);
    REQUIRE(body.find("Hektopascal") == std::string::npos);
}

// ── Callsign phonetic expansion (NfL §6 + §13) ───────────────────────
//
// settings::pilot_callsign() routes through this function when
// atc_profile() == "DE" (see src/persistence/settings.cpp:232). These
// tests pin the NfL-conformant output for the three callsign-prefix
// families a German tower will see in the sim (D-, HB-, N-) and lock
// in the edge cases (empty input, mixed case, embedded dashes).

TEST_CASE("expand_callsign_phonetic: D-prefix with dash",
          "[de_phraseology][callsign]") {
    REQUIRE(expand_callsign_phonetic("D-EXYZ") ==
            "Delta Echo X-Ray Yankee Zulu");
}

TEST_CASE("expand_callsign_phonetic: N-prefix US registration ziffernweise",
          "[de_phraseology][callsign]") {
    REQUIRE(expand_callsign_phonetic("N123AB") ==
            "November eins zwo drei Alfa Bravo");
}

TEST_CASE("expand_callsign_phonetic: HB-prefix Swiss letters-only",
          "[de_phraseology][callsign]") {
    REQUIRE(expand_callsign_phonetic("HBAKA") == "Hotel Bravo Alfa Kilo Alfa");
}

TEST_CASE("expand_callsign_phonetic: lowercase input",
          "[de_phraseology][callsign]") {
    REQUIRE(expand_callsign_phonetic("d-exyz") ==
            "Delta Echo X-Ray Yankee Zulu");
}

TEST_CASE("expand_callsign_phonetic: empty input",
          "[de_phraseology][callsign][edge]") {
    REQUIRE(expand_callsign_phonetic("").empty());
}

TEST_CASE("expand_callsign_phonetic: pure digits",
          "[de_phraseology][callsign][edge]") {
    REQUIRE(expand_callsign_phonetic("123") == "eins zwo drei");
}

TEST_CASE("expand_callsign_phonetic: dashes and spaces are skipped",
          "[de_phraseology][callsign][edge]") {
    REQUIRE(expand_callsign_phonetic("N 1-2-3 AB") ==
            "November eins zwo drei Alfa Bravo");
}

// ── parse_spoken_icao (NATO-reverse destination) ─────────────────────

TEST_CASE("parse_spoken_icao: four NATO letters -> ICAO",
          "[de_phraseology][icao]") {
    REQUIRE(parse_spoken_icao("echo delta mike alfa") == "EDMA");
}

TEST_CASE("parse_spoken_icao: tolerant 'alpha' spelling variant",
          "[de_phraseology][icao]") {
    REQUIRE(parse_spoken_icao("echo delta mike alpha") == "EDMA");
}

TEST_CASE("parse_spoken_icao: stops at first non-NATO word",
          "[de_phraseology][icao]") {
    REQUIRE(parse_spoken_icao("echo delta delta sierra, information alfa") ==
            "EDDS");
}

TEST_CASE("parse_spoken_icao: leading non-NATO word is a miss",
          "[de_phraseology][icao][edge]") {
    REQUIRE(parse_spoken_icao("links").empty());
}

TEST_CASE("parse_spoken_icao: too few letters is a miss",
          "[de_phraseology][icao][edge]") {
    REQUIRE(parse_spoken_icao("echo delta").empty());
}

TEST_CASE("parse_spoken_icao: empty input", "[de_phraseology][icao][edge]") {
    REQUIRE(parse_spoken_icao("").empty());
}

// ── De-shout (issue #62): ALL-CAPS phraseology must not reach TTS in ──
// uppercase, or a moderation "shouting" heuristic 403-blocks it. ────────

TEST_CASE("de-shout: BZF readback correction is lowercased for speech",
          "[de_phraseology][deshout]") {
    REQUIRE(normalize_for_speech(
                "Charlie Oscar Lima eins, WIEDERHOLEN SIE WOERTLICH.") ==
            "Charlie Oscar Lima eins, wiederholen sie woertlich.");
}

TEST_CASE("de-shout: standalone NEGATIV keyword", "[de_phraseology][deshout]") {
    REQUIRE(normalize_for_speech("NEGATIV") == "negativ");
}

TEST_CASE("de-shout: uppercase umlaut is lowercased (UTF-8 aware)",
          "[de_phraseology][deshout]") {
    // Input already carries the uppercase Oe; restore_umlaute leaves it,
    // the de-shout pass lowercases it to "woertlich" via the umlaut.
    REQUIRE(normalize_for_speech("W\xC3\x96RTLICH") == "w\xC3\xB6rtlich");
}

TEST_CASE("de-shout: QNH acronym stays uppercase",
          "[de_phraseology][deshout]") {
    const std::string out = normalize_for_speech("QNH 1013");
    REQUIRE(out.find("QNH") != std::string::npos);
    REQUIRE(out.find("qnh") == std::string::npos);
}

TEST_CASE("de-shout: Title-case and number words are untouched",
          "[de_phraseology][deshout]") {
    // "Charlie" / "Hektopascal" carry lowercase -> not shouting;
    // ziffernweise output is already lowercase.
    REQUIRE(normalize_for_speech("Charlie Hektopascal") ==
            "Charlie Hektopascal");
}

TEST_CASE("de-shout: idempotent", "[de_phraseology][deshout]") {
    const std::string once =
        normalize_for_speech("WIEDERHOLEN SIE WOERTLICH");
    REQUIRE(normalize_for_speech(once) == once);
}
