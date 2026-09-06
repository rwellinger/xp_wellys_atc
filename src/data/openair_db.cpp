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

#include "data/openair_db.hpp"
#include "core/logging.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace openair_db {

namespace {

constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
constexpr double kRad2Deg = 180.0 / 3.14159265358979323846;
// Nautical miles per degree of latitude (WGS-84 mean). Longitude is scaled
// by cos(latitude) at the zone's centre. Flat-earth approximation — accurate
// to well under a nautical mile at CTR/RMZ scale, which is all we need for
// point-in-polygon containment.
constexpr double kNmPerDeg = 60.0;
// Chord step for tessellating circles/arcs (degrees of arc). 5 deg => 72
// vertices on a full circle, plenty for containment tests.
constexpr double kArcStepDeg = 5.0;

struct Entry {
  std::string name;
  AirspaceClass ac_class = AirspaceClass::OTHER;
  int floor_ft = 0;
  int ceiling_ft = 0;
  // Bounding box for fast rejection.
  double bbox_min_lat = 0.0, bbox_max_lat = 0.0;
  double bbox_min_lon = 0.0, bbox_max_lon = 0.0;
  double bbox_area = 0.0; // (lat span) * (lon span) — proxy for polygon size
  std::vector<std::pair<double, double>> polygon; // (lat, lon) pairs
};

// Point-in-polygon via ray casting.
static bool
point_in_polygon(double lat, double lon,
                 const std::vector<std::pair<double, double>> &poly) {
  if (poly.size() < 3)
    return false;
  bool inside = false;
  std::size_t n = poly.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    double xi = poly[i].second, yi = poly[i].first;
    double xj = poly[j].second, yj = poly[j].first;
    if (((yi > lat) != (yj > lat)) &&
        (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi))
      inside = !inside;
  }
  return inside;
}

// Parse "DD:MM:SS N DDD:MM:SS E" (also accepts decimal seconds/minutes and a
// leading "X=" from V records). Returns false on malformed input.
static bool parse_coord(const char *s, double &lat, double &lon) {
  while (*s == ' ')
    ++s;
  int latd, latm, lond, lonm;
  double lats, lons;
  char latdir[4] = {}, londir[4] = {};
  // Fixed-format coordinate parse; the returned field count (8) is checked.
  // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion)
  if (std::sscanf(s, " %d:%d:%lf %3s %d:%d:%lf %3s", &latd, &latm, &lats,
                  latdir, &lond, &lonm, &lons, londir) != 8)
    return false;
  lat = latd + latm / 60.0 + lats / 3600.0;
  if (latdir[0] == 'S')
    lat = -lat;
  lon = lond + lonm / 60.0 + lons / 3600.0;
  if (londir[0] == 'W')
    lon = -lon;
  return true;
}

// Parse "AH/AL <value>" — supports "FL095", "4000 MSL", "2500 AGL", "GND",
// "SFC", "UNLIM", plain integers. AGL values are stored as-is (MSL not known
// at parse time); callers that need MSL must add terrain/airport elevation.
static int parse_alt(const char *val) {
  while (*val == ' ')
    ++val;
  if (std::strncmp(val, "FL", 2) == 0)
    return static_cast<int>(std::strtol(val + 2, nullptr, 10)) * 100;
  if (std::strncmp(val, "GND", 3) == 0 || std::strncmp(val, "SFC", 3) == 0)
    return 0;
  if (std::strncmp(val, "UNLIM", 5) == 0 || std::strncmp(val, "UNL", 3) == 0)
    return 99999;
  return static_cast<int>(std::strtol(val, nullptr, 10));
}

static AirspaceClass parse_class(const char *s) {
  if (std::strcmp(s, "CTR") == 0)
    return AirspaceClass::CTR;
  if (std::strcmp(s, "TMA") == 0)
    return AirspaceClass::TMA;
  if (std::strcmp(s, "CTA") == 0)
    return AirspaceClass::CTA;
  if (std::strcmp(s, "RMZ") == 0)
    return AirspaceClass::RMZ;
  if (std::strcmp(s, "TMZ") == 0)
    return AirspaceClass::TMZ;
  // German restricted areas: OpenAir "R", sometimes "ED-R"/"EDR".
  if (std::strcmp(s, "R") == 0 || std::strcmp(s, "ED-R") == 0 ||
      std::strcmp(s, "EDR") == 0)
    return AirspaceClass::ED_R;
  return AirspaceClass::OTHER;
}

