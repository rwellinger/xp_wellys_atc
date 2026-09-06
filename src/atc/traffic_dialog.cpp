/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "atc/traffic_dialog.hpp"

#include "atc/atc_templates.hpp"
#include "atc/flows/ground_operations.hpp"
#include "core/logging.hpp"
#include "data/traffic_context.hpp"

#include <cstdio>
#include <map>
#include <string>

namespace traffic_dialog {

namespace {

// Single template block in atc_templates.json. The block is consulted
// only by traffic_dialog — the ATC main flow never sees it.
constexpr const char *kTemplateState = "TRAFFIC_DIALOG";

State state_ = State::IDLE;
uint32_t pending_target_id_ = 0;

const traffic_context::TrafficTarget *
find_target(const traffic_context::TrafficContext &snap, uint32_t modeS_id) {
  for (const auto &t : snap.targets) {
    if (t.modeS_id == modeS_id)
      return &t;
  }
  return nullptr;
}

void inject_geometry(std::map<std::string, std::string> &vars,
                     const traffic_context::TrafficTarget &t) {
  char clock_buf[8];
  char dist_buf[16];
  std::snprintf(clock_buf, sizeof(clock_buf), "%.0f", t.clock_position);
  std::snprintf(dist_buf, sizeof(dist_buf), "%.0f", t.distance_to_user_nm);
  vars["clock"] = clock_buf;
  vars["distance"] = dist_buf;
}

} // namespace

void init() {
  state_ = State::IDLE;
  pending_target_id_ = 0;
}

void stop() { init(); }

void reset() { init(); }

State get_state() { return state_; }

bool is_awaiting_ack() { return state_ == State::AWAITING_ACK; }

uint32_t pending_target_id() { return pending_target_id_; }

void on_advisory_emitted(uint32_t target_modeS_id) {
  state_ = State::AWAITING_ACK;
  pending_target_id_ = target_modeS_id;
  logging::info("traffic_dialog: AWAITING_ACK (target_id=%u)", target_modeS_id);
}

Reply handle_pilot(const intent_parser::PilotMessage &msg,
                   const xplane_context::XPlaneContext &ctx) {
  Reply r;
  if (state_ != State::AWAITING_ACK)
    return r;

  using PI = intent_parser::PilotIntent;
  const bool is_traffic_intent = msg.intent == PI::TRAFFIC_IN_SIGHT ||
                                 msg.intent == PI::TRAFFIC_NEGATIVE_CONTACT ||
                                 msg.intent == PI::TRAFFIC_LOOKING;
  if (!is_traffic_intent)
    return r;

  // Grab the standard ATC vars (callsign abbreviation, airport, ...).
  // The pilot_message we pass through carries the parsed callsign so
  // get_callsign() picks the right form.
  auto vars = ground_ops::build_vars(msg, ctx);

  if (msg.intent == PI::TRAFFIC_IN_SIGHT) {
    auto tmpl = atc_templates::lookup(true, kTemplateState, "TRAFFIC_IN_SIGHT");
    r.text = atc_templates::fill(tmpl.response_template, vars);
    r.handled = true;
    r.acknowledged_with_visual = true;
    logging::info("traffic_dialog: IN_SIGHT (target_id=%u)",
                  pending_target_id_);
    state_ = State::IDLE;
    pending_target_id_ = 0;
    return r;
  }

  // TRAFFIC_NEGATIVE_CONTACT / TRAFFIC_LOOKING — re-issue once with
  // refreshed clock + distance from the live snapshot. If the target
  // has dropped out of range, fall back to a neutral acknowledgement.
  const auto &snap = traffic_context::current();
  const auto *target = find_target(snap, pending_target_id_);
  if (target) {
    inject_geometry(vars, *target);
  } else {
    vars["clock"] = "12";
    vars["distance"] = "0";
  }

  const char *intent_key = (msg.intent == PI::TRAFFIC_LOOKING)
                               ? "TRAFFIC_LOOKING"
                               : "TRAFFIC_NEGATIVE_CONTACT";
  auto tmpl = atc_templates::lookup(true, kTemplateState, intent_key);
  r.text = atc_templates::fill(tmpl.response_template, vars);
  r.handled = true;
  r.acknowledged_with_visual = false;
  logging::info("traffic_dialog: %s (target_id=%u)", intent_key,
                pending_target_id_);
  state_ = State::IDLE;
  pending_target_id_ = 0;
  return r;
}

} // namespace traffic_dialog
