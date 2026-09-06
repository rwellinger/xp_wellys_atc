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

#include "atc/flight_phase.hpp"
#include "atc/atc_state_machine.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <json.hpp>

#include <cmath>
#include <fstream>

namespace flight_phase {

// ── Constants ────────────────────────────────────────────────────

static constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
static constexpr double kEarthRadiusM = 6371000.0;
static constexpr double kMetersPerNm = 1852.0;

// ── Phase thresholds (loaded from JSON) ──────────────────────────

struct PhaseThresholds {
  float taxi_min_gs_kt = 5.0f;
  float roll_min_gs_kt = 40.0f;
  float climb_min_vs_fpm = 300.0f;
  float pattern_max_agl_ft = 3000.0f;
  float near_airport_nm = 5.0f;
  float runway_aligned_deg = 30.0f;
  float final_descent_rate_fpm = -200.0f;
};

struct HysteresisConfig {
  float ground_to_airborne_sec = 0.5f;
  float airborne_to_landing_sec = 0.3f;
  float auto_correction_delay_sec = 3.0f;
};

// ── Module state ─────────────────────────────────────────────────

static PhaseThresholds thresholds_;
static HysteresisConfig hysteresis_;
static std::map<std::string, IntentPrecondition> preconditions_;
static std::map<std::string, std::map<std::string, AutoCorrection>>
    auto_corrections_;
static std::map<std::string, std::map<std::string, FrequencyAutoCorrection>>
    frequency_auto_corrections_;
static std::map<std::string, FrequencyRule> intent_frequency_;
static std::map<std::string, std::string> pilot_phraseology_;
static std::map<xplane_context::FrequencyType, std::vector<std::string>>
    state_frequency_validity_;
static std::vector<IdleRedirect> idle_redirects_;
static std::vector<StateRevert> state_reverts_;
static std::map<std::string, std::string> tower_only_auto_advance_;
static FrequencyHint frequency_hint_;
static bool frequency_hint_set_ = false;
static bool loaded_ = false;

static FlightPhase current_phase_ = FlightPhase::PARKED;
static FlightPhase candidate_phase_ = FlightPhase::PARKED;
static float candidate_timer_ = 0.0f;
static bool was_airborne_ = false;

// ── Helpers ──────────────────────────────────────────────────────

static double haversine_distance_nm(double lat1, double lon1, double lat2,
                                    double lon2) {
  double dlat = (lat2 - lat1) * kDeg2Rad;
  double dlon = (lon2 - lon1) * kDeg2Rad;
  double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
             std::cos(lat1 * kDeg2Rad) * std::cos(lat2 * kDeg2Rad) *
                 std::sin(dlon / 2) * std::sin(dlon / 2);
  double m = kEarthRadiusM * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
  return m / kMetersPerNm;
}

static float active_runway_heading(const xplane_context::XPlaneContext &ctx) {
  // Prefer the ATC-locked runway over the wind-determined active one so
  // FINAL_APPROACH alignment stays stable when the wind shifts after the
  // pilot has already been cleared to land on a specific runway.
  std::string rwy_num = atc_state_machine::effective_runway(ctx);
  for (const auto &rwy : ctx.runways) {
    if (rwy.end1.number == rwy_num)
      return rwy.end1.heading_deg;
    if (rwy.end2.number == rwy_num)
      return rwy.end2.heading_deg;
  }
  return -1.0f;
}

static float angle_diff(float a, float b) {
  float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
  return std::fabs(d);
}

// ── Phase name mapping ───────────────────────────────────────────

static const char *kPhaseNames[] = {
    "PARKED",  "TAXI",           "TAKEOFF_ROLL", "CLIMB",
    "PATTERN", "FINAL_APPROACH", "LANDING_ROLL", "CRUISE",
};
static constexpr int kPhaseCount =
    static_cast<int>(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]));

const char *phase_name(FlightPhase phase) {
  return kPhaseNames[static_cast<int>(phase)];
}

FlightPhase phase_from_name(const std::string &name) {
  for (int i = 0; i < kPhaseCount; ++i) {
    if (name == kPhaseNames[i])
      return static_cast<FlightPhase>(i);
  }
  return FlightPhase::PARKED;
}

