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

#include "atc/atc_templates.hpp"
#include "core/logging.hpp"

#include <set>
#include "persistence/settings.hpp"

#include <json.hpp>

#include <fstream>

namespace atc_templates {

static nlohmann::json templates_;
static nlohmann::json prompts_;
static bool loaded_ = false;
static bool prompts_loaded_ = false;

static void load_from_file() {
  std::string path = settings::atc_profile_data_dir() + "/atc_templates.json";
  std::ifstream in(path);
  if (!in.good()) {
    logging::info("Warning: atc_templates.json not found");
    loaded_ = false;
    return;
  }

  try {
    in >> templates_;
    loaded_ = true;
    logging::info("ATC templates loaded");
  } catch (...) {
    logging::info("Warning: failed to parse atc_templates.json");
    loaded_ = false;
  }
}

static void load_prompts() {
  std::string path = settings::get_data_dir() + "/atc_prompt_templates.json";
  std::ifstream in(path);
  if (!in.good()) {
    logging::info("Warning: atc_prompt_templates.json not found");
    prompts_loaded_ = false;
    return;
  }

  try {
    in >> prompts_;
    prompts_loaded_ = true;
    logging::info("ATC prompt templates loaded");
  } catch (...) {
    logging::info("Warning: failed to parse atc_prompt_templates.json");
    prompts_loaded_ = false;
  }
}

void init() {
  load_from_file();
  load_prompts();
}

void stop() {
  templates_.clear();
  prompts_.clear();
  loaded_ = false;
  prompts_loaded_ = false;
}

void reload() {
  load_from_file();
  load_prompts();
}

TemplateEntry lookup(bool is_towered, const std::string &state,
                     const std::string &intent_key, bool tower_only) {
  static const TemplateEntry fallback{"Say again, {callsign}.", "IDLE", false};

  if (!loaded_)
    return fallback;

  const char *type = is_towered ? "towered" : "uncontrolled";

  if (!templates_.contains(type) || !templates_[type].contains(state))
    return fallback;

  auto &state_obj = templates_[type][state];

  // Prefer tower_only variant when applicable, then exact intent, then _INVALID
  std::string tower_only_key = intent_key + "_TOWER_ONLY";
  const std::string *key = &intent_key;
  static const std::string invalid_key = "_INVALID";
  if (tower_only && state_obj.contains(tower_only_key))
    key = &tower_only_key;
  else if (!state_obj.contains(intent_key))
    key = &invalid_key;

  if (!state_obj.contains(*key))
    return fallback;

  auto &entry = state_obj[*key];
  return TemplateEntry{
      entry.value("response", fallback.response_template),
      entry.value("next_state", state),
      entry.value("requires_readback", false),
  };
}

std::vector<std::string> valid_intents(bool is_towered, const std::string &state,
                                       bool post_landing) {
  std::vector<std::string> result;

  if (!loaded_)
    return result;

  const char *type = is_towered ? "towered" : "uncontrolled";

  if (!templates_.contains(type) || !templates_[type].contains(state))
    return result;

  // Towered IDLE carries the union of first-contact and post-landing intents
  // under one key (the state machine cannot tell fresh-spawn-IDLE from post-
  // landing-IDLE). Select the half matching the session phase so the LM
  // classifier (post-hoc validated against this set) cannot pick a ground
  // first contact after a completed flight, nor a sign-off before one. Other
  // states and uncontrolled fields are unaffected.
  //
  // INITIAL_CALL_INBOUND/_VRP are deliberately NOT filtered by post_landing:
  // they are airborne approach calls, legitimate whether the session has been
  // airborne or not (a cross-country leg to a NEW field reaches IDLE with
  // was_airborne=true but still needs the inbound first contact, Issue #14).
  // Their own phase precondition (flight_rules.json rejection_ground) is the
  // ground/air authority, so an on-ground post-landing mis-pick is caught there.
  static const std::set<std::string> kFirstContactOnly = {
      "INITIAL_CALL_GROUND", "INITIAL_CALL_TOWER"};
  static const std::set<std::string> kPostLandingOnly = {"LEAVING_FREQUENCY"};
  const bool filter_idle = is_towered && state == "IDLE";

  for (auto &[key, _] : templates_[type][state].items()) {
    if (key == "_INVALID")
      continue;
    if (filter_idle && post_landing && kFirstContactOnly.count(key))
      continue;
    if (filter_idle && !post_landing && kPostLandingOnly.count(key))
      continue;
    result.push_back(key);
  }
  return result;
}

std::string fill(const std::string &tmpl,
                 const std::map<std::string, std::string> &vars) {
  std::string result = tmpl;
  for (auto &[key, value] : vars) {
    std::string placeholder = "{" + key + "}";
    std::string::size_type pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
      result.replace(pos, placeholder.size(), value);
      pos += value.size();
    }
  }
  return result;
}

std::string get_prompt(const std::string &key) {
  if (!prompts_loaded_ || !prompts_.contains(key))
    return {};
  auto &entry = prompts_[key];
  if (entry.is_object() && entry.contains("prompt"))
    return entry["prompt"].get<std::string>();
  if (entry.is_string())
    return entry.get<std::string>();
  return {};
}

std::string lookup_fallback(const std::string &key,
                            const std::string &default_value) {
  if (!loaded_ || !templates_.contains("fallbacks"))
    return default_value;
  auto &fb = templates_["fallbacks"];
  if (!fb.contains(key) || !fb[key].is_string())
    return default_value;
  return fb[key].get<std::string>();
}

std::string lookup_bzf_strict(const std::string &key,
                              const std::string &default_value) {
  if (!loaded_ || !templates_.contains("bzf_strict"))
    return default_value;
  auto &bs = templates_["bzf_strict"];
  if (!bs.contains(key) || !bs[key].is_string())
    return default_value;
  return bs[key].get<std::string>();
}

} // namespace atc_templates
