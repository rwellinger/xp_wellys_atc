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
#include <vector>

// BZF-Strict-Mode pilot-utterance conformance checker for the DE/BZF
// profile. Implements NfL Sprechfunk 2024 §25 b) Nr. 1 readback
// obligations: a pilot must read back the safety-relevant items of a
// tower clearance (runway, QNH, frequency, SSR code, etc.). When this
// module reports a missing element, the state machine sends a
// corrective tower response (atc_templates.json :: bzf_strict.*)
// instead of silently absorbing the readback.
//
// SDK-free. Lives in xp_atc_engine OBJECT lib so atc_repl and tests can
// use it directly. Operates on raw (pre-normalize_for_speech) text:
// tower templates contain "QNH {qnh}" / "Piste {runway}", and the
// pilot transcript has already been reverse-normalised by
// de_phraseology::parse_spoken_number(), so both sides speak the same
// digit form when this check runs.
namespace bzf_compliance {

// Safety-relevant readback elements from NfL §25 b) Nr. 1. The enum
// values map 1:1 to keys in atc_templates.json :: bzf_strict (e.g.
// Element::QNH -> "missing_qnh").
enum class Element {
  Callsign,  // §14 c) Nr. 2 — full callsign on all clearances
  Runway,    // §25 b) Nr. 1 ii) — runway operations
  QNH,       // §25 b) Nr. 1 iii) — altimeter setting
  Frequency, // §25 b) Nr. 1 iii) — newly assigned channel
  Squawk,    // §25 b) Nr. 1 iii) — SSR code
};

// Structured snapshot of a readback-demanding tower clearance. Built at
// clearance-generation time from the template variable map (the exact
// values the controller spoke), NOT re-parsed from rendered free text.
// `required` carries which Kat-1 elements (NfL §25 b) Nr. 1) THIS
// transmission actually obligated the pilot to read back — a fixed
// phraseological table per clearance type, gated by what was issued
// (e.g. QNH only when the controller passed it, not when it came from
// ATIS). `callsign` is the phonetic-expanded session callsign; the
// value fields hold raw-digit forms ("06", "1013", "118.300").
struct ClearanceComponents {
  std::string callsign;
  std::string runway;
  std::string qnh;
  std::string frequency;
  std::string squawk;
  std::vector<Element> required;
};

// Stable lowercase short name ("qnh", "runway", ...) for logging.
const char *element_name(Element e);

// Key into atc_templates.json :: bzf_strict ("missing_qnh", ...).
const char *missing_key(Element e);

// Extract the readback-mandatory elements from a tower response.
// Detects structural markers (the literal token "QNH" + digits, the
// literal "Piste" + digits, a frequency in "NNN.NNN" form, "Squawk"
// + 4 digits). The tower's own callsign use is implicit — every
// readback must repeat the pilot's callsign per §14 c) Nr. 2, so
// Callsign is included whenever the tower response is non-empty.
std::vector<Element> extract_required(const std::string &tower_response);

// Check the pilot's transcript against the required-elements list.
// Returns the subset that is MISSING (empty vector = fully conformant).
// `pilot_callsign` is the phonetic-expanded form from
// settings::pilot_callsign() — substring match is case-insensitive.
std::vector<Element> check_pilot_readback(const std::string &pilot_transcript,
                                          const std::vector<Element> &required,
                                          const std::string &pilot_callsign);

// Character-robust readback coverage check against a structured
// clearance. The pilot transcript is canonicalised — lowercase, spoken
// number words reverse-normalised to digits (de_phraseology::
// parse_spoken_number), known Whisper NATO-letter variants unified, then
// all whitespace + punctuation stripped — and each stored value is
// searched as a substring. Because word boundaries are eliminated,
// Whisper word-welds ("Start frei" -> "startfrei", "Piste 06" ->
// "piste06", "QNH 1013" -> "qnh1013") still match by construction.
// Returns the subset of comp.required that is MISSING (empty = every
// mandated element was read back). This replaces the word-boundary
// regexes of check_pilot_readback for the value-precise path and is the
// single matcher used by both readback recognition (engine) and the
// BZF-strict completeness check (state machine).
std::vector<Element> missing_readback_elements(const ClearanceComponents &comp,
                                               const std::string &pilot_transcript);

// Per-element verdict of a readback against the stored clearance. Unlike
// missing_readback_elements (which only says "not covered"), this
// distinguishes an OMITTED element (pilot said nothing of that type) from
// a WRONG one (pilot read back a value of the right type but the wrong
// digits — e.g. "QNH 1030" against a cleared "QNH 1013"). The distinction
// drives NfL-correct correction phraseology: §25 b) Nr. 3 obliges the
// controller to actively correct a discrepancy (Unstimmigkeit), whereas a
// merely incomplete readback is met with the standard "WIEDERHOLEN SIE
// WOERTLICH" (READ BACK) without naming the value.
enum class ReadbackStatus {
  Ok,      // element was read back correctly (value covered)
  Missing, // no value of this element type was stated at all
  Wrong,   // a value of this type was stated, but it differs from the soll
};

// One per element in comp.required. `expected` is the soll raw-digit form
// ("1013" / "25" / "118.300" / "7000"; empty for Callsign). `stated` is the
// pilot's raw-digit value for this element type, captured from the
// transcript — empty when status is Missing (or for Callsign, which has no
// Wrong verdict — a partial callsign is treated as Missing).
struct FieldDiff {
  Element element;
  ReadbackStatus status;
  std::string expected;
  std::string stated;
};

// Full per-field diff of the pilot transcript against the stored clearance.
// Ok is decided exactly as in missing_readback_elements (value_covered), so
// the completeness semantics are unchanged; the added value is the
// Missing-vs-Wrong split on the non-Ok elements. Callsign is only ever Ok or
// Missing. missing_readback_elements() is a thin wrapper returning every
// element whose status != Ok.
std::vector<FieldDiff> diff_readback(const ClearanceComponents &comp,
                                     const std::string &pilot_transcript);

// Recognition threshold (deliberately lenient, distinct from the strict
// completeness obligation). Returns true when the utterance is a readback
// ATTEMPT: the callsign AND at least one fact element (Runway/QNH/
// Frequency/Squawk) of the clearance are covered. Callsign ALONE does not
// qualify — otherwise any utterance carrying the callsign would be
// silently accepted as a phantom readback. If the clearance has no fact
// element at all, a covered callsign suffices. `missing` is the output of
// missing_readback_elements for the same clearance.
bool readback_covers_core(const std::vector<Element> &required,
                          const std::vector<Element> &missing);

// Build the corrective tower response. With multiple missing elements,
// uses the "missing_multi" template; with exactly one, picks the
// element-specific template. The {callsign} placeholder gets filled
// from the `callsign` argument.
std::string build_correction_response(const std::string &callsign,
                                      const std::vector<Element> &missing);

// NfL-grounded correction from a full per-field diff. Wording is pulled from
// atc_templates.json :: bzf_strict (hot-reloadable, ASCII):
//   - only Missing element(s), no Wrong ones -> the standard READ BACK
//     instruction ("{callsign}, WIEDERHOLEN SIE WOERTLICH.") — the tower
//     does NOT re-state the value (NfL Prüfungsfrage 65 / glossary READ BACK).
//   - at least one Wrong element -> the tower berichtigt the discrepancy and
//     names each correct soll value (NfL §25 b) Nr. 3), e.g.
//     "{callsign}, NEGATIV, QNH 1013, WIEDERHOLEN SIE WOERTLICH."
// Returns "" when every element is Ok (nothing to correct).
std::string build_correction_response(const std::string &callsign,
                                      const ClearanceComponents &comp,
                                      const std::vector<FieldDiff> &diff);

} // namespace bzf_compliance