bool is_airborne(FlightPhase phase) {
  return phase == FlightPhase::CLIMB || phase == FlightPhase::PATTERN ||
         phase == FlightPhase::FINAL_APPROACH || phase == FlightPhase::CRUISE;
}

bool is_on_ground(FlightPhase phase) {
  return phase == FlightPhase::PARKED || phase == FlightPhase::TAXI ||
         phase == FlightPhase::TAKEOFF_ROLL ||
         phase == FlightPhase::LANDING_ROLL;
}

// ── Frequency-name mapping (forward-declared, used in load_from_file) ──

static xplane_context::FrequencyType
freq_type_from_name(const std::string &name);

// ── JSON loading ─────────────────────────────────────────────────

static void load_from_file() {
  std::string path = settings::atc_profile_data_dir() + "/flight_rules.json";
  std::ifstream in(path);
  if (!in.good()) {
    logging::info("Warning: flight_rules.json not found");
    loaded_ = false;
    return;
  }

  try {
    nlohmann::json j;
    in >> j;

    // Phase thresholds
    if (j.contains("phase_thresholds")) {
      auto &pt = j["phase_thresholds"];
      thresholds_.taxi_min_gs_kt = pt.value("taxi_min_gs_kt", 5.0f);
      thresholds_.roll_min_gs_kt = pt.value("roll_min_gs_kt", 40.0f);
      thresholds_.climb_min_vs_fpm = pt.value("climb_min_vs_fpm", 300.0f);
      thresholds_.pattern_max_agl_ft = pt.value("pattern_max_agl_ft", 3000.0f);
      thresholds_.near_airport_nm = pt.value("near_airport_nm", 5.0f);
      thresholds_.runway_aligned_deg = pt.value("runway_aligned_deg", 30.0f);
      thresholds_.final_descent_rate_fpm =
          pt.value("final_descent_rate_fpm", -200.0f);
    }

    // Hysteresis
    if (j.contains("hysteresis")) {
      auto &hy = j["hysteresis"];
      hysteresis_.ground_to_airborne_sec =
          hy.value("ground_to_airborne_sec", 0.5f);
      hysteresis_.airborne_to_landing_sec =
          hy.value("airborne_to_landing_sec", 0.3f);
      hysteresis_.auto_correction_delay_sec =
          hy.value("auto_correction_delay_sec", 3.0f);
    }

    // Intent preconditions
    preconditions_.clear();
    if (j.contains("intent_preconditions")) {
      for (auto &[key, val] : j["intent_preconditions"].items()) {
        IntentPrecondition pc;
        if (val.contains("allowed_phases")) {
          for (auto &p : val["allowed_phases"])
            pc.allowed_phases.push_back(phase_from_name(p.get<std::string>()));
        }
        pc.rejection_parked = val.value("rejection_parked", "");
        pc.rejection_airborne = val.value("rejection_airborne", "");
        pc.rejection_ground = val.value("rejection_ground", "");
        preconditions_[key] = std::move(pc);
      }
    }

    // Auto corrections
    auto_corrections_.clear();
    if (j.contains("auto_corrections")) {
      for (auto &[state, conditions] : j["auto_corrections"].items()) {
        std::map<std::string, AutoCorrection> state_corrections;
        for (auto &[cond_name, cond_val] : conditions.items()) {
          AutoCorrection ac;
          if (cond_val.contains("phases")) {
            for (auto &p : cond_val["phases"])
              ac.phases.push_back(phase_from_name(p.get<std::string>()));
          }
          ac.next_state = cond_val.value("next_state", "IDLE");
          ac.delay_sec = cond_val.value("delay_sec", 3.0f);
          state_corrections[cond_name] = std::move(ac);
        }
        auto_corrections_[state] = std::move(state_corrections);
      }
    }

    // Frequency-driven auto corrections
    frequency_auto_corrections_.clear();
    if (j.contains("frequency_auto_corrections")) {
      for (auto &[state, conditions] :
           j["frequency_auto_corrections"].items()) {
        std::map<std::string, FrequencyAutoCorrection> state_corrections;
        for (auto &[cond_name, cond_val] : conditions.items()) {
          FrequencyAutoCorrection fc;
          if (cond_val.contains("frequencies")) {
            for (auto &f : cond_val["frequencies"])
              fc.frequencies.push_back(
                  freq_type_from_name(f.get<std::string>()));
          }
          fc.next_state = cond_val.value("next_state", "IDLE");
          fc.log = cond_val.value("log", "");
          state_corrections[cond_name] = std::move(fc);
        }
        frequency_auto_corrections_[state] = std::move(state_corrections);
      }
    }

    // Intent frequency mapping — object form only: { allowed: [...],
    // rejection: "..." }. Array form is rejected to keep schema consistent
    // across regions.
    intent_frequency_.clear();
    if (j.contains("intent_frequency")) {
      for (auto &[key, val] : j["intent_frequency"].items()) {
        if (!val.is_object() || !val.contains("allowed") ||
            !val["allowed"].is_array()) {
          throw std::runtime_error(
              "intent_frequency." + key +
              " must be object with fields 'allowed' (array) and 'rejection'");
        }
        FrequencyRule rule;
        for (auto &f : val["allowed"])
          rule.allowed.push_back(f.get<std::string>());
        rule.rejection = val.value("rejection", "");
        intent_frequency_[key] = std::move(rule);
      }
    }

    // Pilot phraseology
    pilot_phraseology_.clear();
    if (j.contains("pilot_phraseology")) {
      for (auto &[key, val] : j["pilot_phraseology"].items()) {
        pilot_phraseology_[key] = val.get<std::string>();
      }
    }

    // State-vs-frequency validity (replaces hardcoded if/else in
    // atc_state_machine::process). Each freq_type maps to the set of ATC
    // state names that remain valid when the pilot is on that frequency.
    state_frequency_validity_.clear();
    if (j.contains("state_frequency_validity")) {
      for (auto &[freq_name, allowed_states] :
           j["state_frequency_validity"].items()) {
        auto ft = freq_type_from_name(freq_name);
        std::vector<std::string> states;
        if (allowed_states.is_array()) {
          for (auto &s : allowed_states)
            states.push_back(s.get<std::string>());
        }
        state_frequency_validity_[ft] = std::move(states);
      }
    }

    // IDLE-state intent redirects (REQUEST_TAXI on Tower freq → "contact
    // ground", etc.). First-match-wins; empty list disables the feature.
    idle_redirects_.clear();
    if (j.contains("idle_redirects") && j["idle_redirects"].is_array()) {
      for (auto &node : j["idle_redirects"]) {
        IdleRedirect r;
        if (node.contains("intent_in") && node["intent_in"].is_array()) {
          for (auto &k : node["intent_in"])
            r.intent_in.push_back(k.get<std::string>());
        }
        if (node.contains("freq_type"))
          r.freq_type = freq_type_from_name(node.value("freq_type", ""));
        r.unless_flag = node.value("unless_flag", "");
        r.response = node.value("response", "");
        r.log = node.value("log", "");
        idle_redirects_.push_back(std::move(r));
      }
    }

    // Pre-template state reverts (RE-CLEARANCE).
    state_reverts_.clear();
    if (j.contains("state_reverts") && j["state_reverts"].is_array()) {
      for (auto &node : j["state_reverts"]) {
        StateRevert r;
        if (node.contains("on_intent_in") && node["on_intent_in"].is_array()) {
          for (auto &k : node["on_intent_in"])
            r.on_intent_in.push_back(k.get<std::string>());
        }
        r.in_state = node.value("in_state", "");
        r.revert_to = node.value("revert_to", "");
        r.reset_departure_type = node.value("reset_departure_type", false);
        r.log = node.value("log", "");
        state_reverts_.push_back(std::move(r));
      }
    }

    // Tower-only auto-advance (e.g. TAXI_CLEARED → TOWER_CONTACT when no
    // ground controller exists at this airport).
    tower_only_auto_advance_.clear();
    if (j.contains("tower_only_auto_advance") &&
        j["tower_only_auto_advance"].is_object()) {
      for (auto &[from_state, to_state] :
           j["tower_only_auto_advance"].items()) {
        if (to_state.is_string())
          tower_only_auto_advance_[from_state] = to_state.get<std::string>();
      }
    }

    // Wrong-frequency hint at towered airports.
    frequency_hint_ = {};
    frequency_hint_set_ = false;
    if (j.contains("frequency_hint") && j["frequency_hint"].is_object()) {
      auto &fh = j["frequency_hint"];
      if (fh.contains("ground_intents") && fh["ground_intents"].is_array()) {
        for (auto &k : fh["ground_intents"])
          frequency_hint_.ground_intents.push_back(k.get<std::string>());
      }
      frequency_hint_.ground_response = fh.value("ground_response", "");
      frequency_hint_.tower_response = fh.value("tower_response", "");
      frequency_hint_set_ = true;
    }

    loaded_ = true;
    logging::info("Flight rules loaded");
  } catch (...) {
    logging::info("Warning: failed to parse flight_rules.json");
    loaded_ = false;
  }
}

