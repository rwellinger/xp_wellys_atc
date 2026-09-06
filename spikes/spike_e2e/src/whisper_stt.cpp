#include "whisper_stt.hpp"

#include "whisper.h"

#include <algorithm>
#include <thread>

namespace spike_e2e {

WhisperStt::WhisperStt() = default;

WhisperStt::~WhisperStt() {
    if (ctx_) whisper_free(ctx_);
}

bool WhisperStt::open(const std::string& model_path) {
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu    = true;   // Metal backend
    cparams.flash_attn = false;

    ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx_) return false;

    // Half the perf cores, mirroring spike_whisper. whisper.cpp scales
    // sub-linearly above 4 threads on M1.
    const unsigned hw = std::thread::hardware_concurrency();
    n_threads_ = hw == 0 ? 4 : int(std::min(8u, std::max(2u, hw / 2)));
    return true;
}

std::string WhisperStt::transcribe(const std::vector<float>& pcm_16k_mono) {
    if (!ctx_ || pcm_16k_mono.empty()) return {};

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language         = "en";
    wparams.translate        = false;
    wparams.no_context       = true;
    wparams.single_segment   = false;
    wparams.print_progress   = false;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.print_special    = false;
    wparams.n_threads        = n_threads_;

    if (whisper_full(ctx_, wparams,
                     pcm_16k_mono.data(),
                     int(pcm_16k_mono.size())) != 0) {
        return {};
    }

    std::string transcript;
    const int n_segments = whisper_full_n_segments(ctx_);
    for (int i = 0; i < n_segments; ++i) {
        const char* seg = whisper_full_get_segment_text(ctx_, i);
        if (seg) transcript += seg;
    }

    // Whisper sometimes emits a leading space; trim for cleaner downstream
    // prompts.
    while (!transcript.empty() && transcript.front() == ' ') {
        transcript.erase(transcript.begin());
    }
    return transcript;
}

}  // namespace spike_e2e