// Only index awareness-relevant classes — skip danger/prohibited/glider etc.
static bool is_indexed(AirspaceClass c) {
  return c != AirspaceClass::OTHER;
}

// ── Circle / arc tessellation ────────────────────────────────────────────
// OpenAir defines many CTR/RMZ/TMZ zones as a centre + circle (DC) or as
// arcs (DA/DB) rather than explicit DP polygon vertices. The upstream IFR
// parser ignored these, which loses most German small-field zones — so we
// tessellate them into polygon points here.

// Point at (bearing_deg true, radius_nm) from a centre. Flat-earth offset.
static std::pair<double, double> point_at(double clat, double clon,
                                          double radius_nm, double bearing_deg) {
  double dlat = (radius_nm / kNmPerDeg) * std::cos(bearing_deg * kDeg2Rad);
  double coslat = std::cos(clat * kDeg2Rad);
  if (std::fabs(coslat) < 1e-9)
    coslat = 1e-9;
  double dlon =
      (radius_nm / kNmPerDeg) * std::sin(bearing_deg * kDeg2Rad) / coslat;
  return {clat + dlat, clon + dlon};
}

// True bearing (0=N, clockwise) from centre to a point; flat-earth.
static double bearing_to(double clat, double clon, double plat, double plon) {
  double dlat = plat - clat;
  double dlon = (plon - clon) * std::cos(clat * kDeg2Rad);
  double b = std::atan2(dlon, dlat) * kRad2Deg;
  if (b < 0.0)
    b += 360.0;
  return b;
}

// Flat-earth distance (NM) from centre to a point.
static double dist_nm(double clat, double clon, double plat, double plon) {
  double dlat = (plat - clat) * kNmPerDeg;
  double dlon = (plon - clon) * std::cos(clat * kDeg2Rad) * kNmPerDeg;
  return std::sqrt(dlat * dlat + dlon * dlon);
}

// Push one vertex onto the entry, growing its bounding box.
static void push_vertex(Entry &e, double lat, double lon) {
  if (e.polygon.empty()) {
    e.bbox_min_lat = e.bbox_max_lat = lat;
    e.bbox_min_lon = e.bbox_max_lon = lon;
  } else {
    if (lat < e.bbox_min_lat)
      e.bbox_min_lat = lat;
    if (lat > e.bbox_max_lat)
      e.bbox_max_lat = lat;
    if (lon < e.bbox_min_lon)
      e.bbox_min_lon = lon;
    if (lon > e.bbox_max_lon)
      e.bbox_max_lon = lon;
  }
  e.polygon.emplace_back(lat, lon);
}

// Emit a full closed circle of radius_nm around (clat,clon).
static void emit_circle(Entry &e, double clat, double clon, double radius_nm) {
  int n = static_cast<int>(360.0 / kArcStepDeg); // 72 vertices at 5 deg
  for (int i = 0; i < n; ++i) {
    auto p = point_at(clat, clon, radius_nm, i * kArcStepDeg);
    push_vertex(e, p.first, p.second);
  }
}

// Emit an arc from start_brg to end_brg around (clat,clon) at radius_nm.
// dir = +1 clockwise (increasing bearing), -1 counter-clockwise.
static void emit_arc(Entry &e, double clat, double clon, double radius_nm,
                     double start_brg, double end_brg, int dir) {
  // Normalise sweep into [0, 360) in the travel direction.
  double sweep = dir > 0 ? end_brg - start_brg : start_brg - end_brg;
  while (sweep < 0.0)
    sweep += 360.0;
  while (sweep >= 360.0)
    sweep -= 360.0;
  int steps = static_cast<int>(sweep / kArcStepDeg);
  for (int i = 0; i <= steps; ++i) {
    double brg = start_brg + dir * i * kArcStepDeg;
    auto p = point_at(clat, clon, radius_nm, brg);
    push_vertex(e, p.first, p.second);
  }
  // Ensure the exact end bearing is included.
  auto pe = point_at(clat, clon, radius_nm, end_brg);
  push_vertex(e, pe.first, pe.second);
}

std::vector<Entry> s_entries;
std::atomic<bool> s_ready{false};
std::thread s_thread;

