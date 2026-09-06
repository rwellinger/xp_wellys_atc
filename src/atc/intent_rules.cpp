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

#include "atc/intent_rules.hpp"
#include "atc/atc_state_machine.hpp"
#include "atc/intent_parser.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <json.hpp>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace intent_rules {

using PI = intent_parser::PilotIntent;

// ── Predicate model ─────────────────────────────────────────────────────────

enum class PredKind {
  CONTAINS,
  STARTS_WITH,
  ENDS_WITH,
  HAS_FACILITY,
  ENDS_WITH_RUNWAY_TOKEN,
  COMPOUND, // nested any/all/none — same evaluation logic as a Rule body
};

struct Predicate {
  PredKind kind = PredKind::CONTAINS;
  std::string value; // unused for ENDS_WITH_RUNWAY_TOKEN and COMPOUND
  // Only populated for COMPOUND.
  std::vector<Predicate> any_;
  std::vector<Predicate> all_;
  std::vector<Predicate> none_;
};

struct Rule {
  PI intent = PI::UNKNOWN;
  float confidence = 0.0f;
  std::vector<Predicate> any_;
  std::vector<Predicate> all_;
  std::vector<Predicate> none_;
};

struct Adjustment {
  // Conditions (all must hold)
  std::optional<PI> intent_eq;
  std::vector<PI> intent_in;
  std::optional<bool> intent_is_airborne_only;
  std::optional<bool> intent_is_ground_only;
  std::optional<bool> on_ground;
  std::optional<bool> is_towered;
  std::optional<bool> vrp_name_set;
  std::string text_contains;
  // Session-lifecycle conditions sourced directly from
  // atc_state_machine (see Architektur-Entscheidung in
  // plans/.../was_airborne-veto: apply_adjustments runs on the flight-
  // loop thread, same as the State-Machine, so a direct singleton read
  // is consistent with the existing require_context_flag pattern).
  std::optional<bool> readback_pending;
  std::optional<bool> was_airborne;
  // Context-flag conditions sourced from atc_state_machine. Supported:
  // "just_landed" (120 s time window after touchdown) and
  // "at_airport_after_landing" (state window: on ground, last landing in
  // history, no DEPARTURE_CLEARED since — survives long roll-out).
  std::string require_context_flag;
  // Actions
  std::optional<PI> set_intent;
  std::optional<float> set_confidence;
};

struct RuleTable {
  std::vector<std::pair<std::string, std::string>> normalizations;
  std::vector<Rule> rules;
  std::vector<Adjustment> adjustments;
  bool loaded = false;
};

static RuleTable g_table;

// ── Intent set helpers (mirror old hardcoded lambdas) ───────────────────────

static bool is_airborne_only_intent(PI i) {
  return i == PI::REPORT_POSITION || i == PI::REPORT_POSITION_DOWNWIND ||
         i == PI::REPORT_POSITION_BASE || i == PI::REPORT_POSITION_FINAL ||
         i == PI::REQUEST_LANDING || i == PI::REQUEST_TOUCH_AND_GO ||
         i == PI::GO_AROUND;
}

static bool is_ground_only_intent(PI i) {
  return i == PI::INITIAL_CALL || i == PI::INITIAL_CALL_GROUND ||
         i == PI::INITIAL_CALL_TOWER || i == PI::REQUEST_TAXI ||
         i == PI::REQUEST_TAXI_PARKING || i == PI::READY_FOR_DEPARTURE ||
         i == PI::READY_FOR_DEPARTURE_VFR || i == PI::RUNWAY_VACATED ||
         i == PI::SELF_ANNOUNCE_DEPARTURE;
}

// ── Spoken-digit table for ENDS_WITH_RUNWAY_TOKEN ───────────────────────────
// Mirrors kSpokenDigits in intent_parser.cpp but only the words for runway
// numbers 1..36 + the L/R/C suffixes.

