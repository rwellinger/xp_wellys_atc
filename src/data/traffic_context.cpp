/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "data/traffic_context.hpp"

#include <cctype>
#include <utility>

namespace traffic_context {

namespace {
TrafficContext g_snapshot_;
} // namespace

const TrafficContext &current() { return g_snapshot_; }

void set_for_test(TrafficContext snapshot) {
  g_snapshot_ = std::move(snapshot);
}

std::string trim_callsign(const char *raw, std::size_t len) {
  // Walk to first NUL or end-of-buffer; that fixes the live length.
  std::size_t end = 0;
  for (; end < len; ++end) {
    if (raw[end] == '\0')
      break;
  }
  // Strip trailing whitespace.
  while (end > 0 && std::isspace(static_cast<unsigned char>(raw[end - 1]))) {
    --end;
  }
  // Strip leading whitespace.
  std::size_t start = 0;
  while (start < end && std::isspace(static_cast<unsigned char>(raw[start]))) {
    ++start;
  }
  return std::string(raw + start, end - start);
}

} // namespace traffic_context
