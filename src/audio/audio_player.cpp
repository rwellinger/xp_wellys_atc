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

#include "audio/audio_player.hpp"
#include "persistence/settings.hpp"

#include <XPLMSound.h>
#include <XPLMUtilities.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <random>

namespace audio_player {

static constexpr float kClickFreqHz = 880.0f;
static constexpr float kClickDurationSec = 0.08f;
static constexpr float kClickSampleRate = 44100.0f;

// ── FMOD channel tracking ────────────────────────────────────────

static std::atomic<bool> is_playing_{false};
static FMOD_CHANNEL *active_channel_ = nullptr;
static std::mutex channel_mutex_;

// PCM buffer must remain valid until FMOD completion callback fires
static std::vector<int16_t> active_pcm16_;

static void pcm_complete_cb(void * /*inRefcon*/, FMOD_RESULT /*status*/) {
  std::lock_guard<std::mutex> lock(channel_mutex_);
  active_channel_ = nullptr;
  is_playing_ = false;
}

// ── Core helper: play int16 PCM on given bus ─────────────────────
// Must be called on the X-Plane main thread.

static void play_pcm16(std::vector<int16_t> pcm16, int freq_hz, int channels,
                       float volume, XPLMAudioBus bus) {
  // Stop any current playback
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (active_channel_) {
      XPLMStopAudio(active_channel_);
      active_channel_ = nullptr;
    }
  }
  is_playing_ = false;

  if (pcm16.empty()) {
    XPLMDebugString("[xp_wellys_vfr_atc] play_pcm16: empty buffer\n");
    return;
  }

  // Keep PCM alive for FMOD
  active_pcm16_ = std::move(pcm16);

  FMOD_CHANNEL *ch = XPLMPlayPCMOnBus(
      active_pcm16_.data(),
      static_cast<uint32_t>(active_pcm16_.size() * sizeof(int16_t)),
      FMOD_SOUND_FORMAT_PCM16, freq_hz, channels,
      /*loop=*/0, bus, pcm_complete_cb, nullptr);

  if (!ch) {
    XPLMDebugString("[xp_wellys_vfr_atc] XPLMPlayPCMOnBus failed\n");
    return;
  }

  XPLMSetAudioVolume(ch, volume);

  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    active_channel_ = ch;
  }
  is_playing_ = true;

  const char *bus_name = "unknown";
  if (bus == xplm_AudioRadioCom1)
    bus_name = "COM1";
  else if (bus == xplm_AudioRadioCom2)
    bus_name = "COM2";
  else if (bus == xplm_AudioUI)
    bus_name = "UI";

  if (settings::debug_logging()) {
    char log[128];
    std::snprintf(
        log, sizeof(log),
        "[xp_wellys_vfr_atc][DEBUG] Playback started: %zu samples, %d Hz, "
        "%s bus\n",
        active_pcm16_.size() / channels, freq_hz, bus_name);
    XPLMDebugString(log);
  }
}

// ── WAV decode (PCM16 only) ───────────────────────────────────────

