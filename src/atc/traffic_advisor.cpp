/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "atc/traffic_advisor.hpp"

#include "data/traffic_geometry.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace traffic_advisor {

namespace {

constexpr double kDeg2Rad = M_PI / 180.0;

// Cosine of (track - bearing). Positive when "track" points along the
// LOS, negative when away.
double cos_track_minus(double track_deg, double bearing_deg) {
  return std::cos((track_deg - bearing_deg) * kDeg2Rad);
}

// Closure rate in knots (positive = range decreasing). Computed from
// the user + target velocity vectors projected onto the line of sight.
double closure_kts(const UserState &user,
                   const traffic_context::TrafficTarget &t) {
  double bearing = t.bearing_from_user_deg;
  double user_along =
      user.groundspeed_kts * cos_track_minus(user.track_deg, bearing);
  double tgt_along = t.groundspeed_kts * cos_track_minus(t.track_deg, bearing);
  // user_along positive: user moves toward target.
  // tgt_along positive: target moves AWAY from user (down-LOS).
  // Closure = user_along - tgt_along.
  return user_along - tgt_along;
}

// Target qualifies as "laterally converging": user and target tracks
// are roughly perpendicular (60-120 deg or 240-300 deg). Used as the
// fallback when closure_kts is non-positive but the geometry still
// warrants a callout.
bool laterally_converging(const UserState &user,
                          const traffic_context::TrafficTarget &t) {
  double diff = std::fmod(t.track_deg - user.track_deg + 720.0, 360.0);
  return (diff >= 60.0 && diff <= 120.0) || (diff >= 240.0 && diff <= 300.0);
}

bool in_forward_arc(double clock_pos) {
  // Forward arc rounded to hour clock positions: 9, 10, 11, 12, 1, 2, 3.
  // The clock_position helper collapses the 0/12 boundary onto 12 and
  // never returns 0, so we just check inclusive ranges.
  if (clock_pos >= 9.0 && clock_pos <= 12.0)
    return true;
  if (clock_pos >= 1.0 && clock_pos <= 3.0)
    return true;
  return false;
}

bool gating_passes(const UserState &user) {
  if (user.atc_state == atc_state_machine::ATCState::IDLE)
    return false;
  if (user.atc_state == atc_state_machine::ATCState::UNICOM_ACTIVE)
    return false;
  return user.on_active_atc_freq;
}

bool target_qualifies(const traffic_context::TrafficTarget &t,
                      const UserState &user) {
  if (t.distance_to_user_nm < kMinDistanceNm ||
      t.distance_to_user_nm > kMaxDistanceNm)
    return false;
  if (std::fabs(t.altitude_diff_ft) > kMaxAltDiffFt)
    return false;
  if (!in_forward_arc(t.clock_position))
    return false;
  // Domain match: airborne pilot wants airborne targets, ground pilot
  // wants ground targets. Cross-domain (e.g. airborne traffic advised
  // to a parked pilot) is muted. Phase 4/5 will reintroduce limited
  // cross-domain awareness for takeoff/landing conflict cases.
  bool target_on_ground = t.alt_agl_ft <= kGroundDomainAglFt;
  if (user.on_ground != target_on_ground)
    return false;
  if (closure_kts(user, t) <= 0.0 && !laterally_converging(user, t))
    return false;
  return true;
}

std::map<std::string, std::string>
build_advisory_vars(const traffic_context::TrafficTarget &t,
                    const UserState &user) {
  char clock_buf[8];
  char dist_buf[16];
  std::snprintf(clock_buf, sizeof(clock_buf), "%.0f", t.clock_position);
  std::snprintf(dist_buf, sizeof(dist_buf), "%.0f", t.distance_to_user_nm);

  std::string direction = traffic_geometry::classify_relative_track(
      user.track_deg, t.track_deg, t.clock_position);
  std::string altitude_info = traffic_geometry::format_altitude_info(
      t.alt_msl_ft, user.alt_msl_ft, user.target_has_mode_c_default);

  // ICAO type from the provider when present (e.g. "C172", "PA28").
  // Otherwise the EU phraseology fallback wins.
  std::string type =
      t.icao_type.empty() ? std::string{"type unknown"} : t.icao_type;

  return {
      {"clock", clock_buf},
      {"distance", dist_buf},
      {"direction", std::move(direction)},
      {"altitude_info", std::move(altitude_info)},
      {"type", std::move(type)},
  };
}

// ── Ground-conflict helpers (Phase 3) ───────────────────────────────

// Maps a target's clock position to the {side} placeholder used by
// the taxi_caution / taxi_give_way templates. Clock 1-3 -> right,
// 9-11 -> left. Anything else falls back to "left" as the safest
// neutral phrasing (the target is somewhere ahead or behind, not
// abeam — the controller still calls out a side rather than going
// silent).
std::string side_from_clock(double clock_pos) {
  if (clock_pos >= 1.0 && clock_pos <= 3.0)
    return "right";
  if (clock_pos >= 9.0 && clock_pos <= 11.0)
    return "left";
  return "left";
}

bool ground_target_qualifies(const traffic_context::TrafficTarget &t) {
  if (t.distance_to_user_nm >= kGroundConflictMaxNm)
    return false;
  // Target must itself be moving on the surface (or just lifted off /
  // touched down). OnGround targets are stationary and not a conflict.
  if (t.phase != traffic_context::TrafficPhase::Taxi &&
      t.phase != traffic_context::TrafficPhase::Takeoff &&
      t.phase != traffic_context::TrafficPhase::Landed)
    return false;
  return true;
}

// Pick the right template + phraseology for a qualifying ground
// target. Three tiers, controller-style:
//   - directly ahead (clock 11..1)            -> taxi_hold_position
//   - abeam (clock 2..3 / 9..10)              -> taxi_caution
//   - approaching from the side at speed       -> taxi_give_way
const char *ground_template_for(const traffic_context::TrafficTarget &t) {
  const double c = t.clock_position;
  const bool directly_ahead = c >= 11.0 || c <= 1.0;
  if (directly_ahead)
    return "taxi_hold_position";
  // Faster movers on a converging side path get the give-way verb;
  // slow taxiers get the gentler caution.
  if (t.groundspeed_kts >= 15.0)
    return "taxi_give_way";
  return "taxi_caution";
}

std::map<std::string, std::string>
build_ground_vars(const traffic_context::TrafficTarget &t) {
  std::string type =
      t.icao_type.empty() ? std::string{"aircraft"} : t.icao_type;
  return {
      {"side", side_from_clock(t.clock_position)},
      {"type", std::move(type)},
  };
}

std::optional<TrafficAdvisory>
evaluate_ground_conflict(const traffic_context::TrafficContext &traffic,
                         const UserState &user, const AdvisoryHistory &history,
                         double now_secs) {
  if (!user.user_taxiing)
    return std::nullopt;

  if (now_secs - history.last_global_emit_secs < kGroundGlobalCooldownSec)
    return std::nullopt;

  for (const auto &t : traffic.targets) {
    if (!ground_target_qualifies(t))
      continue;
    if (!traffic_geometry::path_intersects_cone(
            user.lat, user.lon, user.heading_deg, kGroundConeHalfDeg,
            kGroundConeDistM, t.lat, t.lon, t.track_deg, t.groundspeed_kts,
            kGroundLookaheadSec))
      continue;

    auto issued = history.last_issued_secs.find(t.modeS_id);
    if (issued != history.last_issued_secs.end() &&
        now_secs - issued->second < kGroundPerTargetCooldownSec)
      continue;

    TrafficAdvisory adv;
    adv.modeS_id = t.modeS_id;
    adv.template_key = ground_template_for(t);
    adv.vars = build_ground_vars(t);
    adv.requires_ack = false;
    return adv;
  }

  return std::nullopt;
}

} // namespace

