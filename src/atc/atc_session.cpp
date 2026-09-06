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

#include "atc/atc_session.hpp"
#include "atc/atc_state_machine.hpp"
#include "atc/atis_generator.hpp"
#include "atc/de_phraseology.hpp"
#include "atc/en_phraseology.hpp"
#include "atc/engine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/intent_parser.hpp"
#include "audio/audio_player.hpp"
#include "audio/audio_recorder.hpp"
#include "backends/manager.hpp"
#include "core/logging.hpp"
#include "core/xplane_context.hpp"
#include "persistence/model_manifest.hpp"
#include "persistence/settings.hpp"

#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

namespace atc_session {

static PTTState state_ = PTTState::IDLE;

// True between speak_response() submitting a TTS job and the async
// callback firing. Without this flag, update() flips PLAYING->IDLE in
// the window where TTS is still synthesising (audio_player::is_playing()
// returns false because play_pcm hasn't been called yet) — which let
// the per-tick traffic advisor poll fire and stack a fresh advisory on
// top of the ack TTS that hadn't been spoken yet. Drained on the main
// thread by the manager's callback queue, so no atomic is needed.
static bool tts_pending_ = false;

static float last_duration_ = 0.0f;
static size_t last_samples_ = 0;
static size_t last_wav_bytes_ = 0;

static std::vector<TranscriptEntry> transcript_;
static intent_parser::PilotMessage last_pilot_message_;
static int total_transcriptions_ = 0;
static int total_inferences_ = 0;
static constexpr float kMinRecordingDuration = 0.5f;

// ATIS playback state
static bool atis_playing_ = false;
// COM (1 or 2) the active ATIS broadcast started on. Pinned at trigger
// time so the abort check stays consistent if the pilot rapidly cycles
// COM2 while COM1 is also on ATIS — we abort the stream that's playing,
// not whichever COM still happens to be on the ATIS freq.
static int atis_active_com_ = 0;
static float atis_cooldown_ = 0.0f;
// 120 s cooldown: an inbound VFR pilot at LSZB tunes ATIS, then APP, then
// TWR within ~30-90 s. With a 30 s cooldown the previous-airport's ATIS
// re-fires when the pilot retunes ATIS during that sequence — annoying
// and unrealistic. 120 s is long enough to span a normal arrival
// sequence but short enough that a pilot who deliberately retunes ATIS
// minutes later (e.g. checking for a new letter) still gets fresh
// playback.
static constexpr float kAtisCooldownSec = 120.0f;
static float atis_tuned_timer_ = 0.0f;           // how long tuned to ATIS freq
static constexpr float kAtisTuneDelaySec = 2.0f; // wait before playing

// ATIS TTS retry: a transient cloud-TTS timeout (15 s curl limit) used to
// leave the pilot waiting silently for the full 120 s cooldown. Instead we
// re-try the broadcast a few times with a short gap, then warn the user via
// a transcript line + squelch burst (the user does not watch Log.txt). The
// short retry runs through the existing flight-loop cooldown poller, so PTT
// stays free between attempts.
static constexpr int kAtisMaxTtsTries = 3;        // attempts before giving up
static constexpr float kAtisRetryDelaySec = 15.0f; // gap between failed tries
static int atis_tts_failures_ = 0;                 // consecutive TTS failures

// Dialog (tower/ground) TTS retry — mirrors the ATIS retry above but for
// the interactive pilot<->controller exchange (Issue #46). A transient
// cloud-TTS timeout (15 s curl limit) used to give up after ONE attempt,
// leaving the pilot waiting silently with only a transcript line. Instead
// we re-dispatch the SAME utterance a few times with a short gap through
// the flight-loop poller (PTT stays blocked between tries), then surface a
// UI-queryable network-error state with a manual "retry" button.
static constexpr int kDialogMaxTtsTries = 3;       // attempts before giving up
static constexpr float kDialogRetryDelaySec = 3.0f; // gap between failed tries
static int dialog_tts_failures_ = 0;               // consecutive TTS failures
static float dialog_retry_timer_ = 0.0f;           // >0 = auto-retry scheduled
static bool tts_network_error_ = false;            // UI give-up flag (retryable)
// Content-moderation give-up flag (issue #62). Distinct from
// tts_network_error_: a provider guardrail rejected the text, so retrying
// is pointless — the UI shows an honest notice and NO retry button.
static bool tts_blocked_ = false;

// Full parameters of the last guarded TTS dispatch, retained so the
// flight-loop poller can re-send the identical utterance without re-running
// the engine pipeline (state stays at post-process between auto-retries).
struct GuardedTtsRequest {
  std::string final_text;                  // already normalized for speech
  model_manifest::VoiceRole role = model_manifest::VoiceRole::Tower;
  float length_scale = 1.0f;
  atc_state_machine::AtcStateSnapshot pre_snap;
  uint64_t expected_gen = 0;
  bool valid = false;
};
static GuardedTtsRequest last_guarded_req_;

// Last pilot transcript + quality, retained for the manual retry button.
// After a give-up the ATC state was reverted to pre_snap, so re-running the
// whole pipeline from that transcript reproduces the identical controller
// reply AND re-applies the state transition — keeping the generation-guard
// semantics intact (no state desync from replaying TTS alone).
static std::string last_pilot_transcript_;
static float last_pilot_quality_ = 1.0f;

// Map the pilot's currently-tuned frequency to a logical voice role.
// The transmitting controller is whichever one owns the freq the pilot
// is listening to — *not* the state machine's next_state. Without this,
// a Ground handoff message ("contact Tower on 120.100") gets spoken
// with the Tower voice, because the state has already advanced to
// TOWER_CONTACT by the time speak_response runs.
//
// Tower-only airports collapse Ground onto the Tower voice (one
// controller handles everything on the tower freq). Uncontrolled
// fields (UNICOM / CTAF self-announce) get their own voice so they
// sound distinct from a controlled Tower; an unknown freq falls back
// to the Tower voice.
static model_manifest::VoiceRole
role_for_frequency(const xplane_context::XPlaneContext &ctx) {
  using FT = xplane_context::FrequencyType;
  using R = model_manifest::VoiceRole;
  if (ctx.tower_only)
    return R::Tower;
  switch (ctx.frequency_type) {
  case FT::ATIS:
    return R::Atis;
  case FT::DELIVERY:
  case FT::GROUND:
    return R::Ground;
  case FT::UNICOM:
  case FT::CTAF:
  case FT::INFO:
  case FT::RADIO:
    return R::Unicom;
  case FT::TOWER:
  case FT::UNKNOWN:
    return R::Tower;
  }
  return R::Tower;
}

// Speak ATC response via local TTS, then transition to PLAYING → IDLE.
// `length_scale` > 1.0 makes Piper speak slower (used for ATIS).
// `on_playback_starting` (optional) fires on the main thread the moment
// audio is about to play — only after a successful synthesis. Used by
// the ATIS path to delay the transcript line until the user actually
// hears the broadcast, so a silent TTS failure does not leave a ghost
// entry in the history.
//
// `on_failure` (optional) fires on the main thread when synthesis fails.
// Used by the ATIS path to drive the retry/give-up logic; on success the
// failure callback is never invoked.
static void
speak_response(const std::string &text, model_manifest::VoiceRole role,
               float length_scale = 1.0f, int com_override = 0,
               std::function<void()> on_playback_starting = nullptr,
               std::function<void()> on_failure = nullptr) {
  state_ = PTTState::PLAYING;
  tts_pending_ = true;
  ++total_inferences_; // TTS inference

  // BZF-Phraseology-Normalizer: in DE region, expand numeric aviation
  // patterns to ziffernweise spoken form before TTS. Other regions
  // pass through unchanged.
  std::string final_text =
      (settings::atc_profile() == "DE")
          ? de_phraseology::normalize_for_speech(text)
          : (settings::atc_profile() == "EN"
                 ? en_phraseology::normalize_for_speech(text)
                 : text);

  backends::tts::synthesize_async(
      final_text, role, length_scale,
      [com_override, on_playback_starting = std::move(on_playback_starting),
       on_failure = std::move(on_failure)](backends::tts::Audio audio,
                                           bool success) {
        tts_pending_ = false;
        if (success && !audio.pcm16.empty()) {
          if (settings::debug_logging()) {
            char dbg[160];
            std::snprintf(
                dbg, sizeof(dbg),
                "[xp_wellys_vfr_atc][DEBUG] TTS produced %zu samples "
                "@ %u Hz\n",
                audio.pcm16.size(), audio.sample_rate_hz);
            XPLMDebugString(dbg);
          }
          if (on_playback_starting)
            on_playback_starting();
          int com = com_override > 0 ? com_override : settings::active_com();
          audio_player::play_pcm_on_com(com, std::move(audio.pcm16),
                                        audio.sample_rate_hz, audio.channels,
                                        settings::volume());
        } else {
          XPLMDebugString(
              "[xp_wellys_vfr_atc][ERROR] TTS failed, skipping playback\n");
          state_ = PTTState::IDLE;
          if (on_failure)
            on_failure();
        }
      });
}

// Speak a tower response with the state-revert guard active. Used for
// engine output that committed a semantic state transition — if the
// TTS playback fails, the pilot never heard the clearance, so the
// state must be rolled back (or, when a later mutation makes the
// rollback unsafe, the clearance text must remain available for
// REQUEST_REPEAT replay).
//
//   pre_snap     = atc_state_machine::capture_snapshot() taken BEFORE
//                  the process() call that produced `text`.
//   expected_gen = atc_state_machine::current_gen() taken IMMEDIATELY
//                  AFTER that process() call.
//
// On success: nothing else happens — the response plays, state stays.
// On the FINAL TTS failure (after kDialogMaxTtsTries auto-retries): a
//   squelch burst is played on the active COM, then either (a) restore —
//   state rolled back, system entry suggests re-issuing the request, OR
//   (b) stale — a later mutation already bumped gen past expected_gen, the
//   unsent clearance text still lives in last_tower_response_text_, system
//   entry steers the pilot toward "Wiederholen Sie" so REQUEST_REPEAT
//   replays it. In both cases tts_network_error_ is raised so the UI can
//   show the manual retry button.
//
// Reads its parameters from last_guarded_req_ so the flight-loop poller can
// re-invoke it for the delayed auto-retries without touching the pipeline.
static void dispatch_guarded_tts() {
  if (!last_guarded_req_.valid)
    return;

  state_ = PTTState::PLAYING;
  tts_pending_ = true;
  ++total_inferences_;

  backends::tts::synthesize_async(
      last_guarded_req_.final_text, last_guarded_req_.role,
      last_guarded_req_.length_scale,
      [](backends::tts::Audio audio, bool success) {
        tts_pending_ = false;
        if (success && !audio.pcm16.empty()) {
          dialog_tts_failures_ = 0;
          tts_network_error_ = false;
          tts_blocked_ = false;
          last_guarded_req_.valid = false;
          if (settings::debug_logging()) {
            char dbg[160];
            std::snprintf(
                dbg, sizeof(dbg),
                "[xp_wellys_vfr_atc][DEBUG] TTS produced %zu samples "
                "@ %u Hz\n",
                audio.pcm16.size(), audio.sample_rate_hz);
            XPLMDebugString(dbg);
          }
          int com = settings::active_com();
          audio_player::play_pcm_on_com(com, std::move(audio.pcm16),
                                        audio.sample_rate_hz, audio.channels,
                                        settings::volume());
          return;
        }
        // A provider content-moderation guardrail (issue #62) is
        // deterministic — retrying the identical text can never succeed —
        // so skip the retry loop entirely and degrade immediately.
        // Transient failures still get the auto-retry treatment.
        const bool blocked = audio.content_blocked;
        if (!blocked) {
          // TTS failed transiently. Auto-retry a few times through the
          // flight-loop poller (state stays PLAYING so PTT is blocked; no
          // squelch/revert between tries) before engaging the revert guard.
          ++dialog_tts_failures_;
          if (dialog_tts_failures_ < kDialogMaxTtsTries) {
            dialog_retry_timer_ = kDialogRetryDelaySec;
            char dbg[128];
            std::snprintf(dbg, sizeof(dbg),
                          "[xp_wellys_vfr_atc] TTS failed (%d/%d), dialog "
                          "retry scheduled in %.0fs\n",
                          dialog_tts_failures_, kDialogMaxTtsTries,
                          kDialogRetryDelaySec);
            XPLMDebugString(dbg);
            return;
          }
        }

        // Give up — engage the revert guard. `blocked` short-circuits here
        // on the first failure (no retries); a transient failure lands here
        // after exhausting kDialogMaxTtsTries.
        XPLMDebugString(
            blocked ? "[xp_wellys_vfr_atc][ERROR] TTS content blocked by "
                      "provider guardrail, degrading (squelch + state check)\n"
                    : "[xp_wellys_vfr_atc][ERROR] TTS failed after retries, "
                      "applying revert guard (squelch + state check)\n");
        const int com = settings::active_com();
        audio_player::play_squelch_burst(com);

        const bool restored = atc_state_machine::restore_snapshot_if_gen(
            last_guarded_req_.pre_snap, last_guarded_req_.expected_gen);
        const char *sys_text;
        if (blocked)
          sys_text =
              restored
                  ? "Sprachausgabe vom Anbieter blockiert - Funkspruch nicht "
                    "uebertragen. Text im Transkript."
                  : "Sprachausgabe vom Anbieter blockiert - Anweisung nicht "
                    "uebertragen. Text im Transkript oder 'Wiederholen Sie' "
                    "sagen.";
        else
          sys_text =
              restored ? "Netzwerkfehler - Funkspruch nicht uebertragen. "
                         "'Nochmal versuchen' druecken oder wiederholen."
                       : "Netzwerkfehler - Anweisung nicht uebertragen. "
                         "'Nochmal versuchen' druecken oder 'Wiederholen Sie' "
                         "sagen.";
        transcript_.push_back(TranscriptEntry{
            static_cast<double>(XPLMGetElapsedTime()),
            TranscriptKind::System,
            sys_text,
            "",
        });
        if (blocked)
          tts_blocked_ = true;
        else
          tts_network_error_ = true;
        dialog_tts_failures_ = 0;
        dialog_retry_timer_ = 0.0f;
        last_guarded_req_.valid = false;
        state_ = PTTState::IDLE;
      });
}

// Speak a tower response with the state-revert guard active. Used for
// engine output that committed a semantic state transition — if the
// TTS playback fails (after auto-retries), the pilot never heard the
// clearance, so the state must be rolled back (or, when a later mutation
// makes the rollback unsafe, the clearance text must remain available for
// REQUEST_REPEAT replay).
//
//   pre_snap     = atc_state_machine::capture_snapshot() taken BEFORE
//                  the process() call that produced `text`.
//   expected_gen = atc_state_machine::current_gen() taken IMMEDIATELY
//                  AFTER that process() call.
//
// Stashes the fully-normalized utterance into last_guarded_req_ and hands
// off to dispatch_guarded_tts(), which owns the retry/give-up logic.
static void speak_response_guarded(const std::string &text,
                                   model_manifest::VoiceRole role,
                                   float length_scale,
                                   atc_state_machine::AtcStateSnapshot pre_snap,
                                   uint64_t expected_gen) {
  std::string final_text =
      (settings::atc_profile() == "DE")
          ? de_phraseology::normalize_for_speech(text)
          : (settings::atc_profile() == "EN"
                 ? en_phraseology::normalize_for_speech(text)
                 : text);

  last_guarded_req_ = GuardedTtsRequest{
      std::move(final_text), role, length_scale, std::move(pre_snap),
      expected_gen,          true,
  };
  dialog_tts_failures_ = 0;
  dialog_retry_timer_ = 0.0f;
  tts_network_error_ = false;
  tts_blocked_ = false;
  dispatch_guarded_tts();
}

// Shared "got a pilot transcript, run it through the engine and speak the
// reply" path. Used by:
//   - the STT callback in on_ptt_released() — voice path; quality comes
//     from the STT result and may be < 0.3 (triggers the engine's
//     "say again" branch without writing a pilot transcript row).
//   - submit_text() — Debug-Texteingabe; quality is always 1.0.
//
// Pre-conditions: state_ == PTTState::PROCESSING. ++total_transcriptions_
// has been incremented by the caller.
static void dispatch_pilot_transcript(const std::string &text, float quality) {
  ++total_inferences_;

  // Retain the transmission for the manual retry button and clear any prior
  // network-error state — a fresh exchange starts here (voice PTT,
  // submit_text, or the retry button itself all funnel through this path).
  last_pilot_transcript_ = text;
  last_pilot_quality_ = quality;
  tts_network_error_ = false;
  tts_blocked_ = false;

  const auto &ctx = xplane_context::get();
  float active_freq =
      (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
  char freq_str[16];
  std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);

  // Transcript writing is a UI concern, done here before handing off to
  // the engine. Low-quality transcripts skip the pilot row and go
  // straight to a "say again" response from the engine.
  bool is_pilot_row_written = false;
  if (quality >= 0.3f) {
    transcript_.push_back(TranscriptEntry{
        static_cast<double>(XPLMGetElapsedTime()),
        TranscriptKind::Pilot,
        text,
        freq_str,
    });
    is_pilot_row_written = true;
  }

  std::string freq_str_copy = freq_str;

  engine::Input in{
      text,
      quality,
      &ctx,
      settings::pilot_callsign(),
      static_cast<double>(XPLMGetElapsedTime()),
  };

  // Snapshot the ATC state BEFORE process() runs so the TTS revert
  // guard can roll it back on synthesis failure. The matching
  // expected_gen is captured inside the engine callback — directly
  // after process() — so a later auto-correction between the synthesise
  // call and the TTS error reply is detected and switches us to the
  // "stale" branch (clearance text stays accessible via REQUEST_REPEAT).
  auto pre_snap = atc_state_machine::capture_snapshot();

  engine::process_transcript(
      std::move(in),
      [freq_str_copy, is_pilot_row_written,
       pre_snap = std::move(pre_snap)](const engine::Output &out) mutable {
        last_pilot_message_ = out.parsed;
        if (out.response_text.empty()) {
          state_ = PTTState::IDLE;
          return;
        }
        // expected_gen lives between process() and TTS callback —
        // captured here, the instant the engine returns.
        const uint64_t expected_gen = atc_state_machine::current_gen();
        // Quality-rejection path didn't write a pilot row — the ATC
        // "say again" still deserves a transcript entry with the active
        // frequency.
        std::string freq_for_atc =
            is_pilot_row_written ? freq_str_copy : std::string();
        transcript_.push_back(TranscriptEntry{
            static_cast<double>(XPLMGetElapsedTime()),
            TranscriptKind::Tower,
            out.response_text,
            freq_for_atc,
        });
        // Role follows the frequency the pilot is currently tuned to —
        // that's the controller actually transmitting. Tying it to the
        // state machine misroutes handoff messages (Ground saying
        // "contact Tower" would speak with Tower's voice).
        const auto &c = xplane_context::get();
        auto role = role_for_frequency(c);
        speak_response_guarded(out.response_text, role, 1.0f,
                               std::move(pre_snap), expected_gen);
      });
}

void init() {
  state_ = PTTState::IDLE;
  tts_pending_ = false;
  last_duration_ = 0.0f;
  last_samples_ = 0;
  last_wav_bytes_ = 0;
  transcript_.clear();
  last_pilot_message_ = {};
  total_transcriptions_ = 0;
  total_inferences_ = 0;
  engine::reset();
  atis_playing_ = false;
  atis_active_com_ = 0;
  atis_cooldown_ = 0.0f;
  atis_tuned_timer_ = 0.0f;
  atis_tts_failures_ = 0;
  dialog_tts_failures_ = 0;
  dialog_retry_timer_ = 0.0f;
  tts_network_error_ = false;
  tts_blocked_ = false;
  last_guarded_req_.valid = false;
  last_pilot_transcript_.clear();
  last_pilot_quality_ = 1.0f;
}

void stop() {
  state_ = PTTState::IDLE;
  tts_pending_ = false;
  atis_tts_failures_ = 0; // mirror init(): drop the cross-flight TTS-retry counter
  dialog_tts_failures_ = 0;
  dialog_retry_timer_ = 0.0f;
  tts_network_error_ = false;
  tts_blocked_ = false;
  last_guarded_req_.valid = false;
}

void on_ptt_pressed() {
  if (state_ != PTTState::IDLE) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "[xp_wellys_vfr_atc] PTT blocked, state=%d\n",
                  static_cast<int>(state_));
    XPLMDebugString(buf);
    return;
  }

  // Radio requires power (checks COM radio power DataRef, handles
  // avionics master, battery, and individual radio switches)
  const auto &ctx = xplane_context::get();
  if (!ctx.com_radio_powered) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] PTT blocked - COM radio not powered\n");
    return;
  }

  // Backends must be loaded — without STT we cannot transcribe and
  // without LM we cannot reliably resolve low-confidence transcripts.
  // The plugin's startup path surfaces the model-download dialog when
  // anything is missing; this gate prevents PTT from doing nothing
  // visible.
  if (!backends::stt_ready() || !backends::lm_ready() ||
      !backends::tts_ready()) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc][ERROR] PTT blocked - local models not "
        "loaded (open the plugin window to download)\n");
    return;
  }

  state_ = PTTState::RECORDING;
  audio_player::play_ptt_click();
  audio_recorder::start_recording();
  if (settings::debug_logging())
    XPLMDebugString("[xp_wellys_vfr_atc][DEBUG] PTT pressed\n");
}

