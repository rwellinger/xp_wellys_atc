#include "llama_lm.hpp"

#include "llama.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace spike_e2e {

namespace {

std::string apply_chat_template(const llama_model*                       model,
                                const std::vector<llama_chat_message>&   msgs,
                                size_t                                   total_chars) {
    const char* tmpl = llama_model_chat_template(model, /*name=*/nullptr);
    std::vector<char> buf(std::max<size_t>(2 * total_chars, 1024));

    int32_t n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(),
                                          /*add_ass=*/true, buf.data(),
                                          int32_t(buf.size()));
    if (n > int32_t(buf.size())) {
        buf.resize(n);
        n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(),
                                      /*add_ass=*/true, buf.data(),
                                      int32_t(buf.size()));
    }
    if (n <= 0) return {};
    return std::string(buf.data(), size_t(n));
}

std::vector<llama_token> tokenize(const llama_vocab* vocab,
                                  const std::string& text,
                                  bool               add_special) {
    int32_t needed = -llama_tokenize(vocab, text.c_str(), int32_t(text.size()),
                                     nullptr, 0, add_special,
                                     /*parse_special=*/true);
    std::vector<llama_token> out(needed);
    int32_t n = llama_tokenize(vocab, text.c_str(), int32_t(text.size()),
                               out.data(), int32_t(out.size()),
                               add_special, /*parse_special=*/true);
    if (n < 0) return {};
    out.resize(n);
    return out;
}

std::string token_to_piece(const llama_vocab* vocab, llama_token tok) {
    char    buf[256];
    int32_t n = llama_token_to_piece(vocab, tok, buf, sizeof(buf),
                                     /*lstrip=*/0, /*special=*/false);
    if (n < 0) return {};
    return std::string(buf, size_t(n));
}

}  // namespace

LlamaLm::LlamaLm() = default;

LlamaLm::~LlamaLm() {
    if (sampler_) llama_sampler_free(sampler_);
    if (ctx_)     llama_free(ctx_);
    if (model_)   llama_model_free(model_);
}

bool LlamaLm::open(const std::string& model_path) {
    // Silence ggml/llama log spam (warnings still go through).
    llama_log_set([](enum ggml_log_level lvl, const char* msg, void*) {
        if (lvl >= GGML_LOG_LEVEL_WARN) std::fputs(msg, stderr);
    }, nullptr);

    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 999;
    mparams.use_mmap     = true;
    mparams.use_mlock    = false;

    model_ = llama_model_load_from_file(model_path.c_str(), mparams);
    if (!model_) return false;

    vocab_ = llama_model_get_vocab(model_);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx           = 2048;
    cparams.n_batch         = 512;
    cparams.n_ubatch        = 512;
    cparams.n_threads       = 4;
    cparams.n_threads_batch = 4;
    cparams.no_perf         = true;

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_) return false;

    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    sparams.no_perf = true;
    sampler_ = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler_, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(sampler_, llama_sampler_init_top_p(0.9f, 1));
    llama_sampler_chain_add(sampler_, llama_sampler_init_temp(0.3f));
    llama_sampler_chain_add(sampler_, llama_sampler_init_dist(/*seed=*/1234));

    return true;
}

std::string LlamaLm::respond(const std::string& system_prompt,
                             const std::string& user_text) {
    if (!ctx_ || !sampler_ || !model_ || !vocab_) return {};

    // Each call is a fresh turn. Drop everything from the previous one.
    llama_memory_clear(llama_get_memory(ctx_), /*data=*/true);
    llama_sampler_reset(sampler_);

    std::vector<llama_chat_message> msgs;
    msgs.push_back({"system", system_prompt.c_str()});
    msgs.push_back({"user",   user_text.c_str()});

    const std::string formatted = apply_chat_template(
        model_, msgs, system_prompt.size() + user_text.size() + 32);
    if (formatted.empty()) return {};

    std::vector<llama_token> prompt_tokens = tokenize(vocab_, formatted, /*add_special=*/true);
    if (prompt_tokens.empty()) return {};

    llama_batch batch = llama_batch_get_one(prompt_tokens.data(),
                                            int32_t(prompt_tokens.size()));
    if (llama_decode(ctx_, batch) != 0) return {};

    std::string out;
    for (int i = 0; i < max_new_tok_; ++i) {
        llama_token tok = llama_sampler_sample(sampler_, ctx_, -1);
        if (llama_vocab_is_eog(vocab_, tok)) break;
        out += token_to_piece(vocab_, tok);

        llama_batch one = llama_batch_get_one(&tok, 1);
        if (llama_decode(ctx_, one) != 0) break;
    }

    // Trim trailing whitespace.
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '
                            || out.back() == '\r' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

}  // namespace spike_e2e
