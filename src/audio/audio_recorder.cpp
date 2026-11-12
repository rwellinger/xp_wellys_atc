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

#include "audio/audio_recorder.hpp"
#include "audio/mic_permission.hpp"
#include "persistence/settings.hpp"

#include <XPLMUtilities.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#elif defined(_WIN32)
// Windows capture via miniaudio (WASAPI). Only the low-level device layer
// is needed — no decoders, encoders, resource manager, node graph, or the
// high-level engine — but ma_device still performs the native->16 kHz
// resample + mono downmix we rely on. MINIAUDIO_IMPLEMENTATION lives in
// this single TU (every other TU would include just the header). No extra
// link libraries: miniaudio loads ole32/WASAPI dynamically at runtime.
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#endif

namespace audio_recorder {

static constexpr unsigned kDesiredSampleRate = 16000;
static constexpr unsigned kBitsPerSample = 16;
static constexpr unsigned kNumChannels = 1;

static unsigned actual_sample_rate_ = kDesiredSampleRate;

static std::vector<int16_t> buffer_;
static std::mutex buffer_mutex_;
static std::atomic<bool> recording_{false};
static bool initialized_ = false;
static std::string active_device_name_;
// Peak of the last recording; -1 while there is none to judge.
static std::atomic<int> last_peak_{-1};

// Log.txt and the ImGui font only render ASCII. Device names are user data
// and may not be ("Roberts Kopfhoerer" with a typographic apostrophe, say);
// each multi-byte UTF-8 sequence collapses to one '?'.
static std::string ascii_safe(const std::string &utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (const char c : utf8) {
    const auto b = static_cast<unsigned char>(c);
    if (b >= 0x20 && b <= 0x7E)
      out.push_back(c);
    else if (b >= 0xC0) // lead byte; continuation bytes are dropped
      out.push_back('?');
  }
  return out;
}

#if defined(__APPLE__)

static AudioComponentInstance audio_unit_ = nullptr;

static std::atomic<int> render_call_count_{0};
static std::chrono::steady_clock::time_point record_start_;

static OSStatus render_callback(void * /*inRefCon*/,
                                AudioUnitRenderActionFlags *io_action_flags,
                                const AudioTimeStamp *in_time_stamp,
                                UInt32 in_bus_number, UInt32 in_number_frames,
                                AudioBufferList * /*io_data*/) {
  int call_count = ++render_call_count_;
  if (call_count == 1) {
    if (settings::debug_logging())
      XPLMDebugString(
          "[xp_wellys_vfr_atc][DEBUG] Render callback firing (first call)\n");
  }

  AudioBufferList buf_list;
  buf_list.mNumberBuffers = 1;
  buf_list.mBuffers[0].mDataByteSize = in_number_frames * sizeof(int16_t);
  buf_list.mBuffers[0].mNumberChannels = 1;

  std::vector<int16_t> temp(in_number_frames);
  buf_list.mBuffers[0].mData = temp.data();

  OSStatus status = AudioUnitRender(audio_unit_, io_action_flags, in_time_stamp,
                                    1, in_number_frames, &buf_list);
  if (status != noErr) {
    if (call_count <= 3) {
      char log[128];
      std::snprintf(
          log, sizeof(log),
          "[xp_wellys_vfr_atc] AudioUnitRender error: %d (call #%d)\n",
          static_cast<int>(status), call_count);
      XPLMDebugString(log);
    }
    return status;
  }

  if (recording_.load()) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    buffer_.insert(buffer_.end(), temp.begin(), temp.end());
  }

  return noErr;
}

static AudioObjectPropertyAddress global_prop(AudioObjectPropertySelector sel) {
  AudioObjectPropertyAddress prop{};
  prop.mSelector = sel;
  prop.mScope = kAudioObjectPropertyScopeGlobal;
  prop.mElement = kAudioObjectPropertyElementMain;
  return prop;
}

// Read a CFString property (name, UID) as UTF-8; "" on failure.
static std::string device_string(AudioDeviceID dev,
                                 AudioObjectPropertySelector sel) {
  const AudioObjectPropertyAddress prop = global_prop(sel);
  CFStringRef ref = nullptr;
  UInt32 size = sizeof(CFStringRef);
  const OSStatus err = AudioObjectGetPropertyData(dev, &prop, 0, nullptr, &size,
                                                  static_cast<void *>(&ref));
  if (err != noErr || !ref)
    return {};
  char buf[256] = {};
  CFStringGetCString(ref, buf, sizeof(buf), kCFStringEncodingUTF8);
  CFRelease(ref);
  return buf;
}

