// Catch2 unit tests for the state-machine history deque introduced
// in support of the LSZG post-landing disambiguation work. Covers:
//   - transition_to() pushes prev state, caps at kHistoryCap
//   - previous_state() / get_history() / history_csv() reads
//   - was_recently_in() time-window semantics
//   - just_landed() domain helper
//   - init/stop/reset clear history; disregard preserves it

#include "atc/atc_state_machine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"

#include <catch2/catch_amalgamated.hpp>

using atc_state_machine::ATCState;

namespace {
xplane_context::XPlaneContext make_ctx_on_ground() {
  xplane_context::XPlaneContext ctx{};
  ctx.on_ground = true;
  ctx.facility_type = xplane_context::FacilityType::TOWERED;
  ctx.nearest_airport_id = "LSZG";
  return ctx;
}
} // namespace

TEST_CASE("history: previous_state empty after init", "[state_history]")
{
  atc_state_machine::init();
  REQUIRE(atc_state_machine::get_history().empty());
  REQUIRE(atc_state_machine::previous_state() == ATCState::IDLE);
}

TEST_CASE("history: set_state pushes previous into history", "[state_history]")
{
  atc_state_machine::init();
  atc_state_machine::set_state(ATCState::TAXI_CLEARED);
  REQUIRE(atc_state_machine::get_state() == ATCState::TAXI_CLEARED);
  REQUIRE(atc_state_machine::get_history().size() == 1);
  REQUIRE(atc_state_machine::previous_state() == ATCState::IDLE);

  atc_state_machine::set_state(ATCState::TOWER_CONTACT);
  REQUIRE(atc_state_machine::get_history().size() == 2);
  REQUIRE(atc_state_machine::previous_state() == ATCState::TAXI_CLEARED);
}

TEST_CASE("history: same-state transition is a no-op", "[state_history]")
{
  atc_state_machine::init();
  atc_state_machine::set_state(ATCState::TAXI_CLEARED);
  const std::size_t before = atc_state_machine::get_history().size();
  atc_state_machine::set_state(ATCState::TAXI_CLEARED);
  REQUIRE(atc_state_machine::get_history().size() == before);
}

TEST_CASE("history: caps at 8 oldest-first", "[state_history]")
{
  atc_state_machine::init();
  // Walk a long sequence of distinct states so every set_state pushes.
  ATCState walk[] = {
      ATCState::GROUND_CONTACT,    ATCState::TAXI_CLEARED,
      ATCState::TOWER_CONTACT,     ATCState::DEPARTURE_CLEARED,
      ATCState::PATTERN_ENTRY,     ATCState::LANDING_CLEARED,
      ATCState::TOUCH_AND_GO_CLEARED, ATCState::PATTERN_ENTRY,
      ATCState::LANDING_CLEARED,   ATCState::IDLE,
  };
  for (auto s : walk)
    atc_state_machine::set_state(s);
  REQUIRE(atc_state_machine::get_history().size() == 8);
  // The two oldest pushes (initial IDLE -> GROUND_CONTACT and
  // GROUND_CONTACT -> TAXI_CLEARED) must have been dropped, so the
  // front entry is now the third push (the prev-state TAXI_CLEARED).
  REQUIRE(atc_state_machine::get_history().front().state ==
          ATCState::TAXI_CLEARED);
}

TEST_CASE("history: was_recently_in respects time window",
          "[state_history]")
{
  atc_state_machine::init();
  xplane_context::XPlaneContext ctx = make_ctx_on_ground();
  intent_parser::PilotMessage msg;
  msg.intent = intent_parser::PilotIntent::UNKNOWN;
  // Walk through LANDING_CLEARED via process() so we get a real
  // timestamp on the history entry.
  atc_state_machine::set_state(ATCState::LANDING_CLEARED);
  // set_state used last_now_secs_ which is 0 right after init — fine.
  // Now transition further so LANDING_CLEARED moves into history.
  atc_state_machine::set_state(ATCState::IDLE);

  // Within window: query with now_secs=10 (history entry is at 0).
  REQUIRE(atc_state_machine::was_recently_in(ATCState::LANDING_CLEARED, 60.0,
                                              10.0));
  // Outside window: now_secs=120 with window=60 (entry at t=0, age=120>60).
  REQUIRE_FALSE(atc_state_machine::was_recently_in(ATCState::LANDING_CLEARED,
                                                    60.0, 120.0));
}

TEST_CASE("history: was_recently_in returns true for current state",
          "[state_history]")
{
  atc_state_machine::init();
  atc_state_machine::set_state(ATCState::LANDING_CLEARED);
  REQUIRE(atc_state_machine::was_recently_in(ATCState::LANDING_CLEARED, 1.0,
                                              999.0));
}

TEST_CASE("history: just_landed after LANDING_CLEARED -> IDLE",
          "[state_history]")
{
  atc_state_machine::init();
  atc_state_machine::set_state(ATCState::LANDING_CLEARED);
  atc_state_machine::set_state(ATCState::IDLE);
  REQUIRE(atc_state_machine::just_landed(60.0));
  // Outside default 120 s window, no longer "just" landed.
  REQUIRE_FALSE(atc_state_machine::just_landed(200.0));
}

TEST_CASE("history: just_landed false on cold IDLE", "[state_history]")
{
  atc_state_machine::init();
  REQUIRE_FALSE(atc_state_machine::just_landed(0.0));
  REQUIRE_FALSE(atc_state_machine::just_landed(500.0));
}

TEST_CASE("history: just_landed true after TOUCH_AND_GO_CLEARED",
          "[state_history]")
{
  atc_state_machine::init();
  atc_state_machine::set_state(ATCState::TOUCH_AND_GO_CLEARED);
  atc_state_machine::set_state(ATCState::PATTERN_ENTRY);
  REQUIRE(atc_state_machine::just_landed(30.0));
}

TEST_CASE("history: reset clears", "[state_history]")
{
  atc_state_machine::init();
  atc_state_machine::set_state(ATCState::LANDING_CLEARED);
  atc_state_machine::set_state(ATCState::IDLE);
  REQUIRE_FALSE(atc_state_machine::get_history().empty());
  atc_state_machine::reset();
  REQUIRE(atc_state_machine::get_history().empty());
  REQUIRE_FALSE(atc_state_machine::just_landed(0.0));
}

TEST_CASE("history: disregard does NOT clear", "[state_history]")
{
  atc_state_machine::init();
  atc_state_machine::set_state(ATCState::LANDING_CLEARED);
  // disregard on the ground -> IDLE. The previous state (LANDING_CLEARED)
  // must remain visible so post-landing classifiers can still see it.
  xplane_context::XPlaneContext ctx = make_ctx_on_ground();
  atc_state_machine::disregard(ctx, flight_phase::FlightPhase::PARKED, 5.0);
  REQUIRE(atc_state_machine::get_state() == ATCState::IDLE);
  REQUIRE(atc_state_machine::just_landed(60.0));
}

TEST_CASE("history: history_csv includes current state at end",
          "[state_history]")
{
  atc_state_machine::init();
  atc_state_machine::set_state(ATCState::LANDING_CLEARED);
  atc_state_machine::set_state(ATCState::IDLE);
  // Two history entries (IDLE -> LANDING_CLEARED, LANDING_CLEARED -> IDLE
  // pushed prev IDLE and LANDING_CLEARED), current state IDLE appended.
  // Pattern/ prefix added in step 3b of the A1 flow-split refactor —
  // LANDING_CLEARED is a Pattern-flow state.
  REQUIRE(atc_state_machine::history_csv() ==
          std::string{"IDLE,Pattern/LANDING_CLEARED,IDLE"});
}
