/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "core/xplane_context.hpp"
#include "data/airspace_db.hpp"
#include "persistence/settings.hpp"

#include <XPLMDataAccess.h>
#include <XPLMNavigation.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xplane_context {

static XPlaneContext ctx;

static XPLMDataRef dr_latitude = nullptr;
static XPLMDataRef dr_longitude = nullptr;
static XPLMDataRef dr_altitude = nullptr;
static XPLMDataRef dr_groundspeed = nullptr;
static XPLMDataRef dr_indicated_airspeed = nullptr;
static XPLMDataRef dr_vertical_speed = nullptr;
static XPLMDataRef dr_heading_true = nullptr;
static XPLMDataRef dr_y_agl = nullptr;
static XPLMDataRef dr_com1_freq = nullptr;
static XPLMDataRef dr_com2_freq = nullptr;
static XPLMDataRef dr_active_com = nullptr;
static XPLMDataRef dr_aircraft_icao = nullptr;
static XPLMDataRef dr_avionics_on = nullptr;
static XPLMDataRef dr_com1_power = nullptr;
static XPLMDataRef dr_com2_power = nullptr;
static XPLMDataRef dr_bus_volts = nullptr;
static XPLMDataRef dr_barometer = nullptr;
static XPLMDataRef dr_wind_direction = nullptr;
static XPLMDataRef dr_wind_speed = nullptr;
static XPLMDataRef dr_visibility = nullptr;
static XPLMDataRef dr_cloud_base = nullptr;
static XPLMDataRef dr_cloud_type = nullptr;
static XPLMDataRef dr_temperature = nullptr;
static XPLMDataRef dr_dewpoint = nullptr;

static int frame_counter = 0;

static XPLMDataRef dr_com1_standby = nullptr;
static XPLMDataRef dr_com2_standby = nullptr;
static XPLMDataRef dr_transponder_code = nullptr;
static XPLMDataRef dr_transponder_mode = nullptr;

// ── Airport frequency + runway cache (built from apt.dat) ───────
static std::unordered_map<std::string, AirportFrequencies> freq_cache_;
static std::unordered_map<std::string, std::vector<RunwayInfo>> runway_cache_;
static std::unordered_map<std::string, std::string> name_cache_;
// Airport reference point (lat,lon) — midpoint of first runway, for range
// checks during frequency-driven active-airport switching.
static std::unordered_map<std::string, std::pair<double, double>> pos_cache_;
// Field elevation in feet, parsed from apt.dat code-1 token #1 (0-indexed).
static std::unordered_map<std::string, float> elevation_cache_;
static std::atomic<bool> towered_cache_ready_{false};

// Airport picker: when set, overrides nearest-airport selection logic.
static std::string locked_airport_id_;

static constexpr double kDeg2Rad = M_PI / 180.0;
static constexpr double kEarthRadiusM = 6371000.0;
static constexpr double kMetersPerNm = 1852.0;

// Realistic VHF range thresholds for frequency-driven airport switching.
// Tuning a tower freq counts as "intent to land here" only within these.
static constexpr double kFreqMatchRangeTowerNm = 20.0;
static constexpr double kFreqMatchRangeAtisNm = 40.0;

