#include "piper_tts.hpp"

#include "piper.h"

#include <cstdint>

namespace spike_e2e {

namespace {

int16_t f32_to_i16(float x) {
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<int16_t>(x * 32767.0f);
}

}  // namespace

PiperTts::PiperTts() = default;

PiperTts::~PiperTts() {
    if (synth_) piper_free(synth_);
}

bool PiperTts::open(const std::string& voice_onnx_path,
                    const std::string& voice_json_path,
                    const std::string& espeakng_data_dir) {
    synth_ = piper_create(voice_onnx_path.c_str(),
                          voice_json_path.c_str(),
                          espeakng_data_dir.c_str());
    return synth_ != nullptr;
}

std::vector<int16_t> PiperTts::synthesize(const std::string& text,
                                          uint32_t&          sample_rate_hz) {
    sample_rate_hz = 0;
    if (!synth_ || text.empty()) return {};

    piper_synthesize_options opts = piper_default_synthesize_options(synth_);

    if (piper_synthesize_start(synth_, text.c_str(), &opts) != PIPER_OK) {
        return {};
    }

    std::vector<int16_t> pcm;
    pcm.reserve(text.size() * 1024);

    piper_audio_chunk chunk{};
    int rc;
    while ((rc = piper_synthesize_next(synth_, &chunk)) != PIPER_DONE) {
        if (rc != PIPER_OK) return {};
        if (sample_rate_hz == 0) sample_rate_hz = uint32_t(chunk.sample_rate);
        const float* s = chunk.samples;
        for (size_t i = 0; i < chunk.num_samples; ++i) {
            pcm.push_back(f32_to_i16(s[i]));
        }
    }
    // The terminating PIPER_DONE chunk may still carry samples.
    if (chunk.num_samples > 0) {
        if (sample_rate_hz == 0) sample_rate_hz = uint32_t(chunk.sample_rate);
        const float* s = chunk.samples;
        for (size_t i = 0; i < chunk.num_samples; ++i) {
            pcm.push_back(f32_to_i16(s[i]));
        }
    }
    return pcm;
}

}  // namespace spike_e2e
