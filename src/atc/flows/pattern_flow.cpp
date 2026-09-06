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

#include "atc/flows/pattern_flow.hpp"

#include "atc/atc_templates.hpp"
#include "atc/landing_sequence.hpp"
#include "core/logging.hpp"
#include "data/airport_vrps.hpp"

#include <optional>

namespace pattern_flow {

using atc_state_machine::ATCState;

const char *state_name(State s) {
  switch (s) {
  case State::DEPARTURE_CLEARED:
    return "Pattern/DEPARTURE_CLEARED";
  case State::PATTERN_ENTRY:
    return "Pattern/PATTERN_ENTRY";
  case State::LANDING_CLEARED:
    return "Pattern/LANDING_CLEARED";
  case State::TOUCH_AND_GO_CLEARED:
    return "Pattern/TOUCH_AND_GO_CLEARED";
  }
  return "Pattern/UNKNOWN";
}

State from_atc_state(ATCState s) {
  switch (s) {
  case ATCState::DEPARTURE_CLEARED:
    return State::DEPARTURE_CLEARED;
  case ATCState::PATTERN_ENTRY:
    return State::PATTERN_ENTRY;
  case ATCState::LANDING_CLEARED:
    return State::LANDING_CLEARED;
  case ATCState::TOUCH_AND_GO_CLEARED:
    return State::TOUCH_AND_GO_CLEARED;
  default:
    // Caller must gate with is_pattern_state(); fallback returns
    // DEPARTURE_CLEARED to keep the contract total.
    return State::DEPARTURE_CLEARED;
  }
}

ATCState to_atc_state(State s) {
  switch (s) {
  case State::DEPARTURE_CLEARED:
    return ATCState::DEPARTURE_CLEARED;
  case State::PATTERN_ENTRY:
    return ATCState::PATTERN_ENTRY;
  case State::LANDING_CLEARED:
    return ATCState::LANDING_CLEARED;
  case State::TOUCH_AND_GO_CLEARED:
    return ATCState::TOUCH_AND_GO_CLEARED;
  }
  return ATCState::DEPARTURE_CLEARED;
}

bool is_pattern_state(ATCState s) {
  switch (s) {
  case ATCState::DEPARTURE_CLEARED:
  case ATCState::PATTERN_ENTRY:
  case ATCState::LANDING_CLEARED:
  case ATCState::TOUCH_AND_GO_CLEARED:
    return true;
  default:
    return false;
  }
}

namespace {

// Resolve the active landing runway from XPlaneContext. Picks the
// runway end whose designator matches `ctx.active_runway`. Returns
// nullopt when the runway cache is empty or the active designator
// doesn't match any cached end — sequencing then no-ops.
std::optional<landing_sequence::ActiveRunway>
resolve_active_runway(const xplane_context::XPlaneContext &ctx) {
  if (ctx.active_runway.empty() || ctx.runways.empty())
    return std::nullopt;
  for (const auto &rw : ctx.runways) {
    const xplane_context::RunwayEnd *end = nullptr;
    double heading = 0.0;
    if (rw.end1.number == ctx.active_runway) {
      end = &rw.end1;
      heading = static_cast<double>(rw.end1.heading_deg);
    } else if (rw.end2.number == ctx.active_runway) {
      end = &rw.end2;
      heading = static_cast<double>(rw.end2.heading_deg);
    }
    if (!end)
      continue;
    landing_sequence::ActiveRunway out;
    out.threshold_lat = end->lat;
    out.threshold_lon = end->lon;
    out.heading_deg = heading;
    out.length_m = static_cast<double>(rw.length_m);
    if (out.length_m < 500.0)
      out.length_m = 2500.0; // sensible default if length missing
    out.designator = end->number;
    return out;
  }
  return std::nullopt;
}

// Template-engine helper. Look up an entry under Pattern/LANDING_CLEARED
// by key, fill it with `vars`, and write the result back into `resp`.
// next_state stays whatever the main pipeline already picked — these
// overlay templates always sit on Pattern/LANDING_CLEARED.
void render_overlay(const std::string &template_key,
                    const std::map<std::string, std::string> &vars,
                    atc_state_machine::ATCResponse &resp) {
  auto tmpl =
      atc_templates::lookup(true, "Pattern/LANDING_CLEARED", template_key);
  if (tmpl.response_template.empty())
    return;
  resp.text = atc_templates::fill(tmpl.response_template, vars);
}

} // namespace

void apply_landing_sequence(const intent_parser::PilotMessage &msg,
                            const xplane_context::XPlaneContext &ctx,
                            const traffic_context::TrafficContext &traffic,
                            std::map<std::string, std::string> &vars,
                            atc_state_machine::ATCResponse &resp) {
  (void)msg;
  // Only when the regular pipeline produced a landing clearance. We do
  // not overlay other Pattern responses (REPORT_POSITION_BASE etc.) —
  // the Tower replies "continue approach" there already, and sequencing
  // belongs to the moment the clearance itself is granted.
  if (resp.next_state != ATCState::LANDING_CLEARED)
    return;

  auto rwy_opt = resolve_active_runway(ctx);
  if (!rwy_opt.has_value())
    return;

  landing_sequence::UserPosition user{ctx.latitude, ctx.longitude};
  auto seq =
      landing_sequence::compute_landing_sequence(traffic, user, *rwy_opt);

  // Priority 1: runway-occupied beats sequencing. The pilot must not
  // be cleared to land into an occupied runway — even number-1 traffic
  // gets "continue approach" instead of "cleared to land".
  if (seq.runway_occupied) {
    render_overlay("continue_approach_traffic_runway", vars, resp);
    // Stay in Pattern/LANDING_CLEARED so the next pilot call (a
    // readback or a position report on short final) still routes
    // through the landing-cleared dialog. The clearance hasn't been
    // *withdrawn*, the controller is simply telling the pilot to keep
    // going while monitoring the conflict. Definitive go-around lives
    // on the unsolicited Tower-call side-channel.
    resp.next_state = ATCState::LANDING_CLEARED;
    logging::info("Landing sequence: runway occupied — continue approach");
    return;
  }

  // Priority 2: number_to_land_follow when there is traffic ahead.
  if (seq.user_position >= 2 && seq.follow_target.has_value()) {
    const auto &follow = *seq.follow_target;
    const std::string pattern_dir = airport_vrps::get_pattern_direction(
        ctx.nearest_airport_id, ctx.active_runway);
    vars["seq_number"] = std::to_string(seq.user_position);
    vars["seq_type"] = follow.icao_type.empty()
                           ? std::string{"traffic"}
                           : follow.icao_type;
    vars["seq_position"] =
        landing_sequence::leg_label(follow, *rwy_opt, pattern_dir);
    render_overlay("number_to_land_follow", vars, resp);
    logging::info("Landing sequence: user is number %d, follow %s on %s",
                  seq.user_position, vars["seq_type"].c_str(),
                  vars["seq_position"].c_str());
    return;
  }

  // No overlay — user is first to land, runway clear. The default
  // "cleared to land runway X" stays.
}

} // namespace pattern_flow