static bool device_has_input(AudioDeviceID dev) {
  AudioObjectPropertyAddress prop{};
  prop.mSelector = kAudioDevicePropertyStreams;
  prop.mScope = kAudioObjectPropertyScopeInput;
  prop.mElement = kAudioObjectPropertyElementMain;
  UInt32 size = 0;
  return AudioObjectGetPropertyDataSize(dev, &prop, 0, nullptr, &size) ==
             noErr &&
         size > 0;
}

static std::vector<AudioDeviceID> input_device_ids() {
  const AudioObjectPropertyAddress prop =
      global_prop(kAudioHardwarePropertyDevices);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0,
                                     nullptr, &size) != noErr)
    return {};
  std::vector<AudioDeviceID> all(size / sizeof(AudioDeviceID));
  if (all.empty() ||
      AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr,
                                 &size, all.data()) != noErr)
    return {};
  std::vector<AudioDeviceID> inputs;
  for (const AudioDeviceID dev : all) {
    if (device_has_input(dev))
      inputs.push_back(dev);
  }
  return inputs;
}

static AudioDeviceID default_input_device() {
  const AudioObjectPropertyAddress prop =
      global_prop(kAudioHardwarePropertyDefaultInputDevice);
  AudioDeviceID dev = kAudioDeviceUnknown;
  UInt32 size = sizeof(dev);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr,
                                 &size, &dev) != noErr)
    return kAudioDeviceUnknown;
  return dev;
}

// The configured device if it is present, otherwise the system default. A
// configured device can legitimately be missing (headset unplugged); the
// setting is kept so it binds again once the device is back.
static AudioDeviceID resolve_input_device() {
  const std::string wanted = settings::audio_input_device();
  if (wanted.empty())
    return default_input_device();

  for (const AudioDeviceID dev : input_device_ids()) {
    if (device_string(dev, kAudioDevicePropertyDeviceUID) == wanted)
      return dev;
  }
  XPLMDebugString("[xp_wellys_vfr_atc] Configured input device not found, "
                  "falling back to system default\n");
  return default_input_device();
}

std::vector<InputDevice> list_input_devices() {
  std::vector<InputDevice> out;
  for (const AudioDeviceID dev : input_device_ids()) {
    std::string uid = device_string(dev, kAudioDevicePropertyDeviceUID);
    if (!uid.empty())
      out.push_back({std::move(uid),
                     ascii_safe(device_string(dev, kAudioObjectPropertyName))});
  }
  return out;
}

// Configure the client-side format on the input bus at `rate` and bring
// the unit up. Success means the AUHAL accepted the format, reported it
// back unchanged, AND initialized -- a set that silently lands on a
// different rate is a failure here. This does not prove the callback really
// delivers that rate, see the rate check in stop_recording().
//
// On failure the unit is left uninitialized so the caller can retry with
// another rate. `out_status` carries the OSStatus for logging.
static bool activate_client_format(double rate, OSStatus *out_status) {
  AudioStreamBasicDescription format{};
  format.mSampleRate = rate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags =
      kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  format.mBytesPerPacket = sizeof(int16_t);
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = sizeof(int16_t);
  format.mChannelsPerFrame = kNumChannels;
  format.mBitsPerChannel = kBitsPerSample;

  OSStatus status =
      AudioUnitSetProperty(audio_unit_, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Output, 1, &format, sizeof(format));
  *out_status = status;
  if (status != noErr)
    return false;

  // HALOutput may accept the call but keep its own rate.
  AudioStreamBasicDescription actual_fmt{};
  UInt32 fmt_size = sizeof(actual_fmt);
  status =
      AudioUnitGetProperty(audio_unit_, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Output, 1, &actual_fmt, &fmt_size);
  *out_status = status;
  if (status != noErr)
    return false;

  const auto got = static_cast<unsigned>(std::lround(actual_fmt.mSampleRate));
  if (got != static_cast<unsigned>(std::lround(rate))) {
    *out_status = kAudioUnitErr_FormatNotSupported;
    return false;
  }

  status = AudioUnitInitialize(audio_unit_);
  *out_status = status;
  if (status != noErr) {
    // Back to the uninitialized state; the format is only settable there.
    AudioUnitUninitialize(audio_unit_);
    return false;
  }

  actual_sample_rate_ = got;
  return true;
}