std::optional<TrafficAdvisory>
evaluate(const traffic_context::TrafficContext &traffic, const UserState &user,
         const AdvisoryHistory &history, double now_secs) {
  // Phase-3 ground-conflict path runs first — it does not need ATC
  // contact and overrides the regular "wait for tower contact" gate.
  // Surface safety advisories are independent of who the pilot is
  // talking to.
  if (auto ground = evaluate_ground_conflict(traffic, user, history, now_secs))
    return ground;

  if (!gating_passes(user))
    return std::nullopt;

  if (now_secs - history.last_global_emit_secs < kGlobalCooldownSec)
    return std::nullopt;

  // Targets in the snapshot are sorted by distance ascending (per the
  // runtime reader + fixture loader), so the first qualifying target is
  // also the nearest qualifying target.
  for (const auto &t : traffic.targets) {
    if (!target_qualifies(t, user))
      continue;

    auto issued = history.last_issued_secs.find(t.modeS_id);
    if (issued != history.last_issued_secs.end() &&
        now_secs - issued->second < kPerTargetCooldownSec)
      continue;

    auto acked = history.acknowledged_visual_secs.find(t.modeS_id);
    if (acked != history.acknowledged_visual_secs.end() &&
        now_secs - acked->second < kVisualAckLockoutSec)
      continue;

    TrafficAdvisory adv;
    adv.modeS_id = t.modeS_id;
    adv.vars = build_advisory_vars(t, user);
    return adv;
  }

  return std::nullopt;
}

void mark_emitted(AdvisoryHistory &history, uint32_t modeS_id,
                  double now_secs) {
  history.last_issued_secs[modeS_id] = now_secs;
  history.last_global_emit_secs = now_secs;
}

void mark_acknowledged_visual(AdvisoryHistory &history, uint32_t modeS_id,
                              double now_secs) {
  history.acknowledged_visual_secs[modeS_id] = now_secs;
}

} // namespace traffic_advisor
