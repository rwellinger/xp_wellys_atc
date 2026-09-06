#include "atc/atc_state_machine.hpp"
#include "core/xplane_context.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

// Locked-runway behavior — corner cases that don't require full template
// loading. Integration coverage (clearance -> lock -> consumers) lives in the
// scenario tests / `make repl` flow.

TEST_CASE("effective_runway: empty lock falls through to ctx.active_runway",
          "[runway_lock]")
{
    atc_state_machine::init(); // ensure clean state

    xplane_context::XPlaneContext ctx{};
    ctx.active_runway = "06";

    REQUIRE(atc_state_machine::assigned_runway().empty());
    REQUIRE(atc_state_machine::effective_runway(ctx) == "06");
}

TEST_CASE("effective_runway: empty lock + empty active = empty",
          "[runway_lock]")
{
    atc_state_machine::init();

    xplane_context::XPlaneContext ctx{};
    REQUIRE(atc_state_machine::effective_runway(ctx).empty());
}

TEST_CASE("set_state(IDLE) leaves lock empty when nothing was set",
          "[runway_lock]")
{
    atc_state_machine::init();
    atc_state_machine::set_state(atc_state_machine::ATCState::IDLE);
    REQUIRE(atc_state_machine::assigned_runway().empty());
}
