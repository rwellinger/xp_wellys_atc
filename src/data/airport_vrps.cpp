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

#include "data/airport_vrps.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <unordered_map>

namespace airport_vrps {

static std::unordered_map<std::string, AirportData> airports_;

static std::string to_lower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

// Parse one airport_vrps.json file into `into`. Existing entries in `into`
// are replaced wholesale when the same ICAO key appears — used by
// load_from_file() to layer user overrides on top of the plugin defaults.
// Returns the number of airport entries parsed from this file (0 if the
// file is missing or unparseable).
static size_t parse_into(const std::string &path,
                         std::unordered_map<std::string, AirportData> &into) {
  std::ifstream in(path);
  if (!in.good())
    return 0;

  nlohmann::json j;
  try {
    in >> j;
  } catch (const std::exception &e) {
    logging::info("Warning: failed to parse %s: %s", path.c_str(), e.what());
    return 0;
  }

  if (!j.is_object())
    return 0;

  size_t loaded = 0;
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (it.key().rfind('_', 0) == 0) // skip _comment etc.
      continue;
    const std::string &icao = it.key();
    const auto &node = it.value();
    if (!node.is_object())
      continue;

    AirportData ad;
    if (node.contains("name") && node["name"].is_string())
      ad.name = node["name"].get<std::string>();

    if (node.contains("vrps") && node["vrps"].is_array()) {
      for (const auto &v : node["vrps"]) {
        if (!v.is_object() || !v.contains("name"))
          continue;
        VRP vrp;
        vrp.name = v["name"].get<std::string>();
        if (v.contains("lat"))
          vrp.lat = v["lat"].get<double>();
        if (v.contains("lon"))
          vrp.lon = v["lon"].get<double>();
        if (v.contains("alt_ft"))
          vrp.alt_ft = v["alt_ft"].get<int>();
        ad.vrps.push_back(std::move(vrp));
      }
    }

    if (node.contains("arrival_routes") && node["arrival_routes"].is_object()) {
      for (auto rit = node["arrival_routes"].begin();
           rit != node["arrival_routes"].end(); ++rit) {
        if (!rit.value().is_array())
          continue;
        std::vector<std::string> names;
        for (const auto &n : rit.value()) {
          if (n.is_string())
            names.push_back(n.get<std::string>());
        }
        ad.arrival_routes[rit.key()] = std::move(names);
      }
    }

    if (node.contains("pattern_direction")) {
      const auto &pd = node["pattern_direction"];
      if (pd.is_string()) {
        ad.pattern_direction["_default"] = pd.get<std::string>();
      } else if (pd.is_object()) {
        for (auto pit = pd.begin(); pit != pd.end(); ++pit) {
          if (pit.value().is_string())
            ad.pattern_direction[pit.key()] = pit.value().get<std::string>();
        }
      }
    }

    into[icao] = std::move(ad);
    ++loaded;
  }
  return loaded;
}

static void load_from_file() {
  airports_.clear();

  // VRPs are geographic data and don't depend on the ATC profile -
  // a Swiss airport's reporting points are the same whether the
  // pilot is training EU or DE phraseology. The bundled file lives
  // at <data>/vrps/airport_vrps.json (single global source).
  const std::string plugin_path = settings::vrps_data_path();
  const size_t plugin_count = parse_into(plugin_path, airports_);
  if (plugin_count == 0)
    logging::info("Warning: airport_vrps.json not found at %s",
                  plugin_path.c_str());

  // Optional user override under
  // <X-Plane>/Output/preferences/xp_wellys_devfr_atc/ (plugin) or
  // $XP_ATC_USER_PREFS_DIR (REPL/tests). Per-ICAO replacement - any airport
  // defined in the override file fully replaces the plugin default for that
  // ICAO. Missing file is silent (the override is purely opt-in; users without
  // one fall back to the bundled data). Profile- independent, just like the
  // bundled file.
  const std::string user_path =
      settings::user_prefs_dir() + "/airport_vrps.json";
  const size_t before = airports_.size();
  const size_t override_seen = parse_into(user_path, airports_);
  const size_t added = airports_.size() - before;
  const size_t replaced = override_seen - added;

  if (override_seen > 0) {
    logging::info("Airport VRPs loaded: %zu airports (%zu plugin, "
                  "%zu user overrides: %zu replaced, %zu added) from %s",
                  airports_.size(), plugin_count, override_seen, replaced,
                  added, user_path.c_str());
  } else {
    logging::info("Airport VRPs loaded: %zu airports", airports_.size());
  }
}

void init() { load_from_file(); }

void stop() { airports_.clear(); }

void reload() { load_from_file(); }

const AirportData *get(const std::string &icao) {
  auto it = airports_.find(icao);
  return (it != airports_.end()) ? &it->second : nullptr;
}

static bool is_word_boundary(const std::string &s, size_t pos) {
  if (pos >= s.size())
    return true;
  char c = s[pos];
  return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_');
}

// Check that `t` contains `needle` as a whole word starting at position `pos`.
static bool word_match(const std::string &t, size_t pos,
                       const std::string &needle) {
  if (pos + needle.size() > t.size())
    return false;
  if (t.compare(pos, needle.size(), needle) != 0)
    return false;
  // Preceding boundary
  if (pos > 0) {
    char c = t[pos - 1];
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
      return false;
  }
  return is_word_boundary(t, pos + needle.size());
}

std::string find_in_transcript(const std::string &icao,
                               const std::string &transcript_lower) {
  const AirportData *ad = get(icao);
  if (!ad || ad->vrps.empty() || transcript_lower.empty())
    return {};

  // Position marker words — require one to precede the VRP name so we don't
  // match phonetic callsign letters like "Whiskey" in registration numbers.
  static const std::vector<std::string> kMarkers = {
      // EN / ICAO bridge markers + mixed transmissions.
      "over ", "at ", "passing ", "approaching ", "abeam ", "via ",
      // DE — NfL Sprechfunk 2024 uses "UEBER (markanter Punkt)" as the
      // standard German position reference. Both spellings because Whisper
      // is inconsistent with the umlaut (same fold rationale as the lexicon).
      "ueber ", "über "};

  for (const auto &marker : kMarkers) {
    size_t pos = 0;
    while ((pos = transcript_lower.find(marker, pos)) != std::string::npos) {
      size_t after = pos + marker.size();
      for (const auto &vrp : ad->vrps) {
        std::string needle = to_lower(vrp.name);
        if (word_match(transcript_lower, after, needle))
          return vrp.name;
      }
      pos = after;
    }
  }
  return {};
}

std::string get_pattern_direction(const std::string &icao,
                                  const std::string &runway) {
  const AirportData *ad = get(icao);
  if (!ad || ad->pattern_direction.empty())
    return {};

  // Exact runway match (e.g. "25L")
  auto it = ad->pattern_direction.find(runway);
  if (it != ad->pattern_direction.end())
    return it->second;

  // Base runway — strip L/R/C suffix (e.g. "25L" → "25")
  if (!runway.empty()) {
    char last = runway.back();
    if (last == 'L' || last == 'R' || last == 'C') {
      std::string base = runway.substr(0, runway.size() - 1);
      it = ad->pattern_direction.find(base);
      if (it != ad->pattern_direction.end())
        return it->second;
    }
  }

  // Default for all runways
  it = ad->pattern_direction.find("_default");
  if (it != ad->pattern_direction.end())
    return it->second;

  return {};
}

} // namespace airport_vrps
