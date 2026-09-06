/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "persistence/settings.hpp"

#include "atc/de_phraseology.hpp"
#include "atc/en_phraseology.hpp"
#include "persistence/keychain.hpp"
#include "persistence/models_catalog.hpp"

#include <algorithm> // std::max (path separator search)
#include <cctype>
#include <fstream>
#include <string>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h> // _mkdir (no mode argument on Windows)
#endif

#include <XPLMPlugin.h>
#include <XPLMUtilities.h>
#include <json.hpp>

namespace {
// POSIX mkdir takes a mode; Windows _mkdir does not. Best-effort at every
// call site — an already-existing directory (EEXIST) is fine.
int make_dir(const std::string &p) {
#if defined(_WIN32)
  return _mkdir(p.c_str());
#else
  return mkdir(p.c_str(), 0755);
#endif
}
} // namespace

namespace settings {

using json = nlohmann::json;

static json cfg;
static std::string data_dir_path;

static json default_config() {
  using model_manifest::VoiceRole;
  return {
      {"pilot_callsign_raw", ""},
      {"pilot_callsign", ""},
      {"active_com", 1},
      {"volume", 1.0},
      {"debug_logging", false},
      {"pattern_direction", "left"},
      {"vfr_flight_type", "pattern"},
      {"vfr_destination", ""},
      {"disable_default_atc", false},
      {"skip_radio_power_check", false},
      {"show_phraseology_hints", true},
      {"auto_correction_factor", 1.0},
      // ATC phraseology language. "de" = NfL DACH-VFR (default), "en" =
      // ICAO-VFR. Drives atc_profile() / backend_language() /
      // atc_profile_data_dir(); everything language-specific derives from
      // this single key (Epic #35 / Issue #36).
      {"atc_language", "de"},
      // Legacy mirror kept for schema stability: derived from
      // atc_language ("de"->"DE", "en"->"EN"). Not authoritative.
      {"atc_profile", "DE"},
      // Interface (UI-chrome) language, decoupled from atc_language
      // (Issue #56). Default "en" for fresh installs; existing configs
      // without this key inherit atc_language on migration in init().
      {"ui_language", "en"},
      {"debug_traffic", false},
      {"debug_text_input", false},
      {"traffic_features_enabled", true},
      {"bzf_strict_mode", false},
      {"start_mode", "engines_running"},
      {"backend_mode", "local"},
      {"tts_backend_override", ""},
      {"api_key_saved", false},
      {"openai_stt_model", "whisper-1"},
      {"openai_lm_model", "gpt-4o-mini"},
      {"openai_tts_model", "tts-1"},
      {"openai_tts_voice_atis", "onyx"},
      {"openai_tts_voice_tower", "echo"},
      {"openai_tts_voice_ground", "alloy"},
      {"mistral_api_key_saved", false},
      // Defaults bumped in v3.1 — the dedicated transcribe model
      // produces noticeably better aviation-English transcripts than
      // the multimodal voxtral-mini-2507, and the large LM picks up
      // intent classification edge cases that small misses. Existing
      // installs are migrated in init() below; users who pasted a
      // custom slug keep their choice.
      {"mistral_stt_model", "voxtral-mini-transcribe-2507"},
      {"mistral_lm_model", "mistral-large-latest"},
      {"mistral_tts_model", "voxtral-mini-tts-2603"},
      {"mistral_tts_voice_atis", "gb_oliver_neutral"},
      {"mistral_tts_voice_tower", "en_paul_confident"},
      {"mistral_tts_voice_ground", "en_paul_neutral"},
      {"voice_atis", model_manifest::default_voice_for(VoiceRole::Atis, "de")},
      {"voice_tower",
       model_manifest::default_voice_for(VoiceRole::Tower, "de")},
      {"voice_ground",
       model_manifest::default_voice_for(VoiceRole::Ground, "de")},
      {"voice_unicom",
       model_manifest::default_voice_for(VoiceRole::Unicom, "de")},
      {"window_x", -1.0},
      {"window_y", -1.0},
      {"window_w", -1.0},
      {"window_h", -1.0}};
}

// Strip pre-v1.4 keys that previous plugin versions wrote into
// settings.json under different names. We never read them again;
// clearing them keeps the file tidy when the user upgrades.
// Note: "api_key_saved" was on this list in the local-only era —
// it is now a live key again (dual-backend), so it must NOT appear
// here.
static const char *kLegacyKeys[] = {
    "tts_voice",
    "tts_model",
    "whisper_model",
    "gpt_model",
    "gpt_fallback_enabled",
};

void init() {
  // Resolve plugin path to find data/ directory
  // Installed: .../plugins/xp_wellys_vfr_atc/mac_x64/xp_wellys_vfr_atc.xpl
  // We need to go up 2 levels to reach the plugin root
  char plugin_path_raw[2048] = {};
  XPLMGetPluginInfo(XPLMGetMyID(), nullptr, plugin_path_raw, nullptr, nullptr);

  std::string path_str(plugin_path_raw);
#if defined(__APPLE__)
  // macOS may return an HFS path (colon-separated) — convert to POSIX
  if (path_str.find(':') != std::string::npos &&
      path_str.find('/') == std::string::npos) {
    auto colon = path_str.find(':');
    std::string posix = path_str.substr(colon + 1);
    for (char &c : posix)
      if (c == ':')
        c = '/';
    path_str = "/" + posix;
  }
#endif

  // Strip filename → directory, then strip platform dir (mac_x64/, win_x64/).
  // XPLM_USE_NATIVE_PATHS yields OS-native separators, so accept both '/' and
  // '\': X-Plane on Windows reports backslash paths. Without the backslash arm
  // this fell through to the relative "data" fallback on Windows, so every
  // data/ lookup (settings.json, models_catalog.json with the model URLs and
  // SHA256 hashes, the atc_profiles bundles) resolved against the process CWD
  // instead of the plugin. Same fix model_paths.cpp already carries (#74).
  auto rfind_sep = [&](size_t from) {
    size_t a = path_str.rfind('/', from);
    size_t b = path_str.rfind('\\', from);
    if (a == std::string::npos)
      return b;
    if (b == std::string::npos)
      return a;
    return std::max(a, b);
  };
  auto pos = rfind_sep(std::string::npos);
  if (pos != std::string::npos && pos > 0) {
    pos = rfind_sep(pos - 1);
  }
  if (pos != std::string::npos) {
    data_dir_path = path_str.substr(0, pos) + "/data";
  } else {
    data_dir_path = "data";
  }

  make_dir(data_dir_path);

  // Load the model catalog BEFORE default_config() — default_config()
  // calls model_manifest::default_voice_for(), which now reads from
  // the catalog. A missing/broken file degrades to the built-in
  // defaults rather than failing init().
  models_catalog::init(data_dir_path);

  std::string json_path = data_dir_path + "/settings.json";
  std::ifstream in(json_path);
  bool needs_save = false;
  // Whether the loaded file already carried ui_language (Issue #56). Stays
  // true for fresh installs / parse-fallback (both seed default_config(),
  // which has ui_language="en"), so only an existing file lacking the key
  // triggers the atc_language migration below.
  bool had_ui_language = true;
  if (in.good()) {
    try {
      in >> cfg;
      had_ui_language = cfg.contains("ui_language");
      // Merge any missing defaults
      json defaults = default_config();
      for (auto &[key, value] : defaults.items()) {
        if (!cfg.contains(key)) {
          cfg[key] = value;
        }
      }
    } catch (...) {
      XPLMDebugString(
          "[xp_wellys_vfr_atc] Warning: failed to parse settings.json, "
          "using defaults\n");
      cfg = default_config();
      needs_save = true;
    }
  } else {
    cfg = default_config();
    needs_save = true;
  }

  for (const char *legacy : kLegacyKeys) {
    if (cfg.contains(legacy)) {
      cfg.erase(legacy);
      needs_save = true;
    }
  }

  // Drop any legacy "flow_region" mirror from a multi-profile build.
  if (cfg.contains("flow_region")) {
    cfg.erase("flow_region");
    needs_save = true;
  }
  // Validate the ATC language (Issue #36). Unknown values fall back to
  // "de"; a valid user selection ("de" | "en") is preserved. The legacy
  // atc_profile key is kept in sync as a derived mirror ("de" -> "DE",
  // "en" -> "EN") so a settings.json inspected by hand stays consistent.
  {
    std::string lang = cfg.value("atc_language", std::string("de"));
    if (lang != "de" && lang != "en") {
      lang = "de";
      cfg["atc_language"] = lang;
      needs_save = true;
    }
    std::string derived_profile = lang == "en" ? "EN" : "DE";
    if (cfg.value("atc_profile", std::string{}) != derived_profile) {
      cfg["atc_profile"] = derived_profile;
      needs_save = true;
    }
  }

  // UI-chrome language (Issue #56). An existing config that predates this
  // key inherits atc_language so the interface looks unchanged after the
  // update; fresh installs keep the "en" default from default_config().
  // Then validate: unknown values fall back to "de".
  {
    if (!had_ui_language) {
      cfg["ui_language"] = cfg.value("atc_language", std::string("de"));
      needs_save = true;
    }
    std::string ui_lang = cfg.value("ui_language", std::string("de"));
    if (ui_lang != "de" && ui_lang != "en") {
      cfg["ui_language"] = "de";
      needs_save = true;
    }
  }

  // Mistral-default bump (v3.1): users on the previous hardcoded
  // defaults (voxtral-mini-2507 / mistral-small-latest) get upgraded
  // to the better aviation-English combo. Any user who pasted a
  // different slug keeps it. Non-destructive — match the OLD default
  // exactly, otherwise leave untouched.
  if (cfg.value("mistral_stt_model", std::string{}) == "voxtral-mini-2507") {
    cfg["mistral_stt_model"] = "voxtral-mini-transcribe-2507";
    needs_save = true;
  }
  // Heal an invalid STT slug (issue #79): voxtral-small-2507 is an
  // audio-instruct model, not a transcription model — the
  // /v1/audio/transcriptions endpoint rejects it with HTTP 400
  // 'invalid_model', so every PTT fails silently. Point it back at the
  // dedicated transcribe model so a stuck install recovers on next launch.
  if (cfg.value("mistral_stt_model", std::string{}) == "voxtral-small-2507") {
    cfg["mistral_stt_model"] = "voxtral-mini-transcribe-2507";
    needs_save = true;
  }
  if (cfg.value("mistral_lm_model", std::string{}) == "mistral-small-latest") {
    cfg["mistral_lm_model"] = "mistral-large-latest";
    needs_save = true;
  }

  if (needs_save)
    save();

  XPLMDebugString("[xp_wellys_vfr_atc] Settings loaded\n");
}

void stop() {}

std::string get_data_dir() { return data_dir_path; }

std::string atc_profile_data_dir() {
  // Profile bundle directory derived from atc_language: data/atc_profiles/de
  // or data/atc_profiles/en. The six asset loaders (templates, flight_rules,
  // intent_rules, phraseology_hints, ui_strings, conformance) resolve through
  // here, so a language switch repoints all of them at once.
  return data_dir_path + "/atc_profiles/" + atc_language();
}

std::string ui_profile_data_dir() {
  // UI-chrome bundle directory derived from ui_language (Issue #56), so the
  // interface language is independent of the ATC phraseology language. Only
  // ui_strings.json resolves through here; it reuses the per-language
  // bundles under atc_profiles/{de,en}.
  return data_dir_path + "/atc_profiles/" + ui_language();
}

std::string vrps_data_path() {
  return data_dir_path + "/vrps/airport_vrps.json";
}

std::string airspaces_data_path() {
  return data_dir_path + "/airspaces/de_airspace.txt";
}

std::string user_prefs_dir() {
  // XPLMGetSystemPath returns the X-Plane root with a trailing slash.
  // Sits under Output/preferences/ so it survives plugin re-installs.
  char xp_root[2048] = {};
  XPLMGetSystemPath(xp_root);
  std::string path =
      std::string(xp_root) + "Output/preferences/xp_wellys_devfr_atc";
  make_dir(path);
  return path;
}

std::string output_dir() {
  // <X-Plane>/Output/xp_wellys_devfr_atc — runtime output (flight logs).
  // Lives under Output/ (not the plugin dir) so it survives plugin
  // re-installs, mirroring how StableApproach writes to Output/[name].
  // XPLMGetSystemPath returns the X-Plane root with a trailing slash.
  char xp_root[2048] = {};
  XPLMGetSystemPath(xp_root);
  std::string path = std::string(xp_root) + "Output/xp_wellys_devfr_atc";
  make_dir(path);
  return path;
}

void save() {
  std::string json_path = data_dir_path + "/settings.json";
  std::ofstream out(json_path);
  if (out.good()) {
    out << cfg.dump(2) << std::endl;
  } else {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] Error: failed to write settings.json\n");
  }
}