static const std::vector<std::string> &runway_token_suffixes() {
  static const std::vector<std::string> v = {
      "left",
      "right",
      "center",
      "one",
      "two",
      "three",
      "four",
      "five",
      "six",
      "seven",
      "eight",
      "nine",
      "niner",
      "ten",
      "eleven",
      "twelve",
      "thirteen",
      "fourteen",
      "fifteen",
      "sixteen",
      "seventeen",
      "eighteen",
      "nineteen",
      "twenty",
      "twenty one",
      "twenty two",
      "twenty three",
      "twenty four",
      "twenty five",
      "twenty six",
      "twenty seven",
      "twenty eight",
      "twenty nine",
      "thirty",
      "thirty one",
      "thirty two",
      "thirty three",
      "thirty four",
      "thirty five",
      "thirty six",
      // DE BZF additions (additive — only matched in DE transcripts,
      // EN-region transcripts never end with these tokens).
      "links",
      "rechts",
      "mitte",
      "null",
      "eins",
      "zwo",
      "zwei",
      "drei",
      "vier",
      "fuenf",
      "sechs",
      "sieben",
      "acht",
      "neun",
  };
  return v;
}

// ── Predicate evaluation ────────────────────────────────────────────────────

static bool ends_with(const std::string &hay, const std::string &needle) {
  if (needle.size() > hay.size())
    return false;
  return hay.compare(hay.size() - needle.size(), needle.size(), needle) == 0;
}

static bool starts_with(const std::string &hay, const std::string &needle) {
  return hay.rfind(needle, 0) == 0;
}

// Padded standalone-word match: normalize non-alnum runs to spaces, surround
// the input with spaces, then look for " <facility> ". Same semantics as the
// old has_facility_keyword() helper.
static bool has_facility(const std::string &t, const std::string &facility) {
  std::string norm;
  norm.reserve(t.size() + 2);
  norm += ' ';
  for (char c : t) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      norm += c;
    else
      norm += ' ';
  }
  norm += ' ';
  std::string padded = " " + facility + " ";
  return norm.find(padded) != std::string::npos;
}

// Forward decls for mutual recursion with COMPOUND.
static bool any_match(const std::vector<Predicate> &preds,
                      const std::string &text);
static bool all_match(const std::vector<Predicate> &preds,
                      const std::string &text);
static bool none_match(const std::vector<Predicate> &preds,
                       const std::string &text);

static bool eval_predicate(const Predicate &p, const std::string &text) {
  switch (p.kind) {
  case PredKind::CONTAINS:
    return text.find(p.value) != std::string::npos;
  case PredKind::STARTS_WITH:
    return starts_with(text, p.value);
  case PredKind::ENDS_WITH:
    return ends_with(text, p.value);
  case PredKind::HAS_FACILITY:
    return has_facility(text, p.value);
  case PredKind::ENDS_WITH_RUNWAY_TOKEN:
    for (const auto &suffix : runway_token_suffixes()) {
      if (ends_with(text, suffix))
        return true;
    }
    return false;
  case PredKind::COMPOUND:
    return none_match(p.none_, text) && any_match(p.any_, text) &&
           all_match(p.all_, text);
  }
  return false;
}

static bool any_match(const std::vector<Predicate> &preds,
                      const std::string &text) {
  if (preds.empty())
    return true; // empty `any` means "no constraint"
  for (const auto &p : preds)
    if (eval_predicate(p, text))
      return true;
  return false;
}

static bool all_match(const std::vector<Predicate> &preds,
                      const std::string &text) {
  for (const auto &p : preds)
    if (!eval_predicate(p, text))
      return false;
  return true;
}

static bool none_match(const std::vector<Predicate> &preds,
                       const std::string &text) {
  for (const auto &p : preds)
    if (eval_predicate(p, text))
      return false;
  return true;
}

// ── JSON parsing ────────────────────────────────────────────────────────────

static Predicate parse_predicate(const nlohmann::json &node);

static std::vector<Predicate> parse_predicate_list(const nlohmann::json &arr) {
  std::vector<Predicate> out;
  if (!arr.is_array())
    return out;
  out.reserve(arr.size());
  for (const auto &node : arr)
    out.push_back(parse_predicate(node));
  return out;
}

