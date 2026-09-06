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

#ifndef SETTINGS_HPP
#define SETTINGS_HPP

#include "persistence/model_manifest.hpp"

#include <string>

namespace settings {

void init();
void stop();
void save();

// Data directory path (plugin-relative <plugin>/data)
std::string get_data_dir();

// ATC-profile-scoped data directory, derived from atc_language():
// <data>/atc_profiles/de (NfL DACH-VFR) or <data>/atc_profiles/en
// (ICAO-VFR). The six profile-bundle loaders resolve through here.
std::string atc_profile_data_dir();

// UI-chrome-scoped data directory, derived from ui_language() (Issue #56):
// <data>/atc_profiles/de or <data>/atc_profiles/en. Only ui_strings.json
// resolves through here, so the interface language is decoupled from the
// ATC phraseology language (atc_profile_data_dir()).
std::string ui_profile_data_dir();

// Global, profile-independent VRP file path (<data>/vrps/airport_vrps.json).
// VRPs are geographic data and don't depend on which ATC style the pilot
// is training.
std::string vrps_data_path();

// Bundled OpenAir airspace file (<data>/airspaces/de_airspace.txt). Starter
// set for the 3-D airspace DB (openair_db); a full openaip.net Germany
// export can be dropped into user_prefs_dir()/airspace.txt as an override.
std::string airspaces_data_path();

// User preferences directory — under
// <X-Plane>/Output/preferences/xp_wellys_devfr_atc/. Survives plugin
// re-installs. Used for optional per-user data overrides (e.g.
// airport_vrps_<region>.json sourced from Navigraph Charts). Created on first
// call if absent.
std::string user_prefs_dir();

// Runtime output directory — under <X-Plane>/Output/xp_wellys_devfr_atc/.
// Top-level under Output/ (not the plugin dir) so generated artifacts like
// the per-flight ATC logbook survive plugin re-installs, mirroring how other
// plugins (e.g. StableApproach) write to Output/[name]. Created on first
// call if absent.
std::string output_dir();

// Getters
std::string pilot_callsign();
int active_com();
float volume();
bool debug_logging();
std::string pattern_direction();
// Planned VFR flight type for the departure phraseology hints: "pattern"
// (Platzrunde, default) or "cross_country" (Ueberlandflug). Drives the
// {intention} hint variable (NfL 2024 1.4.7 a/b). User-selected up front;
// the sim has no flight plan to derive it from.
std::string vfr_flight_type();
// Destination aerodrome for cross-country departures ("VFR nach <dest>",
// NfL 2024 1.4.7 b). Free text, empty by default.
std::string vfr_destination();
bool disable_default_atc();
bool skip_radio_power_check();
bool show_phraseology_hints();
float auto_correction_factor();

// ATC phraseology language: "de" (NfL DACH-VFR, default) or "en"
// (ICAO-VFR). The single authoritative language switch (Issue #36);
// atc_profile() / backend_language() / atc_profile_data_dir() all derive
// from it.
std::string atc_language();

// Interface (UI-chrome) language: "de" or "en" (Issue #56). Decoupled from
// atc_language() so a pilot can run the operating UI in English while
// keeping German NfL phraseology. Default "en" for fresh installs; existing
// configs inherit their atc_language on first migration. Only ui_strings
// (buttons/labels/tabs/tooltips) resolves through it — the spoken
// phraseology, ATC responses and hint contents stay on atc_language().
std::string ui_language();

// Active ATC training profile, derived from atc_language(): "DE" for
// German, "EN" for English. The region gates across the engine compare
// against these uppercase strings.
std::string atc_profile();

// ISO-639-1 language code, equal to atc_language() ("de" | "en"). Used by
// the cloud STT backends as the Whisper/Voxtral `language` parameter and
// as the suffix that selects the LM prompts in atc_prompt_templates.json.
std::string backend_language();
// Cockpit start state assumed at plugin boot. Drives the initial
// ATCState the state machine adopts. One of:
//   "cold_and_dark"     — IDLE, pilot expected to power up + tune freq
//   "engines_running"   — IDLE, pilot ready to call Ground (default)
//   "ready_for_takeoff" — TOWER_CONTACT, pilot at holding point
std::string start_mode();
bool debug_traffic();

// Debug helper: replace the mic / PTT path with a typed text field in
// the Status tab. When true, the UI shows an InputText below the
// transcript that injects the typed string directly into
// engine::process_transcript via atc_session::submit_text — skipping
// STT entirely. The Tower reply still runs through the live LM + TTS
// strategy, so the rest of the pipeline (including audible Tower voice)
// is exercised. Useful on laptops without a headset and for isolating
// ATC-logic bugs from STT misrecognitions. Default false. PTT remains
// functional in parallel.
bool debug_text_input();

// BZF strict mode. When true, the tower performs pilot-utterance
// conformance checks against NfL Sprechfunk 2024 §25 b) Nr. 1 readback
// obligations (QNH, runway, frequency, squawk, callsign) and surfaces
// missing elements via corrective tower responses
// (data/atc_profiles/de/atc_templates.json :: bzf_strict.*). Default
// false — simulation mode stays tolerant.
bool bzf_strict_mode();

// Master switch for the traffic subsystem (Phase 2/3/4 advisories,
// landing sequencing, go-around trigger). Default true — TCAS dataRefs
// exist on every X-Plane install, and any traffic provider (LiveTraffic,
// xPilot, swift, X-IvAp, native AI) fills them. When false, the runtime
// reader returns early with an empty snapshot and every downstream
// consumer (advisor / pattern_flow overlay / poll_go_around) becomes a
// no-op without further code paths.
bool traffic_features_enabled();
void set_traffic_features_enabled(bool v);

// Backend selection. Picks the full inference pipeline:
//   "local"   — whisper.cpp + llama.cpp + Piper (Apple Silicon only)
//   "openai"  — Whisper API + Chat Completions + TTS API
//   "mistral" — Voxtral STT + Mistral chat completions + Voxtral TTS
// STT+LM always come from this one backend. Default "local". The TTS
// stage can be split off via tts_backend_override() below.
std::string backend_mode();

// TTS stage override (issue #66, Apple Silicon only). "" = TTS follows
// backend_mode; "local" = force the local Piper voice for speech output
// while STT+LM stay on a cloud backend_mode — a native German voice
// without running the heavy local whisper/llama models. Inert on the
// cloud-only slice (no Piper compiled in).
std::string tts_backend_override();
void set_tts_backend_override(const std::string &v);

// True when an OpenAI API key was saved to the Keychain. The actual
// key is never persisted to settings.json — only this flag.
bool api_key_saved();

// OpenAI model selection. Defaults match the v1.3.x integration.
std::string openai_stt_model();
std::string openai_lm_model();
std::string openai_tts_model();

// OpenAI TTS voice per role. One of
// alloy / echo / fable / onyx / nova / shimmer.
std::string openai_tts_voice_atis();
std::string openai_tts_voice_tower();
std::string openai_tts_voice_ground();

// True when a Mistral API key was saved to the Keychain. Mirrors
// api_key_saved() for the OpenAI side.
bool mistral_api_key_saved();

// Mistral model selection. Defaults: voxtral-mini-2507 for STT,
// mistral-small-latest for LM, empty for TTS (Voxtral TTS picks the
// default model server-side).
std::string mistral_stt_model();
std::string mistral_lm_model();
std::string mistral_tts_model();

// Mistral TTS voice per role. Free strings — Voxtral TTS preset voice
// ids are not whitelisted client-side, so the user can paste any
// custom voice id from the Mistral dashboard.
std::string mistral_tts_voice_atis();
std::string mistral_tts_voice_tower();
std::string mistral_tts_voice_ground();

// Setters for the dual-backend settings (used by the Settings UI tab).
void set_backend_mode(const std::string &v);
void set_openai_stt_model(const std::string &v);
void set_openai_lm_model(const std::string &v);
void set_openai_tts_model(const std::string &v);
void set_openai_tts_voice_atis(const std::string &v);
void set_openai_tts_voice_tower(const std::string &v);
void set_openai_tts_voice_ground(const std::string &v);

void set_mistral_stt_model(const std::string &v);
void set_mistral_lm_model(const std::string &v);
void set_mistral_tts_model(const std::string &v);
void set_mistral_tts_voice_atis(const std::string &v);
void set_mistral_tts_voice_tower(const std::string &v);
void set_mistral_tts_voice_ground(const std::string &v);

// Keychain-backed OpenAI API key handling. save_api_key() also
// updates the api_key_saved flag and persists settings.json.
// load_api_key() returns an empty string when no key is stored.
// delete_api_key() clears both the Keychain entry and the flag.
bool save_api_key(const std::string &key);
std::string load_api_key();
void delete_api_key();

// Mistral-side mirror of the OpenAI key helpers above. Uses a
// separate Keychain entry (service "com.xp_wellys_devfr_atc.mistral") so
// both keys can persist in parallel and a Backend Mode flip does not
// require re-pasting either one.
bool save_mistral_api_key(const std::string &key);
std::string load_mistral_api_key();
void delete_mistral_api_key();

// Setters
std::string pilot_callsign_raw();
void set_pilot_callsign_raw(const std::string &raw);
std::string to_icao_phonetic(const std::string &raw);
void set_volume(float v);
void set_debug_logging(bool v);
void set_active_com(int com);
void set_pattern_direction(const std::string &v);
void set_vfr_flight_type(const std::string &v);
void set_vfr_destination(const std::string &v);
void set_disable_default_atc(bool v);
void set_skip_radio_power_check(bool v);
void set_show_phraseology_hints(bool v);
void set_auto_correction_factor(float v);

// Set the ATC phraseology language: "en" selects ICAO-VFR, anything else
// falls back to "de" (NfL DACH-VFR). Keeps the derived atc_profile mirror
// in sync. A restart is required for the profile bundles / voices to
// reload.
void set_atc_language(const std::string &v);

// Legacy entry point kept for API stability (scenario loader / headless
// REPL call it): translates an uppercase profile ("DE"/"EN") to the
// language and forwards to set_atc_language().
void set_atc_profile(const std::string &v);

// Set the interface (UI-chrome) language: "en" or "de" (Issue #56).
// Independent of set_atc_language(). Callers should ui_strings::reload()
// afterwards for a live switch without a restart.
void set_ui_language(const std::string &v);

void set_debug_traffic(bool v);
void set_debug_text_input(bool v);
void set_bzf_strict_mode(bool v);
void set_start_mode(const std::string &v);

// Reset the test-mutable settings (atc_language, bzf_strict_mode, vfr
// intention, pilot callsign) back to their defaults. Used by the module-reset
// listener so a test that flips a setting cannot leak it into the next
// test under --order rand. See tests/module_reset_listener.cpp and
// Issue #3.
void reset_for_test();

// Voice id (Piper voice_id, e.g. "en_US-lessac-medium") currently
// assigned to a logical ATC role. Defaults to the manifest default if
// the setting is missing or points at an unknown voice id.
std::string voice_for_role(model_manifest::VoiceRole role);
void set_voice_for_role(model_manifest::VoiceRole role,
                        const std::string &voice_id);

// Window geometry (-1 = use default/center)
float window_x();
float window_y();
float window_w();
float window_h();
void set_window_geometry(float x, float y, float w, float h);
void reset_window_geometry();

} // namespace settings

#endif // SETTINGS_HPP
