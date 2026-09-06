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

#include "atc/flows/ground_operations.hpp"

#include "atc/atc_templates.hpp"
#include "atc/atis_generator.hpp"
#include "atc/de_phraseology.hpp"
#include "atc/en_phraseology.hpp"
#include "atc/flight_phase.hpp"
#include "atc/flows/state_storage.hpp"
#include "atc/initial_call_conformance.hpp"
#include "core/logging.hpp"
#include "data/airport_vrps.hpp"
#include "persistence/settings.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace ground_ops {

using atc_state_machine::ATCState;
using atc_state_machine::state_from_name;
using atc_state_machine::state_name;
namespace internal = atc_state_machine::internal;

// ── Helpers (formerly file-local statics in atc_state_machine.cpp) ──

static std::string abbreviate_callsign(const std::string &cs) {
  std::vector<std::string> words;
  std::string word;
  for (char c : cs) {
    if (c == ' ') {
      if (!word.empty())
        words.push_back(word);
      word.clear();
    } else {
      word += c;
    }
  }
  if (!word.empty())
    words.push_back(word);

  if (words.size() <= 3)
    return cs;

  std::string result;
  for (size_t i = words.size() - 3; i < words.size(); ++i) {
    if (!result.empty())
      result += " ";
    result += words[i];
  }
  return result;
}

static std::string get_callsign(const PilotMessage &msg) {
  // Session-locked callsign wins over per-utterance parsing once the
  // dialog has left IDLE — keeps the tower addressing the pilot by
  // the established name even when a later transmission is garbled
  // and the parser pulls a fragment like "Delta" out of the noise.
  const std::string &session_cs = internal::session_callsign_ref();
  std::string cs;
  if (!session_cs.empty())
    cs = session_cs;
  else if (!msg.callsign.empty())
    cs = msg.callsign;
  else
    cs = settings::pilot_callsign();
  if (internal::get_state_ref() != ATCState::IDLE)
    cs = abbreviate_callsign(cs);
  return cs;
}

static std::string get_runway(const PilotMessage &msg,
                              const XPlaneContext &ctx) {
  if (!msg.runway.empty())
    return msg.runway;
  const std::string &assigned = internal::assigned_runway_ref();
  if (!assigned.empty())
    return assigned;
  if (!ctx.active_runway.empty())
    return ctx.active_runway;
  return "28";
}

static std::string format_qnh(float inhg) {
  int hpa = static_cast<int>(std::round(inhg * 33.8639f));
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", hpa);
  return buf;
}

static std::string format_altimeter(float inhg) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2f", inhg);
  return buf;
}

static std::string format_wind(float dir, float spd) {
  bool de = (settings::atc_profile() == "DE");
  if (spd < 3.0f)
    return de ? "ruhig" : "calm";
  char buf[64];
  std::snprintf(buf, sizeof(buf),
                de ? "%03.0f Grad %02.0f Knoten" : "%03.0f degrees %02.0f knots",
                dir, spd);
  return buf;
}

static std::string airport_name(const XPlaneContext &ctx) {
  if (!ctx.nearest_airport_name.empty())
    return ctx.nearest_airport_name;
  if (!ctx.nearest_airport_id.empty())
    return ctx.nearest_airport_id;
  return "Airport";
}

// Spoken facility call-name for the {station} template variable. German
// uncontrolled fields carry their facility suffix in the call: "<Platz>
// Information" (AFIS) / "<Platz> Radio" (Funkstelle mit Flugleiter). Other
// frequency types fall back to the bare airport name.
static std::string facility_callname(const XPlaneContext &ctx) {
  using FT = xplane_context::FrequencyType;
  switch (ctx.frequency_type) {
  case FT::INFO:
    return airport_name(ctx) + " Information";
  case FT::RADIO:
    return airport_name(ctx) + " Radio";
  case FT::TOWER:
    return airport_name(ctx) + " Tower";
  default:
    return airport_name(ctx);
  }
}

