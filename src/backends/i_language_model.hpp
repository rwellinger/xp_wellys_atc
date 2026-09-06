/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_I_LANGUAGE_MODEL_HPP
#define BACKENDS_I_LANGUAGE_MODEL_HPP

#include <string>

namespace backends {

class ILanguageModel {
public:
  virtual ~ILanguageModel() = default;

  // Run a single completion with the given system prompt + user message.
  // Implementations must clear any KV / sampler state between calls so a
  // long-running orchestrator can hand them an unbounded sequence of
  // independent turns. Returns the assistant reply (empty on failure).
  virtual std::string respond(const std::string &system_prompt,
                              const std::string &user_text) = 0;

  // Same contract as respond(), but the output is constrained to
  // grammar_gbnf (a GBNF grammar string from llama.cpp). Used by the
  // intent classifier to force JSON-shaped, enum-bounded output so the
  // model cannot invent intents outside the valid list. Default impl
  // ignores the grammar — test mocks and non-llama backends keep
  // working without change.
  virtual std::string respond_constrained(const std::string &system_prompt,
                                          const std::string &user_text,
                                          const std::string &grammar_gbnf) {
    (void)grammar_gbnf;
    return respond(system_prompt, user_text);
  }
};

} // namespace backends

#endif // BACKENDS_I_LANGUAGE_MODEL_HPP
