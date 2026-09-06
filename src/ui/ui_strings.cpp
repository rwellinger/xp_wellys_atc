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

#include "ui/ui_strings.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <json.hpp>

#include <fstream>
#include <string>
#include <unordered_map>

namespace ui_strings {

static std::unordered_map<std::string, std::string> strings_;
static bool loaded_ = false;

static void load_from_file() {
  strings_.clear();
  loaded_ = false;

  std::string path = settings::ui_profile_data_dir() + "/ui_strings.json";
  std::ifstream in(path);
  if (!in.good()) {
    logging::info("Warning: ui_strings.json not found");
    return;
  }
  try {
    nlohmann::json j;
    in >> j;
    if (!j.is_object()) {
      logging::info("Warning: ui_strings.json root must be an object, ignored");
      return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.value().is_string())
        strings_.emplace(it.key(), it.value().get<std::string>());
    }
    loaded_ = true;
    logging::info("UI strings loaded (%zu entries)", strings_.size());
  } catch (...) {
    logging::info("Warning: failed to parse ui_strings.json");
    strings_.clear();
  }
}

void init() { load_from_file(); }

void stop() {
  strings_.clear();
  loaded_ = false;
}

void reload() { load_from_file(); }

const char *tr(const char *key) {
  if (!key)
    return "";
  if (!loaded_)
    return key;
  auto it = strings_.find(key);
  if (it == strings_.end())
    return key;
  return it->second.c_str();
}

} // namespace ui_strings
