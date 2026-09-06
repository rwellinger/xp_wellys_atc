/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Issue #85: on macOS the STT input path used to decimate 48 kHz -> 16 kHz
 * with unfiltered linear interpolation, folding everything above 8 kHz back
 * into the speech band. Spelled NATO callsigns collapsed first, because the
 * sibilants that separate "Foxtrott" / "Oscar" / "Sierra" live at 4-10 kHz.
 *
 * The load-bearing test here is REJECTS_ABOVE_NYQUIST. It fails against the
 * old linear resampler and passes against the windowed-sinc one.
 */

#include "audio/pcm_resample.hpp"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<int16_t> make_sine(double freq_hz, double rate_hz, double seconds,
                               double amplitude = 0.5) {
  const auto n = static_cast<size_t>(rate_hz * seconds);
  std::vector<int16_t> pcm;
  pcm.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / rate_hz;
    const double v = amplitude * std::sin(2.0 * kPi * freq_hz * t);
    pcm.push_back(static_cast<int16_t>(v * 32767.0));
  }
  return pcm;
}

double rms(const std::vector<float> &samples) {
  if (samples.empty())
    return 0.0;
  double acc = 0.0;
  for (float s : samples)
    acc += static_cast<double>(s) * static_cast<double>(s);
  return std::sqrt(acc / static_cast<double>(samples.size()));
}

double rms(const std::vector<int16_t> &samples) {
  if (samples.empty())
    return 0.0;
  double acc = 0.0;
  for (int16_t s : samples) {
    const double v = static_cast<double>(s) / 32768.0;
    acc += v * v;
  }
  return std::sqrt(acc / static_cast<double>(samples.size()));
}

// Goertzel: energy at a single bin, no FFT dependency. Returns the
// magnitude normalised by the sample count.
double tone_magnitude(const std::vector<float> &samples, double freq_hz,
                      double rate_hz) {
  if (samples.empty())
    return 0.0;
  const double w = 2.0 * kPi * freq_hz / rate_hz;
  const double coeff = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (float x : samples) {
    const double s0 = static_cast<double>(x) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double mag = std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
  return mag / static_cast<double>(samples.size());
}

// Ignore the filter's edge transient at both ends; the windowed sinc needs
// ~half a kernel to settle and the recording boundaries are edge-extended.
std::vector<float> trim_edges(const std::vector<float> &in, size_t margin) {
  if (in.size() <= 2 * margin)
    return {};
  return std::vector<float>(in.begin() + static_cast<long>(margin),
                            in.end() - static_cast<long>(margin));
}

} // namespace

TEST_CASE("pcm_resample: 16 kHz input is a pure scale, sample for sample",
          "[pcm_resample]") {
  const std::vector<int16_t> pcm = {0, 1, -1, 32767, -32768, 12345, -12345};
  const auto out = pcm_resample::to_float_16k(pcm, 16000);

  REQUIRE(out.size() == pcm.size());
  for (size_t i = 0; i < pcm.size(); ++i)
    CHECK(out[i] == Catch::Approx(static_cast<float>(pcm[i]) / 32768.0f));
}

TEST_CASE("pcm_resample: unknown rate (0) falls through unscaled",
          "[pcm_resample]") {
  const std::vector<int16_t> pcm = {100, -100, 200};
  const auto out = pcm_resample::to_float_16k(pcm, 0);

  REQUIRE(out.size() == pcm.size());
  CHECK(out[0] == Catch::Approx(100.0f / 32768.0f));
}

TEST_CASE("pcm_resample: empty input yields empty output", "[pcm_resample]") {
  CHECK(pcm_resample::to_float_16k({}, 48000).empty());
  CHECK(pcm_resample::to_float_16k({}, 16000).empty());
}

TEST_CASE("pcm_resample: 48 kHz decimates 3:1", "[pcm_resample]") {
  const auto pcm = make_sine(1000.0, 48000.0, 0.5);
  const auto out = pcm_resample::to_float_16k(pcm, 48000);

  // 24000 in -> 8000 out, allow one sample of rounding slack.
  CHECK(out.size() >= pcm.size() / 3 - 1);
  CHECK(out.size() <= pcm.size() / 3 + 1);
}

