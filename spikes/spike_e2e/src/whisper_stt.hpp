// Concrete ISpeechToText backed by whisper.cpp + Metal. Loads the GGUF model
// once via `open()`; subsequent `transcribe()` calls reuse the same context.

#pragma once

#include "i_speech_to_text.hpp"

#include <string>

struct whisper_context;

namespace spike_e2e {

class WhisperStt final : public ISpeechToText {
public:
    WhisperStt();
    ~WhisperStt() override;

    WhisperStt(const WhisperStt&)            = delete;
    WhisperStt& operator=(const WhisperStt&) = delete;

    // Load the model at `model_path`. Returns false on failure.
    bool open(const std::string& model_path);

    std::string transcribe(const std::vector<float>& pcm_16k_mono) override;

private:
    whisper_context* ctx_       = nullptr;
    int              n_threads_ = 0;
};

}  // namespace spike_e2e
