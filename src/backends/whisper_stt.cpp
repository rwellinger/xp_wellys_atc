/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Lifted from spikes/spike_e2e/src/whisper_stt.cpp; the spike validated
 * the parameter set on M-series Metal. The only addition vs. the spike
 * is the optional `airport_context` initial-prompt biasing.
 */

#include "backends/whisper_stt.hpp"

#include "core/logging.hpp"
#include "whisper.h"

#include <algorithm>
#include <thread>

namespace backends {

namespace {
constexpr const char *kBackendTag = "STT-LOCAL";
}

WhisperStt::WhisperStt() = default;

WhisperStt::~WhisperStt() {
  if (ctx_)
    whisper_free(ctx_);
}

bool WhisperStt::open(const std::string &model_path,
                      const std::string &language) {
  whisper_context_params cparams = whisper_context_default_params();
  // Whichever GPU backend the prebuilt ggml carries (Metal on macOS, Vulkan on
  // Windows). ggml falls back to its CPU backend on its own if no device is
  // usable, so this stays true unconditionally.
  cparams.use_gpu = true;
  cparams.flash_attn = false;

  ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
  if (!ctx_)
    return false;

  // Half the perf cores, mirroring spike_whisper. whisper.cpp scales
  // sub-linearly above 4 threads on M1.
  const unsigned hw = std::thread::hardware_concurrency();
  n_threads_ =
      static_cast<int>(hw == 0 ? 4u : std::min(8u, std::max(2u, hw / 2)));
  lang_ = language.empty() ? "en" : language;
  return true;
}

std::string WhisperStt::transcribe(const std::vector<float> &pcm_16k_mono,
                                   const std::string &airport_context) {
  if (!ctx_ || pcm_16k_mono.empty())
    return {};
  logging::info("[%s][%s] transcribe %zu PCM samples (whisper.cpp)",
                kBackendTag, lang_.c_str(), pcm_16k_mono.size());

  whisper_full_params wparams =
      whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  wparams.language = lang_.c_str();
  wparams.translate = false;
  wparams.no_context = true;
  wparams.single_segment = false;
  wparams.print_progress = false;
  wparams.print_realtime = false;
  wparams.print_timestamps = false;
  wparams.print_special = false;
  wparams.n_threads = n_threads_;

  // Optional initial-prompt biasing toward the local airport / facility
  // names. Whisper conditions only on this prompt, no other state — so
  // biasing here cannot leak across consecutive transcriptions.
  if (!airport_context.empty()) {
    wparams.initial_prompt = airport_context.c_str();
  }

  if (whisper_full(ctx_, wparams, pcm_16k_mono.data(),
                   static_cast<int>(pcm_16k_mono.size())) != 0) {
    return {};
  }

  std::string transcript;
  const int n_segments = whisper_full_n_segments(ctx_);
  for (int i = 0; i < n_segments; ++i) {
    const char *seg = whisper_full_get_segment_text(ctx_, i);
    if (seg)
      transcript += seg;
  }

  // whisper.cpp sometimes emits a leading space; trim for cleaner
  // downstream prompts.
  while (!transcript.empty() && transcript.front() == ' ') {
    transcript.erase(transcript.begin());
  }
  return transcript;
}

} // namespace backends