// --- Getters ---

// Expand a raw callsign phonetically using the active profile's tables:
// ICAO ("November one two tree Alfa Bravo") for EN, BZF-German
// ("November eins zwo drei Alfa Bravo") for DE. Single dispatch point so
// getter and setter stay in lock-step (Issue #41).
static std::string expand_callsign(const std::string &raw) {
  return atc_language() == "en"
             ? en_phraseology::expand_callsign_phonetic(raw)
             : de_phraseology::expand_callsign_phonetic(raw);
}

std::string pilot_callsign_raw() {
  return cfg.value("pilot_callsign_raw", std::string(""));
}
std::string pilot_callsign() {
  // Profile-aware: compute on the fly so an ATC-language switch flips
  // ICAO-NATO ("November one two tree Alfa Bravo") to BZF-German
  // ("November eins zwo drei Alfa Bravo") without needing to re-save the
  // callsign. Falls back to the cached value only when no raw callsign is
  // present (legacy settings.json).
  std::string raw = cfg.value("pilot_callsign_raw", std::string(""));
  if (raw.empty())
    return cfg.value("pilot_callsign", std::string(""));
  return expand_callsign(raw);
}
int active_com() { return cfg.value("active_com", 1); }
float volume() { return cfg.value("volume", 1.0f); }
bool debug_logging() { return cfg.value("debug_logging", false); }
std::string pattern_direction() {
  return cfg.value("pattern_direction", std::string("left"));
}
std::string vfr_flight_type() {
  return cfg.value("vfr_flight_type", std::string("pattern"));
}
std::string vfr_destination() {
  return cfg.value("vfr_destination", std::string(""));
}
bool disable_default_atc() { return cfg.value("disable_default_atc", false); }
bool skip_radio_power_check() {
  return cfg.value("skip_radio_power_check", false);
}
bool show_phraseology_hints() {
  return cfg.value("show_phraseology_hints", true);
}
float auto_correction_factor() {
  return cfg.value("auto_correction_factor", 1.0f);
}
std::string atc_language() {
  std::string v = cfg.value("atc_language", std::string("de"));
  if (v != "de" && v != "en")
    v = "de";
  return v;
}
std::string atc_profile() {
  // Derived from atc_language: "de" -> "DE" (NfL DACH-VFR), "en" -> "EN"
  // (ICAO-VFR). The region gates across the engine compare against these
  // uppercase strings.
  return atc_language() == "en" ? "EN" : "DE";
}
std::string backend_language() { return atc_language(); }
std::string ui_language() {
  std::string v = cfg.value("ui_language", std::string("de"));
  if (v != "de" && v != "en")
    v = "de";
  return v;
}
bool debug_traffic() { return cfg.value("debug_traffic", false); }
bool debug_text_input() { return cfg.value("debug_text_input", false); }
bool bzf_strict_mode() { return cfg.value("bzf_strict_mode", false); }
bool traffic_features_enabled() {
  return cfg.value("traffic_features_enabled", true);
}
std::string start_mode() {
  std::string v = cfg.value("start_mode", std::string("engines_running"));
  if (v != "cold_and_dark" && v != "engines_running" &&
      v != "ready_for_takeoff")
    v = "engines_running";
  return v;
}

