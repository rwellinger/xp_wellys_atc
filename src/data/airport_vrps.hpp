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

#ifndef AIRPORT_VRPS_HPP
#define AIRPORT_VRPS_HPP

#include <map>
#include <string>
#include <vector>

namespace airport_vrps {

struct VRP {
  std::string name; // canonical capitalized name, e.g. "Whiskey"
  double lat = 0.0;
  double lon = 0.0;
  int alt_ft = 0;
};

struct AirportData {
  std::string name;
  std::vector<VRP> vrps;
  // arrival_routes: runway designator (e.g. "14") → list of preferred VRP
  // names for that runway. Empty if not published.
  std::map<std::string, std::vector<std::string>> arrival_routes;
  // pattern_direction: runway designator → "left"/"right".
  // Key "_default" applies when no runway-specific entry matches.
  std::map<std::string, std::string> pattern_direction;
};

void init();
void stop();
void reload();

// Get VRP data for an airport. Returns nullptr when not in the database.
const AirportData *get(const std::string &icao);

// Scan a lowercased transcript for the first VRP name registered at the
// given airport. Returns the canonical VRP name ("Whiskey"), or empty
// string if nothing matched. Matching requires a position marker prefix
// ("over", "at", "passing", "abeam", "approaching") to avoid collisions
// with phonetic callsign letters.
std::string find_in_transcript(const std::string &icao,
                               const std::string &transcript_lower);

// Return pattern direction ("left"/"right") for an airport+runway from the
// airport database. Lookup chain: exact runway → base runway (strip L/R/C)
// → _default → empty string (caller should fall back to global settings).
std::string get_pattern_direction(const std::string &icao,
                                  const std::string &runway);

} // namespace airport_vrps

#endif // AIRPORT_VRPS_HPP
