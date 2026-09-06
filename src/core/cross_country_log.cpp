/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "core/cross_country_log.hpp"

#include <json.hpp>

#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h> // _mkdir (no mode argument on Windows)
#endif

#include <cstdio> // std::rename
#include <ctime>
#include <fstream>
#include <mutex>
#include <set>

namespace cross_country_log {

namespace {

// ordered_json keeps the document field order (version, flight, summary,
// transmissions) and the per-transmission field order stable, which makes
// the logbook pleasant to read by hand.
using json = nlohmann::ordered_json;

std::mutex g_mtx;                // guards everything below
std::string g_dir = "flightlog"; // output directory (relative by default)
std::string g_lm_backend;        // backend label set by the loader

bool g_force_new = false; // force a fresh flight on the next write()

bool g_flight_open = false;  // a flight document is currently in memory
bool g_was_airborne = false; // the open flight has been airborne at least once
json g_flight;               // the open flight document
std::string g_flight_path;   // its target file path

// Per-flight naming + compliance state, all reset by open_flight().
std::string g_departure_id;      // sanitized departure airport (file name)
std::string g_destination_id;    // sanitized destination, set on inbound call
std::string g_flight_filestamp;  // frozen at open_flight so a rename keeps it
bool g_saw_initial_call = false; // any INITIAL_CALL_* seen this flight
bool g_departure_controlled = false; // field was controlled (Tower/Ground/AFIS)

// ── Wall-clock helpers (SDK-free) ─────────────────────────────────────
struct TimeParts {
  std::string datetime;  // YYYY-MM-DDTHH:MM:SS  (started_at / per-tx time)
  std::string filestamp; // YYYY-MM-DD_HHMM      (file name)
  long long epoch = 0;   // Unix epoch seconds (UTC) — same clock xp_pilot logs
};

TimeParts now_parts() {
  std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  char dt[32] = {};
  char fs[32] = {};
  std::strftime(dt, sizeof(dt), "%Y-%m-%dT%H:%M:%S", &tm_buf);
  std::strftime(fs, sizeof(fs), "%Y-%m-%d_%H%M", &tm_buf);
  // Keep the raw epoch alongside the local-time strings so the trainer can
  // correlate transmissions against xp_pilot's UTC epoch track without
  // reconstructing a TZ/DST offset (Issue #17).
  return {dt, fs, static_cast<long long>(t)};
}

bool is_airborne_phase(const std::string &p) {
  return p == "TAKEOFF_ROLL" || p == "CLIMB" || p == "PATTERN" ||
         p == "FINAL_APPROACH" || p == "LANDING_ROLL" || p == "CRUISE";
}

bool is_ground_phase(const std::string &p) {
  return p == "PARKED" || p == "TAXI";
}

// Departure-type initial call: the real-world anchor that begins a new
// flight (pilot calls Ground/Tower for departure). The generic
// INITIAL_CALL maps to the tower default, so it counts as departure too.
bool is_departure_initial_call(const std::string &i) {
  return i == "INITIAL_CALL_GROUND" || i == "INITIAL_CALL_TOWER" ||
         i == "INITIAL_CALL";
}

// Inbound initial call: belongs to the current flight and supplies its
// destination — never starts a new flight.
bool is_inbound_initial_call(const std::string &i) {
  return i == "INITIAL_CALL_INBOUND" || i == "INITIAL_CALL_INBOUND_VRP";
}

bool is_any_initial_call(const std::string &i) {
  return is_departure_initial_call(i) || is_inbound_initial_call(i);
}

// Controlled field where an initial call is expected (Issue #16 scope:
// Tower / Ground / AFIS). The strings match xplane_context::
// frequency_type_name() exactly (mixed case). UNICOM/CTAF self-announce
// fields are out of scope.
bool is_controlled_field(const std::string &ft) {
  return ft == "Tower" || ft == "Ground" || ft == "Delivery" || ft == "Info" ||
         ft == "Radio";
}

bool file_exists(const std::string &path) {
  std::ifstream f(path);
  return f.good();
}

std::string sanitize_airport(const std::string &id) {
  std::string out;
  for (char c : id) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      out += c;
    else if (c >= 'a' && c <= 'z')
      out += static_cast<char>(c - 'a' + 'A');
  }
  return out.empty() ? "UNKNOWN" : out;
}

// Build the per-transmission JSON object. Field order mirrors the Entry
// struct; the leading "time" stamps the logbook entry.
json entry_to_json(const Entry &e, const TimeParts &parts) {
  json j;
  j["time"] = parts.datetime;
  j["ts"] = parts.epoch; // Unix epoch seconds (UTC) for trainer correlation
  j["transcript"] = e.transcript;
  j["quality"] = e.quality;
  j["atc_response"] = e.atc_response;
  j["intent"] = e.intent;
  j["confidence"] = e.confidence;
  j["path"] = e.path;

  j["lm_used"] = e.lm_used;
  if (e.lm_used) {
    j["lm_backend"] = g_lm_backend.empty() ? json(nullptr) : json(g_lm_backend);
    j["lm_ready"] = e.lm_ready;
  } else {
    j["lm_backend"] = nullptr;
    j["lm_ready"] = nullptr;
  }

  j["outcome"] = e.outcome;
  j["state"] = e.state;
  j["flight_phase"] = e.flight_phase;

  j["expected_intent"] = e.expected_intent;
  j["vrp_name_set"] = e.vrp_name_set;
  j["vrp_name"] = e.vrp_name_set ? json(e.vrp_name) : json(nullptr);

  if (e.is_readback)
    j["readback_missing_elements"] = e.readback_missing_elements;
  else
    j["readback_missing_elements"] = nullptr;

  j["failure_locus"] =
      e.emit_failure_locus ? json(e.failure_locus) : json(nullptr);
  return j;
}

// Recompute the flight summary purely from the logged transmissions — no
// behaviour-changing classification, just counting what was recorded.
json compute_summary(const json &transmissions) {
  int classified = 0, unknown = 0, garbled = 0, lm_fallbacks = 0,
      readback_issues = 0;
  std::vector<std::string> phases;
  std::set<std::string> seen_phases;

  for (const auto &t : transmissions) {
    const std::string outcome = t.value("outcome", "");
    if (outcome == "classified")
      ++classified;
    else if (outcome == "unknown")
      ++unknown;
    else if (outcome == "tower_reported_garbled")
      ++garbled;

    if (t.value("lm_used", false))
      ++lm_fallbacks;

    auto rb = t.find("readback_missing_elements");
    if (rb != t.end() && rb->is_array() && !rb->empty())
      ++readback_issues;

    const std::string phase = t.value("flight_phase", "");
    if (!phase.empty() && seen_phases.insert(phase).second)
      phases.push_back(phase);
  }

  json s;
  s["transmissions"] = transmissions.size();
  s["classified"] = classified;
  s["unknown"] = unknown;
  s["garbled"] = garbled;
  s["lm_fallbacks"] = lm_fallbacks;
  s["readback_issues"] = readback_issues;
  s["phases"] = phases;
  return s;
}

// Pick a not-yet-existing file name for the open flight. The airport part
// is "<DEP>" until a destination is known, then "<DEP>-<DEST>". The
// timestamp is frozen at open time (g_flight_filestamp) so a later rename
// keeps it. `ignore` lets a rename skip the file it is renaming away from
// when probing for collisions.
std::string pick_flight_path(const std::string &ignore = "") {
  const std::string airport_part =
      g_destination_id.empty() ? g_departure_id
                               : (g_departure_id + "-" + g_destination_id);
  const std::string base =
      g_dir + "/" + g_flight_filestamp + "_" + airport_part;
  std::string candidate = base + ".json";
  for (int n = 2; candidate != ignore && file_exists(candidate); ++n)
    candidate = base + "_" + std::to_string(n) + ".json";
  return candidate;
}

// Open a fresh flight document in memory using `e` as the header source.
void open_flight(const Entry &e, const TimeParts &parts) {
  g_departure_id = sanitize_airport(e.airport_id);
  g_destination_id.clear();
  g_flight_filestamp = parts.filestamp;
  g_saw_initial_call = false;
  g_departure_controlled = is_controlled_field(e.frequency_type);

  g_flight = json::object();
  g_flight["version"] = 2;
  g_flight["flight"] = {{"started_at", parts.datetime},
                        {"started_at_epoch", parts.epoch},
                        {"departure_airport", e.airport_id.empty()
                                                  ? json(nullptr)
                                                  : json(e.airport_id)},
                        {"destination_airport", nullptr},
                        {"missing_initial_call", false},
                        {"pilot_callsign", e.pilot_callsign}};
  g_flight["summary"] = compute_summary(json::array());
  g_flight["transmissions"] = json::array();

  g_flight_path = pick_flight_path();
  g_flight_open = true;
  g_was_airborne = false;
}

// Rename the on-disk flight file once a destination is known, so the name
// becomes "<DEP>-<DEST>.json". Atomic std::rename on the same volume; the
// subsequent flush rewrites the updated content, leaving no stale
// "<DEP>.json" behind.
void rename_flight_file() {
  const std::string new_path = pick_flight_path(g_flight_path);
  if (new_path == g_flight_path)
    return;
  std::rename(g_flight_path.c_str(), new_path.c_str()); // best-effort
  g_flight_path = new_path;
}

// Atomically rewrite the open flight to disk: temp file + rename. A failed
// open must never disturb the sim, so this is best-effort.
void flush_flight() {
  // lazy; -1/EEXIST is fine. POSIX mkdir takes a mode; Windows _mkdir does not.
#if defined(_WIN32)
  _mkdir(g_dir.c_str());
#else
  mkdir(g_dir.c_str(), 0755);
#endif

  // Compact-but-readable pretty print; invalid UTF-8 bytes replaced rather
  // than thrown — a German VRP name with a stray byte must not abort a run.
  const std::string text =
      g_flight.dump(2, ' ', false, json::error_handler_t::replace);

  const std::string tmp = g_flight_path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f)
      return;
    f << text << '\n';
    if (!f)
      return;
  }
  std::rename(tmp.c_str(), g_flight_path.c_str());
}

} // namespace

