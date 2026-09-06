/*
 * xp_wellys_vfr_atc - headless CLI
 *
 * Scenario loader + runner. Parses JSON under testscripts/ into an
 * XPlaneContext + a list of pilot-say steps and drives them through
 * engine::process_transcript. Optional substring assertions are
 * checked case-insensitively against ATC response_text.
 */

#ifndef ATC_REPL_SCENARIO_HPP
#define ATC_REPL_SCENARIO_HPP

#include "core/xplane_context.hpp"

#include <optional>
#include <string>
#include <vector>

namespace scenario {

struct Step {
  std::string text;                  // empty = set-only step (no transcript)
  std::optional<std::string> expect; // empty = execute only, no assertion
  std::optional<std::string> expect_state; // assert ATCState name post-step
  std::optional<std::string> expect_not;   // fail if substring appears in response
  std::optional<float> quality; // engine::Input.quality (default 1.0f)
  std::optional<int> wait_sec;  // drive flight_phase + auto-correction this long
  // Context fields to apply BEFORE processing `text`. Ordered so an
  // airport+frequency+freq_type bundle can be changed atomically.
  std::vector<std::pair<std::string, std::string>> set_fields;
  std::optional<std::string> note; // printed to stderr before step
  // Phase-2 hooks: directly drive ATCState (used to land in a state
  // that's hard to reach via pilot intents alone — e.g. EN_ROUTE for
  // an in-contact transit) and tick the per-frame traffic advisor.
  std::optional<std::string> set_state;
  std::optional<double> advisor_tick_now_secs;
};

struct Scenario {
  std::string name;   // JSON "name" or fallback to filename
  std::string region; // "EU" or "US" — default "EU" if absent
  xplane_context::XPlaneContext ctx;
  std::string pilot_callsign;
  std::vector<Step> steps;
  // Optional traffic-context fixture (path resolved relative to the
  // scenario file). When present the loader reads it via
  // traffic_fixture::load() and pushes the snapshot into
  // traffic_context::set_for_test() at run() time. Required for any
  // scenario that drives advisor_tick.
  std::optional<std::string> traffic_fixture_path;
};

// Throws std::runtime_error on parse failure (missing required field,
// wrong type, unknown freq_type string).
Scenario load(const std::string &path);

struct RunResult {
  int steps = 0;      // total steps executed (including set-only)
  int assertions = 0; // steps that had an `expect` clause
  int mismatches = 0; // assertions that failed
};

// Run all steps. Prints on stdout: "PILOT: ...", "ATC: ...",
// "EXPECT: <ok|MISMATCH>". No summary — caller aggregates across files.
RunResult run(const Scenario &scn);

} // namespace scenario
#endif
