/*
 * xp_wellys_vfr_atc - headless CLI
 *
 * Minimal settings::* implementations so the engine OBJECT library
 * links in the CLI without pulling in the plugin-only settings.cpp
 * (which depends on XPLMGetPluginInfo to resolve the data dir). Only
 * the getters engine code actually calls are defined here; setters
 * and other entry points are omitted.
 */

#include "persistence/settings.hpp"

#include <cstdlib>
#include <string>

namespace settings {

static std::string env_or(const char *key, const std::string &fallback) {
  const char *v = std::getenv(key);
  return v ? std::string(v) : fallback;
}

// ── Getters used by engine modules ───────────────────────────────

bool debug_logging() { return std::getenv("XP_ATC_DEBUG") != nullptr; }

// Overridable at runtime so scenarios / REPL `set callsign` feed the
// value used by the intent parser (which matches the transcript against
// the configured pilot callsign).
static std::string g_pilot_callsign =
    env_or("XP_ATC_CALLSIGN", "November One Two Three Alpha Bravo");

std::string pilot_callsign() { return g_pilot_callsign; }

void set_pilot_callsign_raw(const std::string &v) { g_pilot_callsign = v; }

std::string pattern_direction() { return "left"; }
float auto_correction_factor() { return 1.0f; }

// VFR intention defaults (mutable so scenario/REPL/tests can drive the
// {intention} / {destination} hint variables; env override for the REPL).
static std::string g_vfr_flight_type =
    env_or("XP_ATC_VFR_FLIGHT_TYPE", "pattern");
static std::string g_vfr_destination = env_or("XP_ATC_VFR_DESTINATION", "");
std::string vfr_flight_type() { return g_vfr_flight_type; }
std::string vfr_destination() { return g_vfr_destination; }
void set_vfr_flight_type(const std::string &v) { g_vfr_flight_type = v; }
void set_vfr_destination(const std::string &v) { g_vfr_destination = v; }

// ATC phraseology language ("de" | "en"). Mutable so scenarios / tests
// can flip the profile; env override for the REPL. Mirrors the production
// atc_language seam (Issue #36).
static std::string g_atc_language = env_or("XP_ATC_LANGUAGE", "de");
static std::string valid_lang(const std::string &v) {
  return v == "en" ? "en" : "de";
}

std::string atc_language() { return g_atc_language; }
void set_atc_language(const std::string &v) { g_atc_language = valid_lang(v); }

// Derived from atc_language(): "de" -> "DE", "en" -> "EN".
std::string atc_profile() { return g_atc_language == "en" ? "EN" : "DE"; }

std::string backend_language() { return g_atc_language; }

void set_atc_profile(const std::string &v) {
  // Legacy entry point (scenario loader / REPL): translate the uppercase
  // profile back to a language code.
  set_atc_language(v == "EN" ? "en" : "de");
}

std::string get_data_dir() { return env_or("XP_ATC_DATA_DIR", "./data"); }

std::string atc_profile_data_dir() {
  return get_data_dir() + "/atc_profiles/" + g_atc_language;
}

std::string vrps_data_path() {
  return get_data_dir() + "/vrps/airport_vrps.json";
}

// Headless equivalent of the plugin's user_prefs_dir(). The plugin path
// (<X-Plane>/Output/preferences/xp_wellys_devfr_atc) does not exist in CLI
// runs; XP_ATC_USER_PREFS_DIR overrides it for fixture-driven tests,
// otherwise we fall back to a sibling of the data dir so REPL users can
// drop overrides next to the bundle.
std::string user_prefs_dir() {
  return env_or("XP_ATC_USER_PREFS_DIR", "./user_prefs");
}

// Unused-by-engine but referenced elsewhere during link: defined just in
// case they creep into the engine OBJECT library from a transitive include.
bool skip_radio_power_check() { return true; }

// Headless scenarios always start fresh (IDLE+PARKED), the start_mode
// setting is a UI affordance only.
std::string start_mode() { return "engines_running"; }

// BZF-Strict-Mode toggle — overridable so tests can flip it. Default
// off (matches the JSON-default in settings.cpp), so the existing
// scenario tests stay in tolerant simulation mode.
static bool g_bzf_strict_mode = false;
bool bzf_strict_mode() { return g_bzf_strict_mode; }
void set_bzf_strict_mode(bool v) { g_bzf_strict_mode = v; }

// Restore the test-mutable statics to the same defaults their static
// initializers use (env overrides honored, identical to startup). The
// Catch2 module-reset listener calls this before every test case so a
// flipped setting cannot leak across tests under --order rand (Issue #3).
void reset_for_test() {
  g_atc_language = env_or("XP_ATC_LANGUAGE", "de");
  g_bzf_strict_mode = false;
  g_vfr_flight_type = env_or("XP_ATC_VFR_FLIGHT_TYPE", "pattern");
  g_vfr_destination = env_or("XP_ATC_VFR_DESTINATION", "");
  g_pilot_callsign =
      env_or("XP_ATC_CALLSIGN", "November One Two Three Alpha Bravo");
}

// Voice resolver — pulled into the engine library by backends::manager.
// The headless tools never synthesize audio (TTS calls short-circuit on
// `tts_ready() == false`) so this getter is only here to satisfy the
// linker; its return value never reaches Piper.
std::string voice_for_role(model_manifest::VoiceRole role) {
  return model_manifest::default_voice_for(role);
}

void set_voice_for_role(model_manifest::VoiceRole, const std::string &) {}

} // namespace settings