std::string backend_mode() {
  std::string v = cfg.value("backend_mode", std::string("local"));
  if (v != "local" && v != "openai" && v != "mistral")
    v = "local";
  return v;
}

std::string tts_backend_override() {
  // "" = follow backend_mode for TTS; "local" = force Piper TTS while
  // STT+LM stay on the cloud backend_mode (issue #66, Apple Silicon only).
  std::string v = cfg.value("tts_backend_override", std::string(""));
  if (v != "" && v != "local")
    v = "";
  return v;
}

bool api_key_saved() { return cfg.value("api_key_saved", false); }

std::string openai_stt_model() {
  return cfg.value("openai_stt_model", std::string("whisper-1"));
}
std::string openai_lm_model() {
  return cfg.value("openai_lm_model", std::string("gpt-4o-mini"));
}
std::string openai_tts_model() {
  return cfg.value("openai_tts_model", std::string("tts-1"));
}

namespace {
bool is_valid_openai_voice(const std::string &v) {
  return v == "alloy" || v == "echo" || v == "fable" || v == "onyx" ||
         v == "nova" || v == "shimmer";
}
} // namespace

std::string openai_tts_voice_atis() {
  std::string v = cfg.value("openai_tts_voice_atis", std::string("onyx"));
  return is_valid_openai_voice(v) ? v : "onyx";
}
std::string openai_tts_voice_tower() {
  std::string v = cfg.value("openai_tts_voice_tower", std::string("echo"));
  return is_valid_openai_voice(v) ? v : "echo";
}
std::string openai_tts_voice_ground() {
  std::string v = cfg.value("openai_tts_voice_ground", std::string("alloy"));
  return is_valid_openai_voice(v) ? v : "alloy";
}