static double haversine_distance(double lat1, double lon1, double lat2,
                                 double lon2) {
  double dlat = (lat2 - lat1) * kDeg2Rad;
  double dlon = (lon2 - lon1) * kDeg2Rad;
  double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
             std::cos(lat1 * kDeg2Rad) * std::cos(lat2 * kDeg2Rad) *
                 std::sin(dlon / 2) * std::sin(dlon / 2);
  return kEarthRadiusM * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

static float initial_bearing(double lat1, double lon1, double lat2,
                             double lon2) {
  double lat1r = lat1 * kDeg2Rad;
  double lat2r = lat2 * kDeg2Rad;
  double dlon = (lon2 - lon1) * kDeg2Rad;
  double y = std::sin(dlon) * std::cos(lat2r);
  double x = std::cos(lat1r) * std::sin(lat2r) -
             std::sin(lat1r) * std::cos(lat2r) * std::cos(dlon);
  double bearing = std::atan2(y, x) / kDeg2Rad;
  return static_cast<float>(std::fmod(bearing + 360.0, 360.0));
}

// Forward decl (defined further down).
static std::string select_active_runway(const std::vector<RunwayInfo> &runways,
                                        float wind_dir, float wind_speed);

// Populate ctx airport fields from caches for a given ICAO.
// Used for both the locked-airport path and as the tail of update().
static void populate_ctx_from_cache(const std::string &icao,
                                    double fallback_lat, double fallback_lon) {
  ctx.nearest_airport_id = icao;

  auto pos_it = pos_cache_.find(icao);
  if (pos_it != pos_cache_.end()) {
    ctx.airport_lat = pos_it->second.first;
    ctx.airport_lon = pos_it->second.second;
  } else {
    ctx.airport_lat = fallback_lat;
    ctx.airport_lon = fallback_lon;
  }

  auto name_it = name_cache_.find(icao);
  ctx.nearest_airport_name =
      (name_it != name_cache_.end()) ? name_it->second : "";

  auto freq_it = freq_cache_.find(icao);
  if (freq_it != freq_cache_.end()) {
    ctx.airport_freqs = freq_it->second;
    ctx.facility_type = classify_facility(ctx.airport_freqs);
    ctx.tower_only = ctx.is_towered() && !ctx.airport_freqs.has_ground();
    ctx.atis_freq_mhz = ctx.airport_freqs.first_mhz(FrequencyType::ATIS);
  } else {
    ctx.airport_freqs = {};
    ctx.facility_type = FacilityType::UNKNOWN;
    ctx.tower_only = false;
    ctx.atis_freq_mhz = 0.0f;
  }

  auto rwy_it = runway_cache_.find(icao);
  if (rwy_it != runway_cache_.end()) {
    ctx.runways = rwy_it->second;
    ctx.active_runway = select_active_runway(
        ctx.runways, ctx.wind_direction_deg, ctx.wind_speed_kt);
  } else {
    ctx.runways.clear();
    ctx.active_runway.clear();
  }
}

// Find an airport whose frequency table contains `freq_khz` within realistic
// VHF range. Skips `skip_id` (the geometric nearest, already handled).
// Returns matched ICAO id, or empty string if no match.
static std::string find_freq_match(const std::string &skip_id,
                                   uint32_t freq_khz, double ac_lat,
                                   double ac_lon) {
  if (freq_khz == 0)
    return {};

  // Bbox prefilter: ±1° lat, ±1° lon (cosine-corrected) ≈ 60 NM.
  // Cheap check before haversine.
  const double cos_lat = std::cos(ac_lat * kDeg2Rad);
  const double lat_window = 1.0;
  const double lon_window = (cos_lat > 0.01) ? 1.0 / cos_lat : 1.0;

  std::string best_id;
  double best_dist_nm = 1e9;

  for (const auto &kv : freq_cache_) {
    if (kv.first == skip_id)
      continue;
    auto pos_it = pos_cache_.find(kv.first);
    if (pos_it == pos_cache_.end())
      continue;
    double apt_lat = pos_it->second.first;
    double apt_lon = pos_it->second.second;
    if (std::fabs(apt_lat - ac_lat) > lat_window)
      continue;
    if (std::fabs(apt_lon - ac_lon) > lon_window)
      continue;

    // Match the frequency against this airport's table
    FrequencyType matched = FrequencyType::UNKNOWN;
    for (const auto &f : kv.second.all) {
      uint32_t diff = (freq_khz > f.freq_khz) ? freq_khz - f.freq_khz
                                              : f.freq_khz - freq_khz;
      if (diff <= 1) {
        matched = f.type;
        break;
      }
    }
    if (matched == FrequencyType::UNKNOWN)
      continue;

    double dist_nm =
        haversine_distance(ac_lat, ac_lon, apt_lat, apt_lon) / kMetersPerNm;

    double range_limit = (matched == FrequencyType::ATIS)
                             ? kFreqMatchRangeAtisNm
                             : kFreqMatchRangeTowerNm;
    if (dist_nm > range_limit)
      continue;

    if (dist_nm < best_dist_nm) {
      best_dist_nm = dist_nm;
      best_id = kv.first;
    }
  }
  return best_id;
}

// Surface codes 1 (asphalt) and 2 (concrete) are paved
static bool is_paved(int surface_code) {
  return surface_code == 1 || surface_code == 2;
}

static std::string select_active_runway(const std::vector<RunwayInfo> &runways,
                                        float wind_dir, float wind_speed) {
  if (runways.empty())
    return "";

  // Calm wind (< 3 kt): pick longest paved runway, prefer lower-numbered end
  if (wind_speed < 3.0f) {
    const RunwayInfo *best = nullptr;
    for (const auto &rwy : runways) {
      if (!best ||
          (is_paved(rwy.surface_code) && !is_paved(best->surface_code)) ||
          (is_paved(rwy.surface_code) == is_paved(best->surface_code) &&
           rwy.length_m > best->length_m))
        best = &rwy;
    }
    // Return lower-numbered end
    if (best->end1.number < best->end2.number)
      return best->end1.number;
    return best->end2.number;
  }

  // Wind-based: find runway end with largest headwind component
  // When headwind difference < 1 kt, prefer paved runway
  std::string best_end;
  float best_headwind = -9999.0f;
  float best_length = 0.0f;
  bool best_paved = false;

  for (const auto &rwy : runways) {
    bool paved = is_paved(rwy.surface_code);
    for (const auto *end : {&rwy.end1, &rwy.end2}) {
      float diff =
          std::fmod(wind_dir - end->heading_deg + 540.0f, 360.0f) - 180.0f;
      float headwind =
          wind_speed * std::cos(diff * static_cast<float>(kDeg2Rad));
      // Clearly better headwind (> 1 kt margin), or within 1 kt margin
      // with paved preference, then headwind, then length as tiebreakers
      bool clearly_better = headwind > best_headwind + 1.0f;
      bool within_margin = headwind > best_headwind - 1.0f;
      bool tiebreak_wins =
          within_margin &&
          (std::make_tuple(paved, headwind, rwy.length_m) >
           std::make_tuple(best_paved, best_headwind, best_length));
      if (clearly_better || tiebreak_wins) {
        best_headwind = headwind;
        best_end = end->number;
        best_length = rwy.length_m;
        best_paved = paved;
      }
    }
  }
  return best_end;
}

static std::string xplane_system_path() {
  char raw[2048] = {};
  XPLMGetSystemPath(raw);
  std::string p(raw);
#if defined(__APPLE__)
  // macOS may return an HFS path (colon-separated, no slashes) — convert
  if (p.find(':') != std::string::npos && p.find('/') == std::string::npos) {
    auto colon = p.find(':');
    std::string posix = p.substr(colon + 1);
    for (char &c : posix)
      if (c == ':')
        c = '/';
    p = "/" + posix;
  }
#endif
  if (!p.empty() && p.back() != '/')
    p += '/';
  return p;
}

// Frequency code mapping: apt.dat row code → FrequencyType
struct FreqCodeMapping {
  int old_code; // 50-55
  int new_code; // 1050-1055
  FrequencyType type;
};
static constexpr FreqCodeMapping kFreqCodes[] = {
    {50, 1050, FrequencyType::ATIS},     {51, 1051, FrequencyType::UNICOM},
    {52, 1052, FrequencyType::DELIVERY}, {53, 1053, FrequencyType::GROUND},
    {54, 1054, FrequencyType::TOWER},
};

// Parse a line code from the start of a line. Returns -1 if not a frequency.
static int parse_line_code(const std::string &line) {
  if (line.empty())
    return -1;
  // Try to parse leading digits
  size_t i = 0;
  while (i < line.size() && line[i] >= '0' && line[i] <= '9')
    ++i;
  if (i == 0 || i > 4)
    return -1;
  if (i < line.size() && line[i] != ' ' && line[i] != '\t')
    return -1;
  return std::stoi(line.substr(0, i));
}

static void build_towered_cache() {
  std::string apt_path =
      xplane_system_path() +
      "Global Scenery/Global Airports/Earth nav data/apt.dat";

  std::ifstream file(apt_path);
  if (!file.is_open()) {
    char log[512];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_vfr_atc] WARNING: Could not open %s\n",
                  apt_path.c_str());
    XPLMDebugString(log);
    towered_cache_ready_ = true;
    return;
  }

  XPLMDebugString(
      "[xp_wellys_vfr_atc] Building airport cache from apt.dat...\n");

  std::unordered_map<std::string, AirportFrequencies> freqs;
  std::unordered_map<std::string, std::vector<RunwayInfo>> runways;
  std::unordered_map<std::string, std::string> names;
  std::unordered_map<std::string, std::pair<double, double>> positions;
  std::unordered_map<std::string, float> elevations;
  std::string current_icao;
  std::string line;

  while (std::getline(file, line)) {
    if (line.empty())
      continue;

    int code = parse_line_code(line);

    // Airport header: code 1 (airport), 16 (seaplane), 17 (heliport)
    if (code == 1 || code == 16 || code == 17) {
      // Tokens: 0=code, 1=elevation_ft, 2=deprecated tower flag,
      // 3=deprecated, 4=icao, 5+=airport name. Capture elevation while
      // walking to ICAO so traffic AGL fallback has field elevation.
      int token = 0;
      size_t i = 0;
      current_icao.clear();
      float current_elev_ft = 0.0f;
      bool current_elev_ok = false;
      while (i < line.size() && token < 5) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
          ++i;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t')
          ++i;
        if (token == 1) {
          try {
            current_elev_ft = std::stof(line.substr(start, i - start));
            current_elev_ok = true;
          } catch (...) {
            current_elev_ok = false;
          }
        }
        if (token == 4)
          current_icao = line.substr(start, i - start);
        ++token;
      }
      if (!current_icao.empty() && current_elev_ok) {
        elevations[current_icao] = current_elev_ft;
      }
      // Everything after ICAO is the airport name
      if (!current_icao.empty()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
          ++i;
        if (i < line.size()) {
          // Trim trailing whitespace/CR
          size_t end = line.size();
          while (end > i && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                             line[end - 1] == '\r'))
            --end;
          names[current_icao] = line.substr(i, end - i);
        }
      }
      continue;
    }

    // Frequency lines: codes 50-55 (old) and 1050-1055 (X-Plane 12)
    for (const auto &fc : kFreqCodes) {
      if (code == fc.old_code || code == fc.new_code) {
        if (current_icao.empty())
          break;
        bool is_new_format = (code >= 1000);
        std::istringstream iss(line);
        std::string code_tok, freq_tok;
        if (iss >> code_tok >> freq_tok) {
          uint32_t freq_int = static_cast<uint32_t>(std::stoul(freq_tok));
          uint32_t freq_khz;
          if (is_new_format) {
            // New format (1050-1055): value is already kHz, e.g. 121900
            freq_khz = freq_int;
          } else {
            // Old format (50-55): value is MHz*100, e.g. 12190 → 121900 kHz
            freq_khz = freq_int * 10;
          }
          // Remainder of the line is the frequency name. apt.dat encodes German
          // AFIS/Info and Radio facilities under the Tower code; refine TOWER
          // into INFO/RADIO by the name suffix (no-op for other types).
          std::string name_rest;
          std::getline(iss, name_rest);
          FrequencyType type = classify_by_name(fc.type, name_rest);
          freqs[current_icao].all.push_back({freq_khz, type});
        }
        break;
      }
    }

    // Land runway: code 100
    if (code == 100) {
      if (current_icao.empty())
        continue;

      std::istringstream iss(line);
      std::vector<std::string> tokens;
      std::string tok;
      while (iss >> tok)
        tokens.push_back(tok);

      if (tokens.size() < 20)
        continue;

      RunwayInfo rwy;
      rwy.width_m = std::stof(tokens[1]);
      rwy.surface_code = std::stoi(tokens[2]);
      rwy.end1.number = tokens[8];
      rwy.end1.lat = std::stod(tokens[9]);
      rwy.end1.lon = std::stod(tokens[10]);
      rwy.end2.number = tokens[17];
      rwy.end2.lat = std::stod(tokens[18]);
      rwy.end2.lon = std::stod(tokens[19]);

      rwy.length_m = static_cast<float>(haversine_distance(
          rwy.end1.lat, rwy.end1.lon, rwy.end2.lat, rwy.end2.lon));
      rwy.end1.heading_deg = initial_bearing(rwy.end1.lat, rwy.end1.lon,
                                             rwy.end2.lat, rwy.end2.lon);
      rwy.end2.heading_deg = std::fmod(rwy.end1.heading_deg + 180.0f, 360.0f);

      runways[current_icao].push_back(rwy);

      // Store airport reference position from the first runway encountered
      // (midpoint of both ends — close to the airport reference point).
      if (positions.find(current_icao) == positions.end()) {
        positions[current_icao] = {(rwy.end1.lat + rwy.end2.lat) * 0.5,
                                   (rwy.end1.lon + rwy.end2.lon) * 0.5};
      }
    }
  }

  freq_cache_ = std::move(freqs);
  runway_cache_ = std::move(runways);
  name_cache_ = std::move(names);
  pos_cache_ = std::move(positions);
  elevation_cache_ = std::move(elevations);
  towered_cache_ready_ = true;

  // Count towered airports for log
  size_t towered_count = 0;
  size_t atis_count = 0;
  for (const auto &kv : freq_cache_) {
    if (kv.second.has(FrequencyType::TOWER))
      ++towered_count;
    if (kv.second.has(FrequencyType::ATIS))
      ++atis_count;
  }

  char log[256];
  std::snprintf(
      log, sizeof(log),
      "[xp_wellys_vfr_atc] Airport cache ready: %zu with freqs (%zu "
      "towered, %zu ATIS), %zu with runway data\n",
      freq_cache_.size(), towered_count, atis_count, runway_cache_.size());
  XPLMDebugString(log);
}

