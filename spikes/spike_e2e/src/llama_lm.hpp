// Concrete ILanguageModel backed by llama.cpp + Metal. Loads the GGUF model
// once and reuses the context, sampler, and chat template across `respond()`
// calls. The KV cache is cleared at the start of every call so each turn is
// independent — multi-turn state belongs in the caller's state machine.

#pragma once

#include "i_language_model.hpp"

#include <string>

struct llama_model;
struct llama_context;
struct llama_sampler;
struct llama_vocab;

namespace spike_e2e {

class LlamaLm final : public ILanguageModel {
public:
    LlamaLm();
    ~LlamaLm() override;

    LlamaLm(const LlamaLm&)            = delete;
    LlamaLm& operator=(const LlamaLm&) = delete;

    // Load the GGUF model and build the inference context + sampler chain.
    // Returns false on failure.
    bool open(const std::string& model_path);

    std::string respond(const std::string& system_prompt,
                        const std::string& user_text) override;

private:
    llama_model*       model_       = nullptr;
    llama_context*     ctx_         = nullptr;
    llama_sampler*     sampler_     = nullptr;
    const llama_vocab* vocab_       = nullptr;
    int                max_new_tok_ = 96;  // ATC replies are short
};

}  // namespace spike_e2e