static Predicate parse_predicate(const nlohmann::json &node) {
  Predicate p;
  if (node.is_string()) {
    p.kind = PredKind::CONTAINS;
    p.value = node.get<std::string>();
    return p;
  }
  if (!node.is_object()) {
    p.kind = PredKind::CONTAINS;
    return p;
  }
  if (node.contains("contains")) {
    p.kind = PredKind::CONTAINS;
    p.value = node.value("contains", "");
  } else if (node.contains("starts_with")) {
    p.kind = PredKind::STARTS_WITH;
    p.value = node.value("starts_with", "");
  } else if (node.contains("ends_with")) {
    p.kind = PredKind::ENDS_WITH;
    p.value = node.value("ends_with", "");
  } else if (node.contains("has_facility")) {
    p.kind = PredKind::HAS_FACILITY;
    p.value = node.value("has_facility", "");
  } else if (node.contains("ends_with_runway_token")) {
    p.kind = PredKind::ENDS_WITH_RUNWAY_TOKEN;
  } else if (node.contains("any") || node.contains("all") ||
             node.contains("none")) {
    p.kind = PredKind::COMPOUND;
    if (node.contains("any"))
      p.any_ = parse_predicate_list(node["any"]);
    if (node.contains("all"))
      p.all_ = parse_predicate_list(node["all"]);
    if (node.contains("none"))
      p.none_ = parse_predicate_list(node["none"]);
  } else {
    p.kind = PredKind::CONTAINS;
  }
  return p;
}

static Rule parse_rule(const nlohmann::json &node) {
  Rule r;
  std::string intent_key = node.value("intent", "UNKNOWN");
  r.intent = intent_parser::intent_from_key(intent_key);
  r.confidence = node.value("confidence", 0.0f);
  if (node.contains("any"))
    r.any_ = parse_predicate_list(node["any"]);
  if (node.contains("all"))
    r.all_ = parse_predicate_list(node["all"]);
  if (node.contains("none"))
    r.none_ = parse_predicate_list(node["none"]);
  return r;
}

static Adjustment parse_adjustment(const nlohmann::json &node) {
  Adjustment a;
  if (node.contains("if") && node["if"].is_object()) {
    const auto &cond = node["if"];
    if (cond.contains("intent") && cond["intent"].is_string()) {
      a.intent_eq =
          intent_parser::intent_from_key(cond["intent"].get<std::string>());
    }
    if (cond.contains("intent_in") && cond["intent_in"].is_array()) {
      for (const auto &k : cond["intent_in"]) {
        if (k.is_string())
          a.intent_in.push_back(
              intent_parser::intent_from_key(k.get<std::string>()));
      }
    }
    if (cond.contains("intent_is_airborne_only"))
      a.intent_is_airborne_only = cond["intent_is_airborne_only"].get<bool>();
    if (cond.contains("intent_is_ground_only"))
      a.intent_is_ground_only = cond["intent_is_ground_only"].get<bool>();
    if (cond.contains("on_ground"))
      a.on_ground = cond["on_ground"].get<bool>();
    if (cond.contains("is_towered"))
      a.is_towered = cond["is_towered"].get<bool>();
    if (cond.contains("vrp_name_set"))
      a.vrp_name_set = cond["vrp_name_set"].get<bool>();
    if (cond.contains("text_contains") && cond["text_contains"].is_string())
      a.text_contains = cond["text_contains"].get<std::string>();
    if (cond.contains("readback_pending"))
      a.readback_pending = cond["readback_pending"].get<bool>();
    if (cond.contains("was_airborne"))
      a.was_airborne = cond["was_airborne"].get<bool>();
    if (cond.contains("require_context_flag") &&
        cond["require_context_flag"].is_string())
      a.require_context_flag = cond["require_context_flag"].get<std::string>();
  }
  if (node.contains("set_intent") && node["set_intent"].is_string()) {
    a.set_intent =
        intent_parser::intent_from_key(node["set_intent"].get<std::string>());
  }
  if (node.contains("set_confidence")) {
    a.set_confidence = node["set_confidence"].get<float>();
  }
  return a;
}

static void load_from_file() {
  g_table = {};
  std::string path = settings::atc_profile_data_dir() + "/intent_rules.json";
  std::ifstream in(path);
  if (!in.good()) {
    logging::info("Warning: intent_rules.json not found at %s", path.c_str());
    return;
  }
  nlohmann::json doc;
  try {
    in >> doc;
  } catch (...) {
    logging::info("Warning: failed to parse intent_rules.json");
    return;
  }

  if (doc.contains("normalize") && doc["normalize"].is_array()) {
    for (const auto &node : doc["normalize"]) {
      if (!node.is_object())
        continue;
      g_table.normalizations.push_back(
          {node.value("from", ""), node.value("to", "")});
    }
  }
  if (doc.contains("rules") && doc["rules"].is_array()) {
    g_table.rules.reserve(doc["rules"].size());
    for (const auto &node : doc["rules"])
      g_table.rules.push_back(parse_rule(node));
  }
  if (doc.contains("adjustments") && doc["adjustments"].is_array()) {
    g_table.adjustments.reserve(doc["adjustments"].size());
    for (const auto &node : doc["adjustments"])
      g_table.adjustments.push_back(parse_adjustment(node));
  }
  g_table.loaded = true;
  logging::info(
      "Intent rules loaded: %zu rules, %zu adjustments, %zu normalizations",
      g_table.rules.size(), g_table.adjustments.size(),
      g_table.normalizations.size());
}

