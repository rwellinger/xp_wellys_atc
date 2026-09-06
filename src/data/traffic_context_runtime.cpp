/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Plugin-only runtime reader for the SDK-free `traffic_context` API.
 * Reads the standard `sim/cockpit2/tcas/targets/...` dataRefs (works
 * for LiveTraffic, X-IvAp, swift, native AI) and turns them into a
 * `TrafficContext` snapshot which the engine + UI consume via
 * `traffic_context::current()`.
 *
 * Provider-agnostic: never touches LTAPI or any LiveTraffic-specific
 * symbol. Never calls `XPLMAcquirePlanes` — we only READ.
 */

#include "data/traffic_context.hpp"

#include "core/xplane_context.hpp"
#include "data/traffic_geometry.hpp"
#include "data/traffic_phase_classifier.hpp"
#include "persistence/settings.hpp"

#include <XPLMDataAccess.h>
#include <XPLMGraphics.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace traffic_context {

namespace {

// Standard X-Plane TCAS slots — slot 0 is the user, 1..63 are AI/multi.
constexpr std::size_t kMaxSlots = 64;
constexpr float kSentinelLocal = 9999999.0f;
constexpr double kMaxRangeNm = 40.0;
constexpr std::size_t kFlightIdLen = 8;

XPLMDataRef dr_x = nullptr;
XPLMDataRef dr_y = nullptr;
XPLMDataRef dr_z = nullptr;
XPLMDataRef dr_modeS = nullptr;
XPLMDataRef dr_flight_id = nullptr;
XPLMDataRef dr_v_msc = nullptr;
XPLMDataRef dr_vs = nullptr;
XPLMDataRef dr_psi = nullptr;
XPLMDataRef dr_wake_cat = nullptr;  // optional — null if provider lacks it
XPLMDataRef dr_icao_type = nullptr; // optional — provider may leave blank

WakeCategory map_wake(int code) {
  // Per X-Plane SDK: 0=unknown, 1=light, 2=medium, 3=heavy, 4=super.
  switch (code) {
  case 1:
    return WakeCategory::Light;
  case 2:
    return WakeCategory::Medium;
  case 3:
    return WakeCategory::Heavy;
  case 4:
    return WakeCategory::Super;
  default:
    return WakeCategory::Unknown;
  }
}

} // namespace

void init() {
  dr_x = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/x");
  dr_y = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/y");
  dr_z = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/z");
  dr_modeS = XPLMFindDataRef("sim/cockpit2/tcas/targets/modeS_id");
  dr_flight_id = XPLMFindDataRef("sim/cockpit2/tcas/targets/flight_id");
  dr_v_msc = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/V_msc");
  dr_vs = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/vertical_speed");
  dr_psi = XPLMFindDataRef("sim/cockpit2/tcas/targets/position/psi");
  // Wake-cat is optional — only populated by some providers. We tolerate
  // a null handle and default everyone to Unknown if it's missing.
  dr_wake_cat = XPLMFindDataRef("sim/cockpit2/tcas/targets/wake/wake_cat");
  // ICAO aircraft type — also optional. Providers that publish it use a
  // packed 8-byte ASCII slot per target (same layout as flight_id).
  dr_icao_type = XPLMFindDataRef("sim/cockpit2/tcas/targets/icao_type");

  // Reset the snapshot — the next update() will refill it.
  set_for_test(TrafficContext{});

  if (settings::debug_logging()) {
    char log[256];
    std::snprintf(
        log, sizeof(log),
        "[xp_wellys_vfr_atc] traffic_context init: x=%p modeS=%p flight=%p "
        "wake_cat=%p\n",
        static_cast<void *>(dr_x), static_cast<void *>(dr_modeS),
        static_cast<void *>(dr_flight_id), static_cast<void *>(dr_wake_cat));
    XPLMDebugString(log);
  }
}

void stop() {
  set_for_test(TrafficContext{});
  dr_x = dr_y = dr_z = nullptr;
  dr_modeS = dr_flight_id = nullptr;
  dr_v_msc = dr_vs = dr_psi = nullptr;
  dr_wake_cat = nullptr;
  dr_icao_type = nullptr;
}

