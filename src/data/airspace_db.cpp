/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "data/airspace_db.hpp"
#include "core/logging.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace airspace_db {

const char *role_name(ControllerRole r) {
  switch (r) {
  case ControllerRole::TWR:
    return "TWR";
  case ControllerRole::TRACON:
    return "TRACON";
  case ControllerRole::CTR:
    return "CTR";
  default:
    return "UNKNOWN";
  }
}

// ── Module state ─────────────────────────────────────────────────────────

static std::vector<std::unique_ptr<Controller>> controllers_;
static std::atomic<bool> ready_{false};
static std::atomic<bool> enabled_{false};
static std::string data_path_;
static std::mutex poly_mutex_;        // protects lazy-loaded polygons + LRU
static std::deque<Controller *> lru_; // most-recent at back
static constexpr std::size_t kLruMax = 50;

// ── Helpers ──────────────────────────────────────────────────────────────

static ControllerRole parse_role(const std::string &s) {
  if (s == "twr")
    return ControllerRole::TWR;
  if (s == "tracon")
    return ControllerRole::TRACON;
  if (s == "ctr")
    return ControllerRole::CTR;
  return ControllerRole::UNKNOWN;
}

// Point-in-polygon via ray casting. points is a closed ring (or open; the
// ray-cast wraps around regardless).
static bool point_in_ring(double lat, double lon,
                          const std::vector<std::pair<double, double>> &pts) {
  bool inside = false;
  const std::size_t n = pts.size();
  if (n < 3)
    return false;
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    double yi = pts[i].first, xi = pts[i].second;
    double yj = pts[j].first, xj = pts[j].second;
    bool intersect =
        ((yi > lat) != (yj > lat)) &&
        (lon <
         (xj - xi) * (lat - yi) / ((yj - yi) != 0.0 ? (yj - yi) : 1e-12) + xi);
    if (intersect)
      inside = !inside;
  }
  return inside;
}

static double haversine_nm(double lat1, double lon1, double lat2, double lon2) {
  constexpr double R = 3440.065; // nm
  constexpr double D2R = M_PI / 180.0;
  double dlat = (lat2 - lat1) * D2R;
  double dlon = (lon2 - lon1) * D2R;
  double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
             std::cos(lat1 * D2R) * std::cos(lat2 * D2R) * std::sin(dlon / 2) *
                 std::sin(dlon / 2);
  return 2 * R * std::asin(std::min(1.0, std::sqrt(a)));
}

// ── Loader: Pass 1 (eager) — parse headers + BBox, skip polygon detail ───