void on_ptt_released() {
  if (state_ != PTTState::RECORDING)
    return;

  audio_recorder::stop_recording();

  last_duration_ = audio_recorder::duration_seconds();
  last_samples_ = audio_recorder::buffer_samples();

  // Minimum duration gate
  if (last_duration_ < kMinRecordingDuration) {
    char buf[128];
    std::snprintf(
        buf, sizeof(buf),
        "[xp_wellys_vfr_atc] Recording too short (%.2fs), discarding\n",
        last_duration_);
    XPLMDebugString(buf);
    state_ = PTTState::IDLE;
    return;
  }

  std::vector<int16_t> pcm = audio_recorder::take_pcm();
  unsigned src_rate = audio_recorder::sample_rate_hz();
  last_wav_bytes_ = pcm.size() * sizeof(int16_t);

  if (settings::debug_logging()) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "[xp_wellys_vfr_atc][DEBUG] Recording stopped: %.1fs, %zu "
                  "samples @ %u Hz\n",
                  last_duration_, last_samples_, src_rate);
    XPLMDebugString(buf);
  }

  state_ = PTTState::PROCESSING;

  // Build airport context for the Whisper initial prompt — biases
  // transcription of local proper nouns ("Grenchen", "Speck", etc.).
  // While a runway is locked by ATC clearance, append it so Whisper
  // doesn't drift to the new wind-active runway after a wind shift.
  const auto &ctx_for_whisper = xplane_context::get();
  std::string airport_ctx = ctx_for_whisper.nearest_airport_id;
  if (!ctx_for_whisper.nearest_airport_name.empty())
    airport_ctx += " " + ctx_for_whisper.nearest_airport_name;
  const std::string &locked_rwy = atc_state_machine::assigned_runway();
  if (!locked_rwy.empty())
    airport_ctx += " runway " + locked_rwy;

  // Bias toward the pilot's own callsign. The spelled-out phonetic
  // alphabet is what the small local Whisper model mangles most — its
  // German language prior turns "Whiskey Romeo Oscar" into real words
  // ("Wisskrieg", "Wiesbaecki"), which destroys intent detection.
  // Conditioning the initial prompt on the expected spoken callsign
  // anchors the transcription. pilot_callsign() returns the
  // profile-aware German-expanded spoken form ("Hotel Bravo Whiskey
  // Romeo Oscar") — exactly the token sequence we want recognized.
  // Also feeds the cloud backends: OpenAI consumes it as free-prompt
  // text, Mistral splits it into per-word context_bias[] tokens.
  const std::string callsign = settings::pilot_callsign();
  if (!callsign.empty())
    airport_ctx += " " + callsign;

  // Bias toward German VFR ground/pattern vocabulary. Whisper's German prior
  // mangles low-frequency aviation terms into near-homophones (observed:
  // "Vorfeld" -> "Fahrfeld"), which breaks substring-based intent/position
  // detection downstream. Conditioning the prompt on the expected vocabulary
  // anchors the transcription across all three backends (OpenAI free-prompt,
  // local initial_prompt, Mistral per-word context_bias[]). Compact + constant
  // to stay well under the OpenAI 224-token prompt limit. See GitHub issue #9.
  //
  // The readback-critical NfL §25 b) Nr. 1 elements — Piste, QNH, Squawk,
  // Transponder, Frequenz — plus the departure clearance word Startfrei are
  // anchored too: Voxtral (Mistral STT) is materially weaker on German
  // aviation terms and was observed mis-hearing "Piste" as "Biste" and
  // dropping "QNH" entirely, which fails every readback-conformance check
  // downstream (issue #79). Biasing these is backend-agnostic and helps
  // OpenAI + local as well.
  static const char *const kVfrVocabBias =
      "Vorfeld Rollhalt Platzrunde Gegenanflug Queranflug Endanflug "
      "Abstellposition Piste QNH Squawk Transponder Frequenz Startfrei";
  airport_ctx += " ";
  airport_ctx += kVfrVocabBias;

  backends::stt::transcribe_async(
      std::move(pcm), src_rate,
      [](const backends::stt::TranscriptResult &wr) {
        if (!wr.success) {
          logging::error("STT error: %s", wr.text.c_str());
          transcript_.push_back(TranscriptEntry{
              static_cast<double>(XPLMGetElapsedTime()),
              TranscriptKind::System,
              wr.text,
              "",
          });
          state_ = PTTState::IDLE;
          return;
        }

        ++total_transcriptions_;
        dispatch_pilot_transcript(wr.text, wr.quality);
      },
      airport_ctx);
}