void update() {
  // Master switch (settings::traffic_features_enabled). When the user
  // has disabled the traffic subsystem, drop the live snapshot and
  // return immediately. Every downstream consumer (advisor / pattern_flow
  // landing-sequence overlay / engine::poll_go_around) reads
  // traffic_context::current() and becomes a no-op against an empty
  // snapshot — so the master switch needs gating at exactly one place.
  static bool was_disabled_ = false;
  if (!settings::traffic_features_enabled()) {
    if (!was_disabled_) {
      set_for_test(TrafficContext{});
      was_disabled_ = true;
    }
    return;
  }
  if (was_disabled_) {
    // Re-enabled mid-session — force a fresh snapshot on the next pass
    // even if the dataRefs haven't changed since.
    was_disabled_ = false;
  }

  // Required handles — bail if any is missing (e.g. before init or on a
  // stripped X-Plane install). Optional ones (wake_cat, v_msc, vs, psi)
  // are tolerated as zero.
  if (!dr_x || !dr_y || !dr_z || !dr_modeS) {
    return;
  }

  std::array<float, kMaxSlots> xs{};
  std::array<float, kMaxSlots> ys{};
  std::array<float, kMaxSlots> zs{};
  std::array<int, kMaxSlots> modeS{};
  std::array<float, kMaxSlots> v_msc{};
  std::array<float, kMaxSlots> vs{};
  std::array<float, kMaxSlots> psi{};
  std::array<int, kMaxSlots> wake{};
  std::array<char, kFlightIdLen * kMaxSlots> flight_ids{};
  std::array<char, kFlightIdLen * kMaxSlots> icao_types{};

  constexpr int kSlotsI = static_cast<int>(kMaxSlots);
  XPLMGetDatavf(dr_x, xs.data(), 0, kSlotsI);
  XPLMGetDatavf(dr_y, ys.data(), 0, kSlotsI);
  XPLMGetDatavf(dr_z, zs.data(), 0, kSlotsI);
  XPLMGetDatavi(dr_modeS, modeS.data(), 0, kSlotsI);
  if (dr_v_msc)
    XPLMGetDatavf(dr_v_msc, v_msc.data(), 0, kSlotsI);
  if (dr_vs)
    XPLMGetDatavf(dr_vs, vs.data(), 0, kSlotsI);
  if (dr_psi)
    XPLMGetDatavf(dr_psi, psi.data(), 0, kSlotsI);
  if (dr_wake_cat)
    XPLMGetDatavi(dr_wake_cat, wake.data(), 0, kSlotsI);
  if (dr_flight_id) {
    XPLMGetDatab(dr_flight_id, flight_ids.data(), 0,
                 static_cast<int>(flight_ids.size()));
  }
  if (dr_icao_type) {
    XPLMGetDatab(dr_icao_type, icao_types.data(), 0,
                 static_cast<int>(icao_types.size()));
  }

  const auto &user = xplane_context::get();
  const double user_lat = user.latitude;
  const double user_lon = user.longitude;
  const double user_alt_ft = static_cast<double>(user.altitude_ft_msl);
  const double user_heading = static_cast<double>(user.heading_true);

  const float field_elev_ft =
      xplane_context::airport_elevation_ft(user.nearest_airport_id);
  const bool field_elev_known =
      xplane_context::airport_elevation_known(user.nearest_airport_id);

  // Phase-4: build the active-runway hints once per tick so the
  // classifier can promote Pattern/Final targets. Picks the threshold
  // matching `user.active_runway` from the runway cache; falls back to
  // no hints when we can't resolve a unique threshold.
  std::optional<traffic_phase_classifier::AirportRunwayHints> rwy_hints;
  if (!user.active_runway.empty() && !user.runways.empty() &&
      (user.airport_lat != 0.0 || user.airport_lon != 0.0)) {
    for (const auto &rw : user.runways) {
      const xplane_context::RunwayEnd *end = nullptr;
      double heading_deg = 0.0;
      if (rw.end1.number == user.active_runway) {
        end = &rw.end1;
        heading_deg = static_cast<double>(rw.end1.heading_deg);
      } else if (rw.end2.number == user.active_runway) {
        end = &rw.end2;
        heading_deg = static_cast<double>(rw.end2.heading_deg);
      }
      if (!end)
        continue;
      traffic_phase_classifier::AirportRunwayHints h;
      h.airport_lat = user.airport_lat;
      h.airport_lon = user.airport_lon;
      h.threshold_lat = end->lat;
      h.threshold_lon = end->lon;
      h.runway_heading_deg = heading_deg;
      // pattern_direction stays empty here; the sequencing layer
      // resolves it via airport_vrps when needed.
      rwy_hints = h;
      break;
    }
  }

  // Per-target previous phase, keyed by modeS_id. Persisted across
  // ticks so the classifier's Landed branch can fire. Entries for
  // targets that disappeared from the snapshot are GC'd at the end of
  // the loop. Static-local lifetime is fine — single writer (this fn
  // is called from XPlaneFlightLoop on the main thread).
  static std::unordered_map<uint32_t, TrafficPhase> prev_phase;
  std::unordered_set<uint32_t> seen_ids;
  seen_ids.reserve(16);

  TrafficContext snapshot;
  snapshot.targets.reserve(16);

  for (std::size_t i = 1; i < kMaxSlots; ++i) {
    // XPMP2 / LiveTraffic sentinel: empty slots receive a far-away X.
    if (xs[i] > kSentinelLocal || ys[i] > kSentinelLocal ||
        zs[i] > kSentinelLocal) {
      continue;
    }
    // Some providers leave modeS at 0 to signal an unused slot.
    if (modeS[i] == 0) {
      continue;
    }

    double lat = 0.0;
    double lon = 0.0;
    double alt_m = 0.0;
    XPLMLocalToWorld(static_cast<double>(xs[i]), static_cast<double>(ys[i]),
                     static_cast<double>(zs[i]), &lat, &lon, &alt_m);

    const double dist_nm =
        traffic_geometry::distance_nm(user_lat, user_lon, lat, lon);
    if (dist_nm > kMaxRangeNm) {
      continue;
    }

    TrafficTarget t;
    t.modeS_id = static_cast<uint32_t>(modeS[i]);
    t.callsign = trim_callsign(&flight_ids[i * kFlightIdLen], kFlightIdLen);
    t.icao_type = dr_icao_type ? trim_callsign(&icao_types[i * kFlightIdLen],
                                               kFlightIdLen)
                               : std::string{};
    t.lat = lat;
    t.lon = lon;
    t.alt_msl_ft = alt_m * 3.28084;
    if (field_elev_known) {
      t.alt_agl_ft = t.alt_msl_ft - static_cast<double>(field_elev_ft);
      if (t.alt_agl_ft < 0.0)
        t.alt_agl_ft = 0.0;
    } else {
      t.alt_agl_ft = 0.0;
    }
    t.bearing_from_user_deg =
        traffic_geometry::bearing_deg(user_lat, user_lon, lat, lon);
    t.clock_position =
        traffic_geometry::clock_position(user_heading, t.bearing_from_user_deg);
    t.distance_to_user_nm = dist_nm;
    t.altitude_diff_ft = t.alt_msl_ft - user_alt_ft;
    t.groundspeed_kts = static_cast<double>(v_msc[i]) * 1.94384;
    t.vertical_speed_fpm = static_cast<double>(vs[i]);
    t.track_deg = static_cast<double>(psi[i]);
    t.wake = dr_wake_cat ? map_wake(wake[i]) : WakeCategory::Unknown;

    // Phase 3 classifier. Without a known field elevation we can't
    // reason about AGL, so we keep the target in Unknown rather than
    // misclassifying an airborne target as OnGround.
    TrafficPhase prev = TrafficPhase::Unknown;
    auto prev_it = prev_phase.find(t.modeS_id);
    if (prev_it != prev_phase.end())
      prev = prev_it->second;
    t.phase = field_elev_known
                  ? traffic_phase_classifier::classify(t, prev, rwy_hints)
                  : TrafficPhase::Unknown;
    prev_phase[t.modeS_id] = t.phase;
    seen_ids.insert(t.modeS_id);

    snapshot.targets.push_back(std::move(t));
  }

  // GC dropped targets so the map doesn't grow unbounded across a
  // long session full of transient TCAS contacts.
  for (auto it = prev_phase.begin(); it != prev_phase.end();) {
    if (seen_ids.find(it->first) == seen_ids.end())
      it = prev_phase.erase(it);
    else
      ++it;
  }

  std::sort(snapshot.targets.begin(), snapshot.targets.end(),
            [](const TrafficTarget &a, const TrafficTarget &b) {
              return a.distance_to_user_nm < b.distance_to_user_nm;
            });

  snapshot.last_update_secs = static_cast<double>(XPLMGetElapsedTime());

  set_for_test(std::move(snapshot));
}

} // namespace traffic_context