static bool decode_wav_to_pcm16(const std::vector<uint8_t> &wav_data,
                                std::vector<int16_t> &out_pcm,
                                int &out_channels, int &out_sample_rate) {
  if (wav_data.size() < 44)
    return false;

  if (wav_data[0] != 'R' || wav_data[1] != 'I' || wav_data[2] != 'F' ||
      wav_data[3] != 'F' || wav_data[8] != 'W' || wav_data[9] != 'A' ||
      wav_data[10] != 'V' || wav_data[11] != 'E') {
    XPLMDebugString("[xp_wellys_vfr_atc] WAV: bad magic\n");
    return false;
  }

  auto read_u16 = [&](size_t off) -> uint16_t {
    return static_cast<uint16_t>(wav_data[off]) |
           (static_cast<uint16_t>(wav_data[off + 1]) << 8);
  };
  auto read_u32 = [&](size_t off) -> uint32_t {
    return static_cast<uint32_t>(wav_data[off]) |
           (static_cast<uint32_t>(wav_data[off + 1]) << 8) |
           (static_cast<uint32_t>(wav_data[off + 2]) << 16) |
           (static_cast<uint32_t>(wav_data[off + 3]) << 24);
  };

  size_t pos = 12;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  size_t data_offset = 0;
  uint32_t data_size = 0;

  while (pos + 8 <= wav_data.size()) {
    char id[5] = {};
    std::memcpy(id, wav_data.data() + pos, 4);
    uint32_t chunk_size = read_u32(pos + 4);
    pos += 8;
    if (std::strcmp(id, "fmt ") == 0 && chunk_size >= 16) {
      if (read_u16(pos) != 1) {
        XPLMDebugString("[xp_wellys_vfr_atc] WAV: not PCM\n");
        return false;
      }
      channels = read_u16(pos + 2);
      sample_rate = read_u32(pos + 4);
      bits_per_sample = read_u16(pos + 14);
    } else if (std::strcmp(id, "data") == 0) {
      data_offset = pos;
      data_size = chunk_size;
      break;
    }
    pos += chunk_size;
  }

  if (data_offset == 0 || channels == 0 || bits_per_sample != 16) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_vfr_atc] WAV: invalid (ch=%u sr=%u bps=%u)\n",
                  channels, sample_rate, bits_per_sample);
    XPLMDebugString(log);
    return false;
  }

  size_t num_samples = data_size / sizeof(int16_t);
  out_pcm.resize(num_samples);
  std::memcpy(out_pcm.data(), wav_data.data() + data_offset,
              num_samples * sizeof(int16_t));

  out_channels = static_cast<int>(channels);
  out_sample_rate = static_cast<int>(sample_rate);

  char log[128];
  std::snprintf(log, sizeof(log),
                "[xp_wellys_vfr_atc] WAV decoded: %zu frames, %d ch, %d Hz\n",
                num_samples / channels, out_channels, out_sample_rate);
  XPLMDebugString(log);
  return true;
}

// ── Lifecycle ────────────────────────────────────────────────────

void init() {
  is_playing_ = false;
  active_channel_ = nullptr;
  XPLMDebugString(
      "[xp_wellys_vfr_atc] Audio player initialized (FMOD radio bus)\n");
}

void stop() {
  std::lock_guard<std::mutex> lock(channel_mutex_);
  if (active_channel_) {
    XPLMStopAudio(active_channel_);
    active_channel_ = nullptr;
  }
  is_playing_ = false;
  active_pcm16_.clear();
}

void abort_playback() {
  std::lock_guard<std::mutex> lock(channel_mutex_);
  if (!active_channel_) {
    return;
  }
  XPLMStopAudio(active_channel_);
  active_channel_ = nullptr;
  is_playing_ = false;
  active_pcm16_.clear();
}

// ── PTT click ────────────────────────────────────────────────────

void play_ptt_click() {
  float volume = settings::volume();
  if (volume <= 0.0f)
    return;

  int num_samples = static_cast<int>(kClickSampleRate * kClickDurationSec);
  std::vector<int16_t> samples(num_samples);
  for (int i = 0; i < num_samples; ++i) {
    float t = static_cast<float>(i) / kClickSampleRate;
    float sine = std::sin(2.0f * static_cast<float>(M_PI) * kClickFreqHz * t);
    float env = 1.0f;
    float fade_in = 0.005f * kClickSampleRate;
    float fade_out = 0.01f * kClickSampleRate;
    if (static_cast<float>(i) < fade_in)
      env = static_cast<float>(i) / fade_in;
    else if (static_cast<float>(i) > static_cast<float>(num_samples) - fade_out)
      env =
          (static_cast<float>(num_samples) - static_cast<float>(i)) / fade_out;
    samples[i] = static_cast<int16_t>(sine * env * 32767.0f);
  }

  XPLMAudioBus bus =
      (settings::active_com() == 2) ? xplm_AudioRadioCom2 : xplm_AudioRadioCom1;
  play_pcm16(std::move(samples), static_cast<int>(kClickSampleRate), 1, volume,
             bus);
}

// ── PCM playback (Piper TTS → radio bus) ────────────────────────

void play_pcm(std::vector<int16_t> pcm16, uint32_t sample_rate_hz, int channels,
              float volume) {
  play_pcm_on_com(settings::active_com(), std::move(pcm16), sample_rate_hz,
                  channels, volume);
}

void play_pcm_on_com(int com, std::vector<int16_t> pcm16,
                     uint32_t sample_rate_hz, int channels, float volume) {
  if (pcm16.empty()) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] play_pcm() called with empty buffer\n");
    return;
  }
  if (sample_rate_hz == 0 || channels < 1) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] play_pcm() called with invalid format\n");
    return;
  }

  XPLMAudioBus bus = (com == 2) ? xplm_AudioRadioCom2 : xplm_AudioRadioCom1;
  play_pcm16(std::move(pcm16), static_cast<int>(sample_rate_hz), channels,
             volume, bus);
}