// Debug-Texteingabe convenience: replace the keyword "REG" (case-
// insensitive) with the configured phonetic pilot callsign. Lets the
// user type "Bern Tower REG, ready for departure" instead of spelling
// "November One Two Three Alpha Bravo" every time. Voice path is
// untouched — Whisper produces phonetic words, this expansion only
// runs for typed input.
//
// Word-boundary matching: the keyword must be its own token (after
// stripping surrounding punctuation), so "REGION" stays unchanged.
// All occurrences in the text are replaced.
static std::string expand_callsign_placeholder(const std::string &text) {
  const std::string phonetic = settings::pilot_callsign();
  if (phonetic.empty())
    return text;

  auto eq_ci = [](const std::string &a, const char *b) {
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i])))
        return false;
    }
    return i == a.size() && b[i] == '\0';
  };

  std::string out;
  out.reserve(text.size() + phonetic.size());
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    if (std::isspace(static_cast<unsigned char>(text[i]))) {
      out += text[i++];
      continue;
    }
    size_t tok_start = i;
    while (i < n && !std::isspace(static_cast<unsigned char>(text[i])))
      ++i;
    std::string tok = text.substr(tok_start, i - tok_start);

    // Split off leading and trailing punctuation around the core.
    size_t lead = 0;
    while (lead < tok.size() &&
           !std::isalnum(static_cast<unsigned char>(tok[lead])))
      ++lead;
    size_t trail = tok.size();
    while (trail > lead &&
           !std::isalnum(static_cast<unsigned char>(tok[trail - 1])))
      --trail;
    std::string core = tok.substr(lead, trail - lead);
    if (eq_ci(core, "REG")) {
      out += tok.substr(0, lead);
      out += phonetic;
      out += tok.substr(trail);
    } else {
      out += tok;
    }
  }
  return out;
}

