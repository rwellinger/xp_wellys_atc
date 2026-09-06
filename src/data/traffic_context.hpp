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

#ifndef TRAFFIC_CONTEXT_HPP
#define TRAFFIC_CONTEXT_HPP

#include <cstdint>
#include <string>
#include <vector>

// Phase 1 traffic foundation: SDK-free struct definitions for the
// "other aircraft" snapshot. Phase 3 will fill TrafficPhase via a
// classifier; until then phase stays Unknown for every target.
namespace traffic_context {

enum class WakeCategory {
  Unknown,
  Light,
  Medium,
  Heavy,
  Super,
};

enum class TrafficPhase {
  Unknown,
  OnGround,
  Taxi,
  Takeoff,
  Climb,
  Cruise,
  Descend,
  Final,
  Pattern,
  Landed,
};

struct TrafficTarget {
  uint32_t modeS_id = 0;
  std::string callsign;
  // ICAO aircraft type code (e.g. "C172"). Empty when the provider
  // doesn't publish it; in that case advisories fall back to the
  // generic "type unknown" phraseology.
  std::string icao_type;

  double lat = 0.0;
  double lon = 0.0;
  double alt_msl_ft = 0.0;
  double alt_agl_ft = 0.0;

  double bearing_from_user_deg = 0.0;
  double clock_position = 12.0;
  double distance_to_user_nm = 0.0;
  double altitude_diff_ft = 0.0;

  double groundspeed_kts = 0.0;
  double vertical_speed_fpm = 0.0;
  double track_deg = 0.0;

  WakeCategory wake = WakeCategory::Unknown;
  TrafficPhase phase = TrafficPhase::Unknown;
};

struct TrafficContext {
  std::vector<TrafficTarget> targets;
  // Monotonic seconds since plugin start of the last successful update.
  // 0 until the runtime reader has produced its first snapshot.
  double last_update_secs = 0.0;
};

// Lifecycle hooks. The runtime reader (plugin module only) implements
// these; the headless atc_repl tool / unit tests do not call them.
void init();
void stop();
void update();

const TrafficContext &current();

// Replace the live snapshot. Used by both the runtime reader (each
// 2 Hz tick) and by atc_repl --traffic-fixture / Catch2 tests to inject
// a deterministic snapshot. Single-writer/single-reader on the X-Plane
// main thread — no thread sync required.
void set_for_test(TrafficContext snapshot);

// Trim a TCAS `flight_id` slot (8 ASCII bytes) to a printable callsign.
// Strips a NUL terminator if present, then leading + trailing whitespace.
// Lives in the SDK-free engine OBJECT lib so Catch2 tests can exercise
// the parser without dragging in the XPLM dataref reader.
std::string trim_callsign(const char *raw, std::size_t len);

} // namespace traffic_context

#endif // TRAFFIC_CONTEXT_HPP
