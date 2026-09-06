/*
 * xp_wellys_vfr_atc - headless CLI
 *
 * REPL command dispatcher. Context lives in xplane_context::g_cli_ctx
 * (same as the scenario runner); every command reads/writes it directly
 * so process_transcript sees current state.
 *
 * GPT fallback is always disabled here — process_transcript therefore
 * runs its callback synchronously on the calling thread.
 */

#include "repl.hpp"

#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/engine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/intent_parser.hpp"
#include "data/airport_vrps.hpp"
#include "scenario.hpp"
#include "persistence/settings.hpp"
#include "core/xplane_context.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>

namespace xplane_context {
extern XPlaneContext g_cli_ctx;
}

namespace repl {
namespace {

using xplane_context::FrequencyType;

std::string trim(std::string s) {
  auto notspace = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
  return s;
}

std::pair<std::string, std::string> split_first_word(const std::string &line) {
  auto sp = line.find_first_of(" \t");
  if (sp == std::string::npos) return {line, ""};
  return {line.substr(0, sp), trim(line.substr(sp + 1))};
}

bool parse_bool(const std::string &s, bool &out) {
  std::string lo = s;
  std::transform(lo.begin(), lo.end(), lo.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lo == "true" || lo == "yes" || lo == "1" || lo == "on") {
    out = true;
    return true;
  }
  if (lo == "false" || lo == "no" || lo == "0" || lo == "off") {
    out = false;
    return true;
  }
  return false;
}

bool parse_freq_type(const std::string &s, FrequencyType &out) {
  static const std::unordered_map<std::string, FrequencyType> kMap{
      {"UNKNOWN", FrequencyType::UNKNOWN},
      {"DELIVERY", FrequencyType::DELIVERY},
      {"GROUND", FrequencyType::GROUND},
      {"TOWER", FrequencyType::TOWER},
      {"UNICOM", FrequencyType::UNICOM},
      {"CTAF", FrequencyType::CTAF},
      {"ATIS", FrequencyType::ATIS},
      {"INFO", FrequencyType::INFO},
      {"RADIO", FrequencyType::RADIO},
  };
  std::string up = s;
  std::transform(up.begin(), up.end(), up.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  auto it = kMap.find(up);
  if (it == kMap.end()) return false;
  out = it->second;
  return true;
}

bool parse_facility(const std::string &s, xplane_context::FacilityType &out) {
  static const std::unordered_map<std::string, xplane_context::FacilityType>
      kMap{
          {"UNKNOWN", xplane_context::FacilityType::UNKNOWN},
          {"UNCONTROLLED", xplane_context::FacilityType::UNCONTROLLED},
          {"TOWERED", xplane_context::FacilityType::TOWERED},
          {"AFIS", xplane_context::FacilityType::AFIS},
      };
  std::string up = s;
  std::transform(up.begin(), up.end(), up.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  auto it = kMap.find(up);
  if (it == kMap.end()) return false;
  out = it->second;
  return true;
}

// flight_phase has a temporal-hysteresis window; the runner primes it
// with 30 one-second ticks so ground phases settle immediately.
void prime_flight_phase(const xplane_context::XPlaneContext &ctx) {
  for (int i = 0; i < 30; ++i)
    flight_phase::update(ctx, 1.0f);
}

void ensure_engine_fields(xplane_context::XPlaneContext &ctx) {
  ctx.com_radio_powered = true;
  ctx.avionics_on = true;
  if (ctx.aircraft_icao.empty()) ctx.aircraft_icao = "C172";
}

void print_state(const std::string &callsign) {
  const auto &ctx = xplane_context::g_cli_ctx;
  std::printf("Region:       %s\n", settings::atc_profile().c_str());
  std::printf("Airport:      %s%s%s%s, %s\n", ctx.nearest_airport_id.c_str(),
              ctx.nearest_airport_name.empty() ? "" : " (",
              ctx.nearest_airport_name.c_str(),
              ctx.nearest_airport_name.empty() ? "" : ")",
              xplane_context::facility_type_name(ctx.facility_type));
  std::printf("Runway:       %s   COM: %.3f (%s)\n", ctx.active_runway.c_str(),
              ctx.com1_freq_mhz,
              xplane_context::frequency_type_name(ctx.frequency_type));
  std::printf(
      "On ground:    %s   Alt: %.0fft   AGL: %.0fft   GS: %.0fkt\n",
      ctx.on_ground ? "yes" : "no", ctx.altitude_ft_msl, ctx.height_agl_ft,
      ctx.groundspeed_kts);
  std::printf("Flight phase: %s\n", flight_phase::phase_name(flight_phase::get()));
  std::printf("ATC state:    %s\n",
              atc_state_machine::state_name(atc_state_machine::get_state()));
  std::printf("Callsign:     %s\n", callsign.c_str());
}

void cmd_say(const std::string &callsign, const std::string &rest) {
  if (rest.empty()) {
    std::fprintf(stderr, "Usage: say <transcript text>\n");
    return;
  }
  engine::Input in{
      /*transcript=*/rest,
      /*quality=*/1.0f,
      /*ctx=*/&xplane_context::g_cli_ctx,
      /*pilot_callsign=*/callsign,
  };
  engine::process_transcript(std::move(in), [](engine::Output out) {
    std::printf("PILOT : %s\n", out.parsed.raw_transcript.c_str());
    std::printf("INTENT: %s (confidence=%.2f)\n",
                intent_parser::intent_name(out.parsed.intent),
                out.parsed.confidence);
    std::printf("ATC   : %s\n", out.response_text.empty()
                                    ? "(silent)"
                                    : out.response_text.c_str());
    std::printf("STATE : %s\n",
                atc_state_machine::state_name(atc_state_machine::get_state()));
  });
}

void cmd_set(std::string &callsign, const std::string &rest) {
  auto [field, value] = split_first_word(rest);
  if (field.empty() || value.empty()) {
    std::fprintf(stderr, "Usage: set <field> <value>   (try 'help')\n");
    return;
  }
  auto &ctx = xplane_context::g_cli_ctx;
  try {
    if (field == "airport") {
      ctx.nearest_airport_id = value;
    } else if (field == "airport_name") {
      ctx.nearest_airport_name = value;
    } else if (field == "towered") {
      // Back-compat shorthand: true -> TOWERED, false -> UNCONTROLLED.
      bool b;
      if (!parse_bool(value, b)) throw std::runtime_error("expected bool");
      ctx.facility_type = b ? xplane_context::FacilityType::TOWERED
                            : xplane_context::FacilityType::UNCONTROLLED;
    } else if (field == "facility") {
      xplane_context::FacilityType ft;
      if (!parse_facility(value, ft))
        throw std::runtime_error("unknown facility "
                                 "(unknown|uncontrolled|towered|afis)");
      ctx.facility_type = ft;
    } else if (field == "tower_only") {
      bool b;
      if (!parse_bool(value, b)) throw std::runtime_error("expected bool");
      ctx.tower_only = b;
    } else if (field == "on_ground") {
      bool b;
      if (!parse_bool(value, b)) throw std::runtime_error("expected bool");
      ctx.on_ground = b;
    } else if (field == "com") {
      ctx.com1_freq_mhz = std::stof(value);
      ctx.active_com = 1;
    } else if (field == "freq_type") {
      FrequencyType ft;
      if (!parse_freq_type(value, ft))
        throw std::runtime_error("unknown freq_type");
      ctx.frequency_type = ft;
    } else if (field == "runway") {
      ctx.active_runway = value;
    } else if (field == "altitude_ft") {
      ctx.altitude_ft_msl = std::stof(value);
    } else if (field == "agl_ft") {
      ctx.height_agl_ft = std::stof(value);
    } else if (field == "groundspeed_kt") {
      ctx.groundspeed_kts = std::stof(value);
    } else if (field == "heading") {
      ctx.heading_true = std::stof(value);
    } else if (field == "callsign") {
      callsign = value;
      settings::set_pilot_callsign_raw(value);
    } else if (field == "region") {
      std::string up = value;
      std::transform(up.begin(), up.end(), up.begin(),
                     [](unsigned char c) { return std::toupper(c); });
      // German-VFR-only build: "DE" is the only valid profile.
      if (up != "DE")
        throw std::runtime_error("region must be DE (German-VFR-only build)");
      settings::set_atc_profile(up);
      atc_templates::reload();
      flight_phase::reload();
      airport_vrps::reload();
    } else {
      std::fprintf(stderr, "Unknown field: %s (try 'help')\n", field.c_str());
      return;
    }
    prime_flight_phase(ctx);
    std::fprintf(stderr, "set %s = %s\n", field.c_str(), value.c_str());
  } catch (const std::exception &e) {
    std::fprintf(stderr, "Error setting %s: %s\n", field.c_str(), e.what());
  }
}

void cmd_reset() {
  atc_state_machine::reset();
  engine::reset();
  std::fprintf(stderr, "Engine state reset (context unchanged).\n");
}

void cmd_load(std::string &callsign, const std::string &rest) {
  if (rest.empty()) {
    std::fprintf(stderr, "Usage: load <scenario.json>\n");
    return;
  }
  try {
    auto scn = scenario::load(rest);
    const std::string region = scn.region.empty() ? "DE" : scn.region;
    settings::set_atc_profile(region);
    atc_templates::reload();
    flight_phase::reload();
    airport_vrps::reload();
    xplane_context::g_cli_ctx = std::move(scn.ctx);
    ensure_engine_fields(xplane_context::g_cli_ctx);
    callsign = scn.pilot_callsign;
    if (!callsign.empty()) settings::set_pilot_callsign_raw(callsign);
    atc_state_machine::reset();
    engine::reset();
    prime_flight_phase(xplane_context::g_cli_ctx);
    std::fprintf(stderr, "Loaded: %s [region=%s]\n", scn.name.c_str(),
                 region.c_str());
    print_state(callsign);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "Error loading %s: %s\n", rest.c_str(), e.what());
  }
}

void cmd_help() {
  std::printf(
      "Commands:\n"
      "  say <text>            Process a pilot transcript through the engine\n"
      "  set <field> <value>   Modify context (see fields below)\n"
      "  state                 Show current context, flight phase, ATC state\n"
      "  reset                 Reset engine state (keeps context)\n"
      "  load <file>           Load scenario as new context (resets engine)\n"
      "  help                  This message\n"
      "  quit                  Exit (or Ctrl+D)\n"
      "\n"
      "Set fields:\n"
      "  region DE                Phraseology profile (German-VFR-only)\n"
      "  airport <ICAO>           e.g. LSZH\n"
      "  airport_name <text>\n"
      "  towered true|false        (shorthand: facility TOWERED|UNCONTROLLED)\n"
      "  facility <TYPE>          UNKNOWN|UNCONTROLLED|TOWERED|AFIS\n"
      "  tower_only true|false     (airport has Tower but no Ground)\n"
      "  on_ground true|false\n"
      "  com <MHz>                e.g. 118.100\n"
      "  freq_type <TYPE>         GROUND|TOWER|UNICOM|CTAF|ATIS|DELIVERY|INFO|RADIO\n"
      "  runway <id>              e.g. 28, 10L\n"
      "  altitude_ft <ft>\n"
      "  agl_ft <ft>\n"
      "  groundspeed_kt <kt>\n"
      "  heading <deg>\n"
      "  callsign <text>\n");
}

} // namespace

int run(xplane_context::XPlaneContext ctx, std::string pilot_callsign) {
  ensure_engine_fields(ctx);
  if (!pilot_callsign.empty())
    settings::set_pilot_callsign_raw(pilot_callsign);
  xplane_context::g_cli_ctx = std::move(ctx);
  atc_state_machine::reset();
  engine::reset();
  prime_flight_phase(xplane_context::g_cli_ctx);

  std::fprintf(
      stderr,
      "atc_repl — type 'help' for commands, 'quit' or Ctrl+D to exit.\n");
  print_state(pilot_callsign);

  std::string line;
  while (true) {
    std::fprintf(stderr, "\n> ");
    if (!std::getline(std::cin, line)) {
      std::fprintf(stderr, "\n");
      return 0;
    }
    line = trim(std::move(line));
    if (line.empty()) continue;

    auto [cmd, rest] = split_first_word(line);
    if (cmd == "say")
      cmd_say(pilot_callsign, rest);
    else if (cmd == "set")
      cmd_set(pilot_callsign, rest);
    else if (cmd == "state")
      print_state(pilot_callsign);
    else if (cmd == "reset")
      cmd_reset();
    else if (cmd == "load")
      cmd_load(pilot_callsign, rest);
    else if (cmd == "help")
      cmd_help();
    else if (cmd == "quit" || cmd == "exit")
      return 0;
    else
      std::fprintf(stderr, "Unknown command: %s (try 'help')\n", cmd.c_str());
  }
}

} // namespace repl