void submit_text(const std::string &text) {
  if (state_ != PTTState::IDLE) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "[xp_wellys_vfr_atc] submit_text blocked, state=%d\n",
                  static_cast<int>(state_));
    XPLMDebugString(buf);
    return;
  }
  if (text.empty())
    return;
  // LM + TTS still need to be loaded — text bypasses STT, but the
  // downstream stages are identical to the voice path.
  if (!backends::lm_ready() || !backends::tts_ready()) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc][ERROR] submit_text blocked - LM/TTS not "
        "ready (open the plugin window to download)\n");
    return;
  }
  state_ = PTTState::PROCESSING;
  ++total_transcriptions_;
  // quality = 1.0 — typed text skips the Whisper quality gate; the engine
  // will run the full intent-parse + state-machine path with no "say
  // again" short-circuit. Expand the "REG" placeholder keyword to the
  // configured phonetic callsign so the user doesn't have to spell out
  // the full registration in every test message.
  dispatch_pilot_transcript(expand_callsign_placeholder(text), 1.0f);
}

void update() {
  // Hold PLAYING while a dialog auto-retry is pending (dialog_retry_timer_ >
  // 0) so PTT stays blocked between the silent retry attempts — otherwise
  // the state would flip to IDLE the instant synthesis returned empty.
  if (state_ == PTTState::PLAYING && !tts_pending_ &&
      !audio_player::is_playing() && dialog_retry_timer_ <= 0.0f) {
    if (atis_playing_) {
      atis_playing_ = false;
      if (settings::debug_logging())
        XPLMDebugString("[xp_wellys_vfr_atc][DEBUG] ATIS playback finished, "
                        "state -> IDLE\n");
    } else {
      if (settings::debug_logging())
        XPLMDebugString(
            "[xp_wellys_vfr_atc][DEBUG] Playback finished, state -> IDLE\n");
    }
    state_ = PTTState::IDLE;
  }

  // ATIS cooldown timer
  float dt = 1.0f / 60.0f; // approximate per-frame at ~60fps
  if (atis_cooldown_ > 0.0f)
    atis_cooldown_ -= dt;

  // Dialog TTS auto-retry timer — re-dispatch the same guarded utterance
  // once the short gap elapses (Issue #46). The revert guard only fires
  // after kDialogMaxTtsTries attempts, inside dispatch_guarded_tts().
  if (dialog_retry_timer_ > 0.0f) {
    dialog_retry_timer_ -= dt;
    if (dialog_retry_timer_ <= 0.0f) {
      dialog_retry_timer_ = 0.0f;
      if (last_guarded_req_.valid) {
        XPLMDebugString(
            "[xp_wellys_vfr_atc] Dialog TTS auto-retry firing\n");
        dispatch_guarded_tts();
      }
    }
  }

  // Flight-phase auto-correction of ATC state
  double now_secs_for_state = static_cast<double>(XPLMGetElapsedTime());
  atc_state_machine::check_auto_correction(flight_phase::get(), dt,
                                           now_secs_for_state);

  // Airport-change reset of EN_ROUTE → IDLE. Runs every frame so the
  // UI hint pipeline reflects a new airport's options the moment the
  // airport lock changes, instead of waiting for the next PTT call.
  atc_state_machine::check_airport_change(xplane_context::get(),
                                          now_secs_for_state);

  // Phase-2 traffic advisory poll. SDK-free engine helper consumes the
  // live traffic_context snapshot + state-machine state and may emit a
  // synthetic advisory transition. Only run while idle so a controller
  // utterance never overlaps with a pilot-driven exchange.
  if (state_ == PTTState::IDLE && backends::tts_ready()) {
    const auto &ctx_now = xplane_context::get();
    double now_secs = static_cast<double>(XPLMGetElapsedTime());

    // Phase-4 go-around trigger runs *before* the traffic advisory so a
    // single tick can never produce both: when the runway is occupied,
    // the go-around call is the more urgent of the two.
    std::string go_around_text;
    if (engine::poll_go_around(ctx_now, now_secs, &go_around_text) &&
        !go_around_text.empty()) {
      float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                    : ctx_now.com2_freq_mhz;
      char freq_str[16];
      std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
      transcript_.push_back(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          TranscriptKind::Tower,
          go_around_text,
          freq_str,
      });
      auto role = role_for_frequency(ctx_now);
      speak_response(go_around_text, role, 1.0f);
    } else {
      std::string advisory_text;
      if (engine::poll_traffic_advisory(ctx_now, now_secs, &advisory_text) &&
          !advisory_text.empty()) {
        float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                      : ctx_now.com2_freq_mhz;
        char freq_str[16];
        std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
        transcript_.push_back(TranscriptEntry{
            static_cast<double>(XPLMGetElapsedTime()),
            TranscriptKind::Tower,
            advisory_text,
            freq_str,
        });
        auto role = role_for_frequency(ctx_now);
        speak_response(advisory_text, role, 1.0f);
      }
    }
  }

  // Airport-change detection lives in atc_state_machine::process now —
  // it fires off the next pilot transmission. No per-frame loop here.

  // ATIS playback trigger — requires COM radio power + tuning delay.
  // ATIS reception works on EITHER COM1 or COM2: pilots commonly park
  // ATIS on COM2 (standby) while keeping COM1 on the controller's freq.
  const auto &ctx = xplane_context::get();
  int atis_com =
      ctx.com_radio_powered ? atis_generator::which_com_tuned_to_atis(ctx) : 0;
  bool tuned = atis_com != 0;

  if (tuned) {
    atis_tuned_timer_ += dt;
  } else {
    atis_tuned_timer_ = 0.0f;
    atis_tts_failures_ = 0; // tuning away abandons the retry series
  }

  // Pilot retuned (or powered off) the specific COM that's playing ATIS
  // — abort. Aborts even if the OTHER COM is still on ATIS, because we
  // can't switch buses mid-broadcast. The cooldown stays intact so re-
  // tuning ATIS within the cooldown window stays silent (we already
  // announced this letter).
  if (atis_playing_ && atis_com != atis_active_com_) {
    audio_player::abort_playback();
    atis_playing_ = false;
    state_ = PTTState::IDLE;
    if (settings::debug_logging())
      XPLMDebugString(
          "[xp_wellys_vfr_atc][DEBUG] ATIS aborted: pilot retuned "
          "the COM that was playing ATIS\n");
  }

  // ATIS is a side-channel like Traffic — independent of ATCState.
  // Pilot can re-tune ATIS at any point (e.g. holding point) to refresh
  // the broadcast. Only PTT state and TTS readiness gate playback so we
  // never overlap an active pilot/controller exchange.
  if (state_ == PTTState::IDLE && atis_cooldown_ <= 0.0f && tuned &&
      atis_tuned_timer_ >= kAtisTuneDelaySec && backends::tts_ready()) {
    std::string atis_text = atis_generator::generate_atis_text(ctx);

    if (settings::debug_logging()) {
      char dbg[64];
      std::snprintf(dbg, sizeof(dbg), " (COM%d)", atis_com);
      XPLMDebugString(("[xp_wellys_vfr_atc][DEBUG] ATIS triggered" +
                       std::string(dbg) + ": " + atis_text + "\n")
                          .c_str());
    }

    // Log the ATIS broadcast against the COM it actually plays on, not
    // the active COM — pilot may be on Tower with active=COM1 while
    // ATIS streams through COM2.
    float atis_com_freq =
        (atis_com == 2) ? ctx.com2_freq_mhz : ctx.com1_freq_mhz;
    char freq_str[16];
    std::snprintf(freq_str, sizeof(freq_str), "%.3f", atis_com_freq);

    // Stage the transcript entry but do not publish it yet — the push
    // happens in the TTS success callback so a silent synthesis
    // failure (bad voice id, OpenAI error, network) leaves no ghost
    // line in the history. Cooldown + atis_playing_ ARE set up-front
    // to block a retrigger loop while synthesis is in flight.
    TranscriptEntry pending_entry{
        static_cast<double>(XPLMGetElapsedTime()),
        TranscriptKind::Tower,
        atis_text,
        freq_str,
    };

    atis_playing_ = true;
    atis_active_com_ = atis_com;
    atis_cooldown_ = kAtisCooldownSec;
    // ATIS reads slower than tower/ground — Piper length_scale > 1
    // produces the slower rate the OpenAI path used to get from
    // speed=0.85.
    speak_response(
        atis_text, model_manifest::VoiceRole::Atis, 1.18f, atis_com,
        [entry = std::move(pending_entry)]() mutable {
          atis_tts_failures_ = 0;
          transcript_.push_back(std::move(entry));
        },
        []() {
          // TTS failed: re-trigger after a short gap (the cooldown poller
          // re-fires while the pilot stays tuned), then give up + warn.
          atis_playing_ = false;
          ++atis_tts_failures_;
          if (atis_tts_failures_ < kAtisMaxTtsTries) {
            atis_cooldown_ = kAtisRetryDelaySec;
            XPLMDebugString("[xp_wellys_vfr_atc] ATIS TTS failed, retry "
                            "scheduled in 15s\n");
          } else {
            audio_player::play_squelch_burst(settings::active_com());
            transcript_.push_back(TranscriptEntry{
                static_cast<double>(XPLMGetElapsedTime()),
                TranscriptKind::System,
                "Kommunikation gestoert - Sprachausgabe (TTS) nicht "
                "verfuegbar. Bitte Internetverbindung und Backend pruefen.",
                "",
            });
            atis_cooldown_ = kAtisCooldownSec;
            atis_tts_failures_ = 0;
          }
        });
  }
}

