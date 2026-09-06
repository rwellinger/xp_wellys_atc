/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "engine.hpp"

#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/bzf_compliance.hpp"
#include "atc/flight_phase.hpp"
#include "atc/landing_sequence.hpp"
#include "atc/traffic_advisor.hpp"
#include "atc/traffic_dialog.hpp"
#include "backends/manager.hpp"
#include "core/cross_country_log.hpp"
#include "core/logging.hpp"
#include "data/traffic_context.hpp"
#include "data/traffic_geometry.hpp"
#include "persistence/settings.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

namespace engine {

static int profanity_warnings_ = 0;
static int lm_inferences_ = 0;
static traffic_advisor::AdvisoryHistory advisory_history_;
// Counts back-to-back unintelligible transmissions. Reset whenever the
// pilot lands a valid intent. Drives the escalation from "garbled" to
// "use standard phraseology" so a controller-style nudge follows the
// pilot's repeated unclear calls.
static int unclear_streak_ = 0;

// Phase-4 go-around throttle. Last monotonic clock at which a
// go-around was emitted; -1e9 = never. Keeps the trigger from re-
// firing every frame while the runway stays occupied.
static double last_go_around_emit_secs_ = -1e9;
constexpr double kGoAroundCooldownSec = 60.0;
constexpr double kGoAroundTriggerDistanceNm = 1.0;

void reset() {
  profanity_warnings_ = 0;
  lm_inferences_ = 0;
  unclear_streak_ = 0;
  advisory_history_ = traffic_advisor::AdvisoryHistory{};
  last_go_around_emit_secs_ = -1e9;
  traffic_dialog::reset();
}

int unclear_streak() { return unclear_streak_; }

int lm_inferences() { return lm_inferences_; }

// Lower-case copy used for keyword scanning. ASCII only — Whisper
// transcripts don't contain anything else.
static std::string to_lower_copy(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// Extract the multiset of digits-only tokens from a transcript. Used by
// the LM-repair validator below: a 3B model occasionally invents runway
// numbers / frequencies / altitudes that were never in the pilot's input
// (the example pattern in the prompt has been observed leaking into
// inputs that contain no number at all). If the repair carries a
// numeric token that the original lacks, we discard the repair and fall
// back to the raw Whisper transcript. Letters and "9er" → "9" mappings
// are deliberately ignored — only contiguous digit runs are compared.
static std::vector<std::string> extract_digit_tokens(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      cur += c;
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

// True when `repaired` contains a digit token that is absent from
// `original`. The check is multiset-based so a repeated runway is fine
// as long as both sides have it. Catches the canonical hallucination:
//   original: "Clear for takeoff Delta Chari Hotel"  (no digits)
//   repaired: "Cleared for takeoff runway 06, ..."   (introduces "06")
static bool repair_invents_digits(const std::string &original,
                                  const std::string &repaired) {
  auto orig_digits = extract_digit_tokens(original);
  auto rep_digits = extract_digit_tokens(repaired);
  for (const auto &d : rep_digits) {
    auto it = std::find(orig_digits.begin(), orig_digits.end(), d);
    if (it == orig_digits.end())
      return true;
    orig_digits.erase(it);
  }
  return false;
}

// Plausibility guard against post-landing repair hallucinations. The
// 3B local LM occasionally rewrites "runway 06 located" (Whisper
// mishearing of "vacated") into "Cleared for takeoff runway 06" when
// it loses track of the just-landed context. Even with the prompt
// updated to forbid that, the model sometimes drifts; this hard check
// is a deterministic safety net.
//
// When `just_landed_flag` is true, any repair containing a tokenised
// takeoff/departure phrase is rejected outright. Caller falls back to
// the raw Whisper transcript.
static bool repair_violates_history(const std::string &repaired,
                                    bool just_landed_flag) {
  if (!just_landed_flag || repaired.empty())
    return false;
  std::string lower = to_lower_copy(repaired);
  static const char *kForbidden[] = {
      "cleared for takeoff", "clear for takeoff", "ready for departure",
      "ready for take",      "line up",
  };
  for (const char *needle : kForbidden) {
    if (lower.find(needle) != std::string::npos)
      return true;
  }
  return false;
}

// True if the transcript carries at least one identifiable ATC element
// (callsign extracted, runway extracted, or any of a handful of
// unambiguous EU/ICAO keywords). Used to distinguish a partially-
// understood transmission ("Tower ... runway 14 ...") from total
// noise. The set is deliberately small: words common across pilot
// requests AND readbacks, picked so a single match means the pilot
// was using radio phraseology even if Whisper killed a key word.
static bool has_recognisable_elements(const intent_parser::PilotMessage &msg) {
  if (!msg.callsign.empty())
    return true;
  if (!msg.runway.empty())
    return true;
  std::string t = to_lower_copy(msg.raw_transcript);
  static const char *kKeywords[] = {
      "tower",    "ground",    "approach",    "runway",  "request", "ready",
      "downwind", "base",      "final",       "holding", "qnh",     "wilco",
      "roger",    "departure", "information", "inbound", "vacated",
  };
  for (const char *kw : kKeywords) {
    if (t.find(kw) != std::string::npos)
      return true;
  }
  return false;
}

// Three-tier "I didn't get that" response. Increments unclear_streak_;
// the caller resets it when a valid intent finally lands. EU/ICAO
// phraseology (Doc 4444 / EU 2020/469):
//   - elements recognised        -> "garbled, say again"
//   - nothing recognised         -> "say again"
//   - 2nd unclear in a row       -> "say again, use standard phraseology"
static std::string
build_unclear_response(const intent_parser::PilotMessage &msg,
                       const std::string &fallback_cs) {
  ++unclear_streak_;
  // Prefer the session-locked callsign so a mistranscribed utterance
  // ("Delta ...") cannot hijack the tower's salutation mid-session.
  const std::string &session_cs = atc_state_machine::session_callsign();
  std::string cs;
  if (!session_cs.empty())
    cs = session_cs;
  else if (!msg.callsign.empty())
    cs = msg.callsign;
  else
    cs = fallback_cs;
  std::string prefix = cs.empty() ? std::string{} : cs + ", ";

  if (unclear_streak_ >= 2)
    return prefix +
           atc_templates::lookup_fallback("say_again_use_standard_phraseology",
                                          "say again, use standard "
                                          "phraseology.");
  if (has_recognisable_elements(msg))
    return prefix + atc_templates::lookup_fallback(
                        "garbled_say_again",
                        "your transmission was garbled, say again.");
  return prefix + atc_templates::lookup_fallback("say_again", "say again.");
}

// Convenience for the quality-rejection path which has no parsed
// PilotMessage yet — only the raw transcript and a probably-empty
// callsign hint from the cockpit settings.
static std::string build_unclear_response_raw(const std::string &transcript,
                                              const std::string &fallback_cs) {
  intent_parser::PilotMessage stub;
  stub.raw_transcript = transcript;
  return build_unclear_response(stub, fallback_cs);
}

// Reset the back-to-back unclear counter. Called whenever a meaningful
// reply (template-rendered, traffic dialog, profanity etc.) is about to
// be returned to the pilot.
static void mark_clear() { unclear_streak_ = 0; }

static std::string build_profanity_response(int warning_number,
                                            const std::string &callsign) {
  if (warning_number == 1) {
    return callsign + ", maintain proper radio discipline. Use standard "
                      "phraseology on this frequency.";
  }
  if (warning_number == 2) {
    return callsign + ", this is your final warning. Continued inappropriate "
                      "language on this frequency will be reported to the "
                      "civil aviation authority. Use standard phraseology.";
  }
  return callsign + ", your conduct has been noted and will be reported to "
                    "the aviation authority. Maintain radio discipline "
                    "immediately.";
}

// Side-channel: when traffic_dialog is awaiting a pilot ack, route the
// transcript there first. Returns true if traffic_dialog handled it
// (the caller should skip the main flow). Updates advisory_history_'s
// visual-ack lockout when the pilot reported visual contact.
static bool try_traffic_dialog(const intent_parser::PilotMessage &msg,
                               const xplane_context::XPlaneContext &ctx,
                               double now_secs, Output &out) {
  if (!traffic_dialog::is_awaiting_ack())
    return false;

  uint32_t target_id = traffic_dialog::pending_target_id();
  auto reply = traffic_dialog::handle_pilot(msg, ctx);
  if (!reply.handled)
    return false;

  if (reply.acknowledged_with_visual)
    traffic_advisor::mark_acknowledged_visual(advisory_history_, target_id,
                                              now_secs);

  if (settings::debug_logging())
    logging::debug("Traffic dialog reply: %s",
                   reply.text.empty() ? "(silent)" : reply.text.c_str());
  out.parsed = msg;
  out.response_text = std::move(reply.text);
  // Pilot landed an intelligible TRAFFIC_* reply — break any in-flight
  // "say again" escalation.
  mark_clear();
  return true;
}

static Output run_state_machine(const intent_parser::PilotMessage &msg,
                                const xplane_context::XPlaneContext &ctx_now,
                                double now_secs) {
  auto atc_resp = atc_state_machine::process(msg, ctx_now, now_secs);
  if (settings::debug_logging())
    logging::debug("ATC response text: %s",
                   atc_resp.text.empty() ? "(silent)" : atc_resp.text.c_str());
  // A landed intent (rule parser or LM both produce non-UNKNOWN) means
  // the pilot was understood — even if the state machine subsequently
  // rejected the request via _INVALID/phase guard. Break the streak so
  // the next garbled call still starts at the friendly "garbled, say
  // again" tier rather than the escalation.
  if (msg.intent != intent_parser::PilotIntent::UNKNOWN)
    mark_clear();
  Output out;
  out.parsed = msg;
  out.response_text = atc_resp.text;
  return out;
}

// ── Cross-country measurement logging (purely observational) ─────────
// Everything below feeds cross_country_log only. It never influences
// matching, routing or classification — it observes the values the flow
// already produced and writes one JSON line per transmission. See
// core/cross_country_log.hpp for the field contract.

// State / phase / valid-intents captured at the moment the pilot spoke,
// BEFORE the state machine processes the transmission. Threaded into the
// async LM callback so a log line reflects time-of-speech context, not
// whatever the state machine moved to by the time we write.
struct CcBase {
  std::string state;
  std::string flight_phase;
  std::string expected_intent; // valid_intents CSV for `state`
  std::string airport_id;      // ctx.nearest_airport_id (flight-header source)
  std::string pilot_callsign;  // pilot callsign for the flight header
  std::string frequency_type;  // scopes missing_initial_call
};

// CSV of the intents valid in the current state — raw material for the
// offline "what was plausibly expected here" judgement. Read-only.
static std::string expected_intents_csv(const xplane_context::XPlaneContext &ctx) {
  using FT = xplane_context::FrequencyType;
  bool is_towered = ctx.is_towered() && ctx.frequency_type != FT::UNICOM &&
                    ctx.frequency_type != FT::CTAF &&
                    ctx.frequency_type != FT::INFO &&
                    ctx.frequency_type != FT::RADIO;
  std::string state_str =
      atc_state_machine::state_name(atc_state_machine::get_state());
  auto valid = atc_templates::valid_intents(is_towered, state_str,
                                            atc_state_machine::was_airborne());
  std::string csv;
  for (const auto &v : valid) {
    if (!csv.empty())
      csv += ",";
    csv += v;
  }
  return csv;
}

// Tower-response tier for the say-again paths, mapped to the log's
// outcome vocabulary.
static const char *unclear_outcome(const intent_parser::PilotMessage &msg) {
  return has_recognisable_elements(msg) ? "tower_reported_garbled" : "unknown";
}

// NON-BINDING failure-locus suggestion. Deliberately a dumb keyword scan
// so it cannot diverge from behaviour: it only annotates the record. The
// raw fields (transcript, expected_intent, vrp_name) stay authoritative
// and let the offline evaluator overrule this.
static std::string guess_failure_locus(const std::string &transcript,
                                       bool vrp_set) {
  std::string t = to_lower_copy(transcript);
  static const char *kPhraseology[] = {
      "rollen",      "piste",     "gegenanflug", "queranflug", "anflug",
      "abflug",      "endteil",   "platzrunde",  "qnh",        "startklar",
      "startfrei",   "durchstart", "steigflug",  "verlasse",   "wiederhol",
      "melde",       "squawk",    "information", "rollhalt",   "freigabe",
  };
  bool phr = false;
  for (const char *kw : kPhraseology) {
    if (t.find(kw) != std::string::npos) {
      phr = true;
      break;
    }
  }
  if (phr && vrp_set)
    return "mixed";
  if (phr)
    return "phraseology";
  if (vrp_set)
    return "proper_name";
  return "unclear";
}

// Assemble + emit one cross-country record. `missing` is non-null only on
// the deterministic readback path (where the soll-ist diff was already
// computed); pass nullptr elsewhere.
static void cc_log(const std::string &transcript, float quality,
                   const intent_parser::PilotMessage &msg,
                   const std::string &path, bool lm_used,
                   const std::string &outcome, const CcBase &base,
                   const std::vector<bzf_compliance::Element> *missing,
                   const std::string &atc_response) {
  cross_country_log::Entry e;
  e.transcript = transcript;
  e.quality = quality;
  e.atc_response = atc_response;
  e.intent = intent_parser::intent_name(msg.intent);
  e.confidence = msg.confidence;
  e.path = path;
  e.lm_used = lm_used;
  e.lm_ready = lm_used ? backends::lm_ready() : false;
  e.outcome = outcome;
  e.state = base.state;
  e.flight_phase = base.flight_phase;
  e.expected_intent = base.expected_intent;
  e.airport_id = base.airport_id;
  e.pilot_callsign = base.pilot_callsign;
  e.frequency_type = base.frequency_type;
  e.vrp_name_set = !msg.vrp_name.empty();
  e.vrp_name = msg.vrp_name;
  e.is_readback = msg.intent == intent_parser::PilotIntent::READBACK;
  if (missing != nullptr) {
    e.is_readback = true;
    for (auto el : *missing)
      e.readback_missing_elements.emplace_back(bzf_compliance::element_name(el));
  }
  if (outcome != "classified" || lm_used) {
    e.emit_failure_locus = true;
    e.failure_locus = guess_failure_locus(transcript, e.vrp_name_set);
  }
  cross_country_log::write(e);
}

void process_transcript(Input in, Done done) {
  if (settings::debug_logging())
    logging::debug("Whisper response (quality=%.2f): \"%s\"", in.quality,
                   in.transcript.c_str());

  // Snapshot the time-of-speech context for cross-country logging before
  // any state transition. get_state()/flight_phase::get() need no ctx;
  // expected_intent does, so it is only filled when a ctx is present.
  CcBase cc_base;
  cc_base.state = atc_state_machine::state_name(atc_state_machine::get_state());
  cc_base.flight_phase = flight_phase::phase_name(flight_phase::get());
  cc_base.pilot_callsign = in.pilot_callsign;
  if (in.ctx != nullptr) {
    cc_base.expected_intent = expected_intents_csv(*in.ctx);
    cc_base.airport_id = in.ctx->nearest_airport_id;
    cc_base.frequency_type =
        xplane_context::frequency_type_name(in.ctx->frequency_type);
  }

  // Poor transcript quality — likely noise or engine sounds. Even at
  // very low quality the transcript may still contain a recognised
  // ATC keyword, so route via the unclear-response builder instead of
  // the fixed "say again". Never the moment to land a valid intent,
  // so the streak counter advances normally.
  if (in.quality < 0.3f) {
    logging::info("Transcript quality too low, requesting say again");
    Output out;
    out.response_text =
        build_unclear_response_raw(in.transcript, in.pilot_callsign);
    intent_parser::PilotMessage stub;
    stub.raw_transcript = in.transcript;
    cc_log(in.transcript, in.quality, stub, "rule_skip_lm", false,
           unclear_outcome(stub), cc_base, nullptr, out.response_text);
    done(std::move(out));
    return;
  }

  const auto &ctx = *in.ctx;

  // Parse intent
  auto parsed = intent_parser::parse(in.transcript, ctx);

  if (settings::debug_logging())
    logging::debug("Intent: %s (confidence=%.2f), callsign=%s",
                   intent_parser::intent_name(parsed.intent), parsed.confidence,
                   parsed.callsign.empty() ? "(none)"
                                           : parsed.callsign.c_str());

  // Declared VFR flight intent (NfL 2024 1.4.7): the pilot names the
  // destination/intent ONCE on the taxi/initial-ground call, not on every
  // follow-up transmission. Latch it into the per-flight session state so
  // build_vars() remembers it for the rest of the flight instead of
  // falling back to "pattern" on a destination-less READY_FOR_DEPARTURE
  // (which would wrongly answer "Melden Sie Gegenanflug"). The extractor
  // only fires on a NATO-spelled ICAO after a "nach"/"richtung" anchor,
  // so this is safe to apply regardless of the (possibly pre-LM UNKNOWN)
  // intent. Session state, not settings: cleared on init/stop/reset, never
  // persisted across flights. The snapshot guard captures/restores it with
  // the rest of g_state, so a TTS failure rolls the latch back too.
  if (!parsed.destination.empty()) {
    atc_state_machine::set_flight_intent_cross_country(parsed.destination);
    if (settings::debug_logging())
      logging::debug("Cross-country flight intent latched from speech: %s",
                     parsed.destination.c_str());
  } else if (to_lower_copy(in.transcript).find("platzrunde") !=
             std::string::npos) {
    // Spoken "Platzrunde": latch a pattern flight. A destination extracted
    // this utterance is more specific and wins (handled above), so only
    // apply when none was found. Lets the pilot verbally switch a
    // previously-declared cross-country flight back to a pattern flight
    // without touching the Flugvorbereitung tab.
    atc_state_machine::set_flight_intent_pattern();
    if (settings::debug_logging())
      logging::debug("Pattern flight intent latched from speech: 'Platzrunde'");
  }

  // Traffic dialog short-circuit. When the controller is awaiting a
  // pilot acknowledgement of a traffic advisory and the pilot just
  // matched a TRAFFIC_* intent at high confidence, route directly there
  // and skip the main flow + LM disambig.
  if (traffic_dialog::is_awaiting_ack() &&
      (parsed.intent == intent_parser::PilotIntent::TRAFFIC_IN_SIGHT ||
       parsed.intent == intent_parser::PilotIntent::TRAFFIC_NEGATIVE_CONTACT ||
       parsed.intent == intent_parser::PilotIntent::TRAFFIC_LOOKING) &&
      parsed.confidence >= 0.7f) {
    Output out;
    if (try_traffic_dialog(parsed, ctx, in.now_secs, out)) {
      cc_log(in.transcript, in.quality, out.parsed, "rule_skip_lm", false,
             "classified", cc_base, nullptr, out.response_text);
      done(std::move(out));
      return;
    }
  }

  // Inappropriate language — intercept before state machine.
  // Does NOT change ATC state, pilot can continue normally after.
  if (parsed.intent == intent_parser::PilotIntent::INAPPROPRIATE_LANGUAGE) {
    ++profanity_warnings_;
    const std::string &session_cs = atc_state_machine::session_callsign();
    std::string cs;
    if (!session_cs.empty())
      cs = session_cs;
    else if (!parsed.callsign.empty())
      cs = parsed.callsign;
    else
      cs = in.pilot_callsign;
    logging::info("Radio discipline warning #%d", profanity_warnings_);
    Output out;
    out.parsed = parsed;
    out.response_text = build_profanity_response(profanity_warnings_, cs);
    out.is_warning = true;
    // Coherent (if rude) utterance — no "say again" loop carries over.
    mark_clear();
    cc_log(in.transcript, in.quality, parsed, "rule_skip_lm", false,
           "classified", cc_base, nullptr, out.response_text);
    done(std::move(out));
    return;
  }

  using PI = intent_parser::PilotIntent;

  // ── Readback recognition (deterministic, no LM, backend-agnostic) ──
  // A pending readback is a soll-ist match against the known clearance,
  // not a classification problem — the controller already knows what it
  // sent. Whenever a readback is owed, match the utterance char-robustly
  // (weld-proof) against the stored clearance components instead of
  // guessing an intent or paying an LM round-trip. Runs BEFORE the
  // LM-not-ready fast path AND before the authoritative high-conf rule
  // path, so it works headless and identically across all backends.
  //
  // This gate fires regardless of the rule parser's confidence: a taxi
  // readback ("Rollen zur Rollhaltpiste 06 ... <callsign>") shares its
  // keywords ("rollen", "Piste") with the REQUEST_TAXI clearance the
  // tower just issued, so the context-free parser scores it REQUEST_TAXI
  // at 0.90 — a collision that previously slipped past this gate and
  // re-issued the clearance forever. readback_covers_core() (not the
  // confidence threshold) is the guard against stealing a genuinely
  // different intent: it demands the callsign PLUS at least one fact
  // element (Runway/QNH/Frequency/Squawk) of the stored clearance. An
  // utterance that carries none of the owed soll values (a query, "say
  // again", a divergent request) fails the cover check and falls through
  // to the rule/LM path below. Recognition is lenient (callsign + one
  // fact element); the BZF-strict completeness check inside the state
  // machine enforces the full obligation.
  if (atc_state_machine::is_readback_pending()) {
    const auto comp = atc_state_machine::last_clearance_components();
    const auto missing =
        bzf_compliance::missing_readback_elements(comp, in.transcript);
    if (bzf_compliance::readback_covers_core(comp.required, missing)) {
      auto rb = parsed;
      rb.intent = PI::READBACK;
      rb.confidence = 0.9f;
      if (settings::debug_logging())
        logging::debug("Readback recognised vs clearance (deterministic, "
                       "no LM); missing elements=%zu",
                       missing.size());
      Output out = run_state_machine(rb, ctx, in.now_secs);
      cc_log(in.transcript, in.quality, rb, "clearance_match", false,
             "classified", cc_base, &missing, out.response_text);
      done(std::move(out));
      return;
    }
  }

  // ── LM-not-ready fast path ────────────────────────────────────────
  // Headless tools, scenario tests, and the brief window between
  // plugin start and "models verified" all hit this path. The
  // rule-based parser is authoritative here — same behaviour as
  // before always-on classification was introduced.
  if (!backends::lm_ready()) {
    if (parsed.intent == PI::UNKNOWN) {
      Output out;
      out.parsed = parsed;
      out.response_text = build_unclear_response(parsed, in.pilot_callsign);
      logging::info("ATC (LM unavailable, UNKNOWN): %s",
                    out.response_text.c_str());
      cc_log(in.transcript, in.quality, parsed, "rule_skip_lm", false,
             unclear_outcome(parsed), cc_base, nullptr, out.response_text);
      done(std::move(out));
      return;
    }
    Output out = run_state_machine(parsed, ctx, in.now_secs);
    cc_log(in.transcript, in.quality, parsed, "rule_skip_lm", false,
           "classified", cc_base, nullptr, out.response_text);
    done(std::move(out));
    return;
  }

  // ── LM as fallback only ───────────────────────────────────────────
  // The rule-based parser (data-driven matchers in intent_rules.json
  // + state-history-aware adjustments such as just_landed) is
  // authoritative. The local LM only fires when the rule parser is
  // genuinely unsure (UNKNOWN or confidence < 0.7).
  //
  // Field measurement on Apple Silicon: even with Metal flash-
  // attention and QOS_UTILITY workers, every Llama 3.2 3B classify
  // call costs visible FPS in X-Plane. At conf >= 0.7 the rule
  // parser was empirically right in nearly every observed case
  // (see LSZG circuit log 2026-05-04: REQUEST_TAXI / READBACK /
  // RUNWAY_VACATED / REPORT_POSITION_* all classified correctly at
  // 0.90, while the LM frequently disagreed wrongly or returned
  // _INVALID and was overridden by safety nets).
  if (parsed.confidence >= 0.7f && parsed.intent != PI::UNKNOWN) {
    if (settings::debug_logging())
      logging::debug("Rule-based path: %s (conf=%.2f) — skip LM",
                     intent_parser::intent_name(parsed.intent),
                     parsed.confidence);
    Output out = run_state_machine(parsed, ctx, in.now_secs);
    cc_log(in.transcript, in.quality, parsed, "rule_skip_lm", false,
           "classified", cc_base, nullptr, out.response_text);
    done(std::move(out));
    return;
  }

  // ── Always-on LM classification with constrained JSON output ──────
  // The LM gets the rule-based parser's intent as a low-priority
  // hint, the valid_intents enum for the current state (grammar-
  // enforced — model literally cannot return anything else), and the
  // flight context. It returns {intent, repaired_transcript,
  // whisper_fix}. Whisper-artifact repair is the LM's job; pilot
  // phraseology errors fall through to the state machine which still
  // reacts realistically (frequency guards, phase guards, _INVALID
  // templates).
  using FT = xplane_context::FrequencyType;
  bool is_towered = ctx.is_towered() &&
                    ctx.frequency_type != FT::UNICOM &&
                    ctx.frequency_type != FT::CTAF &&
                    ctx.frequency_type != FT::INFO &&
                    ctx.frequency_type != FT::RADIO;

  std::string state_str =
      atc_state_machine::state_name(atc_state_machine::get_state());
  std::string previous_state_str =
      atc_state_machine::state_name(atc_state_machine::previous_state());
  std::string state_history_csv = atc_state_machine::history_csv();
  bool just_landed_flag = atc_state_machine::just_landed(in.now_secs);
  auto valid = atc_templates::valid_intents(is_towered, state_str,
                                            atc_state_machine::was_airborne());

  // Always include the traffic-acknowledgement intents — they are
  // valid any time the controller has just issued a traffic advisory,
  // regardless of which ATC state we're in.
  for (const char *t :
       {"TRAFFIC_IN_SIGHT", "TRAFFIC_NEGATIVE_CONTACT", "TRAFFIC_LOOKING"}) {
    if (std::find(valid.begin(), valid.end(), t) == valid.end())
      valid.emplace_back(t);
  }

  std::string valid_list;
  for (const auto &v : valid) {
    if (!valid_list.empty())
      valid_list += ", ";
    valid_list += v;
  }

  // Pick the classifier prompt by profile language:
  // gpt_classify_prompt_<lang> (_en for ICAO-VFR, _de for NfL DACH-VFR).
  // Fall back to the German prompt if the language-specific one is absent,
  // then to a minimal inline prompt — so an unknown language never leaves
  // the classifier promptless. The LMs are language-neutral; the language
  // lives entirely in this prompt.
  std::string sys_prompt = atc_templates::get_prompt(
      "gpt_classify_prompt_" + settings::backend_language());
  if (sys_prompt.empty())
    sys_prompt = atc_templates::get_prompt("gpt_classify_prompt_de");
  if (sys_prompt.empty()) {
    sys_prompt = "You are an ATC intent classifier. State: {state}. "
                 "Valid intents: {valid_intents}. Hint: {hint_intent}. "
                 "Transcript: \"{transcript}\". Respond with strict JSON "
                 "{\"intent\":\"...\",\"repaired\":\"...\",\"whisper_fix\":"
                 "false}.";
  }
  sys_prompt = atc_templates::fill(
      sys_prompt,
      {{"state", state_str},
       {"previous_state", previous_state_str},
       {"state_history_csv", state_history_csv},
       {"just_landed", just_landed_flag ? "true" : "false"},
       {"valid_intents", valid_list},
       {"transcript", in.transcript},
       {"frequency_type",
        xplane_context::frequency_type_name(ctx.frequency_type)},
       {"on_ground", ctx.on_ground ? "true" : "false"},
       {"altitude_ft", std::to_string(static_cast<int>(ctx.altitude_ft_msl))},
       {"groundspeed_kts",
        std::to_string(static_cast<int>(ctx.groundspeed_kts))},
       {"airport", ctx.nearest_airport_id},
       {"hint_intent", intent_parser::intent_name(parsed.intent)}});

  if (settings::debug_logging())
    logging::debug("Routing to LM backend classify_with_repair (rule hint=%s "
                   "conf=%.2f)",
                   intent_parser::intent_name(parsed.intent),
                   parsed.confidence);

  // Snapshot ctx + transcript so the async callback sees the state at
  // the moment the pilot spoke, not whatever ctx contains when the LM
  // responds.
  xplane_context::XPlaneContext ctx_snapshot = ctx;
  double now_secs = in.now_secs;
  std::string fallback_cs = in.pilot_callsign;
  std::string original_transcript = in.transcript;
  // Snapshot the just-landed flag too — the async callback may fire
  // after the state machine has moved on, but the post-landing
  // plausibility decision must reflect the moment the pilot spoke.
  bool just_landed_snapshot = just_landed_flag;
  // Snapshot whether a readback was owed at the moment the pilot spoke —
  // the deterministic safety net below turns an LM _INVALID into a silent
  // readback rather than a "say again" loop.
  bool readback_pending_snapshot = atc_state_machine::is_readback_pending();
  // Cross-country logging snapshots (purely observational): quality, the
  // time-of-speech context, and the stored clearance so a readback line
  // can carry its soll-ist diff even when the LM placed the utterance.
  float quality_snapshot = in.quality;
  CcBase cc_base_snapshot = cc_base;
  auto rb_comp_snapshot = atc_state_machine::last_clearance_components();
  ++lm_inferences_;
  backends::lm::classify_with_repair_async(
      in.transcript, sys_prompt, valid,
      // NOLINTNEXTLINE(bugprone-exception-escape)
      [parsed, ctx_snapshot, now_secs, fallback_cs, original_transcript,
       just_landed_snapshot, readback_pending_snapshot, quality_snapshot,
       cc_base_snapshot, rb_comp_snapshot, done = std::move(done)](
          const backends::lm::ClassifyResult &result) mutable {
        std::string intent_key =
            result.success ? result.intent_name : std::string("_INVALID");

        if (settings::debug_logging()) {
          logging::debug(
              "LM classified: intent=%s whisper_fix=%d repaired=\"%s\"",
              intent_key.c_str(), result.whisper_fix ? 1 : 0,
              result.repaired_transcript.c_str());
        }

        // Telemetry: log when LM and rule-based parser disagree.
        // Helps decide whether the 3B model is good enough or we need
        // a bigger one.
        auto rule_intent = parsed.intent;
        auto lm_intent = intent_parser::intent_from_key(intent_key);
        if (rule_intent != intent_parser::PilotIntent::UNKNOWN &&
            lm_intent != rule_intent && intent_key != "_INVALID") {
          logging::info("LM/rule disagree: rule=%s (conf=%.2f) llm=%s",
                        intent_parser::intent_name(rule_intent),
                        parsed.confidence, intent_key.c_str());
        }

        // Readback safety net: trust rule=READBACK whenever the rule
        // parser is confident (>=0.90), regardless of whether
        // readback_pending is currently armed. Two cases this catches:
        //   1) Mid-clearance readbacks where readback_pending=true.
        //      LM occasionally hallucinates TRAFFIC_IN_SIGHT or
        //      READY_FOR_DEPARTURE for a taxi readback whose Whisper
        //      transcription was garbled.
        //   2) Closing readbacks AFTER state→IDLE has already cleared
        //      readback_pending (e.g. post-landing "general aviation
        //      parking via Alpha, good day"). Without this widened
        //      check, LM=REQUEST_TAXI wins and triggers a brand-new
        //      departure cycle (TAXI_CLEARED → TOWER_CONTACT auto-
        //      advance), turning the parking-arrival readback into a
        //      bogus takeoff briefing.
        // The rule parser's READBACK matchers are keyword-anchored
        // (wilco/roger/good day/holding point/cleared+takeoff/qnh/
        // hold short/runway-suffix endings), so false positives are
        // rare. Letting the LM override these consistently produces
        // wrong ATC chatter at moments ICAO requires silence.
        if (rule_intent == intent_parser::PilotIntent::READBACK &&
            parsed.confidence >= 0.90f &&
            lm_intent != intent_parser::PilotIntent::READBACK) {
          logging::info("Readback safety net: keeping rule=READBACK over "
                        "LM=%s (rule_conf=%.2f, readback_pending=%s)",
                        intent_key.c_str(), parsed.confidence,
                        atc_state_machine::is_readback_pending() ? "true"
                                                                 : "false");
          intent_key = "READBACK";
          lm_intent = intent_parser::PilotIntent::READBACK;
        }

        // Validate the repair before letting it influence anything
        // downstream. If the LM invented digits that weren't in the
        // original (a runway number, a frequency, an altitude), drop
        // the repair and keep the raw Whisper text. Logged at info so
        // the rejection is visible without debug-mode.
        bool repair_accepted =
            result.whisper_fix && !result.repaired_transcript.empty();
        if (repair_accepted &&
            repair_invents_digits(original_transcript,
                                  result.repaired_transcript)) {
          logging::info("Whisper repair rejected (invented digits): "
                        "\"%s\" -> \"%s\"",
                        original_transcript.c_str(),
                        result.repaired_transcript.c_str());
          repair_accepted = false;
        } else if (repair_accepted &&
                   repair_violates_history(result.repaired_transcript,
                                           just_landed_snapshot)) {
          logging::info("Whisper repair rejected (post-landing context): "
                        "\"%s\" -> \"%s\"",
                        original_transcript.c_str(),
                        result.repaired_transcript.c_str());
          repair_accepted = false;
        } else if (repair_accepted) {
          logging::info("Whisper repair: \"%s\" -> \"%s\"",
                        original_transcript.c_str(),
                        result.repaired_transcript.c_str());
        }

        // _INVALID: controller asks for say-again. Tier picks itself
        // based on whether anything in the transcript was recognisable.
        if (intent_key == "_INVALID") {
          // Deterministic safety net: a readback was owed and the LM
          // couldn't place the utterance. A real controller silently
          // accepts an expected readback rather than looping "say again,
          // use standard phraseology". This net deliberately skips the
          // coverage check (recognition already failed above) — a silent
          // ack is the lesser evil over the loop. With bzf_strict_mode on,
          // run_state_machine -> apply_bzf_strict_check still validates the
          // content against the stored clearance and asks for what's
          // missing, so this only silently absorbs a complete readback.
          if (readback_pending_snapshot) {
            auto rb = parsed;
            rb.intent = PI::READBACK;
            rb.confidence = 0.7f;
            logging::info("ATC: LM _INVALID but readback pending -> accept "
                          "readback (no say-again loop)");
            Output out = run_state_machine(rb, ctx_snapshot, now_secs);
            auto missing = bzf_compliance::missing_readback_elements(
                rb_comp_snapshot, original_transcript);
            cc_log(original_transcript, quality_snapshot, rb, "lm_fallback",
                   true, "classified", cc_base_snapshot, &missing,
                   out.response_text);
            done(std::move(out));
            return;
          }
          Output out;
          out.parsed = parsed;
          out.response_text = build_unclear_response(parsed, fallback_cs);
          logging::info("ATC (LM _INVALID): %s", out.response_text.c_str());
          cc_log(original_transcript, quality_snapshot, parsed, "lm_fallback",
                 true, unclear_outcome(parsed), cc_base_snapshot, nullptr,
                 out.response_text);
          done(std::move(out));
          return;
        }

        // Build a PilotMessage with the LM-classified intent. Keep
        // the rule-based callsign / runway / VRP extraction — those
        // are deterministic and don't benefit from LM interpretation.
        auto lm_msg = parsed;
        lm_msg.intent = lm_intent;
        lm_msg.confidence = 0.85f;
        if (repair_accepted) {
          // Replace the raw transcript with the repaired one so the
          // UI history shows what the controller acted on. The
          // confidence stays at 0.85 — repair doesn't make us more
          // certain about intent classification.
          lm_msg.raw_transcript = result.repaired_transcript;
        }

        // Traffic dialog short-circuit. The rule parser frequently
        // misses softer phrasings ("looking", "have the traffic") and
        // only the LM lands them on TRAFFIC_*.
        Output out;
        if (try_traffic_dialog(lm_msg, ctx_snapshot, now_secs, out)) {
          cc_log(original_transcript, quality_snapshot, out.parsed,
                 "lm_fallback", true, "classified", cc_base_snapshot, nullptr,
                 out.response_text);
          done(std::move(out));
          return;
        }

        auto atc_resp =
            atc_state_machine::process(lm_msg, ctx_snapshot, now_secs);

        if (settings::debug_logging())
          logging::debug("ATC response text: %s", atc_resp.text.empty()
                                                      ? "(silent)"
                                                      : atc_resp.text.c_str());

        // LM produced a concrete intent — pilot was understood, even
        // if the state machine subsequently rejected the request.
        if (lm_msg.intent != intent_parser::PilotIntent::UNKNOWN)
          mark_clear();
        out.parsed = lm_msg;
        out.response_text = atc_resp.text;
        cc_log(original_transcript, quality_snapshot, lm_msg, "lm_fallback",
               true, "classified", cc_base_snapshot, nullptr,
               out.response_text);
        done(std::move(out));
      });
}

namespace {

// Resolve the active landing runway from a XPlaneContext snapshot.
// Mirrors pattern_flow::resolve_active_runway but kept local so the
// frame-driven go-around path does not have to link pattern_flow's
// internal anonymous namespace.
std::optional<landing_sequence::ActiveRunway>
resolve_active_runway_for_go_around(const xplane_context::XPlaneContext &ctx) {
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
      out.length_m = 2500.0;
    out.designator = end->number;
    return out;
  }
  return std::nullopt;
}

} // namespace

bool poll_go_around(const xplane_context::XPlaneContext &ctx, double now_secs,
                    std::string *out_text) {
  // Gate 1: user is on a granted landing clearance.
  if (atc_state_machine::get_state() !=
      atc_state_machine::ATCState::LANDING_CLEARED)
    return false;

  // Gate 2: cooldown — never fire two go-arounds inside 60 s.
  if (now_secs - last_go_around_emit_secs_ < kGoAroundCooldownSec)
    return false;

  // Gate 3: active runway must resolve to a concrete threshold.
  auto rwy_opt = resolve_active_runway_for_go_around(ctx);
  if (!rwy_opt.has_value())
    return false;

  // Gate 4: user within 1 NM of the threshold.
  const double user_dist_nm = traffic_geometry::distance_nm(
      rwy_opt->threshold_lat, rwy_opt->threshold_lon, ctx.latitude,
      ctx.longitude);
  if (user_dist_nm > kGoAroundTriggerDistanceNm)
    return false;

  // Gate 5: runway-occupied scan via the same sequencing primitive
  // pattern_flow uses for the "continue approach" overlay. We can't
  // cheap-out to a single-target scan — the occupant may be the
  // second-nearest target rather than the first.
  const auto &traffic = traffic_context::current();
  landing_sequence::UserPosition user{ctx.latitude, ctx.longitude};
  auto seq =
      landing_sequence::compute_landing_sequence(traffic, user, *rwy_opt);
  if (!seq.runway_occupied)
    return false;

  // All gates passed — render the unsolicited go-around call. No state
  // change, no traffic_dialog ack hook: this is a controller flight
  // command, the pilot's reaction is to fly, not to speak.
  std::string text = atc_state_machine::render_traffic_advisory(
      {}, ctx, "go_around_traffic_runway");
  last_go_around_emit_secs_ = now_secs;
  if (out_text)
    *out_text = std::move(text);
  logging::info("Engine emitted go-around (user dist=%.2f NM, occupant id=%u)",
                user_dist_nm,
                seq.occupant.has_value() ? seq.occupant->modeS_id : 0u);
  return true;
}

bool poll_traffic_advisory(const xplane_context::XPlaneContext &ctx,
                           double now_secs, std::string *out_text) {
  using FT = xplane_context::FrequencyType;

  // Don't fire fresh advisories while the previous one hasn't been
  // acknowledged yet — the dialog is the gate, not the main ATCState.
  if (traffic_dialog::is_awaiting_ack())
    return false;

  traffic_advisor::UserState user;
  user.atc_state = atc_state_machine::get_state();
  user.on_active_atc_freq =
      ctx.frequency_type == FT::TOWER || ctx.frequency_type == FT::GROUND;
  user.lat = ctx.latitude;
  user.lon = ctx.longitude;
  user.alt_msl_ft = static_cast<double>(ctx.altitude_ft_msl);
  user.heading_deg = static_cast<double>(ctx.heading_true);
  // Ground track == heading_true is a small simplification (no wind
  // crab) but matches the precision the advisory geometry needs (clock
  // positions are rounded to the hour).
  user.track_deg = static_cast<double>(ctx.heading_true);
  user.groundspeed_kts = static_cast<double>(ctx.groundspeed_kts);
  user.on_ground = ctx.on_ground;
  user.target_has_mode_c_default = true;
  user.user_taxiing = flight_phase::get() == flight_phase::FlightPhase::TAXI;

  const auto &traffic = traffic_context::current();

  auto adv =
      traffic_advisor::evaluate(traffic, user, advisory_history_, now_secs);
  if (!adv.has_value())
    return false;

  std::string text = atc_state_machine::render_traffic_advisory(
      adv->vars, ctx, adv->template_key);
  traffic_advisor::mark_emitted(advisory_history_, adv->modeS_id, now_secs);
  // Ground-conflict advisories don't expect a voice ack — the pilot
  // reacts by stopping / giving way. Skip the dialog side-channel so
  // the next pilot transcript still flows through the normal ATC
  // pipeline.
  if (adv->requires_ack)
    traffic_dialog::on_advisory_emitted(adv->modeS_id);

  if (out_text)
    *out_text = std::move(text);

  logging::info("Engine emitted traffic advisory (target_id=%u, template=%s)",
                adv->modeS_id, adv->template_key.c_str());
  return true;
}

} // namespace engine
