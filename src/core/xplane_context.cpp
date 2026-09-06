/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ── POD helpers only. All X-Plane SDK / apt.dat runtime code lives in
 *    xplane_context_runtime.cpp, which is only linked into the plugin.
 */

#include "core/xplane_context.hpp"

#include <cmath>

namespace xplane_context {

namespace {
// Priority ranking for resolving frequency type when multiple apt.dat entries
// share the same frequency (part-time towers like KVRB list 126.300 as both
// TOWER and UNICOM). Higher rank wins.
int priority_rank(FrequencyType t) {
  switch (t) {
  case FrequencyType::TOWER:
    return 7;
  case FrequencyType::GROUND:
    return 6;
  case FrequencyType::DELIVERY:
    return 5;
  case FrequencyType::INFO:
  case FrequencyType::RADIO:
    return 4;
  case FrequencyType::ATIS:
    return 3;
  case FrequencyType::CTAF:
    return 2;
  case FrequencyType::UNICOM:
    return 1;
  case FrequencyType::UNKNOWN:
    return 0;
  }
  return 0;
}
} // namespace

bool AirportFrequencies::has(FrequencyType ft) const {
  for (const auto &f : all)
    if (f.type == ft)
      return true;
  return false;
}

float AirportFrequencies::first_mhz(FrequencyType ft) const {
  for (const auto &f : all)
    if (f.type == ft)
      return static_cast<float>(f.freq_khz) / 1000.0f;
  return 0.0f;
}

FrequencyType AirportFrequencies::lookup(float freq_mhz) const {
  const uint32_t target = static_cast<uint32_t>(std::round(freq_mhz * 1000.0f));
  FrequencyType best = FrequencyType::UNKNOWN;
  int best_rank = -1;
  for (const auto &f : all) {
    const uint32_t diff =
        (target > f.freq_khz) ? target - f.freq_khz : f.freq_khz - target;
    if (diff > 1)
      continue;
    const int rank = priority_rank(f.type);
    if (rank > best_rank) {
      best_rank = rank;
      best = f.type;
    }
  }
  return best;
}

bool AirportFrequencies::has_ground() const {
  return has(FrequencyType::GROUND);
}

const char *frequency_type_name(FrequencyType ft) {
  switch (ft) {
  case FrequencyType::UNKNOWN:
    return "Unknown";
  case FrequencyType::DELIVERY:
    return "Delivery";
  case FrequencyType::GROUND:
    return "Ground";
  case FrequencyType::TOWER:
    return "Tower";
  case FrequencyType::UNICOM:
    return "Unicom";
  case FrequencyType::CTAF:
    return "CTAF";
  case FrequencyType::ATIS:
    return "ATIS";
  case FrequencyType::INFO:
    return "Info";
  case FrequencyType::RADIO:
    return "Radio";
  }
  return "Unknown";
}

const char *facility_type_name(FacilityType ft) {
  switch (ft) {
  case FacilityType::UNKNOWN:
    return "Unknown";
  case FacilityType::UNCONTROLLED:
    return "Uncontrolled";
  case FacilityType::TOWERED:
    return "Towered";
  case FacilityType::AFIS:
    return "AFIS";
  }
  return "Unknown";
}

FacilityType classify_facility(const AirportFrequencies &freqs) {
  if (freqs.has(FrequencyType::TOWER))
    return FacilityType::TOWERED;
  if (freqs.has(FrequencyType::INFO) || freqs.has(FrequencyType::RADIO))
    return FacilityType::AFIS;
  if (freqs.has(FrequencyType::UNICOM) || freqs.has(FrequencyType::CTAF))
    return FacilityType::UNCONTROLLED;
  return FacilityType::UNKNOWN;
}

bool has_contactable_atc(const AirportFrequencies &freqs) {
  return classify_facility(freqs) != FacilityType::UNKNOWN;
}

FrequencyType classify_by_name(FrequencyType base, const std::string &name) {
  if (base != FrequencyType::TOWER)
    return base;

  // Extract the last whitespace-delimited token of the apt.dat frequency name.
  size_t end = name.find_last_not_of(" \t\r\n");
  if (end == std::string::npos)
    return FrequencyType::TOWER; // no name -> treat as Tower (legacy behaviour)
  size_t start = name.find_last_of(" \t\r\n", end);
  start = (start == std::string::npos) ? 0 : start + 1;
  std::string last = name.substr(start, end - start + 1);

  // Upper-case for case-insensitive comparison.
  for (char &c : last)
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');

  if (last == "INFO" || last == "INFORMATION" || last == "IFO")
    return FrequencyType::INFO;
  if (last == "RADIO" || last == "RDO")
    return FrequencyType::RADIO;
  return FrequencyType::TOWER;
}

} // namespace xplane_context
