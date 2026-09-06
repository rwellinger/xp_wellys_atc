/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef CORE_CROSS_COUNTRY_LOG_HPP
#define CORE_CROSS_COUNTRY_LOG_HPP

#include <string>
#include <vector>

// Per-transmission failure logging + per-flight logbook for the
// cross-country measurement session. PURELY OBSERVATIONAL — this module
// never feeds matching, routing or classification. It records every
// processed pilot transmission so the raw material to decide whether a
// phraseology fuzzy-layer or a location-aware proper-name recogniser is
// needed can be evaluated offline, while doubling as an ATC-side flight
// logbook. Each record pairs the pilot transmission with the controller
// reply it produced (atc_response), so clearances and the readbacks that
// answer them read as a complete exchange.
//
// One valid, pretty-printed JSON document per flight is written to
// <dir>/YYYY-MM-DD_HHMM_<DEP>-<DEST>.json (the "-<DEST>" suffix is added
// once an inbound call fixes the destination, renaming the file). The
// whole document is rewritten (atomically, temp file + rename) after every
// transmission, so it is always a complete, well-formed file with an
// up-to-date summary — no "flight finished" event is required.
//
// Flights are split (logging-only) at the real-world anchor: a DEPARTURE-
// type initial call (INITIAL_CALL_GROUND/_TOWER) from IDLE on the ground,
// once the open flight has already flown. Inbound calls belong to the
// current flight and set its destination; they never split. A split can
// also be forced via begin_new_flight(). The flight header carries
// "missing_initial_call": true when a controlled field (Tower/Ground/AFIS)
// is departed without any recognised initial call.
//
// SDK-free. Lives in the xp_atc_engine OBJECT lib (file I/O only, no
// XPLM headers) so atc_repl and the scenario tests can exercise it too.
namespace cross_country_log {

struct Entry {
  // ── Raw observation ────────────────────────────────────────────────
  std::string transcript; // raw Whisper output, unmodified (engine in.transcript)
  float quality = 0.0f;   // Whisper quality value

  // ── Tower side of the exchange ─────────────────────────────────────
  // The controller reply the pilot transmission produced (clearance,
  // instruction, correction or say-again). Empty = silent transition
  // (state changed but nothing was spoken). Pairing the reply into the
  // same record makes clearances and the readbacks that answer them
  // legible without a separate tower-only stream.
  std::string atc_response;

  // ── Classification result ──────────────────────────────────────────
  std::string intent;     // classified / rule-hinted intent name
  float confidence = 0.0f;

  // Which path carried this transmission:
  //   "rule_skip_lm"    — high-conf rule parse (or LM-not-ready), no LM call
  //   "lm_fallback"     — low-conf -> LM classification round-trip
  //   "clearance_match" — deterministic readback recognition vs clearance
  std::string path;

  bool lm_used = false; // true iff the LM backend was invoked
  bool lm_ready = false; // meaningful only when lm_used

  // "classified" | "unknown" | "tower_reported_garbled"
  std::string outcome;

  // ── Context at the moment the pilot spoke ──────────────────────────
  std::string state;        // ATC state name
  std::string flight_phase; // flight phase name

  // ── Flight-header source (only consumed when a new flight opens) ────
  // Used to name the per-flight file and fill its "flight" block; ignored
  // on transmissions appended to an already-open flight.
  std::string airport_id;     // ctx.nearest_airport_id at time of speech
  std::string pilot_callsign; // pilot callsign for this flight
  // ctx.frequency_type name (Tower/Ground/Info/...); scopes the
  // missing_initial_call header flag to controlled fields. Empty when no
  // context was available for this transmission.
  std::string frequency_type;

  // ── Raw material for offline failure attribution ───────────────────
  // The set of intents plausibly expected in this state (valid_intents
  // CSV). Combined with transcript + vrp_name this lets the evaluator
  // decide by hand whether a miss was a phraseology token or a proper
  // name — no behaviour-changing heuristic is involved.
  std::string expected_intent;
  bool vrp_name_set = false;
  std::string vrp_name;

  // READBACK only. When set, readback_missing_elements is serialised as
  // a JSON array; otherwise the field is written as null.
  bool is_readback = false;
  std::vector<std::string> readback_missing_elements;

  // Non-binding suggestion. Emitted (as a string) ONLY when the caller
  // flags a failure (outcome != "classified") or the LM fallback fired;
  // otherwise the field is written as null. The raw fields above remain
  // authoritative and let this suggestion be overruled by hand.
  // "phraseology" | "proper_name" | "mixed" | "unclear"
  bool emit_failure_locus = false;
  std::string failure_locus;
};

// Backend label ("openai" | "mistral" | "local") recorded by the loader
// at backend bring-up. Engine code never inspects settings::backend_mode
// (Backend Adapter Rule) — it only reads the label the backend layer
// reported here, which this module folds into each record.
void set_lm_backend(const std::string &backend);

// Set the directory the per-flight JSON logbook files are written to.
// Default: "flightlog" (relative -> X-Plane working directory). The
// plugin points this at <plugin>/data/flightlog. The directory is
// created lazily on the first write.
void set_dir(const std::string &dir);

// Force the next write() to start a fresh flight file, regardless of the
// automatic split heuristic. The currently open flight is already fully
// persisted on disk; this just drops it so the next transmission opens a
// new document. Lazy — no empty file is created if no transmission
// follows. Wired to the manual "new flight" button and to X-Plane
// reposition / plane-reload messages.
void begin_new_flight();

// Append one transmission to the current flight, recompute its summary
// and atomically rewrite the flight's JSON file. Thread-safe.
void write(const Entry &e);

} // namespace cross_country_log

#endif // CORE_CROSS_COUNTRY_LOG_HPP
