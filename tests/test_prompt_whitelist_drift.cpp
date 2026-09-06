/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under GPL-3.0-or-later. See LICENSE.
 */

// Drift guard: the gpt_classify_prompt_de classifier prompt carries worked
// examples whose `valid=[...]` list advertises the intents the model may emit
// for a given state. The model output is post-hoc validated against
// atc_templates::valid_intents (backends/manager), so any intent the prompt
// advertises but the whitelist does not honor is dead — the classification is
// wasted and lands on _INVALID. See issue #5.
//
// This test asserts every intent named in a prompt example is actually in
// valid_intents() for that example's state. valid_intents (derived from the
// template keys) is the source of truth; the prompt must follow it. The test
// therefore goes red when a PROMPT example is changed to advertise an
// unsupported intent, NOT when the whitelist is legitimately edited together
// with the prompt.
//
// Two wrinkles handled below:
//   1. State naming. Runtime state strings (atc_state_machine::state_name)
//      carry a flow prefix — "Pattern/LANDING_CLEARED", "XC/DEPARTURE_CLEARED".
//      The prompt examples are inconsistent: some spell the bare suffix
//      ("state=LANDING_CLEARED"), some the full key. The resolver tries the
//      bare form and both documented prefixes and unions the matches, so a
//      cosmetic state spelling does not cause a false positive. An unknown
//      state resolves to the empty set and fails loudly.
//   2. IDLE is overloaded (fresh-spawn vs post-landing). valid_intents splits
//      it on post_landing. The example's just_landed flag is mapped to
//      post_landing so an IDLE example is checked against the correct half —
//      a fresh-spawn example advertising the post-landing sign-off (or vice
//      versa) is caught.
//
// All current examples are towered-field context, so is_towered=true.

#include "atc/atc_templates.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Documented flow-prefix conventions on the runtime state strings. The bare
// "" form covers prefixless states (IDLE, GROUND_CONTACT, ...).
const char *kFlowPrefixes[] = {"", "Pattern/", "XC/"};

// Resolve a prompt-example state to the canonical intent set by trying the
// bare form and both flow prefixes against the source-of-truth whitelist,
// unioning the matches. Non-existent state strings contribute the empty set.
std::set<std::string> resolve_valid(const std::string &prompt_state,
                                    bool post_landing) {
  std::set<std::string> all;
  for (const char *pfx : kFlowPrefixes) {
    auto v = atc_templates::valid_intents(
        /*is_towered=*/true, std::string(pfx) + prompt_state, post_landing);
    all.insert(v.begin(), v.end());
  }
  return all;
}

struct Example {
  std::string state;
  bool post_landing = false;
  std::vector<std::string> intents;
  std::string line; // kept for diagnostics
};

std::string trim(const std::string &s) {
  auto b = s.find_first_not_of(" \t");
  if (b == std::string::npos)
    return {};
  auto e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

// Extract every `state=..., [just_landed=...,] valid=[...]` example from the
// prompt. One example per line; prose lines never carry a valid=[...] list, so
// requiring one bounds matching to genuine examples.
std::vector<Example> parse_examples(const std::string &prompt) {
  // Require a non-word char (or line start) before "state=" so the "state="
  // substring inside "previous_state=" cannot be mistaken for the example
  // state, regardless of token order.
  static const std::regex state_re(
      R"((?:^|[^A-Za-z0-9_])state=([A-Za-z0-9_/]+))");
  static const std::regex jl_re(R"(just_landed=(true|false))");
  static const std::regex valid_re(R"(valid=\[([^\]]*)\])");

  std::vector<Example> out;
  std::istringstream stream(prompt);
  std::string line;
  while (std::getline(stream, line)) {
    std::smatch m;
    if (!std::regex_search(line, m, valid_re))
      continue;
    std::string valid_body = m[1];

    Example ex;
    ex.line = line;
    if (std::regex_search(line, m, state_re))
      ex.state = m[1];
    if (ex.state.empty())
      continue;

    // just_landed absent -> false. Only IDLE consults post_landing, and
    // every IDLE example states the flag explicitly.
    if (std::regex_search(line, m, jl_re))
      ex.post_landing = (m[1] == "true");

    std::istringstream vs(valid_body);
    std::string token;
    while (std::getline(vs, token, ',')) {
      token = trim(token);
      if (token.empty() || token == "...") // ellipsis placeholder
        continue;
      ex.intents.push_back(token);
    }
    if (!ex.intents.empty())
      out.push_back(ex);
  }
  return out;
}

} // namespace

TEST_CASE("prompt-drift: every intent advertised in a gpt_classify_prompt_de "
          "example is honored by valid_intents for that state",
          "[prompt_drift][de]") {
  atc_templates::reload();

  std::string prompt = atc_templates::get_prompt("gpt_classify_prompt_de");
  REQUIRE_FALSE(prompt.empty());

  auto examples = parse_examples(prompt);
  // Guard against a renamed/emptied prompt or a broken regex passing
  // vacuously: the prompt ships six worked examples with valid=[...] today.
  REQUIRE(examples.size() >= 5);

  for (const auto &ex : examples) {
    auto whitelist = resolve_valid(ex.state, ex.post_landing);
    for (const auto &intent : ex.intents) {
      INFO("state=" << ex.state << " post_landing=" << ex.post_landing
                    << " intent=" << intent << "\n  example: " << ex.line);
      CHECK(whitelist.count(intent) == 1);
    }
  }
}

// Same drift guard for the English/ICAO-VFR classifier prompt (#39). The
// intent whitelist is profile-neutral (identical keys in de/ and en/
// templates), so the advertised EN example intents are checked against the
// same valid_intents source of truth.
TEST_CASE("prompt-drift: every intent advertised in a gpt_classify_prompt_en "
          "example is honored by valid_intents for that state",
          "[prompt_drift][en]") {
  atc_templates::reload();

  std::string prompt = atc_templates::get_prompt("gpt_classify_prompt_en");
  REQUIRE_FALSE(prompt.empty());

  auto examples = parse_examples(prompt);
  // The EN prompt ships five worked examples with valid=[...] today.
  REQUIRE(examples.size() >= 4);

  for (const auto &ex : examples) {
    auto whitelist = resolve_valid(ex.state, ex.post_landing);
    for (const auto &intent : ex.intents) {
      INFO("state=" << ex.state << " post_landing=" << ex.post_landing
                    << " intent=" << intent << "\n  example: " << ex.line);
      CHECK(whitelist.count(intent) == 1);
    }
  }
}

// The language-parametrised prompt selection (engine.cpp) falls back to the
// German prompt when the language-specific key is missing. That fallback must
// never silently mask an absent EN prompt: assert both language classifier
// prompts exist and are non-empty, so an accidental deletion of the EN prompt
// is caught here rather than shipping EN users the German classifier.
TEST_CASE("both language classifier prompts exist", "[prompt_drift]") {
  atc_templates::reload();
  CHECK_FALSE(atc_templates::get_prompt("gpt_classify_prompt_de").empty());
  CHECK_FALSE(atc_templates::get_prompt("gpt_classify_prompt_en").empty());
}
