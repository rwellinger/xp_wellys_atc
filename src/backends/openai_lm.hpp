/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_OPENAI_LM_HPP
#define BACKENDS_OPENAI_LM_HPP

#include "backends/i_language_model.hpp"
#include "backends/openai_common.hpp"

#include <string>

namespace backends {

// ILanguageModel backed by OpenAI's /v1/chat/completions endpoint.
// respond_constrained() ignores the llama.cpp GBNF grammar and enables
// the OpenAI JSON mode instead — the manager parses the JSON
// client-side, so the contract (JSON-shaped reply) is preserved.
// Every call emits a [LM-OPENAI] audit log line.
class OpenAiLm final : public ILanguageModel {
public:
  OpenAiLm(std::string api_key, std::string model,
           std::string base_url = openai_common::kDefaultBaseUrl);

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

#endif // BACKENDS_OPENAI_LM_HPP
