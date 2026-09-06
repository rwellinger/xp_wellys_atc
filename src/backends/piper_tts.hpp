/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_PIPER_TTS_HPP
#define BACKENDS_PIPER_TTS_HPP

#include "backends/i_text_to_speech.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

struct piper_synthesizer;

namespace backends {

// Concrete ITextToSpeech backed by Piper. The instance owns one
// piper_synthesizer per loaded voice; synthesize() picks the right
// one by voice_id. The first init() call also primes Piper's global
// espeak-ng-data path — that path is shared across all voices and
// cannot be changed afterwards, so we lock it in once.
class PiperTts final : public ITextToSpeech {
public:
  PiperTts();
  ~PiperTts() override;

  PiperTts(const PiperTts &) = delete;
  PiperTts &operator=(const PiperTts &) = delete;

  // One-time setup: locks the espeak-ng-data path used by every
  // subsequent load_voice() call. Must be called before load_voice().
  // Returns false if the directory is missing.
  bool init(const std::string &espeakng_data_dir);

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
  std::string espeakng_dir_; // pinned by init()
  // Voices are loaded/unloaded from the loader thread (load_voice) and
  // synthesised from the worker thread (synthesize). The map itself is
  // protected by mutex_; the piper_synthesizer values are read without
  // the lock during synthesize() but Piper's own lock-free design lets
  // multiple synthesize calls on different voices run concurrently.
  // Same voice synthesised concurrently is serialised by the manager's
  // g_tts_call_mtx.
  mutable std::mutex mutex_;
  std::unordered_map<std::string, piper_synthesizer *> synths_;
};

} // namespace backends

#endif // BACKENDS_PIPER_TTS_HPP