// --- Setters ---

// ── ICAO phonetic alphabet conversion ───────────────────────────

static const char *phonetic_letter(char c) {
  static const char *letters[] = {
      "Alpha",  "Bravo",   "Charlie", "Delta",  "Echo",   "Foxtrot", "Golf",
      "Hotel",  "India",   "Juliet",  "Kilo",   "Lima",   "Mike",    "November",
      "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra", "Tango",   "Uniform",
      "Victor", "Whiskey", "X-Ray",   "Yankee", "Zulu"};
  if (c >= 'A' && c <= 'Z')
    return letters[c - 'A'];
  if (c >= 'a' && c <= 'z')
    return letters[c - 'a'];
  return nullptr;
}

static const char *phonetic_digit(char c) {
  static const char *digits[] = {"Zero", "One", "Two",   "Three", "Four",
                                 "Five", "Six", "Seven", "Eight", "Niner"};
  if (c >= '0' && c <= '9')
    return digits[c - '0'];
  return nullptr;
}

std::string to_icao_phonetic(const std::string &raw) {
  std::string result;
  for (char c : raw) {
    const char *word = phonetic_letter(c);
    if (!word)
      word = phonetic_digit(c);
    if (word) {
      if (!result.empty())
        result += ' ';
      result += word;
    }
    // Skip dashes, spaces, and other non-alphanumeric chars
  }
  return result;
}

