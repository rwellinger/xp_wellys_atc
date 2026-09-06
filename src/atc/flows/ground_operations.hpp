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

#ifndef ATC_FLOWS_GROUND_OPERATIONS_HPP
#define ATC_FLOWS_GROUND_OPERATIONS_HPP

// Shared pre-takeoff pipeline. Step 1 of the A1 flow-split refactor
// extracts these helpers out of atc_state_machine.cpp into their own
// namespace so the future PatternFlow and CrossCountryFlow can drive
// the same ground-side logic without duplication. In step 4 the
// pipeline becomes the canonical entry point for the ground-only
// states (IDLE / GROUND_CONTACT / TAXI_CLEARED / TOWER_CONTACT /
// UNICOM_ACTIVE). The functions still mutate the shared state through
// atc_state_machine::internal — that bridge goes away when each flow
// owns its own state.

#include "atc/atc_state_machine.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"

#include <map>
#include <string>

namespace ground_ops {

using atc_state_machine::ATCResponse;
using intent_parser::PilotMessage;
using xplane_context::XPlaneContext;

// Build the template variable map used by every spoken ATC response.
// Pure read of state + ctx; never mutates. Callsign is abbreviated to
// the last three words once the dialog has left IDLE.
std::map<std::string, std::string> build_vars(const PilotMessage &msg,
                                              const XPlaneContext &ctx);

// ── Pipeline guards (run in process() before the template lookup) ──
// Each returns true when it produced a response and the caller should
// stop processing this turn. Helpers without a response (apply_*)
// return void and only mutate state.

bool handle_negative_correction(const PilotMessage &msg,
                                const XPlaneContext &ctx, ATCResponse &resp);

void apply_state_reverts(const PilotMessage &msg);

// Tower-only first contact collapse (DE profile). At a tower-only field the
// apron initial call is addressed to "Turm" (no separate Ground frequency),
// so the text-based parser yields INITIAL_CALL_TOWER. Rewrite it in place to
// INITIAL_CALL_GROUND so the existing BZF conformance check and the GROUND
// first-contact hint apply — exactly as at a field WITH Ground. Only the
// IDLE apron call collapses; the holding-point "abflugbereit" Tower call
// (state already advanced past IDLE) and airborne/inbound calls are left
// untouched. Mutates msg.intent; no-op for every other case.
void apply_tower_only_initial_collapse(PilotMessage &msg,
                                       const XPlaneContext &ctx);

// Advisory flow for uncontrolled fields with an AFIS/Info facility
// (FrequencyType::INFO). Traffic information only — no clearances, no readback.
// Must be dispatched BEFORE handle_unicom_flow(). NfL 2024 §34 b) / §35.
bool handle_info_flow(const PilotMessage &msg, const XPlaneContext &ctx,
                      ATCResponse &resp);

bool handle_unicom_flow(const PilotMessage &msg, const XPlaneContext &ctx,
                        ATCResponse &resp);

bool handle_frequency_hint(const PilotMessage &msg, const XPlaneContext &ctx,
                           ATCResponse &resp);

void apply_state_frequency_validity(const XPlaneContext &ctx);

void apply_frequency_auto_corrections(const XPlaneContext &ctx);

bool handle_idle_redirects(const PilotMessage &msg, const XPlaneContext &ctx,
                           ATCResponse &resp);

bool check_phase_precondition(const PilotMessage &msg, const XPlaneContext &ctx,
                              ATCResponse &resp);

bool check_freq_precondition(const PilotMessage &msg, const XPlaneContext &ctx,
                             ATCResponse &resp);

// BZF first-call conformance guard (DE profile, INITIAL_CALL_GROUND only).
// Always logs missing elements for later debrief. In strict mode, an
// incomplete first call yields a targeted re-request (state held) and the
// function returns true so the caller stops before the normal reply. In
// non-strict mode `required` is empty for this intent, so it returns false
// and the standard initial-contact reply proceeds unchanged.
bool apply_initial_call_conformance(const PilotMessage &msg,
                                    const XPlaneContext &ctx, ATCResponse &resp);

} // namespace ground_ops

#endif // ATC_FLOWS_GROUND_OPERATIONS_HPP
