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
 *
 * Phase-2 traffic dialog. SDK-free side-channel that runs *parallel* to
 * the main ATCState flow:
 *   - The per-tick traffic_advisor decides whether to issue a callout.
 *   - traffic_dialog::on_advisory_emitted() flips state to AWAITING_ACK
 *     and stashes the target's modeS_id.
 *   - The next pilot transcript is dispatched first to traffic_dialog;
 *     if it matches a TRAFFIC_* intent, traffic_dialog renders the
 *     acknowledgement (and a possible re-issue with refreshed geometry)
 *     and returns to IDLE. The ATC main flow (PATTERN_ENTRY,
 *     TOWER_CONTACT, ...) is never touched.
 *
 * Lives in xp_atc_engine OBJECT lib — no XPLM headers, the headless
 * atc_repl tool drives the same path the plugin does.
 */

#ifndef ATC_TRAFFIC_DIALOG_HPP
#define ATC_TRAFFIC_DIALOG_HPP

#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"

#include <cstdint>
#include <string>

namespace traffic_dialog {

enum class State {
  IDLE,
  AWAITING_ACK,
};

void init();
void stop();
void reset();

State get_state();
bool is_awaiting_ack();
uint32_t pending_target_id();

// Called by engine::poll_traffic_advisory() right after the advisor
// decided to fire. The caller is responsible for rendering and speaking
// the advisory text — this hook only updates internal dialog state.
void on_advisory_emitted(uint32_t target_modeS_id);

// Result of a pilot utterance handled by traffic_dialog.
struct Reply {
  // True when the pilot's intent was a TRAFFIC_* and the dialog handled
  // it. The engine should skip the normal state-machine path in that
  // case and just speak `text`.
  bool handled = false;
  // Rendered ATC reply (e.g. "Charlie Hotel, roger, maintain visual
  // separation."). Empty when `handled` is false or when there is
  // nothing to say.
  std::string text;
  // True when the dialog acknowledged with positive visual contact
  // (TRAFFIC_IN_SIGHT). engine::poll_traffic_advisory uses this to
  // extend the per-target cooldown.
  bool acknowledged_with_visual = false;
};

Reply handle_pilot(const intent_parser::PilotMessage &msg,
                   const xplane_context::XPlaneContext &ctx);

} // namespace traffic_dialog

#endif // ATC_TRAFFIC_DIALOG_HPP
