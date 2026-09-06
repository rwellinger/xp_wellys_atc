/*
 * xp_wellys_vfr_atc - headless CLI shared with Catch2 tests.
 *
 * Loads a deterministic `TrafficContext` fixture from disk. Recomputes
 * derived fields (bearing, clock, distance, alt_diff) via the
 * traffic_geometry helpers so the fixture exercises the same code path
 * the live runtime reader does — minus the XPLM dataref read.
 */

#ifndef XP_WELLYS_ATC_TRAFFIC_FIXTURE_HPP
#define XP_WELLYS_ATC_TRAFFIC_FIXTURE_HPP

#include "data/traffic_context.hpp"

#include <string>

namespace traffic_fixture {

struct UserState {
  double lat = 0.0;
  double lon = 0.0;
  double alt_msl_ft = 0.0;
  double heading_true = 0.0;
  std::string nearest_airport_id;
  double airport_elevation_ft = 0.0;
  // Optional Phase-4 active-runway hints. When present in the fixture
  // JSON ("active_runway" block under "user"), passed through to the
  // traffic_phase_classifier so it can produce Pattern/Final targets.
  bool has_active_runway = false;
  double airport_lat = 0.0;
  double airport_lon = 0.0;
  double threshold_lat = 0.0;
  double threshold_lon = 0.0;
  double runway_heading_deg = 0.0;
  double runway_length_m = 2500.0;
  std::string runway_id;
  std::string pattern_direction;
};

struct LoadedFixture {
  UserState user;
  traffic_context::TrafficContext snapshot;
};

// Load + parse the JSON file at `path`. Filters targets > 40 NM out (so
// the snapshot matches what the live runtime would produce). Sorts by
// distance ascending. Throws std::runtime_error with a context-rich
// message on bad input.
LoadedFixture load(const std::string &path);

} // namespace traffic_fixture

#endif // XP_WELLYS_ATC_TRAFFIC_FIXTURE_HPP
