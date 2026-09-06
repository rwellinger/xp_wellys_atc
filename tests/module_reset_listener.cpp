// Catch2 module-reset listener — Issue #3.
//
// The SDK-free engine modules hold mutable process-global state behind
// independent module boundaries, each with its own reset entry point:
//
//   * atc_state_machine.cpp : static AtcMachineState g_state
//                             (was_airborne_, history_, state_, ...).
//                             Reset via init() (captures the flight-loop
//                             thread id) + reset() (full field sweep,
//                             cascades crosscountry_flow::reset()).
//   * flight_phase.cpp      : current_phase_ / candidate_phase_ /
//                             candidate_timer_ / was_airborne_ runtime
//                             statics (config maps untouched). Reset via
//                             flight_phase::reset() — without it an airborne
//                             test leaves current_phase_ at CLIMB/CRUISE and
//                             the phase-precondition guard early-returns for
//                             a later ground intent (the residual #3 leak).
//   * atis_generator.cpp    : letter_ / last_* statics. Reset via init().
//   * settings (test build) : the headless stub's g_bzf_strict_mode /
//                             g_vfr_* / g_pilot_callsign. Reset via
//                             settings::reset_for_test().
//
// There is NO single global to clear, so this one listener calls each
// module's own reset before every test case. Running it from a listener
// (rather than per-file setup) covers tests that forget to reset — which
// is exactly the latent class of bug #3 documents (rand red / decl green).
//
// Order matters: atc_state_machine::init() must run before reset(),
// because reset() asserts it is on the captured flight-loop thread.

#include "atc/atc_state_machine.hpp"
#include "atc/atis_generator.hpp"
#include "atc/flight_phase.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

namespace {

class ModuleResetListener : public Catch::EventListenerBase {
public:
  using Catch::EventListenerBase::EventListenerBase;

  void testCaseStarting(Catch::TestCaseInfo const & /*info*/) override {
    // 1. init() captures this (the test runner) thread as the flight-loop
    //    thread and resets g_state; reset() then does the full sweep incl.
    //    last_tower_response_text_ and the crosscountry_flow cascade.
    atc_state_machine::init();
    atc_state_machine::reset();
    // 2. Runtime flight phase back to PARKED (config left loaded), so a
    //    leftover airborne phase cannot block a later ground intent.
    flight_phase::reset();
    // 3. ATIS information letter + weather baseline back to 'A' / defaults.
    atis_generator::init();
    // 4. bzf_strict_mode + VFR intention + callsign back to defaults.
    settings::reset_for_test();
  }
};

} // namespace

CATCH_REGISTER_LISTENER(ModuleResetListener)