void init() {
  if (!mic_permission::check_and_request()) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] Audio recorder: no microphone permission, "
        "recording will not work\n");
  }

  AudioComponentDescription desc{};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_HALOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;

  AudioComponent component = AudioComponentFindNext(nullptr, &desc);
  if (!component) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] Error: no HALOutput audio component found\n");
    return;
  }

  OSStatus status = AudioComponentInstanceNew(component, &audio_unit_);
  if (status != noErr) {
    XPLMDebugString("[xp_wellys_vfr_atc] Error: failed to create AudioUnit\n");
    return;
  }

  // Enable input (bus 1)
  UInt32 enable_input = 1;
  status = AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_EnableIO,
                                kAudioUnitScope_Input, 1, &enable_input,
                                sizeof(enable_input));
  if (status != noErr) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] Error: failed to enable audio input\n");
    AudioComponentInstanceDispose(audio_unit_);
    audio_unit_ = nullptr;
    return;
  }

  // Disable output (bus 0)
  UInt32 disable_output = 0;
  AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_EnableIO,
                       kAudioUnitScope_Output, 0, &disable_output,
                       sizeof(disable_output));

  // Bind the configured input device. X-Plane's Radio Device is output-only
  // and not readable through the SDK, so the microphone is chosen here.
  const AudioDeviceID input_device = resolve_input_device();
  if (input_device != kAudioDeviceUnknown) {
    status = AudioUnitSetProperty(
        audio_unit_, kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global, 0, &input_device, sizeof(input_device));
    active_device_name_ =
        ascii_safe(device_string(input_device, kAudioObjectPropertyName));
    char log[384];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_vfr_atc] Input device: \"%s\" (id=%u): %s\n",
                  active_device_name_.c_str(),
                  static_cast<unsigned>(input_device),
                  status == noErr ? "OK" : "FAILED");
    XPLMDebugString(log);
  } else {
    XPLMDebugString("[xp_wellys_vfr_atc] No input device found\n");
  }

  // Log the device's native (hardware) format
  AudioStreamBasicDescription hw_fmt{};
  UInt32 hw_fmt_size = sizeof(hw_fmt);
  status =
      AudioUnitGetProperty(audio_unit_, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Input, 1, &hw_fmt, &hw_fmt_size);
  if (status == noErr) {
    char log[256];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_vfr_atc] Hardware input format: %.0f Hz, %u ch, "
                  "%u bps, formatFlags=0x%X\n",
                  hw_fmt.mSampleRate,
                  static_cast<unsigned>(hw_fmt.mChannelsPerFrame),
                  static_cast<unsigned>(hw_fmt.mBitsPerChannel),
                  static_cast<unsigned>(hw_fmt.mFormatFlags));
    XPLMDebugString(log);
  }

  double device_rate =
      (hw_fmt.mSampleRate > 0) ? hw_fmt.mSampleRate : kDesiredSampleRate;

  // Set render callback on input bus
  AURenderCallbackStruct callback{};
  callback.inputProc = render_callback;
  callback.inputProcRefCon = nullptr;

  status = AudioUnitSetProperty(
      audio_unit_, kAudioOutputUnitProperty_SetInputCallback,
      kAudioUnitScope_Global, 0, &callback, sizeof(callback));
  if (status != noErr) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] Error: failed to set render callback\n");
    AudioComponentInstanceDispose(audio_unit_);
    audio_unit_ = nullptr;
    return;
  }

  // Capture at the device's own rate; pcm_resample converts to 16 kHz.
  //
  // Do NOT ask the AUHAL for 16 kHz first. It accepts that format and reports
  // it back, but on a 48 kHz device it still delivers 48000 frames/s (seen on
  // macOS 15 and 26). On macOS 15 with a USB headset, every 512-frame callback
  // held the same ~171-sample chunk three times: stuttered audio, 3x too long,
  // that STT only half understands.
  //
  // The 16 kHz retry only exists because a dead microphone is worse than a
  // degraded one, for a device that refuses its own reported rate.
  if (!activate_client_format(device_rate, &status)) {
    char log[224];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_vfr_atc] Device rate %.0f Hz rejected (err %d), "
                  "trying 16 kHz client format - STT quality may be "
                  "reduced\n",
                  device_rate, static_cast<int>(status));
    XPLMDebugString(log);

    if (!activate_client_format(kDesiredSampleRate, &status)) {
      std::snprintf(log, sizeof(log),
                    "[xp_wellys_vfr_atc] Error: failed to configure audio "
                    "format at 16 kHz (err %d)\n",
                    static_cast<int>(status));
      XPLMDebugString(log);
      AudioComponentInstanceDispose(audio_unit_);
      audio_unit_ = nullptr;
      return;
    }
  }

  {
    char log[192];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_vfr_atc] Audio format: device %.0f Hz, "
                  "client %u Hz (%u ch, %u bps)\n",
                  device_rate, actual_sample_rate_, kNumChannels,
                  kBitsPerSample);
    XPLMDebugString(log);
  }

  initialized_ = true;
  {
    char log[128];
    std::snprintf(
        log, sizeof(log),
        "[xp_wellys_vfr_atc] Audio recorder initialized (%uHz mono 16-bit)\n",
        actual_sample_rate_);
    XPLMDebugString(log);
  }
}

