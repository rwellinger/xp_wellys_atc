/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

// Tests for the per-flight cross-country logbook (Issue #16):
//   - flights split only on a DEPARTURE-type initial call from IDLE on the
//     ground, once the open flight has already flown;
//   - an inbound call sets the destination and renames the file to
//     "<DEP>-<DEST>.json", leaving no stale "<DEP>.json" behind;
//   - a controlled-field departure without any initial call sets the
//     "missing_initial_call" header flag; an uncontrolled field does not.
//
// The module-reset listener does NOT touch cross_country_log globals, so
// each case isolates itself with a unique set_dir() plus begin_new_flight()
// (which forces a fresh flight on the next write).

#include "core/cross_country_log.hpp"

#include <json.hpp>

#include <catch2/catch_amalgamated.hpp>

#include <dirent.h>
#include <sys/stat.h>

#include <fstream>
#include <string>
#include <vector>

using nlohmann::json;

namespace {

// A unique, empty output directory per case under build/ (CWD is the repo
// root when the test binary runs). Any leftovers from a previous run are
// removed so the file-count assertions are exact.
std::string fresh_dir(const std::string &name) {
  mkdir("build", 0755); // -1/EEXIST is fine
  std::string dir = "build/cc_log_test_";
  dir += name;
  mkdir(dir.c_str(), 0755);
  if (DIR *d = opendir(dir.c_str())) {
    while (dirent *ent = readdir(d)) {
      const std::string n = ent->d_name;
      if (n == "." || n == "..")
        continue;
      std::string path = dir;
      path += "/";
      path += n;
      ::remove(path.c_str());
    }
    closedir(d);
  }
  return dir;
}

std::vector<std::string> list_json(const std::string &dir) {
  std::vector<std::string> out;
  if (DIR *d = opendir(dir.c_str())) {
    while (dirent *ent = readdir(d)) {
      const std::string n = ent->d_name;
      if (n.size() > 5 && n.substr(n.size() - 5) == ".json")
        out.push_back(n);
    }
    closedir(d);
  }
  return out;
}

bool ends_with(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

json read_only_flight(const std::string &dir) {
  const auto files = list_json(dir);
  REQUIRE(files.size() == 1);
  std::ifstream f(dir + "/" + files[0]);
  json j;
  f >> j;
  return j;
}

cross_country_log::Entry entry(const std::string &intent,
                               const std::string &state,
                               const std::string &phase,
                               const std::string &airport,
                               const std::string &freq) {
  cross_country_log::Entry e;
  e.transcript = "test";
  e.quality = 0.9f;
  e.intent = intent;
  e.confidence = 0.9f;
  e.path = "rule_skip_lm";
  e.outcome = "classified";
  e.state = state;
  e.flight_phase = phase;
  e.airport_id = airport;
  e.pilot_callsign = "DEABC";
  e.frequency_type = freq;
  return e;
}

// Start a clean flight in a unique dir, isolated from other cases.
std::string begin_case(const std::string &name) {
  const std::string dir = fresh_dir(name);
  cross_country_log::set_dir(dir);
  cross_country_log::begin_new_flight();
  return dir;
}

} // namespace

TEST_CASE("cc_log: cross-country EDNY->EDTZ stays one file with destination",
          "[cross_country_log]") {
  const std::string dir = begin_case("xc_one_file");

  // Departure from EDNY: ground initial call, on the ground in IDLE.
  cross_country_log::write(
      entry("INITIAL_CALL_GROUND", "IDLE", "PARKED", "EDNY", "Ground"));
  // Climb out / en-route.
  cross_country_log::write(
      entry("REPORT_POSITION", "DEPARTURE_CLEARED", "CLIMB", "EDNY", "Tower"));
  cross_country_log::write(
      entry("LEAVING_FREQUENCY", "EN_ROUTE", "CRUISE", "EDNY", "Unknown"));
  // Airport lock flips to EDTZ -> state machine reverts to IDLE; the
  // inbound call arrives FROM IDLE but must NOT split the flight.
  cross_country_log::write(
      entry("INITIAL_CALL_INBOUND", "IDLE", "CRUISE", "EDTZ", "Tower"));
  // Land + taxi back in IDLE at EDTZ: still the same flight.
  cross_country_log::write(
      entry("RUNWAY_VACATED", "IDLE", "TAXI", "EDTZ", "Ground"));

  const auto files = list_json(dir);
  REQUIRE(files.size() == 1);
  REQUIRE(ends_with(files[0], "_EDNY-EDTZ.json"));

  const json j = read_only_flight(dir);
  REQUIRE(j["flight"]["departure_airport"] == "EDNY");
  REQUIRE(j["flight"]["destination_airport"] == "EDTZ");
  REQUIRE(j["summary"]["transmissions"] == 5);
}

TEST_CASE("cc_log: new departure from destination opens a second file",
          "[cross_country_log]") {
  const std::string dir = begin_case("second_departure");

  cross_country_log::write(
      entry("INITIAL_CALL_GROUND", "IDLE", "PARKED", "EDNY", "Ground"));
  cross_country_log::write(
      entry("REPORT_POSITION", "DEPARTURE_CLEARED", "CLIMB", "EDNY", "Tower"));
  cross_country_log::write(
      entry("INITIAL_CALL_INBOUND", "IDLE", "CRUISE", "EDTZ", "Tower"));
  cross_country_log::write(
      entry("RUNWAY_VACATED", "IDLE", "TAXI", "EDTZ", "Ground"));
  // A fresh ground initial call from IDLE on the ground, after the flight
  // already flew, is the real-world anchor for a new flight.
  cross_country_log::write(
      entry("INITIAL_CALL_GROUND", "IDLE", "PARKED", "EDTZ", "Ground"));

  auto files = list_json(dir);
  REQUIRE(files.size() == 2);

  bool saw_xc = false, saw_second = false;
  for (const auto &f : files) {
    if (ends_with(f, "_EDNY-EDTZ.json"))
      saw_xc = true;
    if (ends_with(f, "_EDTZ.json"))
      saw_second = true;
  }
  REQUIRE(saw_xc);
  REQUIRE(saw_second);
}

TEST_CASE("cc_log: pre-departure radio check does not split off the flight",
          "[cross_country_log]") {
  const std::string dir = begin_case("radio_check");

  // Radio check opens the file but the flight has not flown yet.
  cross_country_log::write(
      entry("RADIO_CHECK", "IDLE", "PARKED", "EDNY", "Ground"));
  // The initial call that follows must stay in the SAME flight.
  cross_country_log::write(
      entry("INITIAL_CALL_GROUND", "IDLE", "PARKED", "EDNY", "Ground"));

  const auto files = list_json(dir);
  REQUIRE(files.size() == 1);
  const json j = read_only_flight(dir);
  REQUIRE(j["summary"]["transmissions"] == 2);
}

TEST_CASE("cc_log: controlled-field departure without initial call is flagged",
          "[cross_country_log]") {
  const std::string dir = begin_case("missing_controlled");

  // Tower field, but the pilot never makes an initial call before flying.
  cross_country_log::write(
      entry("REQUEST_TAXI", "IDLE", "PARKED", "EDNY", "Tower"));
  cross_country_log::write(
      entry("REPORT_POSITION", "IDLE", "CLIMB", "EDNY", "Tower"));

  const json j = read_only_flight(dir);
  REQUIRE(j["flight"]["missing_initial_call"] == true);
}

TEST_CASE("cc_log: uncontrolled-field departure is out of scope (not flagged)",
          "[cross_country_log]") {
  const std::string dir = begin_case("missing_uncontrolled");

  // UNICOM self-announce field: no controller to call, so no flag.
  cross_country_log::write(
      entry("SELF_ANNOUNCE", "UNICOM_ACTIVE", "PARKED", "EDXY", "Unicom"));
  cross_country_log::write(
      entry("SELF_ANNOUNCE", "UNICOM_ACTIVE", "CLIMB", "EDXY", "Unicom"));

  const json j = read_only_flight(dir);
  REQUIRE(j["flight"]["missing_initial_call"] == false);
}

TEST_CASE("cc_log: initial call present clears the missing flag",
          "[cross_country_log]") {
  const std::string dir = begin_case("present_initial");

  cross_country_log::write(
      entry("INITIAL_CALL_GROUND", "IDLE", "PARKED", "EDNY", "Ground"));
  cross_country_log::write(
      entry("REPORT_POSITION", "DEPARTURE_CLEARED", "CLIMB", "EDNY", "Tower"));

  const json j = read_only_flight(dir);
  REQUIRE(j["flight"]["missing_initial_call"] == false);
}

// Issue #17: the schema carries a UTC epoch alongside the local-time strings
// so the trainer can correlate transmissions against xp_pilot's epoch track
// without reconstructing a TZ/DST offset.
TEST_CASE("cc_log: epoch fields emitted for trainer correlation",
          "[cross_country_log]") {
  const std::string dir = begin_case("epoch_fields");

  cross_country_log::write(
      entry("INITIAL_CALL_GROUND", "IDLE", "PARKED", "EDNY", "Ground"));
  cross_country_log::write(
      entry("REPORT_POSITION", "DEPARTURE_CLEARED", "CLIMB", "EDNY", "Tower"));

  const json j = read_only_flight(dir);

  REQUIRE(j["version"] == 2);

  // Flight header epoch is a positive integer (a plausible Unix timestamp).
  const auto &flight = j["flight"];
  REQUIRE(flight.contains("started_at_epoch"));
  REQUIRE(flight["started_at_epoch"].is_number_integer());
  REQUIRE(flight["started_at_epoch"].get<long long>() > 1'000'000'000LL);

  // Every transmission carries an integer "ts" next to its local-time "time".
  for (const auto &t : j["transmissions"]) {
    REQUIRE(t.contains("ts"));
    REQUIRE(t["ts"].is_number_integer());
    REQUIRE(t["ts"].get<long long>() > 1'000'000'000LL);
  }
}
