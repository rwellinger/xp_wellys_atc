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

#ifndef AUDIO_RECORDER_HPP
#define AUDIO_RECORDER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace audio_recorder {

// A selectable microphone. `id` is what settings::audio_input_device()
// persists: the Core Audio device UID on macOS (the numeric AudioDeviceID
// changes between boots), the endpoint name on Windows. `name` is ASCII-safe
// for Log.txt and the ImGui font.
struct InputDevice {
  std::string id;
  std::string name;
};

void init();
void stop();

void start_recording();
void stop_recording();

// Enumerate the capture devices. Not cheap enough for a per-frame call --
// the UI caches the result.
std::vector<InputDevice> list_input_devices();

// Persist `id` ("" = system default) and rebind capture to it without
// restarting X-Plane. Main thread only. Returns false if a recording is in
// progress (nothing changes) or the device could not be brought up.
bool set_input_device(const std::string &id);

// Name of the device capture is bound to; "" if the recorder is not up.
const std::string &active_input_device_name();

// True when the last recording held samples but all of them were digital
// zero. A live microphone always has a noise floor, so this means the input
// is cut off (e.g. a MacBook's built-in mic with the lid closed) rather than
// merely quiet.
bool last_recording_silent();

// Move the captured PCM out of the recorder into the caller. The
// returned buffer is mono, signed 16-bit, at sample_rate_hz(). Used by
// the local STT backend instead of the historical encode_wav() path.
std::vector<int16_t> take_pcm();

// Native capture sample rate (HALOutput chooses the device default;
// observed values are usually 44100 or 48000).
unsigned sample_rate_hz();

std::vector<uint8_t> encode_wav();
float duration_seconds();
size_t buffer_samples();

} // namespace audio_recorder

#endif // AUDIO_RECORDER_HPP
