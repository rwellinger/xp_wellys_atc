/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_I_SPEECH_TO_TEXT_HPP
#define BACKENDS_I_SPEECH_TO_TEXT_HPP

#include <string>
#include <vector>

namespace backends {

class ISpeechToText {
public:
  virtual ~ISpeechToText() = default;

  // Transcribe a 16 kHz mono float32 PCM buffer (range [-1, 1]).
  // Returns the UTF-8 transcript; an empty string indicates failure.
  // `airport_context` is an optional Whisper prompt biasing transcription
  // toward local airport / facility names.
  virtual std::string transcribe(const std::vector<float> &pcm_16k_mono,
                                 const std::string &airport_context) = 0;
};

} // namespace backends

#endif // BACKENDS_I_SPEECH_TO_TEXT_HPP