void set_lm_backend(const std::string &backend) {
  std::lock_guard<std::mutex> lk(g_mtx);
  g_lm_backend = backend;
}

void set_dir(const std::string &dir) {
  std::lock_guard<std::mutex> lk(g_mtx);
  g_dir = dir;
}

void begin_new_flight() {
  std::lock_guard<std::mutex> lk(g_mtx);
  g_force_new = true;
}

void write(const Entry &e) {
  const TimeParts parts = now_parts();

  std::lock_guard<std::mutex> lk(g_mtx);

  // Decide whether this transmission starts a new flight. The anchor is
  // logging-only and real-world conformant: a DEPARTURE-type initial call
  // from IDLE on the ground starts a new flight, but only once the open
  // flight has actually flown (g_was_airborne) — so a pre-departure radio
  // check that opened the file is not split off from the initial call that
  // follows it. Inbound calls and taxi chatter in IDLE never split. The
  // force flag (manual button / X-Plane reposition) always wins.
  bool need_new = !g_flight_open || g_force_new;
  if (!need_new && g_was_airborne && e.state == "IDLE" &&
      is_ground_phase(e.flight_phase) && is_departure_initial_call(e.intent))
    need_new = true;

  if (need_new) {
    open_flight(e, parts);
    g_force_new = false;
  }

  g_flight["transmissions"].push_back(entry_to_json(e, parts));

  // Compliance tracking. Collect controlled-field status only before the
  // flight goes airborne (robust against an en-route frequency change),
  // and freeze missing_initial_call at the first airborne transition.
  if (is_any_initial_call(e.intent))
    g_saw_initial_call = true;
  if (!g_was_airborne)
    g_departure_controlled |= is_controlled_field(e.frequency_type);
  if (!g_was_airborne && is_airborne_phase(e.flight_phase))
    g_flight["flight"]["missing_initial_call"] =
        g_departure_controlled && !g_saw_initial_call;

  if (is_airborne_phase(e.flight_phase))
    g_was_airborne = true;

  // First inbound initial call to a different field fixes the destination
  // and renames the file to "<DEP>-<DEST>.json".
  if (g_flight_open && is_inbound_initial_call(e.intent)) {
    const std::string dest = sanitize_airport(e.airport_id);
    if (dest != "UNKNOWN" && dest != g_departure_id &&
        dest != g_destination_id) {
      g_destination_id = dest;
      g_flight["flight"]["destination_airport"] =
          e.airport_id.empty() ? json(nullptr) : json(e.airport_id);
      rename_flight_file();
    }
  }

  g_flight["summary"] = compute_summary(g_flight["transmissions"]);
  flush_flight();
}

} // namespace cross_country_log
