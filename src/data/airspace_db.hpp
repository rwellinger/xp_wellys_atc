/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef AIRSPACE_DB_HPP
#define AIRSPACE_DB_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace airspace_db {

enum class ControllerRole {
  UNKNOWN,
  TWR,    // tower
  TRACON, // approach / departure
  CTR,    // center / ACC — used for FIS in XP12 atc.dat
};

const char *role_name(ControllerRole r);

// One controller record. Polygon points are lazily loaded on first
// geometric query that needs them (see find_enclosing/lookup_by_freq).
struct Controller {
  std::string name;        // e.g. "ZURICH", "SWITZERLAND"
  std::string facility_id; // e.g. "LSZH", "LSAS" (may be empty)
  ControllerRole role = ControllerRole::UNKNOWN;
  std::vector<uint32_t> freqs_khz;
  std::string airspace_class; // "A".."G" or empty
  int transition_alt_ft = 0;  // 0 if absent
  int floor_ft = 0;           // from AIRSPACE_POLYGON_BEGIN <floor> <ceil>
  int ceiling_ft = 0;
  // Bounding box across all polygons (eager).
  double bbox_min_lat = 0.0, bbox_max_lat = 0.0;
  double bbox_min_lon = 0.0, bbox_max_lon = 0.0;
  bool has_bbox = false;
  // Polygons are loaded lazily; empty() until first touched.
  // Each inner vector is one ring of (lat, lon) pairs.
  mutable std::vector<std::vector<std::pair<double, double>>> polygons;
  mutable bool polygons_loaded = false;
  // File offset (bytes) to the first AIRSPACE_POLYGON_BEGIN line, for lazy
  // load.
  std::uint64_t file_offset = 0;
};

// Load atc.dat from the given path. Pass an empty string to disable
// (useful for headless tests). Caller is responsible for resolving
// the path (plugin uses XPLMGetSystemPath; CLI passes empty).
void init(std::string atc_dat_path);
void stop();

// Returns true when the database is loaded (or known-absent — see loaded()
// vs enabled() distinction). Safe to call any time; DB loads async on init().
bool ready();

// Returns true if atc.dat was found and at least one controller parsed.
// When false, all queries return empty/null. atc.dat ships with X-Plane 12
// by default — a false here means Custom Data is missing or corrupted.
bool enabled();

// Total number of controllers indexed. Useful for UI / diagnostics.
std::size_t controller_count();

// All controllers whose bounding box contains (lat, lon) AND whose
// [floor_ft, ceiling_ft] contains alt_ft AND whose actual polygon
// (point-in-polygon) contains the point. Polygons are loaded on demand.
// Result pointers are stable for the lifetime of the process.
std::vector<const Controller *> find_enclosing(double lat, double lon,
                                               float alt_ft);

// Controller with matching frequency whose polygon contains (lat, lon).
// Falls back to role-based nearest-by-bbox if no geometric hit but a
// frequency match exists within ~100 NM. Returns nullptr if nothing matches.
const Controller *lookup_by_freq(std::uint32_t freq_khz, double lat, double lon,
                                 float alt_ft);

// Nearest controller of given role whose bbox+altitude covers (lat,lon,alt).
// Useful for "who will Tower hand me off to on departure?" queries.
const Controller *find_by_role_near(ControllerRole role, double lat, double lon,
                                    float alt_ft);

} // namespace airspace_db

#endif // AIRSPACE_DB_HPP
