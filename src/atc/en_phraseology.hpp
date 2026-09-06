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

// ICAO-VFR phraseology normalizer — the English counterpart to
// de_phraseology, mirroring its interface exactly. Pre-TTS pass that
// expands numeric aviation patterns into digit-by-digit ICAO spoken form
// ("Runway 25" -> "Runway two fife"; "QNH 1013" -> "QNH one zero one
// tree").
//
// Spoken forms follow ICAO Annex 10 Vol II §5.2.1.4.3.1 (see
// docs/icao/icao_coverage.md §11.1): digits 3/5/9 use the distinctive ATC
// forms "tree/fife/niner". 4 is spoken "four", not the ICAO respelling
// "fower" — that non-word makes TTS engines say it literally (issue #63);
// "tree/fife/niner" survive because they are real words / clean
// homophones. The reverse parser still accepts "fower". The phonetic
// alphabet uses the ICAO official spellings "Alfa" and "Juliett"
// (double-t) and "X-ray" (§11.3). English aviation nouns replace the
// German anchors (Runway/heading/feet/knots/degrees/decimal/o'clock).
// There is NO German umlaut-restoration pass (ASCII English needs none).
//
// SDK-free. Lives in xp_atc_engine OBJECT lib so atc_repl and tests can
// use it directly. Idempotent, like de_phraseology.
//
// Region-gated by the caller: the phraseology dispatch selects this
// module when settings::atc_profile() == "EN"; the German path is
// unchanged. See Epic #35 / Issue #41.
namespace en_phraseology {

// Forward normalizer: raw digits -> ICAO spoken form, for TTS.
//   "Runway 25"        -> "Runway two fife"
//   "QNH 1013"         -> "QNH one zero one tree"
//   "118.300"          -> "one one eight decimal tree zero zero"
//   "3500 feet"        -> "tree thousand fife hundred feet"
std::string normalize_for_speech(const std::string &text);

// Reverse of normalize_for_speech: ICAO spoken digit runs -> raw digits,
// so the downstream intent parser can derive structured values.
//   "QNH one zero one tree"              -> "QNH 1013"
//   "Runway two fife left"               -> "Runway 25 left"
//   "heading zero fife zero"             -> "heading 050"
//   "one one eight decimal tree zero zero" -> "118.300"
// Tolerant of both the ICAO forms (tree/fower/fife/niner) and the plain
// English words a transcriber may emit (three/four/five/nine), and of
// "decimal" or "point". Idempotent; single isolated digit words are NOT
// replaced — only runs of 2..4 contiguous digit words.
std::string parse_spoken_number(const std::string &text);

// ICAO phonetic expansion of a raw callsign:
//   "N123AB" -> "November one two tree Alfa Bravo"
//   "GABCD"  -> "Golf Alfa Bravo Charlie Delta"
// Mirrors de_phraseology::expand_callsign_phonetic in format (single
// space between words, empty input -> empty output) but uses the ICAO
// tables. Non-alphanumeric chars (dash, space) are skipped.
std::string expand_callsign_phonetic(const std::string &raw);

// Reverse of expand_callsign_phonetic for a spoken ICAO destination:
// consumes the leading run of contiguous NATO letter words and returns
// the assembled uppercase ICAO code, empty if no valid 3..4 letter run
// starts the input.
//   "echo delta mike alfa"        -> "EDMA"
//   "echo delta mike alpha, blah" -> "EDMA"  (stops at first non-letter word)
// Input is expected lowercase. Tolerant of common transcription variants
// (alpha/alfa, juliet/juliett, x-ray/xray). SDK-free.
std::string parse_spoken_icao(const std::string &words_lower);

} // namespace en_phraseology
