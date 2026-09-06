// Strategy interface for speech-to-text backends. The plugin in milestone 06
// will inject a concrete implementation through this same shape, so anything
// added here must be implementable both by whisper.cpp and by a hypothetical
// future cloud STT.

#pragma once

#include <string>
#include <vector>

namespace spike_e2e {

class ISpeechToText {
public:
    virtual ~ISpeechToText() = default;

    // Transcribe a 16 kHz mono float32 PCM buffer (range [-1, 1]).
    // Returns the UTF-8 transcript; an empty string indicates failure.
    virtual std::string transcribe(const std::vector<float>& pcm_16k_mono) = 0;
};

}  // namespace spike_e2e