void stop() {
  if (audio_unit_) {
    AudioOutputUnitStop(audio_unit_);
    AudioUnitUninitialize(audio_unit_);
    AudioComponentInstanceDispose(audio_unit_);
    audio_unit_ = nullptr;
  }
  recording_ = false;
  initialized_ = false;
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  buffer_.clear();
}

// The AUHAL can report one rate and deliver another, which no format query
// reveals. Sample count against wall clock does.
static void warn_on_rate_mismatch(size_t samples) {
  constexpr double kMinSeconds = 1.0;
  constexpr double kTolerance = 0.25;
  const std::chrono::duration<double> elapsed =
      std::chrono::steady_clock::now() - record_start_;
  if (elapsed.count() < kMinSeconds)
    return;
  const double measured = static_cast<double>(samples) / elapsed.count();
  const double expected = static_cast<double>(actual_sample_rate_);
  if (std::fabs(measured / expected - 1.0) <= kTolerance)
    return;
  char log[160];
  std::snprintf(log, sizeof(log),
                "[xp_wellys_vfr_atc] WARNING: mic delivered %.0f samples/s, "
                "expected %.0f Hz - audio is distorted, STT will suffer\n",
                measured, expected);
  XPLMDebugString(log);
}

void start_recording() {
  if (!initialized_ || !audio_unit_) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] Warning: audio recorder not initialized\n");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    buffer_.clear();
  }

  render_call_count_ = 0;
  record_start_ = std::chrono::steady_clock::now();
  recording_ = true;
  OSStatus status = AudioOutputUnitStart(audio_unit_);
  if (status != noErr) {
    char log[128];
    std::snprintf(
        log, sizeof(log),
        "[xp_wellys_vfr_atc] Error: AudioOutputUnitStart failed: %d\n",
        static_cast<int>(status));
    XPLMDebugString(log);
    recording_ = false;
  } else {
    if (settings::debug_logging())
      XPLMDebugString("[xp_wellys_vfr_atc][DEBUG] AudioUnit started OK\n");
  }
}

void stop_recording() {
  recording_ = false;
  if (audio_unit_) {
    AudioOutputUnitStop(audio_unit_);
  }
  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    // Compute peak level
    int16_t peak = 0;
    for (auto s : buffer_) {
      int16_t abs_s = (s < 0) ? static_cast<int16_t>(-s) : s;
      if (abs_s > peak)
        peak = abs_s;
    }
    float peak_pct = (static_cast<float>(peak) / 32767.0f) * 100.0f;
    last_peak_ = buffer_.empty() ? -1 : peak;

    if (settings::debug_logging()) {
      char log[256];
      std::snprintf(log, sizeof(log),
                    "[xp_wellys_vfr_atc][DEBUG] Recording stopped: %zu "
                    "samples captured, "
                    "render callbacks: %d, peak: %d (%.1f%%)\n",
                    buffer_.size(), render_call_count_.load(),
                    static_cast<int>(peak), peak_pct);
      XPLMDebugString(log);
    }
    warn_on_rate_mismatch(buffer_.size());
    if (buffer_.empty() && render_call_count_.load() == 0) {
      XPLMDebugString("[xp_wellys_vfr_atc] ERROR: No audio captured. Check: "
                      "System Settings "
                      "> Privacy & Security > Microphone > enable X-Plane\n");
    }
  }
}

