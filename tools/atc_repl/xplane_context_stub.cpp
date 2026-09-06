/*
 * xp_wellys_vfr_atc - headless CLI
 *
 * Engine modules link against xplane_context::get() (declared in
 * xplane_context.hpp). In the plugin, xplane_context_runtime.cpp
 * provides it; in the CLI we expose a single mutable ctx that the
 * REPL main primes before each process_transcript call.
 */

#include "core/xplane_context.hpp"

#include <cstdint>

namespace xplane_context {

// Definition lives here (CLI-only); plugin has its own in the runtime.
XPlaneContext g_cli_ctx;

const XPlaneContext &get() { return g_cli_ctx; }

void set_standby_freq(uint32_t) {}

void lock_airport(const std::string &) {}
void unlock_airport() {}
const std::string &locked_airport() noexcept {
  static const std::string empty;
  return empty;
}

std::vector<NearbyAirport> find_nearby_airports(double, size_t) { return {}; }

float airport_elevation_ft(const std::string &) { return 0.0f; }
bool airport_elevation_known(const std::string &) { return false; }

void init() {}
void stop() {}
void update() {}

} // namespace xplane_context
