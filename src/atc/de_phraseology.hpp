/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <string>

// BZF-Phraseology-Normalizer: pre-TTS pass that expands numeric aviation
// patterns into ziffernweise spoken form per the German Beschraenkt
// Zugeteiltes Sprechfunkzeugnis convention ("Piste 25" -> "Piste zwo
// fuenf"; "QNH 1013" -> "QNH eins null eins drei Hektopascal").
//
// SDK-free. Lives in xp_atc_engine OBJECT lib so atc_repl and tests can
// use it directly.
//
// Idempotent: triggers only on raw digits, never on already-spelled
// number words. Running normalize_for_speech twice yields the same
// output as running it once.
//
// Region-gated by the caller. atc_session::speak_response() invokes
// this when settings::atc_profile() == "DE"; other regions are
// unchanged.
namespace de_phraseology {

std::string normalize_for_speech(const std::string &text);

// Mirror zu normalize_for_speech: konvertiert ziffernweise BZF-Aussprache
// in der Eingabe zurueck in Rohziffern, damit der nachgelagerte
// Intent-Parser strukturierte Werte ableiten kann.
//   "QNH eins null eins drei"              -> "QNH 1013"
//   "Piste zwo fuenf links"                -> "Piste 25 links"
//   "Steuerkurs null fuenf null"           -> "Steuerkurs 050"
//   "eins eins acht Komma drei null null"  -> "118.300"
// Idempotent: zweimaliges Anwenden liefert das gleiche Ergebnis;
// bereits-numerische Eingaben bleiben unangetastet. Single isolated
// digit words (z. B. "eins" als Numerus) werden NICHT ersetzt --
// nur Runs aus 2..4 zusammenhaengenden Ziffernwoertern.
std::string parse_spoken_number(const std::string &text);

// BZF-phonetic expansion of a raw callsign:
//   "HBAKA"  -> "Hotel Bravo Alfa Kilo Alfa"
//   "D-EXYZ" -> "Delta Echo X-Ray Yankee Zulu"
//   "N123AB" -> "November eins zwo drei Alfa Bravo"
// Mirrors settings::to_icao_phonetic() in format (single space between
// words, empty input -> empty output) but uses the BZF tables
// (Alfa/Juliett/X-Ray, null/eins/zwo/...). Non-alphanumeric chars
// (dash, space) are skipped.
std::string expand_callsign_phonetic(const std::string &raw);

// Reverse of expand_callsign_phonetic for a spoken ICAO destination:
// consumes the leading run of contiguous NATO letter words and returns the
// assembled uppercase ICAO code, empty if no valid 3..4 letter run starts
// the input.
//   "echo delta mike alfa"            -> "EDMA"
//   "echo delta mike alpha, blah"     -> "EDMA"  (stops at first non-letter word)
//   "links"                           -> ""      (not a NATO word)
// Input is expected lowercase (post parse_spoken_number). Tolerant of common
// Whisper / German spelling variants (alpha/alfa, juliet/juliett, x-ray/xray).
// SDK-free; used by intent_parser::extract_destination.
std::string parse_spoken_icao(const std::string &words_lower);

} // namespace de_phraseology
