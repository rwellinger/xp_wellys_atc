/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef ENGINE_ENGINE_HPP
#define ENGINE_ENGINE_HPP

#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"

#include <functional>
#include <string>

namespace engine {

struct Input {
  std::string transcript;
  float quality = 1.0f; // Whisper quality; 1.0 for text-only tests
  // Non-owning pointer to the current XPlaneContext. The plugin passes
  // &xplane_context::get(); the CLI will pass a scenario-built context.
  // Lifetime must cover the duration of process_transcript + any async
  // callbacks it spawns.
  const xplane_context::XPlaneContext *ctx = nullptr;
  std::string pilot_callsign;
  // Monotonic clock used by traffic_advisor cooldowns when the pilot's
  // utterance is a TRAFFIC_* acknowledgement. Plugin passes
  // XPLMGetElapsedTime; the headless CLI / tests pass a deterministic
  // counter. Defaults to 0 — fine for code paths that never enter the
  // traffic dialog.
  double now_secs = 0.0;
};

struct Output {
  // Empty = silent transition (state changed but no response to speak).
  std::string response_text;
  intent_parser::PilotMessage parsed;
  // True for radio-discipline warnings — caller uses this if it needs to
  // distinguish "ATC clearance" from "ATC correction". State is unchanged
  // when is_warning is true.
  bool is_warning = false;
};

using Done = std::function<void(Output)>;

// Reset internal counters (profanity warnings, LLM call count). Call from
// plugin init / re-enable. Separate from the per-call flow so engine has
// no "stop" phase.
void reset();

// Number of LLM inferences kicked off by the engine since last reset()
// (intent classification, sub-variant disambiguation). Callers that
// maintain an aggregate inference counter (STT + TTS + LM) add this in.
int lm_inferences();

// Count of consecutive unintelligible pilot transmissions since the
// last successful intent. Reset by reset() and by any clear pilot
// reply. Exposed for tests / instrumentation; the engine drives the
// "say again, use standard phraseology" escalation off this internally.
int unclear_streak();

// Process a pilot transcript end-to-end:
//   - quality check (low quality -> say again)
//   - rule-based intent parse
//   - INAPPROPRIATE_LANGUAGE interception (escalating warnings)
//   - departure sub-variant disambiguation via local LLM (if loaded)
//   - state machine invocation with two-stage (direct vs. LLM) routing
//
// `done` is always called exactly once. On the sync path it runs before
// process_transcript returns; on the LLM-async path it runs later on the
// thread that the LLM callback is dispatched on (main thread, via the
// plugin's callback drain).
void process_transcript(Input in, Done done);

// Per-tick traffic-advisory poll. SDK-free: takes the current
// XPlaneContext, reads traffic_context::current() for the live
// snapshot, and runs traffic_advisor::evaluate(). On a positive
// decision, renders the advisory text via
// atc_state_machine::render_traffic_advisory() and notifies
// traffic_dialog so the next pilot transcript is routed there for
// acknowledgement. Returns true iff an advisory was emitted (caller is
// responsible for routing the text to TTS / transcript display).
//
// `now_secs` is the monotonic clock the cooldown logic compares
// against. In the plugin this is XPLMGetElapsedTime; in the headless
// CLI / tests the caller passes a deterministic counter.
bool poll_traffic_advisory(const xplane_context::XPlaneContext &ctx,
                           double now_secs, std::string *out_text);

// Phase-4 unsolicited go-around trigger. Frame-driven, render-only:
//   - user is in Pattern/LANDING_CLEARED
//   - user is within 1 NM of the active-runway threshold
//   - a ground-phase target sits on the active runway centerline
//   - no go-around has been emitted in the last 60 s
//
// On a positive decision, renders the `go_around_traffic_runway`
// template via atc_state_machine::render_traffic_advisory() and
// returns true. Does NOT change ATCState — go-around is a flight
// command, not a dialog turn (no readback, no ack channel).
//
// `now_secs` is the same monotonic clock poll_traffic_advisory uses.
bool poll_go_around(const xplane_context::XPlaneContext &ctx, double now_secs,
                    std::string *out_text);

} // namespace engine

#endif
