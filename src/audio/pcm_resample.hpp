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

#ifndef AUDIO_PCM_RESAMPLE_HPP
#define AUDIO_PCM_RESAMPLE_HPP

#include <cstdint>
#include <vector>

// SDK-free sample-rate conversion for the STT input path. Lives in the
// engine OBJECT library so it is unit-testable; the capture side
// (audio_recorder) is plugin-only and cannot host it.
namespace pcm_resample {

// Target rate of every ISpeechToText implementation.
constexpr uint32_t kTargetRateHz = 16000;

// 16-bit signed PCM -> float [-1, 1] at kTargetRateHz.
//
// src_rate_hz == 0 or kTargetRateHz is a pure scale (no filtering, no
// resampling) and is the path both platforms should normally take:
// miniaudio (Windows) and Core Audio (macOS) are asked to deliver 16 kHz
// directly, because both resample with a real filter and we do not want
// to duplicate that work.
//
// Any other rate goes through a Blackman-windowed-sinc polyphase
// converter. This is the LAST RESORT, not the normal path -- it exists
// for devices that refuse a 16 kHz client format (some Bluetooth
// headsets). It must still be anti-aliased: a naive decimation folds
// everything above 8 kHz back into the band, and 4-10 kHz is exactly
// where the sibilants live that separate spelled NATO callsigns
// ("Foxtrott" / "Oscar" / "Sierra"). See issue #85.
std::vector<float> to_float_16k(const std::vector<int16_t> &pcm16,
                                uint32_t src_rate_hz);

} // namespace pcm_resample

#endif
