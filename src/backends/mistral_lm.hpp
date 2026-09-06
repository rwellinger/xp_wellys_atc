/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_MISTRAL_LM_HPP
#define BACKENDS_MISTRAL_LM_HPP

#include "backends/i_language_model.hpp"

#include <string>

namespace backends {

// ILanguageModel backed by Mistral's /v1/chat/completions endpoint.
// Request shape is identical to OpenAI's (model + messages array +
// JSON-mode response_format), so the response parsing matches one-to-
// one. respond_constrained() drops the llama.cpp GBNF grammar and
// enables Mistral's JSON mode instead — same contract as OpenAiLm.
// Every call emits a [LM-MISTRAL] audit log line.
//
// Audio helpers (last4) are reused from openai_common since they are
// provider-agnostic and carry no api.openai.com literal — the
// source-level audit grep is unaffected.
class MistralLm final : public ILanguageModel {
public:
  static constexpr const char *kDefaultBaseUrl = "https://api.mistral.ai";

  MistralLm(std::string api_key, std::string model,
            std::string base_url = kDefaultBaseUrl);

  std::string respond(const std::string &system_prompt,
                      const std::string &user_text) override;

  std::string respond_constrained(const std::string &system_prompt,
                                  const std::string &user_text,
                                  const std::string &grammar_gbnf) override;

private:
  std::string call(const std::string &system_prompt,
                   const std::string &user_text, bool json_mode);

  std::string api_key_;
  std::string model_;
  std::string base_url_;
};

} // namespace backends

#endif // BACKENDS_MISTRAL_LM_HPP
