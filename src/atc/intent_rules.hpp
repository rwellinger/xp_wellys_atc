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

#ifndef INTENT_RULES_HPP
#define INTENT_RULES_HPP

// Data-driven intent classification engine. Rules + Whisper-normalization +
// post-match adjustments are loaded from
// `<atc_profile_data_dir>/intent_rules.json`; the C++ side only walks the
// loaded table. This replaces the previous hand-written 29 `match_*` functions
// in `intent_parser.cpp`.
//
// Schema (intent_rules.json):
//
//   {
//     "normalize": [
//       { "from": "take of ", "to": "takeoff " },
//       ...
//     ],
//     "rules": [
//       {
//         "intent": "REQUEST_TAXI",
//         "confidence": 0.90,
//         "any":  [ <Predicate>, ... ],   // at least one must match (or empty)
//         "all":  [ <Predicate>, ... ],   // all must match
//         "none": [ <Predicate>, ... ]    // none may match
//       },
//       ...
//     ],
//     "adjustments": [
//       {
//         "if": { <Conditions> },
//         "set_intent": "READBACK",       // optional
//         "set_confidence": 0.85          // optional
//       },
//       ...
//     ]
//   }
//
// Predicate forms (string shorthand or object):
//   "disregard"                              → contains
//   { "contains": "..." }                    → substring
//   { "starts_with": "..." }                 → prefix
//   { "ends_with": "..." }                   → suffix
//   { "has_facility": "tower" }              → padded standalone-word match
//   { "ends_with_runway_token": true }       → ends with spoken digit 1..36
//                                              or "left"/"right"/"center"
//
// Adjustment conditions:
//   "intent": "X"                            → matched intent equals X
//   "intent_in": ["X","Y"]                   → matched intent in set
//   "intent_is_airborne_only": true          → matched intent is one of the
//                                              airborne-only set (hardcoded)
//   "intent_is_ground_only": true            → matched intent is one of the
//                                              ground-only set (hardcoded)
//   "on_ground": true|false                  → ctx.on_ground equality
//   "is_towered": true|false                 → ctx.is_towered() equality
//                                              (true iff facility_type==TOWERED)
//   "vrp_name_set": true                     → vrp_name non-empty
//   "text_contains": "..."                   → preprocessed text substring
//
// Multiple conditions on the same `if` are AND-ed.

#include "core/xplane_context.hpp"

#include <string>

namespace intent_parser {
enum class PilotIntent;
struct PilotMessage;
} // namespace intent_parser

namespace intent_rules {

void init();
void stop();
void reload();

// Apply Whisper-normalization rules from the loaded `normalize` section.
// Returns the input string with all `from` → `to` replacements applied.
std::string preprocess(const std::string &lowercased_text);

// Match the (already preprocessed + lowercased) transcript against the
// rule table. Returns the first matching rule's intent + confidence, or
// (UNKNOWN, 0.0) if no rule matches.
struct MatchResult {
  intent_parser::PilotIntent intent;
  float confidence;
};
MatchResult match(const std::string &text);

// Apply post-match `adjustments` (intent overrides + confidence mutations)
// to the PilotMessage in place. Mirrors the trailing block of the old
// `parse()` function (flight-phase filter + airport-type adjustments +
// VRP upgrade).
void apply_adjustments(intent_parser::PilotMessage &msg,
                       const xplane_context::XPlaneContext &ctx,
                       const std::string &preprocessed_text);

// True if the rule table loaded successfully. Engine code can short-
// circuit to UNKNOWN/0.0 if false (e.g. when JSON missing in a non-dev
// environment), without crashing.
bool is_loaded();

} // namespace intent_rules

#endif // INTENT_RULES_HPP
