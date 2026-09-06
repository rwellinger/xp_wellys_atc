/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_MISTRAL_TTS_HPP
#define BACKENDS_MISTRAL_TTS_HPP

#include "backends/i_text_to_speech.hpp"

#include <mutex>
#include <string>
#include <unordered_set>

namespace backends {

// ITextToSpeech backed by Mistral's /v1/audio/speech endpoint
// (Voxtral TTS). voice_id is a free-form string — Mistral's preset
// voice ids are not whitelisted client-side, so any string the user
// configured in the Mistral dashboard works (including custom voice
// clones). length_scale has no direct API equivalent and is silently
// ignored; ATIS still plays through the same pipeline, just at the
// model's native rate. load_voice() / unload_voice() track the set
// of known ids so default_voice_for() can pick a sensible fallback.
//
// The endpoint returns a WAV body (response_format=wav); we decode it
// with openai_common::wav_to_pcm_int16(), which is provider-agnostic.
// Every call emits a [TTS-MISTRAL] audit log line.
class MistralTts final : public ITextToSpeech {
public:
  static constexpr const char *kDefaultBaseUrl = "https://api.mistral.ai";

  MistralTts(std::string api_key, std::string model,
             std::string base_url = kDefaultBaseUrl);

  bool load_voice(const std::string &voice_id,
                  const std::string &voice_onnx_path,
                  const std::string &voice_json_path) override;
  void unload_voice(const std::string &voice_id) override;
  bool has_voice(const std::string &voice_id) const override;

  std::vector<int16_t> synthesize(const std::string &voice_id,
                                  const std::string &text, float length_scale,
                                  uint32_t &sample_rate_hz,
                                  TtsFailure &out_failure) override;

  std::string default_voice_for(model_manifest::VoiceRole role) const override;

private:
  std::string api_key_;
  std::string model_;
  std::string base_url_;

  mutable std::mutex mutex_;
  std::unordered_set<std::string> loaded_voices_;
};

} // namespace backends

#endif // BACKENDS_MISTRAL_TTS_HPP