void set_pilot_callsign_raw(const std::string &raw) {
  cfg["pilot_callsign_raw"] = raw;
  cfg["pilot_callsign"] = expand_callsign(raw);
}
void set_volume(float v) { cfg["volume"] = v; }
void set_debug_logging(bool v) { cfg["debug_logging"] = v; }
void set_active_com(int com) { cfg["active_com"] = com; }
void set_pattern_direction(const std::string &v) {
  cfg["pattern_direction"] = v;
}
void set_vfr_flight_type(const std::string &v) { cfg["vfr_flight_type"] = v; }
void set_vfr_destination(const std::string &v) { cfg["vfr_destination"] = v; }
void set_disable_default_atc(bool v) { cfg["disable_default_atc"] = v; }
void set_skip_radio_power_check(bool v) { cfg["skip_radio_power_check"] = v; }
void set_show_phraseology_hints(bool v) { cfg["show_phraseology_hints"] = v; }
void set_auto_correction_factor(float v) {
  if (v < 0.5f)
    v = 0.5f;
  if (v > 2.0f)
    v = 2.0f;
  cfg["auto_correction_factor"] = v;
}
void set_atc_language(const std::string &v) {
  std::string lang = (v == "en") ? "en" : "de";
  cfg["atc_language"] = lang;
  // Keep the derived legacy mirror in sync.
  cfg["atc_profile"] = (lang == "en") ? "EN" : "DE";
}
void set_atc_profile(const std::string &v) {
  // Legacy entry point kept for API stability (scenario loader / REPL
  // still call it). The language is now authoritative via
  // set_atc_language(); translate the uppercase profile back to a
  // language code so old callers keep working.
  set_atc_language(v == "EN" ? "en" : "de");
}
void set_ui_language(const std::string &v) {
  cfg["ui_language"] = (v == "en") ? "en" : "de";
}
void set_debug_traffic(bool v) { cfg["debug_traffic"] = v; }
void set_debug_text_input(bool v) { cfg["debug_text_input"] = v; }
void set_bzf_strict_mode(bool v) { cfg["bzf_strict_mode"] = v; }

