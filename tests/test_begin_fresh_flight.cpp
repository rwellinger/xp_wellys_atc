/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

// Unit tests for atc_state_machine::begin_fresh_flight — the guarded
// between-flights reset wired to XPLM_MSG_AIRPORT_LOADED / PLANE_LOADED.
// Proves:
//   (a) a genuine ground reposition in IDLE/GROUND_CONTACT lands in a
//       clean IDLE with empty history and cleared session locks;
//   (b) a mid-flight or stale-airborne message is a strict no-op — state,
//       history and the revert-guard generation counter all unchanged.

#include "atc/atc_state_machine.hpp"

#include <catch2/catch_amalgamated.hpp>

using atc_state_machine::ATCState;
using atc_state_machine::begin_fresh_flight;
using atc_state_machine::current_gen;

TEST_CASE("begin_fresh_flight: ground reposition lands in clean IDLE",
          "[begin_fresh_flight]") {
  atc_state_machine::init();
  // Dirty the machine the way a finished/ongoing flight would leave it,
  // but stay in an allowed guard state on the ground. set_state pushes the
  // previous state into history and locks nothing on entry; we additionally
  // arm was_airborne to prove the sweep clears it.
  atc_state_machine::set_state(ATCState::GROUND_CONTACT);
  atc_state_machine::set_was_airborne(true);
  REQUIRE_FALSE(atc_state_machine::get_history().empty());

  begin_fresh_flight(/*on_ground=*/true);

  REQUIRE(atc_state_machine::get_state() == ATCState::IDLE);
  REQUIRE(atc_state_machine::get_history().empty());
  REQUIRE(atc_state_machine::assigned_runway().empty());
  REQUIRE(atc_state_machine::session_callsign().empty());
  REQUIRE_FALSE(atc_state_machine::was_airborne());
}

TEST_CASE("begin_fresh_flight: in-progress state blocks reset (no-op)",
          "[begin_fresh_flight]") {
  atc_state_machine::init();
  atc_state_machine::set_state(ATCState::PATTERN_ENTRY); // not IDLE/GROUND_CONTACT
  const std::size_t hist_before = atc_state_machine::get_history().size();
  const uint64_t gen_before = current_gen();

  begin_fresh_flight(/*on_ground=*/true); // guard blocks on state

  REQUIRE(atc_state_machine::get_state() == ATCState::PATTERN_ENTRY);
  REQUIRE(atc_state_machine::get_history().size() == hist_before);
  REQUIRE(current_gen() == gen_before); // strict no-op: gen unchanged
}

TEST_CASE("begin_fresh_flight: stale-airborne blocks reset even in IDLE",
          "[begin_fresh_flight]") {
  atc_state_machine::init();
  // IDLE is an allowed state, but on_ground=false (stale-airborne reposition
  // out of flight) must still block — the guard requires BOTH conditions.
  REQUIRE(atc_state_machine::get_state() == ATCState::IDLE);
  const uint64_t gen_before = current_gen();

  begin_fresh_flight(/*on_ground=*/false); // guard blocks on on_ground

  REQUIRE(atc_state_machine::get_state() == ATCState::IDLE);
  REQUIRE(current_gen() == gen_before); // strict no-op: gen unchanged
}

TEST_CASE("begin_fresh_flight: GROUND_CONTACT on the ground is allowed",
          "[begin_fresh_flight]") {
  atc_state_machine::init();
  atc_state_machine::set_state(ATCState::GROUND_CONTACT);

  begin_fresh_flight(/*on_ground=*/true);

  REQUIRE(atc_state_machine::get_state() == ATCState::IDLE);
  REQUIRE(atc_state_machine::get_history().empty());
}
