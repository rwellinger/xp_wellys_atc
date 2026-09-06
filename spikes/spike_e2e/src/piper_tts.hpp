// Concrete ITextToSpeech backed by Piper (libpiper). Loads the ONNX voice
// model + JSON config + espeak-ng-data directory once via `open()`; subsequent
// `synthesize()` calls reuse the same synthesizer handle.

#pragma once

#include "i_text_to_speech.hpp"

#include <string>

struct piper_synthesizer;

namespace spike_e2e {

class PiperTts final : public ITextToSpeech {
public:
    PiperTts();
    ~PiperTts() override;

    PiperTts(const PiperTts&)            = delete;
    PiperTts& operator=(const PiperTts&) = delete;

    bool open(const std::string& voice_onnx_path,
              const std::string& voice_json_path,
              const std::string& espeakng_data_dir);

    std::vector<int16_t> synthesize(const std::string& text,
                                    uint32_t&          sample_rate_hz) override;

private:
    piper_synthesizer* synth_ = nullptr;
};

}  // namespace spike_e2e
