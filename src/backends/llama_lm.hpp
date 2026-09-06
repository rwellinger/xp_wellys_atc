/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_LLAMA_LM_HPP
#define BACKENDS_LLAMA_LM_HPP

#include "backends/i_language_model.hpp"

#include <string>

struct llama_model;
struct llama_context;
struct llama_sampler;
struct llama_vocab;

namespace backends {

// Concrete ILanguageModel backed by llama.cpp + Metal. open() loads
// the GGUF model and builds the inference context + sampler chain;
// respond() reuses them. The KV cache is cleared on every call so each
// turn is independent — multi-turn state belongs in the caller's state
// machine.
class LlamaLm final : public ILanguageModel {
public:
  LlamaLm();
  ~LlamaLm() override;

  LlamaLm(const LlamaLm &) = delete;
  LlamaLm &operator=(const LlamaLm &) = delete;

  bool open(const std::string &model_path);

  std::string respond(const std::string &system_prompt,
                      const std::string &user_text) override;

  std::string respond_constrained(const std::string &system_prompt,
                                  const std::string &user_text,
                                  const std::string &grammar_gbnf) override;

private:
  // Shared decode loop: generates from the current `ctx_` using the
  // given sampler, up to max_new_tok_ tokens. Returns trimmed output.
  // The KV cache must already be primed with the prompt tokens.
  std::string sample_loop(llama_sampler *sampler);

private:
  llama_model *model_ = nullptr;
  llama_context *ctx_ = nullptr;
  llama_sampler *sampler_ = nullptr;
  const llama_vocab *vocab_ = nullptr;
  int max_new_tok_ = 96; // ATC replies are short
};

} // namespace backends

#endif // BACKENDS_LLAMA_LM_HPP