#elif defined(_WIN32)

static ma_device device_;
static bool device_ready_ = false;
static std::atomic<uint64_t> frames_captured_{0};

// Capture callback: miniaudio hands us `frame_count` frames already
// resampled to kDesiredSampleRate and downmixed to mono s16 (the format
// requested in init()), so we append straight into buffer_.
static void data_callback(ma_device * /*dev*/, void * /*output*/,
                          const void *input, ma_uint32 frame_count) {
  if (input == nullptr || frame_count == 0 || !recording_.load())
    return;
  const int16_t *samples = static_cast<const int16_t *>(input);
  frames_captured_.fetch_add(frame_count);
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  buffer_.insert(buffer_.end(), samples, samples + frame_count);
}

// Visit every capture endpoint. The context only lives for the enumeration;
// a ma_device_id is plain data and stays valid for ma_device_init.
template <typename Fn> static void for_each_capture_device(Fn &&fn) {
  ma_context context;
  if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS)
    return;
  ma_device_info *infos = nullptr;
  ma_uint32 count = 0;
  if (ma_context_get_devices(&context, nullptr, nullptr, &infos, &count) ==
      MA_SUCCESS) {
    for (ma_uint32 i = 0; i < count; ++i)
      fn(infos[i]);
  }
  ma_context_uninit(&context);
}

std::vector<InputDevice> list_input_devices() {
  std::vector<InputDevice> out;
  for_each_capture_device([&out](const ma_device_info &info) {
    out.push_back({info.name, ascii_safe(info.name)});
  });
  return out;
}

void init() {
  // Windows has no per-app mic prompt; this logs and returns true. A
  // system-level privacy block surfaces as an empty capture stream, which
  // stop_recording() reports.
  mic_permission::check_and_request();

  ma_device_config config = ma_device_config_init(ma_device_type_capture);
  config.capture.format = ma_format_s16;
  config.capture.channels = kNumChannels;
  config.sampleRate = kDesiredSampleRate; // miniaudio resamples to this
  config.dataCallback = data_callback;

  // The configured endpoint if present, otherwise the system default. A
  // missing device keeps the setting so it binds again once it is back.
  ma_device_id selected_id{};
  bool selected = false;
  const std::string wanted = settings::audio_input_device();
  if (!wanted.empty()) {
    for_each_capture_device([&](const ma_device_info &info) {
      if (wanted == info.name) {
        selected_id = info.id;
        selected = true;
      }
    });
    if (!selected)
      XPLMDebugString("[xp_wellys_vfr_atc] Configured input device not found, "
                      "falling back to system default\n");
  }
  config.capture.pDeviceID = selected ? &selected_id : nullptr;

  ma_result r = ma_device_init(nullptr, &config, &device_);
  if (r != MA_SUCCESS) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_vfr_atc] Error: ma_device_init failed (%d)\n",
                  static_cast<int>(r));
    XPLMDebugString(log);
    return;
  }
  device_ready_ = true;
  actual_sample_rate_ = kDesiredSampleRate;
  initialized_ = true;
  active_device_name_ = ascii_safe(device_.capture.name);

  char log[384];
  std::snprintf(log, sizeof(log),
                "[xp_wellys_vfr_atc] Audio recorder initialized (miniaudio "
                "WASAPI, %uHz mono 16-bit); input device: \"%s\"\n",
                actual_sample_rate_, active_device_name_.c_str());
  XPLMDebugString(log);
}

void stop() {
  if (device_ready_) {
    ma_device_uninit(&device_); // stops the device + joins the callback
    device_ready_ = false;
  }
  recording_ = false;
  initialized_ = false;
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  buffer_.clear();
}

void start_recording() {
  if (!initialized_ || !device_ready_) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] Warning: audio recorder not initialized\n");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    buffer_.clear();
  }

  frames_captured_ = 0;
  recording_ = true;
  ma_result r = ma_device_start(&device_);
  if (r != MA_SUCCESS) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_vfr_atc] Error: ma_device_start failed (%d)\n",
                  static_cast<int>(r));
    XPLMDebugString(log);
    recording_ = false;
  } else if (settings::debug_logging()) {
    XPLMDebugString("[xp_wellys_vfr_atc][DEBUG] miniaudio device started\n");
  }
}

