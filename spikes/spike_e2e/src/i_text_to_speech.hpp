// Strategy interface for text-to-speech backends.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spike_e2e {

class ITextToSpeech {
public:
    virtual ~ITextToSpeech() = default;

    // Synthesize `text` to mono 16-bit PCM samples. `sample_rate_hz` is filled
    // with the synthesizer's native sample rate (no resampling). An empty
    // result indicates failure.
    virtual std::vector<int16_t> synthesize(const std::string& text,
                                            uint32_t&          sample_rate_hz) = 0;
};

}  // namespace spike_e2e