// ── Raw phase detection (no hysteresis) ──────────────────────────

static FlightPhase detect_raw(const xplane_context::XPlaneContext &ctx) {
  // Ground phases — purely geometric (engine state is pilot's concern,
  // not ATC's, and is not tracked anywhere else in the plugin).
  if (ctx.on_ground) {
    if (ctx.groundspeed_kts >= thresholds_.roll_min_gs_kt) {
      // High speed on ground: takeoff or landing roll
      return was_airborne_ ? FlightPhase::LANDING_ROLL
                           : FlightPhase::TAKEOFF_ROLL;
    }

    if (ctx.groundspeed_kts >= thresholds_.taxi_min_gs_kt)
      return FlightPhase::TAXI;

    return FlightPhase::PARKED;
  }

  // Airborne phases
  double dist_nm = haversine_distance_nm(ctx.latitude, ctx.longitude,
                                         ctx.airport_lat, ctx.airport_lon);
  bool near_airport = (dist_nm < thresholds_.near_airport_nm);
  bool low_agl = (ctx.height_agl_ft < thresholds_.pattern_max_agl_ft);

  if (near_airport && low_agl) {
    // Check final approach: descending + runway-aligned
    float rwy_hdg = active_runway_heading(ctx);
    if (rwy_hdg >= 0.0f &&
        ctx.vertical_speed_fpm < thresholds_.final_descent_rate_fpm &&
        angle_diff(ctx.heading_true, rwy_hdg) <
            thresholds_.runway_aligned_deg) {
      return FlightPhase::FINAL_APPROACH;
    }
    return FlightPhase::PATTERN;
  }

  if (ctx.vertical_speed_fpm > thresholds_.climb_min_vs_fpm)
    return FlightPhase::CLIMB;

  return FlightPhase::CRUISE;
}