void stop_recording() {
  recording_ = false;
  if (device_ready_) {
    // Blocks until the device has stopped and the callback has returned,
    // so reading buffer_ below is race-free.
    ma_device_stop(&device_);
  }

  std::lock_guard<std::mutex> lock(buffer_mutex_);
  int16_t peak = 0;
  for (auto s : buffer_) {
    int16_t abs_s = (s < 0) ? static_cast<int16_t>(-s) : s;
    if (abs_s > peak)
      peak = abs_s;
  }
  float peak_pct = (static_cast<float>(peak) / 32767.0f) * 100.0f;
  last_peak_ = buffer_.empty() ? -1 : peak;

  if (settings::debug_logging()) {
    char log[256];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_vfr_atc][DEBUG] Recording stopped: %zu samples "
                  "captured, %llu frames, peak: %d (%.1f%%)\n",
                  buffer_.size(),
                  static_cast<unsigned long long>(frames_captured_.load()),
                  static_cast<int>(peak), peak_pct);
    XPLMDebugString(log);
  }
  if (buffer_.empty()) {
    XPLMDebugString(
        "[xp_wellys_vfr_atc] ERROR: No audio captured. Check Windows "
        "Settings > Privacy & security > Microphone (enable for desktop apps) "
        "and the selected default input device\n");
  }
}

#else

void init() {
  XPLMDebugString(
      "[xp_wellys_vfr_atc] Warning: audio recorder not supported on "
      "this platform\n");
}
void stop() {}
void start_recording() {}
void stop_recording() {}
std::vector<InputDevice> list_input_devices() { return {}; }

#endif

bool set_input_device(const std::string &id) {
  if (recording_.load())
    return false;
  settings::set_audio_input_device(id);
  settings::save();
  stop();
  active_device_name_.clear();
  last_peak_ = -1;
  init();
  return initialized_;
}

const std::string &active_input_device_name() { return active_device_name_; }

bool last_recording_silent() { return last_peak_.load() == 0; }

std::vector<int16_t> take_pcm() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return std::move(buffer_);
}

unsigned sample_rate_hz() { return actual_sample_rate_; }

std::vector<uint8_t> encode_wav() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  uint32_t data_size = static_cast<uint32_t>(buffer_.size() * sizeof(int16_t));
  uint32_t file_size = 36 + data_size;

  std::vector<uint8_t> wav;
  wav.reserve(44 + data_size);

  auto write_u32 = [&](uint32_t v) {
    wav.push_back(static_cast<uint8_t>(v & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  };
  auto write_u16 = [&](uint16_t v) {
    wav.push_back(static_cast<uint8_t>(v & 0xFFu));
    wav.push_back(
        static_cast<uint8_t>((static_cast<uint32_t>(v) >> 8) & 0xFFu));
  };
  auto write_str = [&](const char *s) {
    while (*s)
      wav.push_back(static_cast<uint8_t>(*s++));
  };

  // RIFF header
  write_str("RIFF");
  write_u32(file_size);
  write_str("WAVE");

  // fmt subchunk
  write_str("fmt ");
  write_u32(16);                  // subchunk size
  write_u16(1);                   // PCM format
  write_u16(kNumChannels);        // channels
  write_u32(actual_sample_rate_); // sample rate
  write_u32(size_t{actual_sample_rate_} * kNumChannels *
            sizeof(int16_t));                        // byte rate
  write_u16(size_t{kNumChannels} * sizeof(int16_t)); // block align
  write_u16(kBitsPerSample);                         // bits per sample

  // data subchunk
  write_str("data");
  write_u32(data_size);

  // Raw PCM samples
  const auto *raw = reinterpret_cast<const uint8_t *>(buffer_.data());
  wav.insert(wav.end(), raw, raw + data_size);

  return wav;
}

float duration_seconds() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  if (buffer_.empty())
    return 0.0f;
  return static_cast<float>(buffer_.size()) /
         static_cast<float>(actual_sample_rate_);
}

size_t buffer_samples() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return buffer_.size();
}

} // namespace audio_recorder