void reset_atis_cooldown() {
  atis_cooldown_ = 0.0f;
  atis_tuned_timer_ = 0.0f;
  atis_tts_failures_ = 0;
}

bool tts_network_error() { return tts_network_error_; }

bool tts_blocked() { return tts_blocked_; }

void retry_last_transmission() {
  // Re-run the last pilot transmission through the full pipeline. After a
  // give-up the ATC state was reverted to pre_snap, so this reproduces the
  // identical controller reply, re-applies the state transition, and starts
  // a fresh kDialogMaxTtsTries auto-retry chain. No-op unless idle, a
  // network error is pending, we have a stored transmission, and TTS is up.
  if (state_ != PTTState::IDLE || !tts_network_error_ ||
      last_pilot_transcript_.empty() || !backends::tts_ready())
    return;

  XPLMDebugString(
      "[xp_wellys_vfr_atc] Manual retry: replaying last transmission\n");
  state_ = PTTState::PROCESSING;
  ++total_transcriptions_;
  dispatch_pilot_transcript(last_pilot_transcript_, last_pilot_quality_);
}

PTTState ptt_state() { return state_; }

std::string ptt_state_label() {
  switch (state_) {
  case PTTState::IDLE:
    return "Ready";
  case PTTState::RECORDING:
    return "[REC]";
  case PTTState::PROCESSING:
    return "[Processing...]";
  case PTTState::PLAYING:
    return "[ATC speaking...]";
  }
  return "UNKNOWN";
}

float last_recording_duration() { return last_duration_; }
size_t last_recording_samples() { return last_samples_; }
size_t last_wav_bytes() { return last_wav_bytes_; }

const intent_parser::PilotMessage &last_pilot_message() {
  return last_pilot_message_;
}

const std::vector<TranscriptEntry> &transcript_entries() { return transcript_; }

void clear_transcript() { transcript_.clear(); }

std::string last_atc_response() {
  // Tower entries only — System entries (e.g. "Funkstoerung") do not
  // count as ATC replies, even though they appear in the transcript.
  for (auto it = transcript_.rbegin(); it != transcript_.rend(); ++it) {
    if (it->kind == TranscriptKind::Tower)
      return it->text;
  }
  return "";
}

int total_transcriptions() { return total_transcriptions_; }
int total_api_calls() { return total_inferences_ + engine::lm_inferences(); }

} // namespace atc_session