TEST_CASE("pcm_resample: passband tone survives 48 kHz -> 16 kHz",
          "[pcm_resample]") {
  const auto pcm = make_sine(1000.0, 48000.0, 0.5);
  const auto out = pcm_resample::to_float_16k(pcm, 48000);
  const auto body = trim_edges(out, 256);

  REQUIRE_FALSE(body.empty());

  // Level preserved: the converter has unity DC gain and 1 kHz is far
  // inside the passband.
  CHECK(rms(body) > 0.85 * rms(pcm));

  // And the energy is still at 1 kHz, not smeared or shifted.
  const double at_1k = tone_magnitude(body, 1000.0, 16000.0);
  const double at_4k = tone_magnitude(body, 4000.0, 16000.0);
  CHECK(at_1k > 10.0 * at_4k);
}

TEST_CASE("pcm_resample: 6 kHz sibilant band survives 48 kHz -> 16 kHz",
          "[pcm_resample]") {
  // The band that carries the difference between spelled NATO words. It
  // sits below the 8 kHz target Nyquist and must NOT be filtered away by
  // the anti-aliasing low-pass.
  const auto pcm = make_sine(6000.0, 48000.0, 0.5);
  const auto out = pcm_resample::to_float_16k(pcm, 48000);
  const auto body = trim_edges(out, 256);

  REQUIRE_FALSE(body.empty());
  CHECK(rms(body) > 0.7 * rms(pcm));
}

TEST_CASE("pcm_resample: content above the target Nyquist is rejected, "
          "not aliased",
          "[pcm_resample]") {
  // 12 kHz at 48 kHz. Decimating 3:1 without a low-pass folds this onto
  // 4 kHz at full amplitude -- straight into the speech band. With a
  // proper anti-aliasing filter it must be attenuated instead.
  const auto pcm = make_sine(12000.0, 48000.0, 0.5);
  const auto out = pcm_resample::to_float_16k(pcm, 48000);
  const auto body = trim_edges(out, 256);

  REQUIRE_FALSE(body.empty());

  // Overall level must collapse.
  CHECK(rms(body) < 0.1 * rms(pcm));

  // Specifically: nothing may show up at the alias frequency.
  const double alias = tone_magnitude(body, 4000.0, 16000.0);
  const double reference = tone_magnitude(
      trim_edges(pcm_resample::to_float_16k(make_sine(4000.0, 48000.0, 0.5),
                                            48000),
                 256),
      4000.0, 16000.0);
  CHECK(alias < 0.05 * reference);
}

TEST_CASE("pcm_resample: 44.1 kHz also rejects above-Nyquist content",
          "[pcm_resample]") {
  // Non-integer ratio -- exercises the fractional-phase path rather than
  // the exact 3:1 decimation.
  const auto pcm = make_sine(11000.0, 44100.0, 0.5);
  const auto out = pcm_resample::to_float_16k(pcm, 44100);
  const auto body = trim_edges(out, 256);

  REQUIRE_FALSE(body.empty());
  CHECK(rms(body) < 0.1 * rms(pcm));

  const auto pass = pcm_resample::to_float_16k(make_sine(1000.0, 44100.0, 0.5),
                                               44100);
  const auto pass_body = trim_edges(pass, 256);
  REQUIRE_FALSE(pass_body.empty());
  CHECK(rms(pass_body) > 0.85 * rms(pcm));
}

TEST_CASE("pcm_resample: upsampling from 8 kHz keeps the signal",
          "[pcm_resample]") {
  const auto pcm = make_sine(1000.0, 8000.0, 0.5);
  const auto out = pcm_resample::to_float_16k(pcm, 8000);
  const auto body = trim_edges(out, 128);

  REQUIRE(out.size() >= pcm.size() * 2 - 1);
  REQUIRE_FALSE(body.empty());
  CHECK(rms(body) > 0.85 * rms(pcm));
}

TEST_CASE("pcm_resample: output stays inside [-1, 1]", "[pcm_resample]") {
  // Full-scale square-ish transients are where a windowed sinc overshoots.
  std::vector<int16_t> pcm;
  pcm.reserve(48000);
  for (size_t i = 0; i < 48000; ++i)
    pcm.push_back((i / 24) % 2 == 0 ? int16_t{32767} : int16_t{-32768});

  const auto out = pcm_resample::to_float_16k(pcm, 48000);
  REQUIRE_FALSE(out.empty());
  for (float s : out) {
    CHECK(s >= -1.0f);
    CHECK(s <= 1.0f);
  }
}
