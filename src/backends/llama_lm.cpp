/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Lifted from spikes/spike_e2e/src/llama_lm.cpp; the spike fixed the
 * sampler chain (top_k=40, top_p=0.9, temp=0.3) on M-series Metal.
 */

#include "backends/llama_lm.hpp"

#include "core/logging.hpp"
#include "llama.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace backends {

namespace {

constexpr const char *kBackendTag = "LM-LOCAL";

std::string apply_chat_template(const llama_model *model,
                                const std::vector<llama_chat_message> &msgs,
                                size_t total_chars) {
  const char *tmpl = llama_model_chat_template(model, /*name=*/nullptr);
  std::vector<char> buf(std::max<size_t>(2 * total_chars, 1024));

  int32_t n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(),
                                        /*add_ass=*/true, buf.data(),
                                        static_cast<int32_t>(buf.size()));
  if (n > static_cast<int32_t>(buf.size())) {
    buf.resize(n);
    n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(),
                                  /*add_ass=*/true, buf.data(),
                                  static_cast<int32_t>(buf.size()));
  }
  if (n <= 0)
    return {};
  return std::string(buf.data(), static_cast<size_t>(n));
}

std::vector<llama_token> tokenize(const llama_vocab *vocab,
                                  const std::string &text, bool add_special) {
  int32_t needed =
      -llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                      nullptr, 0, add_special, /*parse_special=*/true);
  std::vector<llama_token> out(needed);
  int32_t n =
      llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                     out.data(), static_cast<int32_t>(out.size()), add_special,
                     /*parse_special=*/true);
  if (n < 0)
    return {};
  out.resize(n);
  return out;
}

std::string token_to_piece(const llama_vocab *vocab, llama_token tok) {
  char buf[256];
  int32_t n = llama_token_to_piece(vocab, tok, buf, sizeof(buf),
                                   /*lstrip=*/0, /*special=*/false);
  if (n < 0)
    return {};
  return std::string(buf, static_cast<size_t>(n));
}

} // namespace

LlamaLm::LlamaLm() = default;

LlamaLm::~LlamaLm() {
  if (sampler_)
    llama_sampler_free(sampler_);
  if (ctx_)
    llama_free(ctx_);
  if (model_)
    llama_model_free(model_);
}

bool LlamaLm::open(const std::string &model_path) {
  // Silence ggml/llama log spam (warnings still go through).
  llama_log_set(
      [](enum ggml_log_level lvl, const char *msg, void *) {
        if (lvl >= GGML_LOG_LEVEL_WARN)
          std::fputs(msg, stderr);
      },
      nullptr);

  llama_backend_init();

  llama_model_params mparams = llama_model_default_params();
  mparams.n_gpu_layers = 999;
  mparams.use_mmap = true;
  mparams.use_mlock = false;

  model_ = llama_model_load_from_file(model_path.c_str(), mparams);
  if (!model_)
    return false;

  vocab_ = llama_model_get_vocab(model_);

  llama_context_params cparams = llama_context_default_params();
  cparams.n_ctx = 2048;
  // n_batch/n_ubatch must be >= the largest single prompt we ever
  // decode in one llama_batch_get_one() call. The classify_with_repair
  // system prompt (with Whisper-vs-pilot examples + valid_intents
  // enum + flight context) tokenizes to ~1000-1200 tokens; bumping
  // both to n_ctx is the standard llama.cpp pattern for full-prompt
  // decode. Smaller values (e.g. 512) trigger
  // GGML_ASSERT(n_tokens_all <= cparams.n_batch) which abort()s the
  // process — observed crash UUID 935e8d56 from May 2026.
  cparams.n_batch = 2048;
  cparams.n_ubatch = 2048;
  // Token-by-token decode is GPU-bound — two CPU threads are enough and free
  // cores for the X-Plane main thread + physics. The batch path (initial prompt
  // eval) keeps the larger thread pool.
  cparams.n_threads = 2;
  cparams.n_threads_batch = 4;
  // Flash attention cuts attention memory bandwidth roughly in half on Metal
  // for the ~1k-token classify prompts; sampling unchanged. Only forced on
  // Apple, where it is measured: on Vulkan, FA support varies by GPU and driver
  // version, so AUTO lets llama.cpp decide rather than demanding a path the
  // device may not have.
#if defined(__APPLE__)
  cparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
#else
  cparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;
#endif
  cparams.no_perf = true;

  ctx_ = llama_init_from_model(model_, cparams);
  if (!ctx_)
    return false;

  llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
  sparams.no_perf = true;
  sampler_ = llama_sampler_chain_init(sparams);
  llama_sampler_chain_add(sampler_, llama_sampler_init_top_k(40));
  llama_sampler_chain_add(sampler_, llama_sampler_init_top_p(0.9f, 1));
  llama_sampler_chain_add(sampler_, llama_sampler_init_temp(0.3f));
  llama_sampler_chain_add(sampler_, llama_sampler_init_dist(/*seed=*/1234));

  return true;
}