void init() {
  dr_latitude = XPLMFindDataRef("sim/flightmodel/position/latitude");
  dr_longitude = XPLMFindDataRef("sim/flightmodel/position/longitude");
  dr_altitude = XPLMFindDataRef("sim/flightmodel/position/elevation");
  dr_groundspeed = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
  dr_indicated_airspeed =
      XPLMFindDataRef("sim/flightmodel/position/indicated_airspeed");
  dr_vertical_speed = XPLMFindDataRef("sim/flightmodel/position/vh_ind_fpm");
  dr_heading_true = XPLMFindDataRef("sim/flightmodel/position/psi");
  dr_y_agl = XPLMFindDataRef("sim/flightmodel/position/y_agl");
  dr_com1_freq =
      XPLMFindDataRef("sim/cockpit2/radios/actuators/com1_frequency_hz_833");
  dr_com2_freq =
      XPLMFindDataRef("sim/cockpit2/radios/actuators/com2_frequency_hz_833");
  dr_active_com =
      XPLMFindDataRef("sim/cockpit2/radios/actuators/audio_com_selection");
  dr_aircraft_icao = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");
  dr_avionics_on = XPLMFindDataRef("sim/cockpit/electrical/avionics_on");
  dr_com1_power = XPLMFindDataRef("sim/cockpit2/radios/actuators/com1_power");
  dr_com2_power = XPLMFindDataRef("sim/cockpit2/radios/actuators/com2_power");
  dr_bus_volts = XPLMFindDataRef("sim/cockpit2/electrical/bus_volts");
  dr_com1_standby = XPLMFindDataRef(
      "sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833");
  dr_com2_standby = XPLMFindDataRef(
      "sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833");
  dr_transponder_code =
      XPLMFindDataRef("sim/cockpit2/radios/actuators/transponder_code");
  dr_transponder_mode =
      XPLMFindDataRef("sim/cockpit2/radios/actuators/transponder_mode");
  // X-Plane 12 "region" weather DataRefs (replacements for deprecated ones)
  dr_barometer =
      XPLMFindDataRef("sim/weather/region/sealevel_pressure_pas"); // Pascals
  dr_wind_direction =
      XPLMFindDataRef("sim/weather/region/wind_direction_degt"); // float[13]
  dr_wind_speed =
      XPLMFindDataRef("sim/weather/region/wind_speed_msc"); // float[13], m/s
  dr_visibility = XPLMFindDataRef(
      "sim/weather/region/visibility_reported_sm"); // statute mi
  dr_cloud_base =
      XPLMFindDataRef("sim/weather/region/cloud_base_msl_m");       // float[3]
  dr_cloud_type = XPLMFindDataRef("sim/weather/region/cloud_type"); // float[3]
  dr_temperature =
      XPLMFindDataRef("sim/weather/region/sealevel_temperature_c"); // float
  dr_dewpoint =
      XPLMFindDataRef("sim/weather/region/dewpoint_deg_c"); // float[13]

  // Build towered cache on background thread
  std::thread(build_towered_cache).detach();
}

