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

#include "atc/initial_call_conformance.hpp"

#include "atc/de_phraseology.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace initial_call_conformance {

namespace {

nlohmann::json g_config;
bool g_loaded = false;

std::string to_lower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// Normalised pilot transcript: lowercased, spoken-number runs reversed to
// raw digits (so "eins null eins drei" matches "1013"), then every
// non-alphanumeric char turned into a single space and the whole string
// space-padded. Token-bounded keyword search ("  vfr  ".find(" vfr "))
// then avoids substring false hits ("nach" inside "nachbar").
std::string token_form(const std::string &raw) {
  std::string lc = to_lower(de_phraseology::parse_spoken_number(raw));
  std::string out = " ";
  for (char c : lc) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      out += c;
    else
      out += ' ';
  }
  out += ' ';
  return out;
}

bool token_contains(const std::string &padded, const std::string &needle) {
  if (needle.empty())
    return false;
  return padded.find(" " + to_lower(needle) + " ") != std::string::npos;
}

// True when the transcript carries an ATIS information letter: the anchor
// token "information" immediately followed (within two tokens) by a NATO
// letter word. The anchor is distinctive; the letter itself may be
// garbled by STT, so a generous letter set is accepted.
bool has_information_letter(const std::string &raw) {
  static const char *kLetters[] = {
      "alfa",   "alpha",  "bravo",   "charlie", "delta",   "echo",
      "foxtrot", "golf",  "hotel",   "india",   "juliet",  "juliett",
      "kilo",   "lima",   "mike",    "november", "oscar",  "papa",
      "quebec", "romeo",  "sierra",  "tango",   "uniform", "victor",
      "whiskey", "wisky", "xray",    "yankee",  "zulu"};

  // Tokenise the lowercased transcript on whitespace/punctuation.
  std::string lc = to_lower(raw);
  std::vector<std::string> tokens;
  std::string cur;
  for (char c : lc) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      cur += c;
    } else if (!cur.empty()) {
      tokens.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty())
    tokens.push_back(cur);

  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].rfind("information", 0) != 0)
      continue;
    for (std::size_t j = i + 1; j <= i + 2 && j < tokens.size(); ++j) {
      for (const char *l : kLetters)
        if (tokens[j] == l)
          return true;
    }
  }
  return false;
}

std::vector<std::string> keyword_list(const nlohmann::json &node,
                                      const std::string &element) {
  std::vector<std::string> out;
  if (!node.contains("element_keywords"))
    return out;
  const auto &kw = node["element_keywords"];
  if (!kw.contains(element) || !kw[element].is_array())
    return out;
  for (const auto &v : kw[element])
    if (v.is_string())
      out.push_back(v.get<std::string>());
  return out;
}

bool any_keyword(const std::string &padded,
                 const std::vector<std::string> &keywords) {
  for (const auto &k : keywords)
    if (token_contains(padded, k))
      return true;
  return false;
}

// Per-element detector dispatch. Unknown element names return true
// (treated as present) so a misconfigured key never blocks the pilot.
bool element_present(const std::string &element, const nlohmann::json &node,
                     const std::string &padded,
                     const intent_parser::PilotMessage &msg,
                     const xplane_context::XPlaneContext &ctx) {
  if (element == "position")
    return msg.has_position || !msg.vrp_name.empty();

  if (element == "atis_letter")
    return has_information_letter(msg.raw_transcript);

  if (element == "intention")
    return any_keyword(padded, keyword_list(node, "intention"));

  if (element == "request")
    return any_keyword(padded, keyword_list(node, "request"));

  if (element == "aircraft_type") {
    if (any_keyword(padded, keyword_list(node, "aircraft_type")))
      return true;
    // Live acf_ICAO ("DV20") spoken back by the pilot also counts.
    if (!ctx.aircraft_icao.empty() &&
        token_contains(padded, ctx.aircraft_icao))
      return true;
    return false;
  }

  return true; // unknown element key -> never penalise
}

std::vector<std::string> string_array(const nlohmann::json &node,
                                      const char *key) {
  std::vector<std::string> out;
  if (!node.contains(key) || !node[key].is_array())
    return out;
  for (const auto &v : node[key])
    if (v.is_string())
      out.push_back(v.get<std::string>());
  return out;
}

} // namespace

void reload() {
  g_loaded = false;
  g_config = nlohmann::json::object();

  std::string path = settings::atc_profile_data_dir() + "/conformance.json";
  std::ifstream in(path);
  if (!in.good()) {
    logging::info("Conformance rules not found (checker inert)");
    return;
  }
  try {
    in >> g_config;
    g_loaded = true;
    logging::info("Conformance rules loaded");
  } catch (...) {
    g_config = nlohmann::json::object();
    logging::info("Warning: failed to parse conformance.json");
  }
}

void init() { reload(); }

Result evaluate(const std::string &intent_key,
                const intent_parser::PilotMessage &msg,
                const xplane_context::XPlaneContext &ctx) {
  Result r;
  if (!g_loaded || !g_config.contains(intent_key))
    return r;

  const auto &node = g_config[intent_key];
  const std::string padded = token_form(msg.raw_transcript);

  for (const auto &el : string_array(node, "required"))
    if (!element_present(el, node, padded, msg, ctx))
      r.missing_required.push_back(el);

  for (const auto &el : string_array(node, "recommended"))
    if (!element_present(el, node, padded, msg, ctx))
      r.missing_recommended.push_back(el);

  return r;
}

std::string build_request_prompt(const std::string &intent_key,
                                 const std::string &callsign,
                                 const std::vector<std::string> &missing) {
  std::string tmpl = "{callsign}, sagen Sie {missing}.";
  const nlohmann::json *prompts = nullptr;
  if (g_loaded && g_config.contains(intent_key)) {
    const auto &node = g_config[intent_key];
    tmpl = node.value("missing_response", tmpl);
    if (node.contains("element_prompts") && node["element_prompts"].is_object())
      prompts = &node["element_prompts"];
  }

  // Resolve each element key to its readable prompt; default to the raw
  // key so a missing prompt entry is still legible.
  std::vector<std::string> labels;
  for (const auto &el : missing) {
    std::string label = el;
    if (prompts && prompts->contains(el) && (*prompts)[el].is_string())
      label = (*prompts)[el].get<std::string>();
    labels.push_back(label);
  }

  // Join in BZF form: "A", "A und B", "A, B und C".
  std::string joined;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if (i == 0)
      joined = labels[i];
    else if (i + 1 == labels.size())
      joined += " und " + labels[i];
    else
      joined += ", " + labels[i];
  }

  std::string out = tmpl;
  auto replace = [&out](const std::string &ph, const std::string &val) {
    std::string::size_type pos = 0;
    while ((pos = out.find(ph, pos)) != std::string::npos) {
      out.replace(pos, ph.size(), val);
      pos += val.size();
    }
  };
  replace("{callsign}", callsign);
  replace("{missing}", joined);
  return out;
}

} // namespace initial_call_conformance