std::string LlamaLm::sample_loop(llama_sampler *sampler) {
  std::string out;
  for (int i = 0; i < max_new_tok_; ++i) {
    llama_token tok = llama_sampler_sample(sampler, ctx_, -1);
    if (llama_vocab_is_eog(vocab_, tok))
      break;
    out += token_to_piece(vocab_, tok);

    llama_batch one = llama_batch_get_one(&tok, 1);
    if (llama_decode(ctx_, one) != 0)
      break;
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ' ||
                          out.back() == '\r' || out.back() == '\t')) {
    out.pop_back();
  }
  return out;
}

// Decode an arbitrarily-sized prompt in n_batch-sized chunks. A
// single llama_batch_get_one() of more than cparams.n_batch tokens
// triggers GGML_ASSERT(n_tokens_all <= cparams.n_batch) inside
// llama-context.cpp and abort()s the host process — observed crash
// from May 2026 when our system prompt grew past 512 tokens.
// Chunking keeps the decode robust even if a future prompt rewrite
// pushes total tokens above the configured n_batch (currently 2048).
// Returns true if the entire prompt decoded; false if any chunk
// failed (caller treats as transient LM error and returns "").
static bool decode_prompt_chunked(llama_context *ctx,
                                  std::vector<llama_token> &tokens,
                                  int32_t chunk_size) {
  if (tokens.empty())
    return false;
  int32_t total = static_cast<int32_t>(tokens.size());
  for (int32_t off = 0; off < total; off += chunk_size) {
    int32_t this_chunk = std::min(chunk_size, total - off);
    llama_batch batch = llama_batch_get_one(tokens.data() + off, this_chunk);
    if (llama_decode(ctx, batch) != 0)
      return false;
  }
  return true;
}

std::string LlamaLm::respond(const std::string &system_prompt,
                             const std::string &user_text) {
  if (!ctx_ || !sampler_ || !model_ || !vocab_)
    return {};
  logging::info("[%s] respond, %zu+%zu chars (llama.cpp, Metal)", kBackendTag,
                system_prompt.size(), user_text.size());

  // Each call is a fresh turn. Drop everything from the previous one.
  llama_memory_clear(llama_get_memory(ctx_), /*data=*/true);
  llama_sampler_reset(sampler_);

  std::vector<llama_chat_message> msgs;
  msgs.push_back({"system", system_prompt.c_str()});
  msgs.push_back({"user", user_text.c_str()});

  const std::string formatted = apply_chat_template(
      model_, msgs, system_prompt.size() + user_text.size() + 32);
  if (formatted.empty())
    return {};

  std::vector<llama_token> prompt_tokens =
      tokenize(vocab_, formatted, /*add_special=*/true);
  if (prompt_tokens.empty())
    return {};

  // Chunk size deliberately below the n_batch=2048 we configured at
  // open() — keeps headroom for future prompt growth without ever
  // tripping the GGML_ASSERT.
  if (!decode_prompt_chunked(ctx_, prompt_tokens, /*chunk_size=*/1024))
    return {};

  return sample_loop(sampler_);
}

std::string LlamaLm::respond_constrained(const std::string &system_prompt,
                                         const std::string &user_text,
                                         const std::string &grammar_gbnf) {
  if (!ctx_ || !model_ || !vocab_)
    return {};
  if (grammar_gbnf.empty())
    return respond(system_prompt, user_text);
  logging::info("[%s] respond_constrained (GBNF), %zu+%zu chars (llama.cpp, "
                "Metal)",
                kBackendTag, system_prompt.size(), user_text.size());

  // Build a fresh sampler chain for this call only: grammar at the
  // top of the chain so it filters logits before top_k/top_p apply.
  // Lower temperature than the default chain (0.1 vs 0.3) — for a
  // strict-JSON enum-pick task we want determinism, not creativity.
  llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
  sparams.no_perf = true;
  llama_sampler *chain = llama_sampler_chain_init(sparams);
  if (!chain)
    return {};

  llama_sampler *gsam =
      llama_sampler_init_grammar(vocab_, grammar_gbnf.c_str(), "root");
  if (!gsam) {
    llama_sampler_free(chain);
    return {};
  }
  llama_sampler_chain_add(chain, gsam);
  llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
  llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.9f, 1));
  llama_sampler_chain_add(chain, llama_sampler_init_temp(0.1f));
  llama_sampler_chain_add(chain, llama_sampler_init_dist(/*seed=*/1234));

  llama_memory_clear(llama_get_memory(ctx_), /*data=*/true);

  std::vector<llama_chat_message> msgs;
  msgs.push_back({"system", system_prompt.c_str()});
  msgs.push_back({"user", user_text.c_str()});

  const std::string formatted = apply_chat_template(
      model_, msgs, system_prompt.size() + user_text.size() + 32);
  if (formatted.empty()) {
    llama_sampler_free(chain);
    return {};
  }

  std::vector<llama_token> prompt_tokens =
      tokenize(vocab_, formatted, /*add_special=*/true);
  if (prompt_tokens.empty()) {
    llama_sampler_free(chain);
    return {};
  }

  if (!decode_prompt_chunked(ctx_, prompt_tokens, /*chunk_size=*/1024)) {
    llama_sampler_free(chain);
    return {};
  }

  std::string out = sample_loop(chain);
  llama_sampler_free(chain);
  return out;
}

} // namespace backends
