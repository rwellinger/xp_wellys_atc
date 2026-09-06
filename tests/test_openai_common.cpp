/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Pure-data unit tests for the openai_common helpers (no network).
 */

#include "backends/openai_common.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace oc = backends::openai_common;

TEST_CASE("last4 truncates api keys for audit logging", "[openai_common]") {
  REQUIRE(oc::last4("sk-proj-1234567890ABCDEF") == "CDEF");
  REQUIRE(oc::last4("ABCD") == "ABCD");
  REQUIRE(oc::last4("abc") == "abc"); // shorter than 4 → returned whole
  REQUIRE(oc::last4("").empty());
}

TEST_CASE("WAV encode/decode roundtrip preserves samples within int16 quant",
          "[openai_common][wav]") {
  // Build a deterministic float waveform: 1 kHz sine for 100 ms at 16 kHz.
  constexpr uint32_t sr = 16000;
  constexpr size_t n = sr / 10; // 100 ms
  std::vector<float> pcm(n);
  for (size_t i = 0; i < n; ++i) {
    pcm[i] = static_cast<float>(
        0.5 * std::sin(2.0 * 3.14159265 * 1000.0 *
                       static_cast<double>(i) / sr));
  }

  const std::vector<uint8_t> wav = oc::pcm_float32_to_wav(pcm);
  REQUIRE(wav.size() == 44 + n * sizeof(int16_t));

  uint32_t out_sr = 0;
  const std::vector<int16_t> decoded = oc::wav_to_pcm_int16(wav, out_sr);
  REQUIRE(out_sr == sr);
  REQUIRE(decoded.size() == n);

  // Quantization tolerance: ±1 int16 step (~3.05e-5 normalized).
  for (size_t i = 0; i < n; ++i) {
    const int32_t expected = static_cast<int32_t>(std::lround(pcm[i] * 32767.0f));
    REQUIRE(std::abs(decoded[i] - expected) <= 1);
  }
}

TEST_CASE("WAV encode clamps out-of-range float samples",
          "[openai_common][wav]") {
  std::vector<float> pcm = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f};
  const std::vector<uint8_t> wav = oc::pcm_float32_to_wav(pcm);
  uint32_t out_sr = 0;
  const std::vector<int16_t> decoded = oc::wav_to_pcm_int16(wav, out_sr);
  REQUIRE(decoded.size() == pcm.size());
  REQUIRE(decoded[0] == -32767); // clipped to -1 then quantised
  REQUIRE(decoded[1] == -32767);
  REQUIRE(decoded[2] == 0);
  REQUIRE(decoded[3] == 32767);
  REQUIRE(decoded[4] == 32767);
}

TEST_CASE("WAV decoder rejects malformed input", "[openai_common][wav]") {
  uint32_t sr = 999;
  REQUIRE(oc::wav_to_pcm_int16(std::vector<uint8_t>(20, 0), sr).empty());
  REQUIRE(sr == 0);

  // Valid RIFF/WAVE header but bogus fmt chunk → reject.
  std::vector<uint8_t> bad(44, 0);
  bad[0] = 'R';
  bad[1] = 'I';
  bad[2] = 'F';
  bad[3] = 'F';
  bad[8] = 'W';
  bad[9] = 'A';
  bad[10] = 'V';
  bad[11] = 'E';
  REQUIRE(oc::wav_to_pcm_int16(bad, sr).empty());
}
