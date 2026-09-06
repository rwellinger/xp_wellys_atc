// Minimal WAV I/O for the e2e spike. Reader expects the whisper.cpp native
// format (16 kHz mono 16-bit PCM); writer emits whatever sample rate the TTS
// produces (mono 16-bit PCM).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spike_e2e {

struct WavInput {
    std::vector<float> samples;      // normalised to [-1, 1]
    uint32_t           sample_rate;
    uint16_t           channels;
    double             duration_s;
};

// Reads a 16 kHz mono 16-bit PCM WAV. Returns false on failure with `err`
// populated.
bool load_wav_pcm16_mono_16k(const std::string& path,
                             WavInput&          out,
                             std::string&       err);

// Writes a mono 16-bit PCM WAV at the given sample rate.
bool write_wav_pcm16_mono(const std::string&         path,
                          const std::vector<int16_t>& samples,
                          uint32_t                    sample_rate,
                          std::string&                err);

}  // namespace spike_e2e