static std::string extract_position(const PilotMessage &msg,
                                    const XPlaneContext &ctx) {
  std::string rwy = get_runway(msg, ctx);
  std::string apt = airport_name(ctx);

  if (ctx.on_ground)
    return "on the ground at " + apt;

  std::string lower = msg.raw_transcript;
  for (auto &c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  if (lower.find("downwind") != std::string::npos)
    return "downwind runway " + rwy;
  if (lower.find("base") != std::string::npos)
    return "base runway " + rwy;
  if (lower.find("final") != std::string::npos)
    return "final runway " + rwy;
  if (lower.find("crosswind") != std::string::npos)
    return "crosswind runway " + rwy;
  if (lower.find("upwind") != std::string::npos)
    return "upwind runway " + rwy;
  return "in the pattern at " + apt;
}

// ── build_vars: template variable map ───────────────────────────────

std::map<std::string, std::string> build_vars(const PilotMessage &msg,
                                              const XPlaneContext &ctx) {
  static const char *letter_names[] = {
      "Alpha",  "Bravo",   "Charlie", "Delta",  "Echo",   "Foxtrot", "Golf",
      "Hotel",  "India",   "Juliet",  "Kilo",   "Lima",   "Mike",    "November",
      "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra", "Tango",   "Uniform",
      "Victor", "Whiskey", "X-ray",   "Yankee", "Zulu"};
  char letter = atis_generator::current_letter();
  std::string atis_letter_name = letter_names[letter - 'A'];

  using FT = xplane_context::FrequencyType;
  float ground_freq = ctx.airport_freqs.first_mhz(FT::GROUND);
  if (ground_freq < 1.0f && ctx.tower_only)
    ground_freq = ctx.airport_freqs.first_mhz(FT::TOWER);
  float tower_freq = ctx.airport_freqs.first_mhz(FT::TOWER);

  auto format_freq = [](float mhz) -> std::string {
    if (mhz < 100.0f)
      return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.3f", mhz);
    return buf;
  };

  const bool region_de = (settings::atc_profile() == "DE");
  std::string position_remark;
  if (!msg.has_position &&
      (msg.intent == intent_parser::PilotIntent::REQUEST_TAXI ||
       msg.intent == intent_parser::PilotIntent::INITIAL_CALL_GROUND)) {
    // BZF: "Sagen Sie Ihre Position" is the canonical Tower prompt
    // when the pilot omitted the position element on first call.
    // The shorter "Position melden." was read as a complaint, not
    // an instruction — see strict_mode_test_edny.md user feedback.
    position_remark = region_de ? "Sagen Sie Ihre Position. " : "say position. ";
  }

  std::string taxi_controller;
  if (ctx.tower_only)
    taxi_controller = region_de ? "Turm" : "Tower";
  else
    taxi_controller = region_de ? "Rollkontrolle" : "Ground";

  std::string tower_handoff_phrase;
  if (ctx.frequency_type != FT::TOWER && tower_freq >= 100.0f) {
    tower_handoff_phrase = region_de
                               ? ", wechseln Sie auf Tower " + format_freq(tower_freq)
                               : ", contact Tower on " + format_freq(tower_freq);
  }

  // Aircraft type for the VFR initial-call hint. NfL 2024 Anlage 1.4.4
  // lists "Luftfahrzeugmuster" as a typical (not strictly mandatory)
  // element of the first contact, so render it as an optional ", DV20"
  // fragment: empty when X-Plane's acf_ICAO DataRef hasn't been
  // populated yet (cold-start race, payware liveries with empty ICAO),
  // a leading comma + space when set so the template stays readable.
  std::string aircraft_type_phrase;
  if (!ctx.aircraft_icao.empty())
    aircraft_type_phrase = ", " + ctx.aircraft_icao;

  // VFR intention element (NfL 2024 1.4.7 a/b). The sim has no flight plan,
  // so the pilot's intent is a user setting: Platzrunde vs. Ueberlandflug.
  // {intention} carries the full spoken first-call/taxi phrase.
  // {vfr_course_phrase} is the optional ", Kurs nach <dest>" tail for the
  // VFR departure call: a leading-comma fragment only when a cross-country
  // destination is set, empty otherwise — so the empty case reads as the
  // speakable "... abflugbereit" with NO "nach Plan" placeholder.
  // Three-tier precedence (NfL 2024 1.4.7 — the destination is stated once
  // on the taxi/initial call, then the tower remembers it):
  //   1. msg.destination  — extracted from THIS utterance (most specific)
  //   2. session intent   — latched by an earlier transmission this flight,
  //                          so a destination-less READY_FOR_DEPARTURE keeps
  //                          the cross-country intention (the bug this fixes)
  //   3. settings::vfr_*   — the pre-flight Flugvorbereitung field (fallback)
  // The ICAO is expanded phonetically so TTS speaks it letter-by-letter
  // ("EDMA" -> "Echo Delta Mike Alfa") instead of as a word.
  std::string vfr_dest;
  bool cross_country;
  if (!msg.destination.empty()) {
    vfr_dest = msg.destination;
    cross_country = true;
  } else if (atc_state_machine::flight_intent_declared()) {
    cross_country = atc_state_machine::flight_intent_cross_country();
    vfr_dest = atc_state_machine::flight_intent_destination();
  } else {
    cross_country = (settings::vfr_flight_type() == "cross_country");
    vfr_dest = settings::vfr_destination();
  }
  const std::string dest_spoken =
      vfr_dest.empty()
          ? ""
          : (settings::atc_profile() == "EN"
                 ? en_phraseology::expand_callsign_phonetic(vfr_dest)
                 : de_phraseology::expand_callsign_phonetic(vfr_dest));
  // Connective words are language-specific: the DE profile speaks NfL
  // "VFR Platzrunde / VFR nach <dest>", the EN profile ICAO "VFR pattern /
  // VFR to <dest>". dest_spoken is already phonetically expanded per
  // profile above. Keeping this region-aware ensures the EN initial-call /
  // taxi hint reads in English (H2 invariant: the rendered hint must carry
  // an {intention} word listed in en/conformance.json element_keywords).
  std::string intention;
  if (region_de) {
    if (cross_country)
      intention = dest_spoken.empty() ? "VFR Ueberlandflug"
                                      : "VFR nach " + dest_spoken;
    else
      intention = "VFR Platzrunde";
  } else {
    if (cross_country)
      intention =
          dest_spoken.empty() ? "VFR cross country" : "VFR to " + dest_spoken;
    else
      intention = "VFR pattern";
  }
  std::string vfr_course_phrase;
  if (cross_country && !dest_spoken.empty())
    vfr_course_phrase =
        region_de ? ", Kurs nach " + dest_spoken : ", course to " + dest_spoken;

  return {
      {"callsign", get_callsign(msg)},
      {"airport", airport_name(ctx)},
      {"station", facility_callname(ctx)},
      {"runway", get_runway(msg, ctx)},
      {"wind", format_wind(ctx.wind_direction_deg, ctx.wind_speed_kt)},
      {"qnh", format_qnh(ctx.qnh_inhg)},
      {"altimeter", format_altimeter(ctx.qnh_inhg)},
      {"atis_letter", atis_letter_name},
      {"frequency", format_freq(ground_freq)},
      {"tower_frequency", format_freq(tower_freq)},
      {"ground_frequency", format_freq(ground_freq)},
      {"taxi_controller", taxi_controller},
      {"intention", intention},
      {"vfr_course_phrase", vfr_course_phrase},
      {"aircraft_type", ctx.aircraft_icao},
      {"aircraft_type_phrase", aircraft_type_phrase},
      {"position", extract_position(msg, ctx)},
      {"pattern_direction",
       [&]() {
         std::string dir = airport_vrps::get_pattern_direction(
             ctx.nearest_airport_id, ctx.active_runway);
         return dir.empty() ? settings::pattern_direction() : dir;
       }()},
      {"entry_vrp", msg.vrp_name},
      {"position_remark", position_remark},
      {"tower_handoff_phrase", tower_handoff_phrase},
      // Traffic-advisory placeholders. Empty for normal pilot-driven
      // intents — populated by render_traffic_advisory() and
      // traffic_dialog before template fill(). {side} is Phase-3's
      // ground-conflict left/right token.
      {"clock", ""},
      {"distance", ""},
      {"direction", ""},
      {"altitude_info", ""},
      {"type", ""},
      {"side", ""},
      // Phase-4 landing-sequence placeholders. Default to empty —
      // pattern_flow::apply_landing_sequence() overwrites them when
      // sequencing actually applies (user_position >= 2 or runway
      // occupied). {seq_position} carries the leg label of the
      // aircraft directly ahead of the user ("left base" / "right
      // downwind" / ...). Namespaced to "seq_" to avoid collision
      // with {position} which carries the user-side pattern leg.
      {"seq_number", ""},
      {"seq_type", ""},
      {"seq_position", ""},
  };
}

// ── Pipeline guards ─────────────────────────────────────────────────

// Maps a cleared/active state to the state where the pilot can
// re-issue the corresponding request. Used by NEGATIVE_CORRECTION.
static ATCState revert_target(ATCState s) {
  switch (s) {
  case ATCState::DEPARTURE_CLEARED:
    return ATCState::TOWER_CONTACT;
  case ATCState::TAXI_CLEARED:
    return ATCState::GROUND_CONTACT;
  case ATCState::LANDING_CLEARED:
  case ATCState::TOUCH_AND_GO_CLEARED:
    return ATCState::PATTERN_ENTRY;
  case ATCState::PATTERN_ENTRY:
    return ATCState::TOWER_CONTACT;
  case ATCState::GROUND_CONTACT:
  case ATCState::TOWER_CONTACT:
    return ATCState::IDLE;
  default:
    return s;
  }
}

bool handle_negative_correction(const PilotMessage &msg,
                                const XPlaneContext &ctx, ATCResponse &resp) {
  if (msg.intent != intent_parser::PilotIntent::NEGATIVE_CORRECTION)
    return false;
  auto vars = build_vars(msg, ctx);
  ATCState prev = internal::get_state_ref();
  ATCState target = revert_target(prev);
  if (target != prev) {
    internal::transition_to(target, "negative_correction");
    if (prev == ATCState::DEPARTURE_CLEARED)
      internal::set_departure_type(internal::DepartureType::PATTERN);
    resp.text = atc_templates::fill(
        "{callsign}, roger, correction noted, say intentions.", vars);
  } else {
    resp.text = atc_templates::fill("{callsign}, roger, say intentions.", vars);
    logging::info("Correction in neutral state, ack only");
  }
  resp.next_state = internal::get_state_ref();
  return true;
}

void apply_state_reverts(const PilotMessage &msg) {
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  std::string current_state = state_name(internal::get_state_ref());
  for (const auto &r : flight_phase::get_state_reverts()) {
    if (r.in_state != current_state)
      continue;
    bool intent_matches = false;
    for (const auto &k : r.on_intent_in)
      if (k == intent_key) {
        intent_matches = true;
        break;
      }
    if (!intent_matches)
      continue;
    if (!r.log.empty())
      logging::info("%s", r.log.c_str());
    internal::transition_to(state_from_name(r.revert_to), "state_revert");
    if (r.reset_departure_type)
      internal::set_departure_type(internal::DepartureType::PATTERN);
    return;
  }
}

void apply_tower_only_initial_collapse(PilotMessage &msg,
                                       const XPlaneContext &ctx) {
  if (settings::atc_profile() != "DE")
    return;
  // Only the genuine apron first contact: tower-only field, on the ground,
  // dialog still in IDLE (no prior taxi clearance), classified as a Tower
  // initial call. The holding-point "abflugbereit" Tower call has already
  // advanced past IDLE; an airborne inbound is not on_ground / not TOWER.
  if (!ctx.tower_only || !ctx.on_ground)
    return;
  if (internal::get_state_ref() != ATCState::IDLE)
    return;
  // IDLE is overloaded: fresh-spawn-IDLE (never flew) vs post-landing-IDLE
  // (flew, landed, back on the ground). The apron-first-contact collapse is
  // only correct for the former. was_airborne distinguishes them and still
  // reads true here (the line-~905 reset runs later in process()), so a
  // post-landing sign-off misclassified as INITIAL_CALL_TOWER is NOT turned
  // into a new ground first contact.
  if (atc_state_machine::was_airborne())
    return;
  if (msg.intent != intent_parser::PilotIntent::INITIAL_CALL_TOWER)
    return;
  msg.intent = intent_parser::PilotIntent::INITIAL_CALL_GROUND;
  logging::info("Tower-only collapse: INITIAL_CALL_TOWER -> "
                "INITIAL_CALL_GROUND (apron first contact)");
}

// Advisory flow for uncontrolled fields with an AFIS/Info facility. apt.dat
// encodes these under the Tower code; the parser reclassifies the name suffix
// to FrequencyType::INFO (see classify_by_name). Info gives traffic
// information only — no clearances, no readback. NfL 2024 §34 b) / §35.
// Must run BEFORE handle_unicom_flow() so INFO does not fall through the
// generic "!is_towered()" branch there.
bool handle_info_flow(const PilotMessage &msg, const XPlaneContext &ctx,
                      ATCResponse &resp) {
  using FT = xplane_context::FrequencyType;
  if (ctx.frequency_type != FT::INFO)
    return false;
  auto vars = build_vars(msg, ctx);
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  // Synthetic "INFO" state key resolves the uncontrolled/INFO template block;
  // no atc_templates::lookup() signature change needed.
  auto tmpl = atc_templates::lookup(false, "INFO", intent_key);
  resp.text = atc_templates::fill(tmpl.response_template, vars);
  // Honor the template's next_state so advisory transit phrases can move the
  // dialog (RMZ_LEAVE -> EN_ROUTE). Existing INFO templates all carry "IDLE",
  // so this is byte-identical for every pre-RMZ intent. Empty/unknown -> IDLE.
  resp.next_state = atc_state_machine::state_from_name(tmpl.next_state);
  internal::transition_to(resp.next_state, "info_flow");
  return true;
}

bool handle_unicom_flow(const PilotMessage &msg, const XPlaneContext &ctx,
                        ATCResponse &resp) {
  using FT = xplane_context::FrequencyType;
  bool unicom_flow = ctx.frequency_type == FT::UNICOM ||
                     ctx.frequency_type == FT::CTAF ||
                     ctx.frequency_type == FT::RADIO ||
                     (!ctx.is_towered() && ctx.frequency_type != FT::INFO);
  if (!unicom_flow)
    return false;
  auto vars = build_vars(msg, ctx);
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  // RADIO (Funkstelle mit Flugleiter) gets its own template block so it can
  // address the pilot as "<Platz> Radio"; UNICOM/CTAF stay on the generic
  // uncontrolled/IDLE block (byte-identical to before).
  const char *state_key =
      (ctx.frequency_type == FT::RADIO) ? "RADIO" : "IDLE";
  auto tmpl = atc_templates::lookup(false, state_key, intent_key);
  resp.text = atc_templates::fill(tmpl.response_template, vars);
  resp.next_state = ATCState::IDLE;
  internal::transition_to(ATCState::IDLE, "unicom_flow_idle");
  return true;
}

bool handle_frequency_hint(const PilotMessage &msg, const XPlaneContext &ctx,
                           ATCResponse &resp) {
  using FT = xplane_context::FrequencyType;
  if (ctx.frequency_type != FT::UNKNOWN || !ctx.is_towered())
    return false;
  if (msg.intent == intent_parser::PilotIntent::READBACK)
    return false;
  const flight_phase::FrequencyHint *fh = flight_phase::get_frequency_hint();
  if (!fh)
    return false;
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  bool needs_ground = false;
  for (const auto &k : fh->ground_intents)
    if (k == intent_key) {
      needs_ground = true;
      break;
    }
  const std::string &tmpl = (needs_ground && !ctx.tower_only)
                                ? fh->ground_response
                                : fh->tower_response;
  if (tmpl.empty())
    return false;
  auto vars = build_vars(msg, ctx);
  resp.text = atc_templates::fill(tmpl, vars);
  resp.next_state = internal::get_state_ref();
  logging::info("ATC: wrong frequency, hint given");
  return true;
}

void apply_state_frequency_validity(const XPlaneContext &ctx) {
  using FT = xplane_context::FrequencyType;
  ATCState cur = internal::get_state_ref();
  bool needs_freq_validation =
      ctx.frequency_type != FT::UNKNOWN &&
      !(ctx.tower_only && ctx.frequency_type == FT::TOWER) &&
      cur != ATCState::EN_ROUTE;
  if (!needs_freq_validation)
    return;
  const auto *allowed =
      flight_phase::get_state_frequency_validity(ctx.frequency_type);
  if (!allowed)
    return;
  std::string current = state_name(cur);
  for (const auto &s : *allowed)
    if (s == current)
      return;
  internal::transition_to(ATCState::IDLE, "freq_validity_reset");
}

void apply_frequency_auto_corrections(const XPlaneContext &ctx) {
  using FT = xplane_context::FrequencyType;
  // Tower-only fields have no Ground frequency: the pilot is on TOWER from the
  // first contact, so "on TOWER while in GROUND_CONTACT" is the normal state,
  // not a tuned-ahead signal. Frequency-based advancement must stay inert here;
  // the GROUND_CONTACT -> ... -> TAXI_CLEARED -> TOWER_CONTACT path is driven by
  // REQUEST_TAXI + tower_only_auto_advance instead (NfL 1.4.7 a): ERBITTE ROLLEN
  // is mandatory before the taxi clearance). Mirrors the carve-out in
  // apply_state_frequency_validity().
  if (ctx.tower_only && ctx.frequency_type == FT::TOWER)
    return;
  ATCState cur = internal::get_state_ref();
  std::string current_state = state_name(cur);
  auto *fc = flight_phase::get_frequency_auto_corrections(current_state);
  if (!fc)
    return;
  for (const auto &[cond_name, rule] : *fc) {
    bool match = false;
    for (auto ft : rule.frequencies)
      if (ctx.frequency_type == ft) {
        match = true;
        break;
      }
    if (!match)
      continue;
    ATCState target = state_from_name(rule.next_state);
    if (target != cur) {
      std::string reason = "freq_auto_correction:";
      reason += rule.log.empty() ? cond_name : rule.log;
      internal::transition_to(target, reason.c_str());
    }
    return;
  }
}

bool handle_idle_redirects(const PilotMessage &msg, const XPlaneContext &ctx,
                           ATCResponse &resp) {
  if (internal::get_state_ref() != ATCState::IDLE)
    return false;
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  for (const auto &r : flight_phase::get_idle_redirects()) {
    if (r.freq_type != ctx.frequency_type)
      continue;
    if (r.unless_flag == "tower_only" && ctx.tower_only)
      continue;
    bool intent_matches = false;
    for (const auto &k : r.intent_in)
      if (k == intent_key) {
        intent_matches = true;
        break;
      }
    if (!intent_matches)
      continue;
    auto vars = build_vars(msg, ctx);
    resp.text = atc_templates::fill(r.response, vars);
    resp.next_state = ATCState::IDLE;
    internal::transition_to(ATCState::IDLE, "idle_redirect");
    if (!r.log.empty())
      logging::info("%s", r.log.c_str());
    return true;
  }
  return false;
}

bool check_phase_precondition(const PilotMessage &msg, const XPlaneContext &ctx,
                              ATCResponse &resp) {
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  auto phase = flight_phase::get();
  std::string rejection = flight_phase::check_precondition(intent_key, phase);
  if (rejection.empty())
    return false;
  auto vars = build_vars(msg, ctx);
  resp.text = atc_templates::fill(rejection, vars);
  resp.next_state = internal::get_state_ref();
  logging::info("Phase guard: %s blocked in phase %s", intent_key.c_str(),
                flight_phase::phase_name(phase));
  return true;
}

bool check_freq_precondition(const PilotMessage &msg, const XPlaneContext &ctx,
                             ATCResponse &resp) {
  using FT = xplane_context::FrequencyType;
  if (ctx.tower_only && ctx.frequency_type == FT::TOWER)
    return false;
  std::string intent_key = intent_parser::intent_template_key(msg.intent);
  std::string rejection = flight_phase::check_frequency_precondition(
      intent_key, ctx.frequency_type);
  if (rejection.empty())
    return false;
  auto vars = build_vars(msg, ctx);
  resp.text = atc_templates::fill(rejection, vars);
  resp.next_state = internal::get_state_ref();
  logging::info("Frequency guard: %s blocked on freq_type %d",
                intent_key.c_str(), static_cast<int>(ctx.frequency_type));
  return true;
}

static std::string join_csv(const std::vector<std::string> &items) {
  std::string out;
  for (const auto &i : items) {
    if (!out.empty())
      out += ",";
    out += i;
  }
  return out;
}

bool apply_initial_call_conformance(const PilotMessage &msg,
                                    const XPlaneContext &ctx,
                                    ATCResponse &resp) {
  if (msg.intent != intent_parser::PilotIntent::INITIAL_CALL_GROUND)
    return false;
  if (settings::atc_profile() != "DE")
    return false;

  const std::string intent_key = "INITIAL_CALL_GROUND";
  auto r = initial_call_conformance::evaluate(intent_key, msg, ctx);

  // Debrief log: record missing elements regardless of mode (for later
  // training review). ASCII only — Log.txt and the in-sim font reject
  // non-ASCII.
  if (!r.missing_required.empty() || !r.missing_recommended.empty())
    logging::info(
        "BZF conformance: INITIAL_CALL_GROUND missing required=[%s] "
        "recommended=[%s]",
        join_csv(r.missing_required).c_str(),
        join_csv(r.missing_recommended).c_str());

  // Enforce set: strict=false -> only `required` (empty for this intent,
  // so the guard never fires -> no behaviour change vs. today); strict=true
  // -> required + recommended.
  std::vector<std::string> enforce = r.missing_required;
  if (settings::bzf_strict_mode())
    enforce.insert(enforce.end(), r.missing_recommended.begin(),
                   r.missing_recommended.end());
  if (enforce.empty())
    return false;

  auto vars = build_vars(msg, ctx);
  resp.text = initial_call_conformance::build_request_prompt(
      intent_key, vars["callsign"], enforce);
  resp.next_state = internal::get_state_ref(); // hold current state (IDLE)
  resp.requires_readback = false;
  logging::info("BZF strict: INITIAL_CALL_GROUND re-request for [%s]",
                join_csv(enforce).c_str());
  return true;
}

} // namespace ground_ops
