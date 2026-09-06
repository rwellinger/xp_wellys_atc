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

#ifndef AUDIO_PLAYER_HPP
#define AUDIO_PLAYER_HPP

#include <cstdint>
#include <vector>

namespace audio_player {

void init();
void stop();

// Play a short PTT click on the X-Plane radio bus (routes to Radio Device)
void play_ptt_click();

// Play raw int16 PCM samples on the X-Plane radio bus at given volume
// (0.0–1.0). Routes to whichever COM the user has marked active in
// settings. Used for controller TTS replies — the pilot transmits on
// the active COM, so the reply plays back there.
void play_pcm(std::vector<int16_t> pcm16, uint32_t sample_rate_hz, int channels,
              float volume);

// Play raw int16 PCM samples on a specific COM's radio bus (1 or 2). Use
// this when the playback should not follow the active-COM setting —
// e.g. ATIS auto-broadcast on the COM that is tuned to ATIS, even when
// the pilot's active COM is on Tower. Other com values fall back to
// COM1.
void play_pcm_on_com(int com, std::vector<int16_t> pcm16,
                     uint32_t sample_rate_hz, int channels, float volume);

// Play WAV data on the X-Plane radio bus at given volume (0.0–1.0).
// Kept for the audio-self-test feature in the UI which records mic →
// playback to validate the device chain.
void play_wav(const std::vector<uint8_t> &wav_data, float volume);

// Returns true while audio is being played back
bool is_playing();

// Stop the currently-playing audio immediately. Unlike stop() this is
// intended for in-flight aborts (pilot retunes COM during ATIS playback,
// disregards a long ATC reply, etc.) — the audio system stays
// initialized and ready for the next play_pcm() call. Safe to call when
// nothing is playing.
void abort_playback();

// Synthesise and play a short "noisy radio glitch" on the requested
// COM (1 or 2; falls back to the active COM for other values). Used by
// atc_session's TTS revert guard to signal "the reply did not get
// through" without speaking text — the pilot reflexively answers
// "Sagen Sie nochmal" / "Wiederholen Sie" and the REQUEST_REPEAT
// pipeline takes over from there. ~350 ms total: pink noise that
// briefly modulates plus a short squelch click at the tail. Generated
// in-process (std::mt19937 + a one-pole low-pass) — no WAV asset, no
// network, so it cannot fail in the same way the upstream TTS just
// did. Respects settings::volume().
void play_squelch_burst(int com);

} // namespace audio_player

#endif // AUDIO_PLAYER_HPP
