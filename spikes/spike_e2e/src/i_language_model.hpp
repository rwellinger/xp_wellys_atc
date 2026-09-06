// Strategy interface for the language-model backend. Single-shot — the spike
// is not multi-turn. The plugin will own multi-turn state through its ATC
// state machine; the LLM only generates phraseology for one turn at a time.

#pragma once

#include <string>

namespace spike_e2e {

class ILanguageModel {
public:
    virtual ~ILanguageModel() = default;

    // Run a single completion with the given system prompt + user message.
    // Implementations must clear any KV / sampler state between calls so a
    // long-running orchestrator can hand them an unbounded sequence of
    // independent turns. Returns the assistant reply (empty on failure).
    virtual std::string respond(const std::string& system_prompt,
                                const std::string& user_text) = 0;
};

}  // namespace spike_e2e