static void load_headers() {
  if (data_path_.empty()) {
    logging::info("airspace_db: disabled (no atc.dat path configured)");
    enabled_ = false;
    ready_ = true;
    return;
  }
  std::ifstream file(data_path_, std::ios::binary);
  if (!file.is_open()) {
    logging::info("airspace_db: atc.dat not found at %s - install X-Plane 12 "
                  "Custom Data or reinstall XP12",
                  data_path_.c_str());
    enabled_ = false;
    ready_ = true;
    return;
  }

  logging::info("airspace_db: building controller index from atc.dat");

  std::unique_ptr<Controller> cur;
  std::string line;
  std::uint64_t line_offset = 0;
  std::uint64_t next_offset = 0;

  while (std::getline(file, line)) {
    line_offset = next_offset;
    next_offset = static_cast<std::uint64_t>(file.tellg());
    // Trim trailing \r
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line == "CONTROLLER") {
      if (cur)
        controllers_.push_back(std::move(cur));
      cur = std::make_unique<Controller>();
      continue;
    }
    if (!cur)
      continue;

    auto starts_with = [&](const char *p) {
      return std::strncmp(line.c_str(), p, std::strlen(p)) == 0;
    };

    if (starts_with("NAME ")) {
      cur->name = line.substr(5);
    } else if (starts_with("FACILITY_ID ")) {
      cur->facility_id = line.substr(12);
    } else if (starts_with("ROLE ")) {
      cur->role = parse_role(line.substr(5));
    } else if (starts_with("FREQ ")) {
      // atc.dat FREQ is 5-digit in units of 10 kHz (legacy 25-kHz radio
      // convention). For 8.33-kHz channels this is truncated: stored value
      // 11917 represents the real 8.33 channel 119.175 (not 119.170, which
      // is not a valid 8.33 channel). Snap non-8.33-grid values to the
      // nearest valid channel by adding 5 kHz.
      char *endp = nullptr;
      unsigned long raw = std::strtoul(line.c_str() + 5, &endp, 10);
      if (endp != line.c_str() + 5) {
        std::uint32_t khz = static_cast<std::uint32_t>(raw) * 10u;
        std::uint32_t m = khz % 25u;
        if (m != 0u && m != 5u && m != 10u)
          khz += 5u;
        cur->freqs_khz.push_back(khz);
      }
    } else if (starts_with("CHAN ")) {
      // atc.dat CHAN is 6-digit kHz (8.33-kHz channel, exact precision).
      char *endp = nullptr;
      unsigned long raw = std::strtoul(line.c_str() + 5, &endp, 10);
      if (endp != line.c_str() + 5)
        cur->freqs_khz.push_back(static_cast<std::uint32_t>(raw));
    } else if (starts_with("CLASS ")) {
      cur->airspace_class = line.substr(6);
    } else if (starts_with("TRANSITION_ALT ")) {
      char *endp = nullptr;
      long v = std::strtol(line.c_str() + 15, &endp, 10);
      if (endp != line.c_str() + 15)
        cur->transition_alt_ft = static_cast<int>(v);
    } else if (starts_with("AIRSPACE_POLYGON_BEGIN")) {
      // Remember offset of first polygon (for lazy load).
      if (cur->file_offset == 0)
        cur->file_offset = line_offset;
      // Parse floor / ceiling (first BEGIN wins for bookkeeping).
      std::istringstream iss(line);
      std::string tok;
      iss >> tok; // AIRSPACE_POLYGON_BEGIN
      int floor_ft = 0, ceil_ft = 0;
      if (iss >> floor_ft >> ceil_ft) {
        if (cur->ceiling_ft == 0 || ceil_ft > cur->ceiling_ft)
          cur->ceiling_ft = ceil_ft;
        if (cur->floor_ft == 0 || floor_ft < cur->floor_ft)
          cur->floor_ft = floor_ft;
      }
    } else if (starts_with("POINT ")) {
      // Parse to update bbox — we don't store the point here.
      std::istringstream iss(line);
      std::string tok;
      double plat = 0.0, plon = 0.0;
      iss >> tok >> plat >> plon;
      if (!cur->has_bbox) {
        cur->bbox_min_lat = cur->bbox_max_lat = plat;
        cur->bbox_min_lon = cur->bbox_max_lon = plon;
        cur->has_bbox = true;
      } else {
        cur->bbox_min_lat = std::min(cur->bbox_min_lat, plat);
        cur->bbox_max_lat = std::max(cur->bbox_max_lat, plat);
        cur->bbox_min_lon = std::min(cur->bbox_min_lon, plon);
        cur->bbox_max_lon = std::max(cur->bbox_max_lon, plon);
      }
    }
  }
  if (cur)
    controllers_.push_back(std::move(cur));

  enabled_ = !controllers_.empty();
  logging::info("airspace_db: indexed %zu controllers", controllers_.size());
  ready_ = true;
}

// ── Loader: Pass 2 (lazy) — read polygon points for one controller ───────

static void load_polygons_locked(Controller *c) {
  if (c->polygons_loaded)
    return;
  std::ifstream file(data_path_, std::ios::binary);
  if (!file.is_open()) {
    c->polygons_loaded = true;
    return;
  }
  file.seekg(static_cast<std::streamoff>(c->file_offset));
  std::string line;
  std::vector<std::pair<double, double>> current;
  bool in_block = false;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line == "CONTROLLER")
      break; // reached next controller
    if (line.rfind("AIRSPACE_POLYGON_BEGIN", 0) == 0) {
      in_block = true;
      current.clear();
    } else if (line == "AIRSPACE_POLYGON_END") {
      if (!current.empty())
        c->polygons.push_back(std::move(current));
      current.clear();
      in_block = false;
    } else if (in_block && line.rfind("POINT ", 0) == 0) {
      std::istringstream iss(line);
      std::string tok;
      double plat = 0.0, plon = 0.0;
      iss >> tok >> plat >> plon;
      current.emplace_back(plat, plon);
    }
  }
  c->polygons_loaded = true;
}

static void touch_lru(Controller *c) {
  // Move to back (most recent). poly_mutex_ must already be held.
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    if (*it == c) {
      lru_.erase(it);
      break;
    }
  }
  lru_.push_back(c);
  while (lru_.size() > kLruMax) {
    Controller *evict = lru_.front();
    lru_.pop_front();
    evict->polygons.clear();
    evict->polygons.shrink_to_fit();
    evict->polygons_loaded = false;
  }
}