// ── Squelch burst (TTS-failure feedback) ───────────────────────────

// Synthesise a short noisy radio glitch + tail click and play it on
// the requested COM bus. Generated in-process so it cannot fail from
// the same root cause as the upstream TTS (no network, no model
// file). About 350 ms total: low-passed white noise that fades in /
// out with a brief mid-burst dip, then a 25 ms 1 kHz click on the
// tail to mimic the squelch close.
void play_squelch_burst(int com) {
  constexpr int kSampleRate = 22050;
  constexpr float kNoiseDurationSec = 0.30f;
  constexpr float kSquelchClickDurationSec = 0.025f;
  constexpr float kSquelchClickFreqHz = 1000.0f;
  constexpr float kNoiseGain = 0.55f;
  constexpr float kClickGain = 0.65f;
  constexpr float kLpfAlpha = 0.35f; // one-pole LPF, ~kSampleRate*alpha cutoff

  const int noise_samples = static_cast<int>(kNoiseDurationSec * kSampleRate);
  const int click_samples =
      static_cast<int>(kSquelchClickDurationSec * kSampleRate);
  const int total = noise_samples + click_samples;

  std::vector<int16_t> pcm;
  pcm.reserve(total);

  // Fixed seed: the glitch sounds identical across runs — easier for
  // the user to recognise as "the failure tone" rather than something
  // changing. Deterministic also avoids needing to capture Time/RNG
  // state in tests.
  // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
  std::mt19937 rng(0xCAFEBABEu);
  std::uniform_real_distribution<float> uni(-1.0f, 1.0f);

  float lpf_state = 0.0f;
  for (int i = 0; i < noise_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(noise_samples);
    // Envelope: 20 ms attack, 20 ms release, slight mid-dip to give
    // the burst that "broken radio" rhythm instead of flat hiss.
    float env;
    if (t < 0.07f)
      env = t / 0.07f;
    else if (t > 0.93f)
      env = (1.0f - t) / 0.07f;
    else
      env = 1.0f;
    const float dip = 1.0f - 0.35f * std::sin(t * 6.28318f * 3.0f) *
                                 std::sin(t * 6.28318f * 3.0f);
    env *= dip;

    const float raw = uni(rng);
    lpf_state += kLpfAlpha * (raw - lpf_state);
    const float sample = lpf_state * env * kNoiseGain;
    int32_t s = static_cast<int32_t>(sample * 32767.0f);
    if (s > 32767)
      s = 32767;
    if (s < -32768)
      s = -32768;
    pcm.push_back(static_cast<int16_t>(s));
  }

  // Squelch click — short 1 kHz blip with a triangle envelope.
  for (int i = 0; i < click_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(click_samples);
    const float env = (t < 0.5f) ? (t / 0.5f) : ((1.0f - t) / 0.5f);
    const float ph =
        std::sin(2.0f * 3.14159265358979f * kSquelchClickFreqHz *
                 (static_cast<float>(i) / static_cast<float>(kSampleRate)));
    int32_t s = static_cast<int32_t>(ph * env * kClickGain * 32767.0f);
    if (s > 32767)
      s = 32767;
    if (s < -32768)
      s = -32768;
    pcm.push_back(static_cast<int16_t>(s));
  }

  XPLMAudioBus bus = (com == 2) ? xplm_AudioRadioCom2 : xplm_AudioRadioCom1;
  if (com != 1 && com != 2)
    bus = (settings::active_com() == 2) ? xplm_AudioRadioCom2
                                        : xplm_AudioRadioCom1;
  play_pcm16(std::move(pcm), kSampleRate, 1, settings::volume(), bus);
}

// ── WAV playback (test → UI bus, always audible) ────────────────

void play_wav(const std::vector<uint8_t> &wav_data, float volume) {
  if (wav_data.empty()) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] play_wav() called with empty data\n");
    return;
  }

  std::vector<int16_t> pcm16;
  int channels = 0;
  int sample_rate = 0;
  if (!decode_wav_to_pcm16(wav_data, pcm16, channels, sample_rate)) {
    XPLMDebugString("[xp_wellys_vfr_atc] WAV decode failed\n");
    return;
  }

  XPLMAudioBus bus =
      (settings::active_com() == 2) ? xplm_AudioRadioCom2 : xplm_AudioRadioCom1;
  play_pcm16(std::move(pcm16), sample_rate, channels, volume, bus);
}

bool is_playing() { return is_playing_.load(); }

} // namespace audio_player