// ── Public API ──────────────────────────────────────────────────────────────

void init() { load_from_file(); }
void stop() { g_table = {}; }
void reload() { load_from_file(); }

bool is_loaded() { return g_table.loaded; }

std::string preprocess(const std::string &lowercased_text) {
  if (!g_table.loaded || g_table.normalizations.empty())
    return lowercased_text;
  std::string out = lowercased_text;
  for (const auto &[from, to] : g_table.normalizations) {
    if (from.empty())
      continue;
    std::string::size_type pos = 0;
    while ((pos = out.find(from, pos)) != std::string::npos) {
      out.replace(pos, from.size(), to);
      pos += to.size();
    }
  }
  return out;
}

MatchResult match(const std::string &text) {
  if (!g_table.loaded)
    return {PI::UNKNOWN, 0.0f};
  for (const auto &r : g_table.rules) {
    if (none_match(r.none_, text) && any_match(r.any_, text) &&
        all_match(r.all_, text)) {
      return {r.intent, r.confidence};
    }
  }
  return {PI::UNKNOWN, 0.0f};
}

static bool adjustment_applies(const Adjustment &a,
                               const intent_parser::PilotMessage &msg,
                               const xplane_context::XPlaneContext &ctx,
                               const std::string &text) {
  if (a.intent_eq && msg.intent != *a.intent_eq)
    return false;
  if (!a.intent_in.empty()) {
    bool found = false;
    for (PI i : a.intent_in)
      if (msg.intent == i) {
        found = true;
        break;
      }
    if (!found)
      return false;
  }
  if (a.intent_is_airborne_only &&
      is_airborne_only_intent(msg.intent) != *a.intent_is_airborne_only)
    return false;
  if (a.intent_is_ground_only &&
      is_ground_only_intent(msg.intent) != *a.intent_is_ground_only)
    return false;
  if (a.on_ground && ctx.on_ground != *a.on_ground)
    return false;
  if (a.is_towered && ctx.is_towered() != *a.is_towered)
    return false;
  if (a.vrp_name_set && msg.vrp_name.empty() == *a.vrp_name_set)
    return false;
  if (!a.text_contains.empty() &&
      text.find(a.text_contains) == std::string::npos)
    return false;
  if (a.readback_pending &&
      atc_state_machine::is_readback_pending() != *a.readback_pending)
    return false;
  if (a.was_airborne && atc_state_machine::was_airborne() != *a.was_airborne)
    return false;
  if (!a.require_context_flag.empty()) {
    if (a.require_context_flag == "just_landed") {
      if (!atc_state_machine::just_landed(ctx.now_secs))
        return false;
    } else if (a.require_context_flag == "at_airport_after_landing") {
      // State-window variant: lives until DEPARTURE_CLEARED enters
      // history. Survives long roll-out / taxi-back where the 120 s
      // just_landed window already expired.
      if (!atc_state_machine::at_airport_after_landing(ctx))
        return false;
    } else {
      // Unknown flag — be conservative and reject so a typo in JSON
      // doesn't silently turn the rule into an unconditional match.
      return false;
    }
  }
  return true;
}

void apply_adjustments(intent_parser::PilotMessage &msg,
                       const xplane_context::XPlaneContext &ctx,
                       const std::string &preprocessed_text) {
  if (!g_table.loaded)
    return;
  for (const auto &a : g_table.adjustments) {
    if (!adjustment_applies(a, msg, ctx, preprocessed_text))
      continue;
    if (a.set_intent)
      msg.intent = *a.set_intent;
    if (a.set_confidence)
      msg.confidence = *a.set_confidence;
  }
}

} // namespace intent_rules