static void ensure_polygons(Controller *c) {
  std::lock_guard<std::mutex> lk(poly_mutex_);
  if (!c->polygons_loaded)
    load_polygons_locked(c);
  touch_lru(c);
}

// ── Public API ───────────────────────────────────────────────────────────

void init(std::string atc_dat_path) {
  ready_ = false;
  enabled_ = false;
  controllers_.clear();
  lru_.clear();
  data_path_ = std::move(atc_dat_path);
  // Load asynchronously so X-Plane startup isn't blocked (10 MB parse).
  std::thread([]() { load_headers(); }).detach();
}

void stop() {
  controllers_.clear();
  lru_.clear();
  ready_ = false;
  enabled_ = false;
}

bool ready() { return ready_.load(); }
bool enabled() { return enabled_.load(); }
std::size_t controller_count() { return controllers_.size(); }

static bool bbox_contains(const Controller &c, double lat, double lon,
                          float alt_ft) {
  if (!c.has_bbox)
    return false;
  // Reject pathological polygons (antimeridian crossings, polar caps) whose
  // naive min/max bbox spans most of the globe and would falsely include
  // unrelated points. e.g. Oakland Oceanic crosses lon 180 and its naive
  // bbox stretches from -180 to +180.
  if ((c.bbox_max_lon - c.bbox_min_lon) > 180.0)
    return false;
  if ((c.bbox_max_lat - c.bbox_min_lat) > 90.0)
    return false;
  if (lat < c.bbox_min_lat || lat > c.bbox_max_lat)
    return false;
  if (lon < c.bbox_min_lon || lon > c.bbox_max_lon)
    return false;
  // Altitude check — if ceiling is 0 treat as unbounded
  if (c.ceiling_ft > 0 && alt_ft > static_cast<float>(c.ceiling_ft))
    return false;
  if (alt_ft < static_cast<float>(c.floor_ft) - 1.0f)
    return false;
  return true;
}

std::vector<const Controller *> find_enclosing(double lat, double lon,
                                               float alt_ft) {
  std::vector<const Controller *> out;
  if (!enabled_.load())
    return out;
  for (auto &up : controllers_) {
    Controller *c = up.get();
    if (!bbox_contains(*c, lat, lon, alt_ft))
      continue;
    ensure_polygons(c);
    bool hit = false;
    for (auto &ring : c->polygons) {
      if (point_in_ring(lat, lon, ring)) {
        hit = true;
        break;
      }
    }
    if (hit)
      out.push_back(c);
  }
  return out;
}

const Controller *lookup_by_freq(std::uint32_t freq_khz, double lat, double lon,
                                 float alt_ft) {
  if (!enabled_.load() || freq_khz == 0)
    return nullptr;
  const Controller *freq_fallback = nullptr;
  double fallback_dist_nm = 1e9;
  for (auto &up : controllers_) {
    Controller *c = up.get();
    bool freq_match = false;
    for (auto f : c->freqs_khz) {
      if (f == freq_khz) {
        freq_match = true;
        break;
      }
    }
    if (!freq_match)
      continue;
    if (bbox_contains(*c, lat, lon, alt_ft)) {
      ensure_polygons(c);
      for (auto &ring : c->polygons) {
        if (point_in_ring(lat, lon, ring))
          return c;
      }
    }
    // Fallback: remember the nearest frequency-matching controller by bbox
    // center distance.
    if (c->has_bbox) {
      double cx = 0.5 * (c->bbox_min_lat + c->bbox_max_lat);
      double cy = 0.5 * (c->bbox_min_lon + c->bbox_max_lon);
      double d = haversine_nm(lat, lon, cx, cy);
      if (d < fallback_dist_nm && d < 100.0) {
        fallback_dist_nm = d;
        freq_fallback = c;
      }
    }
  }
  return freq_fallback;
}

const Controller *find_by_role_near(ControllerRole role, double lat, double lon,
                                    float alt_ft) {
  if (!enabled_.load())
    return nullptr;
  const Controller *best = nullptr;
  double best_dist_nm = 1e9;
  for (auto &up : controllers_) {
    Controller *c = up.get();
    if (c->role != role)
      continue;
    if (!bbox_contains(*c, lat, lon, alt_ft))
      continue;
    double cx = 0.5 * (c->bbox_min_lat + c->bbox_max_lat);
    double cy = 0.5 * (c->bbox_min_lon + c->bbox_max_lon);
    double d = haversine_nm(lat, lon, cx, cy);
    if (d < best_dist_nm) {
      best_dist_nm = d;
      best = c;
    }
  }
  return best;
}

} // namespace airspace_db