// Restore the test-mutable keys to their compiled-in defaults. Declared
// in the shared header for the Catch2 module-reset listener; the unit
// tests link the headless stub (tools/atc_repl/settings_stub.cpp), so
// this production definition exists only for symmetry / any future tool
// that links the real settings.cpp. See Issue #3.
void reset_for_test() {
  const json d = default_config();
  cfg["atc_language"] = d["atc_language"];
  cfg["atc_profile"] = d["atc_profile"];
  cfg["bzf_strict_mode"] = d["bzf_strict_mode"];
  cfg["tts_backend_override"] = d["tts_backend_override"];
  cfg["vfr_flight_type"] = d["vfr_flight_type"];
  cfg["vfr_destination"] = d["vfr_destination"];
  cfg["pilot_callsign_raw"] = d["pilot_callsign_raw"];
  cfg["pilot_callsign"] = d["pilot_callsign"];
}
void set_traffic_features_enabled(bool v) {
  cfg["traffic_features_enabled"] = v;
}
void set_start_mode(const std::string &v) {
  if (v == "cold_and_dark" || v == "engines_running" ||
      v == "ready_for_takeoff")
    cfg["start_mode"] = v;
  else
    cfg["start_mode"] = "engines_running";
}

// ── Dual-backend settings ─────────────────────────────────────

void set_backend_mode(const std::string &v) {
  if (v == "openai" || v == "mistral")
    cfg["backend_mode"] = v;
  else
    cfg["backend_mode"] = "local";
}
void set_tts_backend_override(const std::string &v) {
  cfg["tts_backend_override"] = (v == "local") ? "local" : "";
}
void set_openai_stt_model(const std::string &v) { cfg["openai_stt_model"] = v; }
void set_openai_lm_model(const std::string &v) { cfg["openai_lm_model"] = v; }
void set_openai_tts_model(const std::string &v) { cfg["openai_tts_model"] = v; }
void set_openai_tts_voice_atis(const std::string &v) {
  if (is_valid_openai_voice(v))
    cfg["openai_tts_voice_atis"] = v;
}
void set_openai_tts_voice_tower(const std::string &v) {
  if (is_valid_openai_voice(v))
    cfg["openai_tts_voice_tower"] = v;
}
void set_openai_tts_voice_ground(const std::string &v) {
  if (is_valid_openai_voice(v))
    cfg["openai_tts_voice_ground"] = v;
}

