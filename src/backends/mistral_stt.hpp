/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_MISTRAL_STT_HPP
#define BACKENDS_MISTRAL_STT_HPP

#include "backends/i_speech_to_text.hpp"

#include <string>

namespace backends {

// ISpeechToText backed by Mistral's /v1/audio/transcriptions endpoint
// (Voxtral). Synchronous — backends::manager runs it on a worker
// thread. Every call emits a [STT-MISTRAL] audit log line.
//
// Language is resolved per request from settings::backend_language()
// inside transcribe(), so an ATC-profile switch (EU/US vs. DE) flips
// the Voxtral `language` parameter immediately, without reloading the
// backend.
//
// Airport context is forwarded as Voxtral's `context_bias[]` multipart
// array — one form field per comma-separated token. This is the
// idiomatic biasing path (vs. OpenAI's freeform `prompt` string).
class MistralStt final : public ISpeechToText {
public:
  static constexpr const char *kDefaultBaseUrl = "https://api.mistral.ai";

  MistralStt(std::string api_key, std::string model,
             std::string base_url = kDefaultBaseUrl);

  std::string transcribe(const std::vector<float> &pcm_16k_mono,
                         const std::string &airport_context) override;

private:
  std::string api_key_;
  std::string model_;
  std::string base_url_;
};

} // namespace backends

#endif // BACKENDS_MISTRAL_STT_HPP
