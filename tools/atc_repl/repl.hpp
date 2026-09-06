/*
 * xp_wellys_vfr_atc - headless CLI
 *
 * Interactive REPL. Loads an initial context (scenario or default) and
 * dispatches commands: say/set/state/reset/load/help/quit. No readline,
 * plain std::getline — works with piped stdin for scripted replay.
 */

#ifndef ATC_REPL_REPL_HPP
#define ATC_REPL_REPL_HPP

#include "core/xplane_context.hpp"

#include <string>

namespace repl {

int run(xplane_context::XPlaneContext ctx, std::string pilot_callsign);

} // namespace repl

#endif
