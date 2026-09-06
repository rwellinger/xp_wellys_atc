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

#ifndef ATC_TEMPLATES_HPP
#define ATC_TEMPLATES_HPP

#include <map>
#include <string>
#include <vector>

namespace atc_templates {

struct TemplateEntry {
  std::string response_template;
  std::string next_state;
  bool requires_readback = false;
};

void init();
void stop();
void reload();

// Lookup template for state+intent, falls back to _INVALID for that state.
// When tower_only is true, tries `<intent>_TOWER_ONLY` first so airports
// without a Ground controller can opt into bundled Tower phraseology.
TemplateEntry lookup(bool is_towered, const std::string &state,
                     const std::string &intent_key, bool tower_only = false);

// Return valid intent keys for a given state (excluding _INVALID).
// post_landing splits the overloaded towered IDLE state: when true
// (was_airborne — flew and back on the ground) the apron/inbound first-
// contact intents are dropped and the sign-off (LEAVING_FREQUENCY) is
// offered; when false (fresh spawn, never flew) the sign-off is dropped and
// the first-contact intents stay. Only affects towered IDLE; every other
// state returns the full key set regardless.
std::vector<std::string> valid_intents(bool is_towered, const std::string &state,
                                       bool post_landing);

// Replace {key} placeholders in template string with values from vars
std::string fill(const std::string &tmpl,
                 const std::map<std::string, std::string> &vars);

// Get a prompt template by key (e.g. "whisper_prompt", "gpt_classify_prompt")
std::string get_prompt(const std::string &key);

// Resolve a controller-level fallback line keyed by name (e.g. "say_again",
// "garbled_say_again", "say_again_use_standard_phraseology") from the
// per-profile "fallbacks" block. Returns default_value when the profile
// does not define the key, so a profile JSON without a "fallbacks" block
// stays on the legacy English wording.
std::string lookup_fallback(const std::string &key,
                            const std::string &default_value);

// Resolve a BZF-strict-mode corrective tower response by key (e.g.
// "missing_qnh", "missing_runway", "missing_callsign") from the
// per-profile "bzf_strict" block. Returns default_value when the
// profile does not define the key. Used by the readback-conformance
// path in atc_state_machine when settings::bzf_strict_mode() is on.
std::string lookup_bzf_strict(const std::string &key,
                              const std::string &default_value);

} // namespace atc_templates

#endif // ATC_TEMPLATES_HPP