static void finalize(std::vector<Entry> &entries, Entry &cur, bool active) {
  if (active && cur.polygon.size() >= 3) {
    cur.bbox_area = (cur.bbox_max_lat - cur.bbox_min_lat) *
                    (cur.bbox_max_lon - cur.bbox_min_lon);
    entries.push_back(std::move(cur));
  }
}

// Parse one OpenAir file, appending indexed airspaces to `entries`.
// Returns the number of entries added.
static std::size_t parse_file(const std::string &path,
                              std::vector<Entry> &entries) {
  if (path.empty())
    return 0;
  // Missing file is not logged here — the bundled vs. optional-override
  // distinction is handled by the caller (load()).
  FILE *f = std::fopen(path.c_str(), "r");
  if (!f)
    return 0;

  std::size_t before = entries.size();
  bool active = false;
  Entry cur;
  // Circle/arc state (reset per airspace by the "V X=" / "AC" handling).
  double center_lat = 0.0, center_lon = 0.0;
  bool have_center = false;
  int arc_dir = 1; // "V D=+" clockwise (default), "V D=-" counter-clockwise

  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    std::size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' '))
      line[--len] = '\0';
    if (len == 0 || line[0] == '*')
      continue;

    if (std::strncmp(line, "AC ", 3) == 0) {
      finalize(entries, cur, active);
      cur = Entry{};
      cur.ac_class = parse_class(line + 3);
      active = is_indexed(cur.ac_class);
      have_center = false;
      arc_dir = 1;
      continue;
    }

    // AN: always process so we can upgrade letter-class entries (AC C, AC D,
    // AC R …) to the correct type when the name spells it out. Many German
    // openAIP OpenAir exports put the ICAO class letter in AC and the actual
    // type (CTR/RMZ/TMZ/TMA) only in the name.
    if (std::strncmp(line, "AN ", 3) == 0) {
      cur.name = line + 3;
      if (cur.ac_class == AirspaceClass::OTHER) {
        const std::string &n = cur.name;
        if (n.find("CTR") != std::string::npos)
          cur.ac_class = AirspaceClass::CTR;
        else if (n.find("RMZ") != std::string::npos)
          cur.ac_class = AirspaceClass::RMZ;
        else if (n.find("TMZ") != std::string::npos)
          cur.ac_class = AirspaceClass::TMZ;
        else if (n.find("TMA") != std::string::npos)
          cur.ac_class = AirspaceClass::TMA;
        else if (n.find("CTA") != std::string::npos)
          cur.ac_class = AirspaceClass::CTA;
        active = is_indexed(cur.ac_class);
      }
      continue;
    }

    if (!active)
      continue;

    if (std::strncmp(line, "AH ", 3) == 0) {
      cur.ceiling_ft = parse_alt(line + 3);
    } else if (std::strncmp(line, "AL ", 3) == 0) {
      cur.floor_ft = parse_alt(line + 3);
    } else if (std::strncmp(line, "DP ", 3) == 0) {
      double lat, lon;
      if (parse_coord(line + 3, lat, lon))
        push_vertex(cur, lat, lon);
    } else if (std::strncmp(line, "V ", 2) == 0) {
      // Variable assignment: "V X=<coord>" (centre) or "V D=+/-" (arc dir).
      const char *p = line + 2;
      while (*p == ' ')
        ++p;
      if ((p[0] == 'X' || p[0] == 'x') && p[1] == '=') {
        double lat, lon;
        if (parse_coord(p + 2, lat, lon)) {
          center_lat = lat;
          center_lon = lon;
          have_center = true;
        }
      } else if ((p[0] == 'D' || p[0] == 'd') && p[1] == '=') {
        arc_dir = (p[2] == '-') ? -1 : 1;
      }
    } else if (std::strncmp(line, "DC ", 3) == 0) {
      // Full circle: radius (NM) around the current centre.
      if (have_center) {
        double radius_nm = std::strtod(line + 3, nullptr);
        emit_circle(cur, center_lat, center_lon, radius_nm);
      }
    } else if (std::strncmp(line, "DA ", 3) == 0) {
      // Arc by radius,angleStart,angleEnd (radius NM, true bearings).
      if (have_center) {
        double radius_nm = 0, a1 = 0, a2 = 0;
        // Fixed-format arc parse; the returned field count (3) is checked.
        // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion)
        if (std::sscanf(line + 3, " %lf , %lf , %lf", &radius_nm, &a1, &a2) == 3)
          emit_arc(cur, center_lat, center_lon, radius_nm, a1, a2, arc_dir);
      }
    } else if (std::strncmp(line, "DB ", 3) == 0) {
      // Arc between two coordinates around the current centre.
      if (have_center) {
        const char *comma = std::strchr(line + 3, ',');
        if (comma) {
          std::string c1(line + 3, comma - (line + 3));
          std::string c2(comma + 1);
          double lat1, lon1, lat2, lon2;
          if (parse_coord(c1.c_str(), lat1, lon1) &&
              parse_coord(c2.c_str(), lat2, lon2)) {
            double radius_nm =
                dist_nm(center_lat, center_lon, lat1, lon1);
            double b1 = bearing_to(center_lat, center_lon, lat1, lon1);
            double b2 = bearing_to(center_lat, center_lon, lat2, lon2);
            emit_arc(cur, center_lat, center_lon, radius_nm, b1, b2, arc_dir);
          }
        }
      }
    }
  }
  finalize(entries, cur, active);

  std::fclose(f);
  std::size_t added = entries.size() - before;
  logging::info("openair_db: parsed %zu airspace entries from %s", added,
                path.c_str());
  return added;
}

