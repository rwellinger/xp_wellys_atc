/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_OPENAI_TTS_HPP
#define BACKENDS_OPENAI_TTS_HPP

#include "backends/i_text_to_speech.hpp"
#include "backends/openai_common.hpp"

#include <mutex>
#include <string>
#include <unordered_set>

namespace backends {

// ITextToSpeech backed by OpenAI's /v1/audio/speech endpoint.
// `voice_id` must be one of: alloy, echo, fable, onyx, nova, shimmer
// (the six TTS voices OpenAI ships). length_scale > 1 (Piper's "slow
// down for ATIS") is mapped to OpenAI's speed = 1.0 / length_scale,
// clamped to [0.25, 4.0] (the API's accepted range).
// load_voice() / unload_voice() are essentially no-ops — there is no
// model to download. The TTS endpoint returns a canonical 16-bit
// mono PCM WAV at 24 kHz; we decode it into the int16 buffer the
// player expects.
class OpenAiTts final : public ITextToSpeech {
public:
  OpenAiTts(std::string api_key, std::string model,
            std::string base_url = openai_common::kDefaultBaseUrl);

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

#endif // BACKENDS_OPENAI_TTS_HPP