// ── Hysteresis logic ─────────────────────────────────────────────

static bool needs_hysteresis(FlightPhase from, FlightPhase to) {
  bool from_ground = is_on_ground(from);
  bool to_ground = is_on_ground(to);

  // Ground → airborne: needs delay
  if (from_ground && !to_ground)
    return true;

  // Airborne → landing roll: needs delay
  if (!from_ground && to == FlightPhase::LANDING_ROLL)
    return true;

  // Pattern ↔ final approach: prevent jitter from heading oscillation
  if ((from == FlightPhase::PATTERN && to == FlightPhase::FINAL_APPROACH) ||
      (from == FlightPhase::FINAL_APPROACH && to == FlightPhase::PATTERN))
    return true;

  return false;
}

static float hysteresis_duration(FlightPhase from, FlightPhase to) {
  if (is_on_ground(from) && !is_on_ground(to))
    return hysteresis_.ground_to_airborne_sec;

  if (!is_on_ground(from) && to == FlightPhase::LANDING_ROLL)
    return hysteresis_.airborne_to_landing_sec;

  // Pattern ↔ final approach: 3 second guard
  if ((from == FlightPhase::PATTERN && to == FlightPhase::FINAL_APPROACH) ||
      (from == FlightPhase::FINAL_APPROACH && to == FlightPhase::PATTERN))
    return 3.0f;

  return 0.0f;
}

