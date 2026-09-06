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

#ifndef ATC_FLOWS_STATE_STORAGE_HPP
#define ATC_FLOWS_STATE_STORAGE_HPP

// Private bridge between atc_state_machine.cpp (which owns the
// long-lived state statics) and the SDK-free flow modules under
// src/atc/flows/ (currently: ground_operations.cpp). Not part of
// atc_state_machine.hpp's public API: external callers continue to use
// the namespace-level functions exposed there.
//
// This header exists so the pipeline helpers (handle_*, apply_*,
// check_*) and build_vars() — extracted from atc_state_machine.cpp
// in step 1 of the flow-split refactor — can read and mutate the
// shared state without becoming new statics in their own translation
// unit. The split between PatternFlow / CrossCountryFlow in later
// steps will replace this bridge with per-flow ownership.

#include "atc/atc_state_machine.hpp"

#include <string>

namespace atc_state_machine::internal {

// Departure-intent flag. Mirrors today's `departure_type_` static in
// atc_state_machine.cpp. Disappears entirely after step 4 (bifurcate).
enum class DepartureType { PATTERN, CROSS_COUNTRY };

const char *departure_type_name(DepartureType t);

// ── State storage accessors (read) ─────────────────────────────────
ATCState get_state_ref();
bool readback_pending();
const std::string &assigned_runway_ref();
const std::string &session_callsign_ref();
DepartureType departure_type();

// ── State storage mutators ─────────────────────────────────────────
// transition_to() is the only path that updates ATCState_ + appends to
// history_. Internal API so helpers in other TUs don't bypass it.
void transition_to(ATCState next, const char *reason);
void set_readback_pending(bool v);
void set_assigned_runway(const std::string &rwy);
void clear_assigned_runway();
void set_session_callsign(const std::string &cs);
void clear_session_callsign();
void set_departure_type(DepartureType t);

// Last-now-secs is the timestamp captured at the public entry points
// (process, check_auto_correction, disregard, check_airport_change).
// Internal helpers calling transition_to() pick this up so every
// history entry has a sensible timestamp without threading the value.
void set_last_now_secs(double t);
double last_now_secs();

} // namespace atc_state_machine::internal

#endif // ATC_FLOWS_STATE_STORAGE_HPP
