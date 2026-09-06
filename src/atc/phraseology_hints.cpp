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

#include "atc/phraseology_hints.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <json.hpp>

#include <fstream>

namespace phraseology_hints {

static nlohmann::json rules_;
static bool loaded_ = false;

static bool match_string(const nlohmann::json &field,
                         const std::string &value) {
  // Field omitted (== null) or "any" -> wildcard.
  if (field.is_null())
    return true;
  if (field.is_string()) {
    const auto &s = field.get_ref<const std::string &>();
    return s == "any" || s == value;
  }
  if (field.is_array()) {
    for (const auto &item : field) {
      if (!item.is_string())
        continue;
      const auto &s = item.get_ref<const std::string &>();
      if (s == "any" || s == value)
        return true;
    }
    return false;
  }
  return false;
}

static bool match_bool(const nlohmann::json &field, bool value) {
  if (field.is_null())
    return true;
  if (field.is_boolean())
    return field.get<bool>() == value;
  if (field.is_string() && field.get<std::string>() == "any")
    return true;
  return false;
}

static void load_from_file() {
  std::string path =
      settings::atc_profile_data_dir() + "/phraseology_hints.json";
  std::ifstream in(path);
  if (!in.good()) {
    logging::info("Warning: phraseology_hints.json not found");
    rules_.clear();
    loaded_ = false;
    return;
  }
  try {
    nlohmann::json j;
    in >> j;
    if (!j.contains("rules") || !j["rules"].is_array()) {
      logging::info(
          "Warning: phraseology_hints.json missing 'rules' array, ignored");
      rules_.clear();
      loaded_ = false;
      return;
    }
    rules_ = j["rules"];
    loaded_ = true;
    logging::info("Phraseology hints loaded (%zu rules)", rules_.size());
  } catch (...) {
    logging::info("Warning: failed to parse phraseology_hints.json");
    rules_.clear();
    loaded_ = false;
  }
}

void init() { load_from_file(); }

void stop() {
  rules_.clear();
  loaded_ = false;
}

void reload() { load_from_file(); }

std::vector<std::string> lookup(const HintQuery &q) {
  std::vector<std::string> result;
  if (!loaded_)
    return result;

  const std::string state_str = atc_state_machine::state_name(q.state);
  const std::string phase_str = flight_phase::phase_name(q.phase);
  // frequency_type_name returns display-cased strings ("Ground", "Tower")
  // but the JSON uses canonical uppercase tokens ("GROUND", "TOWER") to
  // stay consistent with state and phase names. Upcase here so the
  // matrix matches without forcing a rename of the UI-facing helper.
  std::string freq_str = xplane_context::frequency_type_name(q.frequency_type);
  for (char &c : freq_str)
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
  // UNKNOWN maps to a token no rule carries, so an unclassified field falls
  // through to the empty fallback rule -> empty hints panel (visible/debuggable)
  // rather than a silently wrong guess.
  std::string facility_str;
  switch (q.facility) {
  case xplane_context::FacilityType::TOWERED:
    facility_str = "towered";
    break;
  case xplane_context::FacilityType::AFIS:
    facility_str = "afis";
    break;
  case xplane_context::FacilityType::UNCONTROLLED:
    facility_str = "uncontrolled";
    break;
  case xplane_context::FacilityType::UNKNOWN:
    facility_str = "unknown";
    break;
  }

  for (const auto &rule : rules_) {
    if (!rule.is_object())
      continue;

    if (!match_string(rule.value("state", nlohmann::json{}), state_str))
      continue;
    if (!match_string(rule.value("phase", nlohmann::json{}), phase_str))
      continue;
    if (!match_string(rule.value("facility", nlohmann::json{}), facility_str))
      continue;
    if (!match_string(rule.value("frequency_type", nlohmann::json{}), freq_str))
      continue;
    if (!match_bool(rule.value("tower_only", nlohmann::json{}), q.tower_only))
      continue;
    if (!match_bool(rule.value("post_landing", nlohmann::json{}),
                    q.post_landing))
      continue;

    if (rule.contains("intents") && rule["intents"].is_array()) {
      for (const auto &item : rule["intents"]) {
        if (item.is_string())
          result.push_back(item.get<std::string>());
      }
    }
    return result;
  }
  return result;
}

} // namespace phraseology_hints