// ── Public API ───────────────────────────────────────────────────

void init() { load_from_file(); }

void stop() {
  preconditions_.clear();
  auto_corrections_.clear();
  frequency_auto_corrections_.clear();
  intent_frequency_.clear();
  pilot_phraseology_.clear();
  state_frequency_validity_.clear();
  idle_redirects_.clear();
  state_reverts_.clear();
  tower_only_auto_advance_.clear();
  frequency_hint_ = {};
  frequency_hint_set_ = false;
  loaded_ = false;
  current_phase_ = FlightPhase::PARKED;
  candidate_phase_ = FlightPhase::PARKED;
  candidate_timer_ = 0.0f;
  was_airborne_ = false;
}

void reload() { load_from_file(); }

void reset() {
  current_phase_ = FlightPhase::PARKED;
  candidate_phase_ = FlightPhase::PARKED;
  candidate_timer_ = 0.0f;
  was_airborne_ = false;
}

FlightPhase get() { return current_phase_; }

void update(const xplane_context::XPlaneContext &ctx, float dt) {
  FlightPhase raw = detect_raw(ctx);

  // Drive the State-Machine's session-lifecycle was_airborne flag
  // every frame the aircraft is in an airborne phase. The setter is
  // idempotent (only bumps gen on actual value change), so this is
  // frame-spam safe AND survives a snapshot-revert that rolled the
  // flag back: the next airborne frame restores it. Runs BEFORE the
  // stable-phase early-return below so it ticks even when the phase
  // is steady.
  if (is_airborne(current_phase_))
    atc_state_machine::set_was_airborne(true);

  if (raw == current_phase_) {
    // Stable — reset candidate
    candidate_phase_ = raw;
    candidate_timer_ = 0.0f;
    return;
  }

  if (raw != candidate_phase_) {
    // New candidate — start timer
    candidate_phase_ = raw;
    candidate_timer_ = 0.0f;
  }

  if (needs_hysteresis(current_phase_, raw)) {
    candidate_timer_ += dt;
    float required = hysteresis_duration(current_phase_, raw);
    if (candidate_timer_ < required)
      return; // Not yet stable enough
  }

  // Transition
  FlightPhase old = current_phase_;
  current_phase_ = raw;

  // Track airborne state for takeoff/landing disambiguation
  if (is_airborne(current_phase_))
    was_airborne_ = true;
  else if (is_on_ground(current_phase_) &&
           current_phase_ != FlightPhase::TAKEOFF_ROLL &&
           current_phase_ != FlightPhase::LANDING_ROLL)
    was_airborne_ = false;

  // Mirror to the State-Machine's session-lifecycle was_airborne flag
  // on transition. The unconditional same-frame call near the top of
  // update() handles steady-state airborne frames; this transition-
  // path call ensures the flag fires the same frame the aircraft
  // first crosses into an airborne phase, not one frame late.
  if (is_airborne(current_phase_))
    atc_state_machine::set_was_airborne(true);

  logging::info("Flight phase: %s -> %s", phase_name(old),
                phase_name(current_phase_));
}

std::string check_precondition(const std::string &intent_key,
                               FlightPhase phase) {
  auto it = preconditions_.find(intent_key);
  if (it == preconditions_.end())
    return {}; // No precondition defined — always allowed

  const auto &pc = it->second;
  for (auto allowed : pc.allowed_phases) {
    if (phase == allowed)
      return {}; // Phase is allowed
  }

  // Phase not allowed — pick appropriate rejection message
  if (phase == FlightPhase::PARKED && !pc.rejection_parked.empty())
    return pc.rejection_parked;

  if (is_airborne(phase) && !pc.rejection_airborne.empty())
    return pc.rejection_airborne;

  if (is_on_ground(phase) && !pc.rejection_ground.empty())
    return pc.rejection_ground;

  // Generic fallback
  if (!pc.rejection_airborne.empty() && is_airborne(phase))
    return pc.rejection_airborne;
  if (!pc.rejection_ground.empty())
    return pc.rejection_ground;
  if (!pc.rejection_parked.empty())
    return pc.rejection_parked;

  return "{callsign}, unable.";
}