static void load(const std::string &bundled_path,
                 const std::string &user_path) {
  std::vector<Entry> entries;
  std::size_t bundled_added = parse_file(bundled_path, entries);
  if (!bundled_path.empty() && bundled_added == 0)
    logging::info("openair_db: bundled airspace file missing/empty (%s)",
                  bundled_path.c_str());
  // The user override is purely opt-in; a missing file is silent.
  std::size_t user_added = parse_file(user_path, entries);
  s_entries = std::move(entries);
  logging::info("openair_db: %zu airspaces indexed (%zu from user override)",
                s_entries.size(), user_added);
  s_ready = true;
}

} // namespace

const char *class_name(AirspaceClass c) {
  switch (c) {
  case AirspaceClass::CTR:
    return "CTR";
  case AirspaceClass::TMA:
    return "TMA";
  case AirspaceClass::CTA:
    return "CTA";
  case AirspaceClass::RMZ:
    return "RMZ";
  case AirspaceClass::TMZ:
    return "TMZ";
  case AirspaceClass::ED_R:
    return "ED-R";
  case AirspaceClass::OTHER:
    break;
  }
  return "OTHER";
}

void init(std::string bundled_path, std::string user_path) {
  if (bundled_path.empty() && user_path.empty()) {
    s_ready = true;
    return;
  }
  s_thread = std::thread([b = std::move(bundled_path),
                          u = std::move(user_path)]() { load(b, u); });
}

void stop() {
  if (s_thread.joinable())
    s_thread.join();
  s_entries.clear();
  s_ready = false;
}

bool ready() { return s_ready.load(); }

std::size_t entry_count() { return ready() ? s_entries.size() : 0; }

// Shared 3-D containment test (bbox reject → altitude band → polygon).
static bool contains(const Entry &e, double lat, double lon, int alt_ft) {
  if (lat < e.bbox_min_lat || lat > e.bbox_max_lat)
    return false;
  if (lon < e.bbox_min_lon || lon > e.bbox_max_lon)
    return false;
  if (alt_ft < e.floor_ft)
    return false;
  if (e.ceiling_ft > 0 && alt_ft > e.ceiling_ft)
    return false;
  return point_in_polygon(lat, lon, e.polygon);
}

AirspaceEntry find_enclosing(double lat, double lon, int alt_ft) {
  if (!s_ready)
    return {};
  const Entry *best = nullptr;
  for (const auto &e : s_entries) {
    if (!contains(e, lat, lon, alt_ft))
      continue;
    if (!best || e.bbox_area < best->bbox_area)
      best = &e;
  }
  if (!best)
    return {};
  return {best->name, best->ac_class, best->floor_ft, best->ceiling_ft};
}

std::vector<AirspaceEntry> find_all_enclosing(double lat, double lon,
                                              int alt_ft) {
  std::vector<AirspaceEntry> result;
  if (!s_ready)
    return result;
  for (const auto &e : s_entries) {
    if (!contains(e, lat, lon, alt_ft))
      continue;
    result.push_back({e.name, e.ac_class, e.floor_ft, e.ceiling_ft});
  }
  return result;
}

} // namespace openair_db