void stop() {
  ctx = XPlaneContext{};
  frame_counter = 0;
  locked_airport_id_.clear();
}

void update() {
  ctx.now_secs = static_cast<double>(XPLMGetElapsedTime());
  if (dr_latitude)
    ctx.latitude = XPLMGetDatad(dr_latitude);
  if (dr_longitude)
    ctx.longitude = XPLMGetDatad(dr_longitude);
  if (dr_altitude)
    ctx.altitude_ft_msl =
        static_cast<float>(XPLMGetDatad(dr_altitude) * 3.28084);
  if (dr_groundspeed)
    ctx.groundspeed_kts = XPLMGetDataf(dr_groundspeed) * 1.94384f;
  if (dr_indicated_airspeed)
    ctx.indicated_airspeed_kts = XPLMGetDataf(dr_indicated_airspeed);
  if (dr_vertical_speed)
    ctx.vertical_speed_fpm = XPLMGetDataf(dr_vertical_speed);
  if (dr_heading_true)
    ctx.heading_true = XPLMGetDataf(dr_heading_true);

  if (dr_y_agl) {
    float y_agl = XPLMGetDataf(dr_y_agl);
    ctx.height_agl_ft = y_agl * 3.28084f;
    ctx.on_ground = (y_agl < 0.5f);
  }

  if (dr_com1_freq)
    ctx.com1_freq_mhz =
        static_cast<float>(XPLMGetDatai(dr_com1_freq)) / 1000.0f;
  if (dr_com2_freq)
    ctx.com2_freq_mhz =
        static_cast<float>(XPLMGetDatai(dr_com2_freq)) / 1000.0f;
  if (dr_active_com) {
    int raw_com = XPLMGetDatai(dr_active_com);
    // audio_com_selection returns 6=COM1, 7=COM2; normalize to 1/2
    if (raw_com == 6)
      ctx.active_com = 1;
    else if (raw_com == 7)
      ctx.active_com = 2;
    else
      ctx.active_com = raw_com; // pass through if already 1/2
  }

  if (dr_aircraft_icao) {
    char buf[64] = {};
    XPLMGetDatab(dr_aircraft_icao, buf, 0, sizeof(buf) - 1);
    ctx.aircraft_icao = buf;
  }

  if (dr_avionics_on)
    ctx.avionics_on = (XPLMGetDatai(dr_avionics_on) != 0);

  // COM radio power: combine bus voltage (electrical system alive) with
  // per-radio power switch. Bus voltage is the reliable indicator for
  // cold-and-dark state; the actuator alone stays "on" without power.
  {
    XPLMDataRef dr_power =
        (ctx.active_com == 1) ? dr_com1_power : dr_com2_power;
    bool switch_on = dr_power ? (XPLMGetDatai(dr_power) != 0) : true;

    // bus_volts is float[6] array; index 0 = main bus
    bool bus_live = true; // fail-open
    if (dr_bus_volts) {
      float volts[1] = {};
      XPLMGetDatavf(dr_bus_volts, volts, 0, 1);
      bus_live = (volts[0] > 1.0f);
    }

    ctx.com_radio_powered =
        settings::skip_radio_power_check() || (switch_on && bus_live);
  }

  // Standby frequencies
  if (dr_com1_standby)
    ctx.com1_standby_mhz =
        static_cast<float>(XPLMGetDatai(dr_com1_standby)) / 1000.0f;
  if (dr_com2_standby)
    ctx.com2_standby_mhz =
        static_cast<float>(XPLMGetDatai(dr_com2_standby)) / 1000.0f;
  // Transponder: SSR squawk code + actuator mode (0=off,1=standby,2=on/alt).
  if (dr_transponder_code)
    ctx.transponder_code = XPLMGetDatai(dr_transponder_code);
  if (dr_transponder_mode)
    ctx.transponder_mode = XPLMGetDatai(dr_transponder_mode);
  // Barometer: region DataRef returns Pascals, convert to inHg
  if (dr_barometer)
    ctx.qnh_inhg = XPLMGetDataf(dr_barometer) / 3386.39f;
  // Wind: region DataRefs are float[13] arrays, surface layer = index 0
  if (dr_wind_direction) {
    float dir[1] = {};
    XPLMGetDatavf(dr_wind_direction, dir, 0, 1);
    ctx.wind_direction_deg = dir[0];
  }
  if (dr_wind_speed) {
    float spd[1] = {};
    XPLMGetDatavf(dr_wind_speed, spd, 0, 1);
    ctx.wind_speed_kt = std::max(0.0f, spd[0] * 1.94384f); // m/s → kt
  }
  // Visibility: region DataRef returns statute miles, convert to meters
  if (dr_visibility)
    ctx.visibility_m = XPLMGetDataf(dr_visibility) * 1609.34f;
  // Cloud base: float[3] array, lowest layer = index 0
  if (dr_cloud_base) {
    float base_m[1] = {};
    XPLMGetDatavf(dr_cloud_base, base_m, 0, 1);
    ctx.cloud_base_ft_msl = base_m[0] * 3.28084f;
  }
  // Cloud type: float[3] (0=cirrus, 1=stratus, 2=cumulus, 3=cumulonimbus)
  // Map to ATIS categories: 0=clear, 1=few, 2=scattered, 3=broken, 4=overcast
  if (dr_cloud_type) {
    float ct[1] = {};
    XPLMGetDatavf(dr_cloud_type, ct, 0, 1);
    // Use cloud base to determine coverage — if base is very high, treat as
    // clear. The float value indicates cloud type, not coverage directly.
    // We approximate coverage from cloud type + base.
    if (ctx.cloud_base_ft_msl > 50000.0f) {
      ctx.cloud_type = 0; // clear
    } else if (ct[0] < 0.5f) {
      ctx.cloud_type = 1; // cirrus → few
    } else if (ct[0] < 1.5f) {
      ctx.cloud_type = 3; // stratus → broken
    } else if (ct[0] < 2.5f) {
      ctx.cloud_type = 2; // cumulus → scattered
    } else {
      ctx.cloud_type = 4; // cumulonimbus → overcast
    }
  }
  if (dr_temperature)
    ctx.temperature_c = XPLMGetDataf(dr_temperature);
  // Dewpoint: float[13] array, surface layer = index 0
  if (dr_dewpoint) {
    float dp[1] = {};
    XPLMGetDatavf(dr_dewpoint, dp, 0, 1);
    ctx.dewpoint_c = dp[0];
  }

  // Derive frequency type from active COM via airport frequency database.
  {
    float active_freq =
        (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
    ctx.frequency_type = towered_cache_ready_
                             ? ctx.airport_freqs.lookup(active_freq)
                             : FrequencyType::UNKNOWN;
  }

  // Nearest airport lookup — throttled to every 60 frames (~1s)
  if (++frame_counter % 60 == 0) {
    // Refresh enclosing airspaces (atc.dat-based) — cheap when disabled.
    if (airspace_db::enabled()) {
      ctx.enclosing_airspaces = airspace_db::find_enclosing(
          ctx.latitude, ctx.longitude, ctx.altitude_ft_msl);
    } else {
      ctx.enclosing_airspaces.clear();
    }

    // Airport lock: overrides both geometric and freq-match selection.
    if (!locked_airport_id_.empty() && towered_cache_ready_ &&
        pos_cache_.find(locked_airport_id_) != pos_cache_.end()) {
      // Still refresh geometric_nearest_id for info / debug.
      float lat_f = static_cast<float>(ctx.latitude);
      float lon_f = static_cast<float>(ctx.longitude);
      XPLMNavRef airport_ref = XPLMFindNavAid(nullptr, nullptr, &lat_f, &lon_f,
                                              nullptr, xplm_Nav_Airport);
      if (airport_ref != XPLM_NAV_NOT_FOUND) {
        char icao[32] = {};
        XPLMGetNavAidInfo(airport_ref, nullptr, nullptr, nullptr, nullptr,
                          nullptr, nullptr, icao, nullptr, nullptr);
        ctx.geometric_nearest_id = icao;
      }
      populate_ctx_from_cache(locked_airport_id_, ctx.latitude, ctx.longitude);
      return;
    }

    float lat = static_cast<float>(ctx.latitude);
    float lon = static_cast<float>(ctx.longitude);
    XPLMNavRef airport_ref =
        XPLMFindNavAid(nullptr, nullptr, &lat, &lon, nullptr, xplm_Nav_Airport);

    if (airport_ref != XPLM_NAV_NOT_FOUND) {
      char icao[32] = {};
      float apt_lat = 0, apt_lon = 0;
      XPLMGetNavAidInfo(airport_ref, nullptr, &apt_lat, &apt_lon, nullptr,
                        nullptr, nullptr, icao, nullptr, nullptr);
      ctx.geometric_nearest_id = icao;

      // Frequency-driven active airport: if active COM matches a *different*
      // nearby airport's freq table within realistic VHF range, switch the
      // ATC context to that airport. Cached so the scan only re-runs when
      // the COM frequency or geometric nearest changes.
      static std::string cached_match_id;
      static uint32_t cached_match_freq_khz = 0;
      static std::string cached_match_geom_id;

      uint32_t com_khz = 0;
      {
        float active_freq_mhz =
            (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
        if (active_freq_mhz > 1.0f)
          com_khz =
              static_cast<uint32_t>(std::round(active_freq_mhz * 1000.0f));
      }

      // Frequency-driven switch is only valid when airborne. On the ground
      // the geometric nearest is always correct; a mistuned distant-airport
      // frequency must not hijack the ATC context.
      if (ctx.on_ground) {
        cached_match_id.clear();
        cached_match_freq_khz = com_khz;
        cached_match_geom_id = ctx.geometric_nearest_id;
      } else if (towered_cache_ready_ &&
                 (com_khz != cached_match_freq_khz ||
                  ctx.geometric_nearest_id != cached_match_geom_id)) {
        // Only scan if the geometric nearest itself doesn't match the freq
        auto geom_freqs_it = freq_cache_.find(ctx.geometric_nearest_id);
        bool geom_handles_freq = false;
        if (geom_freqs_it != freq_cache_.end()) {
          for (const auto &f : geom_freqs_it->second.all) {
            uint32_t diff = (com_khz > f.freq_khz) ? com_khz - f.freq_khz
                                                   : f.freq_khz - com_khz;
            if (diff <= 1) {
              geom_handles_freq = true;
              break;
            }
          }
        }
        if (geom_handles_freq) {
          cached_match_id.clear();
        } else {
          cached_match_id = find_freq_match(ctx.geometric_nearest_id, com_khz,
                                            ctx.latitude, ctx.longitude);
          if (!cached_match_id.empty()) {
            char mlog[256];
            std::snprintf(
                mlog, sizeof(mlog),
                "[xp_wellys_vfr_atc] Frequency match: %s (active COM "
                "%u kHz) - switching active airport from %s\n",
                cached_match_id.c_str(), com_khz,
                ctx.geometric_nearest_id.c_str());
            XPLMDebugString(mlog);
          }
        }
        cached_match_freq_khz = com_khz;
        cached_match_geom_id = ctx.geometric_nearest_id;
      }

      // Resolve active airport id + position
      if (!cached_match_id.empty()) {
        ctx.nearest_airport_id = cached_match_id;
        auto pos_it = pos_cache_.find(cached_match_id);
        if (pos_it != pos_cache_.end()) {
          ctx.airport_lat = pos_it->second.first;
          ctx.airport_lon = pos_it->second.second;
        } else {
          ctx.airport_lat = apt_lat;
          ctx.airport_lon = apt_lon;
        }
      } else {
        // Issue #60: only accept the geometric nearest as active ATC airport
        // when it has a contactable ATC frequency. A radio-less field (glider
        // strip like Olten) must not become the ATC context; leave it empty.
        auto geom_it = freq_cache_.find(ctx.geometric_nearest_id);
        if (geom_it != freq_cache_.end() &&
            has_contactable_atc(geom_it->second)) {
          ctx.nearest_airport_id = ctx.geometric_nearest_id;
          ctx.airport_lat = apt_lat;
          ctx.airport_lon = apt_lon;
        } else {
          ctx.nearest_airport_id.clear();
          ctx.airport_lat = 0.0;
          ctx.airport_lon = 0.0;
        }
      }

      // Airport name from cache
      auto name_it = name_cache_.find(ctx.nearest_airport_id);
      ctx.nearest_airport_name =
          (name_it != name_cache_.end()) ? name_it->second : "";

      // Lookup in frequency + runway cache. facility_type stays UNKNOWN until
      // the cache is ready (empty hints panel rather than a wrong guess).
      if (towered_cache_ready_) {
        auto freq_it = freq_cache_.find(ctx.nearest_airport_id);
        if (freq_it != freq_cache_.end()) {
          ctx.airport_freqs = freq_it->second;
          ctx.facility_type = classify_facility(ctx.airport_freqs);
          ctx.tower_only = ctx.is_towered() && !ctx.airport_freqs.has_ground();
          ctx.atis_freq_mhz = ctx.airport_freqs.first_mhz(FrequencyType::ATIS);
        } else {
          ctx.airport_freqs = {};
          ctx.facility_type = FacilityType::UNKNOWN;
          ctx.tower_only = false;
          ctx.atis_freq_mhz = 0.0f;
        }

        auto it = runway_cache_.find(ctx.nearest_airport_id);
        if (it != runway_cache_.end()) {
          ctx.runways = it->second;
          ctx.active_runway = select_active_runway(
              ctx.runways, ctx.wind_direction_deg, ctx.wind_speed_kt);
        } else {
          ctx.runways.clear();
          ctx.active_runway.clear();
        }
      } else {
        ctx.facility_type = FacilityType::UNKNOWN;
      }
    } else {
      ctx.nearest_airport_id = "";
      ctx.geometric_nearest_id = "";
      ctx.facility_type = FacilityType::UNKNOWN;
      ctx.tower_only = false;
      ctx.airport_freqs = {};
      ctx.airport_lat = 0.0;
      ctx.airport_lon = 0.0;
      ctx.runways.clear();
      ctx.active_runway.clear();
    }

    // Debug: log frequency status only on change
    if (settings::debug_logging()) {
      static int last_com = -1;
      static float last_freq_mhz = -1.0f;
      static FrequencyType last_type = FrequencyType::UNKNOWN;
      static std::string last_airport;
      float active_freq =
          (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
      bool changed = (ctx.active_com != last_com) ||
                     (std::fabs(active_freq - last_freq_mhz) > 0.0005f) ||
                     (ctx.frequency_type != last_type) ||
                     (ctx.nearest_airport_id != last_airport);
      if (changed) {
        last_com = ctx.active_com;
        last_freq_mhz = active_freq;
        last_type = ctx.frequency_type;
        last_airport = ctx.nearest_airport_id;
        char dbg[256];
        std::snprintf(
            dbg, sizeof(dbg),
            "[xp_wellys_vfr_atc][DEBUG] COM%d: %.3f MHz -> %s | Airport: %s "
            "(%zu freqs, ATIS=%.3f, facility=%s, tower_only=%d)\n",
            ctx.active_com, active_freq,
            frequency_type_name(ctx.frequency_type),
            ctx.nearest_airport_id.c_str(), ctx.airport_freqs.all.size(),
            ctx.atis_freq_mhz, facility_type_name(ctx.facility_type),
            ctx.tower_only ? 1 : 0);
        XPLMDebugString(dbg);
      }
    }
  }
}

const XPlaneContext &get() { return ctx; }

void lock_airport(const std::string &icao) {
  if (icao.empty())
    return;
  if (!towered_cache_ready_)
    return;
  if (pos_cache_.find(icao) == pos_cache_.end())
    return;
  locked_airport_id_ = icao;
  // Apply immediately so ATC logic doesn't lag up to 1 s behind the click.
  populate_ctx_from_cache(icao, ctx.latitude, ctx.longitude);
  char log[128];
  std::snprintf(log, sizeof(log), "[xp_wellys_vfr_atc] Airport lock: %s\n",
                icao.c_str());
  XPLMDebugString(log);
}

void unlock_airport() {
  if (locked_airport_id_.empty())
    return;
  char log[128];
  std::snprintf(log, sizeof(log),
                "[xp_wellys_vfr_atc] Airport unlock (was %s)\n",
                locked_airport_id_.c_str());
  XPLMDebugString(log);
  locked_airport_id_.clear();
  // Force the next throttle tick to re-resolve geometric nearest.
  frame_counter = 59;
}

const std::string &locked_airport() noexcept { return locked_airport_id_; }

std::vector<NearbyAirport> find_nearby_airports(double max_nm,
                                                size_t max_count) {
  std::vector<NearbyAirport> out;
  if (!towered_cache_ready_ || max_count == 0)
    return out;

  const double ac_lat = ctx.latitude;
  const double ac_lon = ctx.longitude;
  const double max_m = max_nm * kMetersPerNm;

  // Prefilter bbox: max_nm converted to degrees of lat, and lon with cos().
  const double lat_window_deg = max_nm / 60.0; // 1 NM ≈ 1/60°
  const double cos_lat = std::cos(ac_lat * kDeg2Rad);
  const double lon_window_deg =
      (cos_lat > 0.01) ? (max_nm / 60.0) / cos_lat : 180.0;

  out.reserve(32);
  for (const auto &kv : pos_cache_) {
    double apt_lat = kv.second.first;
    double apt_lon = kv.second.second;
    if (std::fabs(apt_lat - ac_lat) > lat_window_deg)
      continue;
    if (std::fabs(apt_lon - ac_lon) > lon_window_deg)
      continue;
    double dist_m = haversine_distance(ac_lat, ac_lon, apt_lat, apt_lon);
    if (dist_m > max_m)
      continue;

    // Issue #60: hide fields without a contactable ATC frequency (glider
    // strips etc.) from the picker so they can't be selected as ATC airport.
    auto freq_it = freq_cache_.find(kv.first);
    if (freq_it == freq_cache_.end() || !has_contactable_atc(freq_it->second))
      continue;

    NearbyAirport na;
    na.icao = kv.first;
    na.distance_nm = dist_m / kMetersPerNm;
    auto name_it = name_cache_.find(kv.first);
    if (name_it != name_cache_.end())
      na.name = name_it->second;
    na.has_atis = freq_it->second.has(FrequencyType::ATIS);
    na.has_ground = freq_it->second.has(FrequencyType::GROUND);
    na.has_tower = freq_it->second.has(FrequencyType::TOWER);
    na.has_afis = freq_it->second.has(FrequencyType::INFO) ||
                  freq_it->second.has(FrequencyType::RADIO);
    na.has_unicom = freq_it->second.has(FrequencyType::UNICOM) ||
                    freq_it->second.has(FrequencyType::CTAF);
    out.push_back(std::move(na));
  }

  std::sort(out.begin(), out.end(),
            [](const NearbyAirport &a, const NearbyAirport &b) {
              return a.distance_nm < b.distance_nm;
            });
  if (out.size() > max_count)
    out.resize(max_count);
  return out;
}

float airport_elevation_ft(const std::string &icao) {
  if (!towered_cache_ready_ || icao.empty())
    return 0.0f;
  auto it = elevation_cache_.find(icao);
  return (it != elevation_cache_.end()) ? it->second : 0.0f;
}

bool airport_elevation_known(const std::string &icao) {
  if (!towered_cache_ready_ || icao.empty())
    return false;
  return elevation_cache_.find(icao) != elevation_cache_.end();
}

void set_standby_freq(uint32_t freq_khz) {
  XPLMDataRef dr = (ctx.active_com == 1) ? dr_com1_standby : dr_com2_standby;
  if (dr) {
    XPLMSetDatai(dr, static_cast<int>(freq_khz));
    if (settings::debug_logging()) {
      char dbg[128];
      std::snprintf(dbg, sizeof(dbg),
                    "[xp_wellys_vfr_atc][DEBUG] Set COM%d standby to %u "
                    "(%.3f MHz)\n",
                    ctx.active_com, freq_khz,
                    static_cast<float>(freq_khz) / 1000.0f);
      XPLMDebugString(dbg);
    }
  }
}

} // namespace xplane_context