bool save_api_key(const std::string &key) {
  if (!persistence::keychain::save(key)) {
    XPLMDebugString("[xp_wellys_vfr_atc] Error: failed to save API key to "
                    "Keychain\n");
    return false;
  }
  cfg["api_key_saved"] = true;
  save();
  return true;
}

std::string load_api_key() { return persistence::keychain::load(); }

void delete_api_key() {
  persistence::keychain::remove();
  cfg["api_key_saved"] = false;
  save();
}

// ── Mistral-side settings + Keychain ──────────────────────────────
//
// Mistral lives in a separate Keychain entry so both providers can
// hold a valid key simultaneously. Switching Backend Mode then never
// needs the user to re-paste a key they already saved earlier.

namespace {
constexpr const char *kMistralKcService = "com.xp_wellys_devfr_atc.mistral";
constexpr const char *kMistralKcAccount = "default";
} // namespace

bool mistral_api_key_saved() {
  return cfg.value("mistral_api_key_saved", false);
}

std::string mistral_stt_model() {
  return cfg.value("mistral_stt_model", std::string("voxtral-mini-2507"));
}
std::string mistral_lm_model() {
  return cfg.value("mistral_lm_model", std::string("mistral-small-latest"));
}
std::string mistral_tts_model() {
  // Empty-string fallback heals settings.json files written by the
  // pre-dropdown Mistral build that left the TTS model blank.
  std::string v = cfg.value("mistral_tts_model", std::string(""));
  return v.empty() ? std::string("voxtral-mini-tts-2603") : v;
}
std::string mistral_tts_voice_atis() {
  // Empty-string fallback so users on the pre-dropdown defaults still
  // get a usable voice — the preset catalog only became known after
  // the first Mistral rollout. Cleared settings auto-heal to ATC-best
  // defaults on next read.
  std::string v = cfg.value("mistral_tts_voice_atis", std::string(""));
  return v.empty() ? std::string("gb_oliver_neutral") : v;
}
std::string mistral_tts_voice_tower() {
  std::string v = cfg.value("mistral_tts_voice_tower", std::string(""));
  return v.empty() ? std::string("en_paul_confident") : v;
}
std::string mistral_tts_voice_ground() {
  std::string v = cfg.value("mistral_tts_voice_ground", std::string(""));
  return v.empty() ? std::string("en_paul_neutral") : v;
}

void set_mistral_stt_model(const std::string &v) {
  cfg["mistral_stt_model"] = v;
}
void set_mistral_lm_model(const std::string &v) { cfg["mistral_lm_model"] = v; }
void set_mistral_tts_model(const std::string &v) {
  cfg["mistral_tts_model"] = v;
}
void set_mistral_tts_voice_atis(const std::string &v) {
  cfg["mistral_tts_voice_atis"] = v;
}
void set_mistral_tts_voice_tower(const std::string &v) {
  cfg["mistral_tts_voice_tower"] = v;
}
void set_mistral_tts_voice_ground(const std::string &v) {
  cfg["mistral_tts_voice_ground"] = v;
}

bool save_mistral_api_key(const std::string &key) {
  if (!persistence::keychain::save(kMistralKcService, kMistralKcAccount, key)) {
    XPLMDebugString("[xp_wellys_vfr_atc] Error: failed to save Mistral API "
                    "key to Keychain\n");
    return false;
  }
  cfg["mistral_api_key_saved"] = true;
  save();
  return true;
}

std::string load_mistral_api_key() {
  return persistence::keychain::load(kMistralKcService, kMistralKcAccount);
}

