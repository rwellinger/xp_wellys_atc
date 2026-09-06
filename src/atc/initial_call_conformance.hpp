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

#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"

#include <string>
#include <vector>

// Data-driven BZF first-call conformance checker for the DE/BZF profile.
// Reads data/atc_profiles/de/conformance.json: per intent, a `required`
// set (NfL-mandatory elements) and a `recommended` set (the didactic
// full form expected in a BZF exam). The state machine drives which set
// is hard via settings::bzf_strict_mode() — strict=false enforces only
// `required`, strict=true enforces `required + recommended`.
//
// Scope of this PR: only INITIAL_CALL_GROUND is wired/tested. The data
// shape and detectors are generic so later intents (§1.4/3.1/5.1/11.x)
// can reuse them without code change beyond a new JSON entry.
//
// SDK-free. Lives in the xp_atc_engine OBJECT lib so atc_repl and tests
// can use it directly. Never inspects settings::backend_mode (Backend
// Adapter Rule); pure rule/data logic.
namespace initial_call_conformance {

// Outcome of an evaluate() call: which configured elements were absent
// from the pilot utterance, split by set. Element names are the raw
// JSON keys ("aircraft_type", "position", "intention", "atis_letter").
struct Result {
  std::vector<std::string> missing_required;
  std::vector<std::string> missing_recommended;
};

// Load conformance.json from settings::atc_profile_data_dir(). A missing
// or malformed file is tolerated: the checker degrades to a no-op (every
// evaluate() returns an empty Result), so older data bundles and the
// cloud-only slice keep working unchanged. reload() is the hot-reload
// entry point; init() is the startup alias.
void init();
void reload();

// Pure detection: returns the configured elements for `intent_key` that
// are NOT present in `msg`/`ctx`. An unknown intent key, or one with no
// configured elements, yields an empty Result. An element whose detector
// is unknown is treated as present (never penalised) so a misconfigured
// JSON key cannot spuriously block the pilot.
Result evaluate(const std::string &intent_key,
                const intent_parser::PilotMessage &msg,
                const xplane_context::XPlaneContext &ctx);

// Build the targeted re-request, e.g.
//   "{callsign}, sagen Sie Luftfahrzeugmuster und Information."
// `missing` is an enforce list (raw element keys). The wording comes
// from conformance.json :: <intent_key>.{missing_response,element_prompts};
// falls back to readable defaults when the file is absent.
std::string build_request_prompt(const std::string &intent_key,
                                 const std::string &callsign,
                                 const std::vector<std::string> &missing);

} // namespace initial_call_conformance
