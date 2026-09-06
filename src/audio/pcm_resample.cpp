/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "audio/pcm_resample.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pcm_resample {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Fractional-delay quantisation. The kernel is built once per call for
// kPhases evenly spaced sub-sample offsets and the nearest one is picked
// per output sample. 512 phases put the resulting timing jitter around
// -60 dB at 8 kHz -- far below any microphone's noise floor, and it keeps
// this a single code path instead of one polyphase branch per rational
// ratio.
constexpr int kPhases = 512;

// Low-pass corner. Sits below the 8 kHz Nyquist of the target rate so the
// transition band has somewhere to go.
constexpr double kCutoffHz = 7600.0;

// Sinc lobes per side. Drives the filter length and therefore the
// transition width (Blackman: ~5.5 / taps). 32 gives ~205 taps at 48 kHz,
// i.e. roughly 1.3 kHz of transition -- flat to ~7 kHz, stopband from
// ~8.2 kHz.
constexpr int kZeros = 32;

double sinc(double x) {
  if (x == 0.0)
    return 1.0;
  const double px = kPi * x;
  return std::sin(px) / px;
}

std::vector<float> scale_only(const std::vector<int16_t> &pcm16) {
  std::vector<float> out;
  out.reserve(pcm16.size());
  for (int16_t s : pcm16)
    out.push_back(static_cast<float>(s) / 32768.0f);
  return out;
}

// Windowed-sinc kernel for every fractional phase, laid out as
// kPhases rows of `taps` coefficients. Each row is normalised to unity
// DC gain so the converter neither boosts nor attenuates speech level.
std::vector<float> build_kernel(double fc, int half, int taps) {
  std::vector<float> table(static_cast<size_t>(kPhases) *
                           static_cast<size_t>(taps));
  const double window_half = static_cast<double>(half) + 1.0;

  for (int p = 0; p < kPhases; ++p) {
    const double frac = static_cast<double>(p) / kPhases;
    float *row = &table[static_cast<size_t>(p) * static_cast<size_t>(taps)];
    double sum = 0.0;

    for (int k = 0; k < taps; ++k) {
      // Distance from this source sample to the output position, in
      // source samples. Always within [-half-1, half+1].
      const double t = static_cast<double>(k - half) - frac;
      const double w = 0.42 + 0.5 * std::cos(kPi * t / window_half) +
                       0.08 * std::cos(2.0 * kPi * t / window_half);
      const double v = 2.0 * fc * sinc(2.0 * fc * t) * w;
      row[k] = static_cast<float>(v);
      sum += v;
    }

    const double inv = (sum != 0.0) ? 1.0 / sum : 1.0;
    for (int k = 0; k < taps; ++k)
      row[k] = static_cast<float>(static_cast<double>(row[k]) * inv);
  }
  return table;
}

} // namespace

std::vector<float> to_float_16k(const std::vector<int16_t> &pcm16,
                                uint32_t src_rate_hz) {
  if (pcm16.empty())
    return {};
  if (src_rate_hz == 0 || src_rate_hz == kTargetRateHz)
    return scale_only(pcm16);

  const double src_rate = static_cast<double>(src_rate_hz);

  // Anti-aliasing corner in cycles per source sample. Upsampling needs no
  // band limiting beyond the source's own Nyquist, hence the 0.5 clamp.
  const double fc = std::min(0.5, kCutoffHz / src_rate);
  const int half = static_cast<int>(std::ceil(kZeros / (2.0 * fc)));
  const int taps = 2 * half + 1;
  const std::vector<float> kernel = build_kernel(fc, half, taps);

  const double step = src_rate / static_cast<double>(kTargetRateHz);
  const auto n = static_cast<int64_t>(pcm16.size());
  const auto out_n = static_cast<size_t>(static_cast<double>(n) / step);

  std::vector<float> out;
  out.reserve(out_n);

  for (size_t i = 0; i < out_n; ++i) {
    const double pos = static_cast<double>(i) * step;
    auto base = static_cast<int64_t>(pos);
    int phase = static_cast<int>(
        std::lround((pos - static_cast<double>(base)) * kPhases));
    if (phase >= kPhases) { // rounded up past the sample boundary
      phase = 0;
      ++base;
    }

    const float *row =
        &kernel[static_cast<size_t>(phase) * static_cast<size_t>(taps)];
    double acc = 0.0;
    for (int k = 0; k < taps; ++k) {
      // Edge extension rather than zero padding: no fade-in/out artefact
      // at the ends of the recording.
      int64_t idx = base + (k - half);
      idx = std::max<int64_t>(0, std::min<int64_t>(idx, n - 1));
      acc += static_cast<double>(row[k]) *
             static_cast<double>(pcm16[static_cast<size_t>(idx)]);
    }

    // The windowed sinc can overshoot slightly on transients; every
    // ISpeechToText implementation expects [-1, 1].
    const double v = std::max(-1.0, std::min(1.0, acc / 32768.0));
    out.push_back(static_cast<float>(v));
  }

  return out;
}

} // namespace pcm_resample