void delete_mistral_api_key() {
  persistence::keychain::remove(kMistralKcService, kMistralKcAccount);
  cfg["mistral_api_key_saved"] = false;
  save();
}

// ── Voice assignments ──────────────────────────────────────────

static const char *voice_key(model_manifest::VoiceRole role) {
  using R = model_manifest::VoiceRole;
  switch (role) {
  case R::Atis:
    return "voice_atis";
  case R::Tower:
    return "voice_tower";
  case R::Ground:
    return "voice_ground";
  case R::Unicom:
    return "voice_unicom";
  }
  return "voice_atis";
}

// Validate a stored voice id against the current manifest. If the
// settings file refers to a voice we no longer ship (manifest bumped,
// catalog shrank), fall back to the role's default. Avoids crashing
// the TTS load path on an unknown id.
static bool voice_id_is_known(const std::string &id) {
  const auto &ids = model_manifest::voice_ids();
  for (const auto &v : ids)
    if (v == id)
      return true;
  return false;
}

std::string voice_for_role(model_manifest::VoiceRole role) {
  // Cloud modes read from their own voice slots — the Piper-style
  // model_manifest::voice_ids() are local-only and would be rejected
  // by the cloud TTS has_voice() checks.
  //
  // Hybrid (issue #66): when the TTS stage is overridden to local Piper
  // while STT+LM stay on a cloud backend_mode, the role->voice lookup must
  // resolve to the local Piper slots (below), not the cloud voices — so
  // force the effective mode to "local" for voice resolution.
  const std::string mode =
      (tts_backend_override() == "local") ? std::string("local") : backend_mode();
  if (mode == "openai") {
    using R = model_manifest::VoiceRole;
    switch (role) {
    case R::Atis:
      return openai_tts_voice_atis();
    case R::Tower:
      return openai_tts_voice_tower();
    case R::Ground:
      return openai_tts_voice_ground();
    case R::Unicom:
      // OpenAI has no dedicated UNICOM slot — reuse the Tower voice.
      return openai_tts_voice_tower();
    }
  }
  if (mode == "mistral") {
    using R = model_manifest::VoiceRole;
    switch (role) {
    case R::Atis:
      return mistral_tts_voice_atis();
    case R::Tower:
      return mistral_tts_voice_tower();
    case R::Ground:
      return mistral_tts_voice_ground();
    case R::Unicom:
      // Mistral has no dedicated UNICOM slot — reuse the Tower voice.
      return mistral_tts_voice_tower();
    }
  }
  std::string id = cfg.value(voice_key(role), std::string{});
  // Fall back to the language default when no voice is stored, the stored
  // voice is unknown, or it belongs to a different language than the active
  // profile (e.g. a persisted German voice while running the EN profile) —
  // otherwise a language switch would keep speaking the old language.
  if (id.empty() || !voice_id_is_known(id) ||
      model_manifest::voice_language(id) != backend_language())
    id = model_manifest::default_voice_for(role, backend_language());
  return id;
}

void set_voice_for_role(model_manifest::VoiceRole role,
                        const std::string &voice_id) {
  if (!voice_id_is_known(voice_id))
    return;
  cfg[voice_key(role)] = voice_id;
}

float window_x() { return cfg.value("window_x", -1.0f); }
float window_y() { return cfg.value("window_y", -1.0f); }
float window_w() { return cfg.value("window_w", -1.0f); }
float window_h() { return cfg.value("window_h", -1.0f); }
void set_window_geometry(float x, float y, float w, float h) {
  cfg["window_x"] = x;
  cfg["window_y"] = y;
  cfg["window_w"] = w;
  cfg["window_h"] = h;
}
void reset_window_geometry() {
  cfg["window_x"] = -1.0;
  cfg["window_y"] = -1.0;
  cfg["window_w"] = -1.0;
  cfg["window_h"] = -1.0;
  save();
}

} // namespace settings