const std::map<std::string, AutoCorrection> *
get_auto_corrections(const std::string &atc_state) {
  auto it = auto_corrections_.find(atc_state);
  if (it == auto_corrections_.end())
    return nullptr;
  return &it->second;
}

const std::map<std::string, FrequencyAutoCorrection> *
get_frequency_auto_corrections(const std::string &atc_state) {
  auto it = frequency_auto_corrections_.find(atc_state);
  if (it == frequency_auto_corrections_.end())
    return nullptr;
  return &it->second;
}

static std::string freq_type_name(xplane_context::FrequencyType freq_type) {
  using FT = xplane_context::FrequencyType;
  switch (freq_type) {
  case FT::GROUND:
    return "GROUND";
  case FT::TOWER:
    return "TOWER";
  case FT::UNICOM:
    return "UNICOM";
  case FT::CTAF:
    return "CTAF";
  case FT::INFO:
    return "INFO";
  case FT::RADIO:
    return "RADIO";
  default:
    return {};
  }
}

static xplane_context::FrequencyType
freq_type_from_name(const std::string &name) {
  using FT = xplane_context::FrequencyType;
  if (name == "GROUND")
    return FT::GROUND;
  if (name == "TOWER")
    return FT::TOWER;
  if (name == "UNICOM")
    return FT::UNICOM;
  if (name == "CTAF")
    return FT::CTAF;
  if (name == "DELIVERY")
    return FT::DELIVERY;
  if (name == "ATIS")
    return FT::ATIS;
  if (name == "INFO")
    return FT::INFO;
  if (name == "RADIO")
    return FT::RADIO;
  return FT::UNKNOWN;
}

bool is_intent_valid_for_frequency(const std::string &intent_key,
                                   xplane_context::FrequencyType freq_type) {
  auto it = intent_frequency_.find(intent_key);
  if (it == intent_frequency_.end())
    return true; // No restriction defined — allow

  std::string freq_str = freq_type_name(freq_type);
  if (freq_str.empty())
    return false; // ATIS, UNKNOWN, etc. — no intents valid

  for (const auto &f : it->second.allowed) {
    if (f == freq_str)
      return true;
  }
  return false;
}

std::string
check_frequency_precondition(const std::string &intent_key,
                             xplane_context::FrequencyType freq_type) {
  auto it = intent_frequency_.find(intent_key);
  if (it == intent_frequency_.end())
    return {}; // No restriction defined — always allowed

  std::string freq_str = freq_type_name(freq_type);
  for (const auto &f : it->second.allowed) {
    if (!freq_str.empty() && f == freq_str)
      return {}; // explicitly permitted
  }

  // Not allowed — return configured rejection (may be empty if unconfigured).
  return it->second.rejection;
}

std::string get_pilot_phraseology(const std::string &intent_key) {
  auto it = pilot_phraseology_.find(intent_key);
  if (it == pilot_phraseology_.end())
    return {};
  return it->second;
}

const std::vector<std::string> *
get_state_frequency_validity(xplane_context::FrequencyType freq_type) {
  auto it = state_frequency_validity_.find(freq_type);
  if (it == state_frequency_validity_.end())
    return nullptr;
  return &it->second;
}

const std::vector<IdleRedirect> &get_idle_redirects() {
  return idle_redirects_;
}

const std::vector<StateRevert> &get_state_reverts() { return state_reverts_; }

std::string get_tower_only_auto_advance(const std::string &state) {
  auto it = tower_only_auto_advance_.find(state);
  if (it == tower_only_auto_advance_.end())
    return {};
  return it->second;
}

const FrequencyHint *get_frequency_hint() {
  return frequency_hint_set_ ? &frequency_hint_ : nullptr;
}

} // namespace flight_phase
