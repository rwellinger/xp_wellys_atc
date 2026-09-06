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

#ifndef OPENAIR_DB_HPP
#define OPENAIR_DB_HPP

#include <string>
#include <vector>

// Real 3-D airspace database parsed from OpenAir-format files. Unlike
// data/airspace_db (a coarse controller index derived from X-Plane's
// atc.dat), this module carries actual airspace polygons WITH lower/upper
// limits, so a position can be classified inside a specific CTR / RMZ /
// TMZ / TMA in three dimensions.
//
// German-VFR focus: the indexed classes are the ones a VFR pilot must be
// aware of on the German chart — CTR (Klasse D control zones), RMZ (radio
// mandatory), TMZ (transponder mandatory), TMA/CTA (controlled areas) and
// ED-R restricted areas. Danger/prohibited/glider zones are ignored for
// now (detection only; wiring to the RMZ_* intents + squawk/frequency
// suggestions is a follow-up — see issue #29).
//
// SDK-free: lives in the engine OBJECT library. The plugin (main.cpp)
// resolves the bundled + user-override file paths and passes them to
// init(); headless tools/tests pass fixture paths (or empty to disable).
namespace openair_db {

// Airspace class parsed from the OpenAir "AC" record (or upgraded from the
// "AN" name when the AC line only carries an ICAO class letter).
enum class AirspaceClass {
  CTR,   // Control Zone            — AC CTR (German: Klasse-D-Kontrollzone)
  TMA,   // Terminal Maneuvering    — AC TMA
  CTA,   // Control Area            — AC CTA
  RMZ,   // Radio Mandatory Zone    — AC RMZ (Funkkommunikationspflicht)
  TMZ,   // Transponder Mandatory   — AC TMZ
  ED_R,  // Restricted area         — AC R  (Germany: ED-R)
  OTHER, // everything else (danger, prohibited, glider, class E/F/G …)
};

// Stable human-readable tag ("CTR", "RMZ", …). Never returns nullptr.
const char *class_name(AirspaceClass c);

// Result of find_enclosing(). When the position is outside all indexed
// airspaces, ac_class == OTHER and name is empty.
struct AirspaceEntry {
  std::string name;
  AirspaceClass ac_class = AirspaceClass::OTHER;
  int floor_ft = 0;
  int ceiling_ft = 0;
};

// Load airspaces from up to two OpenAir-format files, layered:
//   bundled_path  — the shipped starter set (data/airspaces/de_airspace.txt)
//   user_path     — optional user override (appended; a full openaip.net
//                   Germany export dropped here supplements the starter set)
// Either may be empty to skip that layer. Parsing runs on a worker thread;
// query functions return empty until ready(). Pass both empty to disable
// (headless tools with no airspace data).
void init(std::string bundled_path, std::string user_path);
void stop();

// Returns true once init() has finished (success or files-absent).
bool ready();

// Number of indexed airspace entries. Useful for UI / diagnostics.
std::size_t entry_count();

// Returns the innermost (smallest bounding-box area) airspace that contains
// (lat, lon, alt_ft) in 3-D: 2-D point-in-polygon AND floor_ft <= alt_ft
// <= ceiling_ft.  Returns an OTHER entry (empty name) when the position is
// outside all indexed airspaces.
AirspaceEntry find_enclosing(double lat, double lon, int alt_ft);

// Returns ALL airspaces that contain (lat, lon, alt_ft) in 3-D. Use this to
// see every zone the aircraft is inside regardless of nesting — a large
// background TMA must not mask an inner CTR/RMZ, and RMZ/TMZ frequently
// overlap the CTR they surround.
std::vector<AirspaceEntry> find_all_enclosing(double lat, double lon,
                                              int alt_ft);

} // namespace openair_db

#endif // OPENAIR_DB_HPP
