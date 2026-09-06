// spike_llama — milestone 03 latency / memory probe for llama.cpp.
//
// Usage: spike_llama <model.gguf> <prompt.txt>
//
// Loads a GGUF instruction-tuned LLM, applies the model's built-in chat
// template to a [system]/[user] prompt file, runs a single completion with a
// fixed sampling config (temp 0.3, top_p 0.9, top_k 40, max_tokens 256), and
// prints the generated text plus a timing breakdown:
//   model load, prompt eval, time-to-first-token, generation, wall total,
//   sustained throughput, peak RSS.
//
// A short warm-up generation is executed first (and discarded) so that
// reported metrics reflect steady-state Metal pipeline behaviour rather than
// first-launch JIT cost.
//
// Deliberately minimal: no streaming UI, no multi-turn history, no GBNF.

#include "llama.h"

#include <sys/resource.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock    = std::chrono::steady_clock;
using millisec = std::chrono::duration<double, std::milli>;

double elapsed_ms(Clock::time_point a, Clock::time_point b) {
    return millisec(b - a).count();
}

double peak_rss_mb() {
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
    // macOS reports ru_maxrss in bytes (Linux: KB); spike is Apple-only.
    return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
}

// -----------------------------------------------------------------------------
// Prompt-file parser.
//
// Format:
//
//   [system]
//   <text...>
//   [user]
//   <text...>
//   [assistant]   (optional, for few-shot priming)
//   <text...>
//
// Section headers must appear at the start of a line. Lines before the first
// header are treated as [user] (so a bare prompt file just works).
// -----------------------------------------------------------------------------

struct ChatTurn {
    std::string role;     // "system" / "user" / "assistant"
    std::string content;
};

bool starts_with(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

bool load_prompt(const std::string& path, std::vector<ChatTurn>& out, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "cannot open " + path; return false; }

    std::string line;
    std::string current_role = "user";
    std::string buf;

    auto flush = [&]() {
        std::string content = rtrim(buf);
        if (!content.empty()) out.push_back({current_role, std::move(content)});
        buf.clear();
    };

    while (std::getline(f, line)) {
        if (starts_with(line, "[system]"))         { flush(); current_role = "system";    continue; }
        if (starts_with(line, "[user]"))           { flush(); current_role = "user";      continue; }
        if (starts_with(line, "[assistant]"))      { flush(); current_role = "assistant"; continue; }
        buf += line;
        buf += '\n';
    }
    flush();

    if (out.empty()) { err = "prompt file is empty"; return false; }
    return true;
}

// -----------------------------------------------------------------------------
// Apply the model's built-in chat template via llama_chat_apply_template.
// Returns the formatted prompt as a single string ready for tokenization.
// -----------------------------------------------------------------------------

std::string apply_chat_template(const llama_model* model, const std::vector<ChatTurn>& turns) {
    std::vector<llama_chat_message> msgs;
    msgs.reserve(turns.size());
    for (const auto& t : turns) msgs.push_back({t.role.c_str(), t.content.c_str()});

    const char* tmpl = llama_model_chat_template(model, /*name=*/nullptr);
    // Recommended buffer size per llama.h: 2 * total chars.
    size_t total_chars = 0;
    for (const auto& t : turns) total_chars += t.role.size() + t.content.size();
    std::vector<char> buf(std::max<size_t>(2 * total_chars, 1024));

    int32_t n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(),
                                          /*add_ass=*/true, buf.data(), int32_t(buf.size()));
    if (n > int32_t(buf.size())) {
        buf.resize(n);
        n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(),
                                      /*add_ass=*/true, buf.data(), int32_t(buf.size()));
    }
    if (n <= 0) return {};
    return std::string(buf.data(), size_t(n));
}

std::vector<llama_token> tokenize(const llama_vocab* vocab, const std::string& text, bool add_special) {
    int32_t needed = -llama_tokenize(vocab, text.c_str(), int32_t(text.size()),
                                     nullptr, 0, add_special, /*parse_special=*/true);
    std::vector<llama_token> out(needed);
    int32_t n = llama_tokenize(vocab, text.c_str(), int32_t(text.size()),
                               out.data(), int32_t(out.size()), add_special, /*parse_special=*/true);
    if (n < 0) return {};
    out.resize(n);
    return out;
}

std::string token_to_piece(const llama_vocab* vocab, llama_token tok) {
    char buf[256];
    int32_t n = llama_token_to_piece(vocab, tok, buf, sizeof(buf), /*lstrip=*/0, /*special=*/false);
    if (n < 0) return {};
    return std::string(buf, size_t(n));
}

// -----------------------------------------------------------------------------
// Generate up to `max_new_tokens` tokens from the current KV state. Stops on
// EOG. Fills `text_out` and reports per-stage timings via the out-params.
// -----------------------------------------------------------------------------

struct GenStats {
    int    n_prompt           = 0;
    int    n_generated        = 0;
    double prompt_eval_ms     = 0.0;
    double first_token_ms     = 0.0;   // TTFT = prompt eval + first sample + first decode of generated token
    double generation_ms      = 0.0;   // wall time for tokens 2..N (i.e. excluding TTFT)
};

bool generate(llama_context*       ctx,
              llama_sampler*       smpl,
              const llama_vocab*   vocab,
              std::vector<llama_token> prompt_tokens,
              int                  max_new_tokens,
              std::string&         text_out,
              GenStats&            stats) {
    stats.n_prompt = int(prompt_tokens.size());

    // --- prompt eval ---------------------------------------------------------
    llama_batch batch = llama_batch_get_one(prompt_tokens.data(), int32_t(prompt_tokens.size()));
    const auto t_prompt_start = Clock::now();
    if (llama_decode(ctx, batch) != 0) {
        std::fprintf(stderr, "llama_decode failed on prompt\n");
        return false;
    }
    const auto t_prompt_end = Clock::now();
    stats.prompt_eval_ms = elapsed_ms(t_prompt_start, t_prompt_end);

    // --- generation loop -----------------------------------------------------
    text_out.clear();
    Clock::time_point t_after_first_token{};
    llama_token tok = 0;

    for (int i = 0; i < max_new_tokens; ++i) {
        tok = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, tok)) break;

        text_out += token_to_piece(vocab, tok);
        stats.n_generated++;

        if (i == 0) {
            t_after_first_token = Clock::now();
            stats.first_token_ms = elapsed_ms(t_prompt_start, t_after_first_token);
        }

        llama_batch one = llama_batch_get_one(&tok, 1);
        if (llama_decode(ctx, one) != 0) {
            std::fprintf(stderr, "llama_decode failed during generation at i=%d\n", i);
            return false;
        }
    }

    if (stats.n_generated > 1) {
        stats.generation_ms = elapsed_ms(t_after_first_token, Clock::now());
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <model.gguf> <prompt.txt>\n", argv[0]);
        return 1;
    }
    const std::string model_path  = argv[1];
    const std::string prompt_path = argv[2];

    const auto t_total_start = Clock::now();

    // --- load prompt --------------------------------------------------------
    std::vector<ChatTurn> turns;
    std::string err;
    if (!load_prompt(prompt_path, turns, err)) {
        std::fprintf(stderr, "prompt load failed: %s\n", err.c_str());
        return 2;
    }
    std::printf("prompt       : %s (%zu turns)\n", prompt_path.c_str(), turns.size());

    // --- silence ggml/llama log spam ----------------------------------------
    llama_log_set([](enum ggml_log_level lvl, const char* msg, void*) {
        if (lvl >= GGML_LOG_LEVEL_WARN) std::fprintf(stderr, "%s", msg);
    }, nullptr);

    llama_backend_init();

    // --- load model ---------------------------------------------------------
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 999;       // Metal: offload everything that fits.
    mparams.use_mmap     = true;
    mparams.use_mlock    = false;

    const auto t_load_start = Clock::now();
    llama_model* model = llama_model_load_from_file(model_path.c_str(), mparams);
    const auto t_load_end = Clock::now();
    if (!model) {
        std::fprintf(stderr, "model load failed: %s\n", model_path.c_str());
        llama_backend_free();
        return 3;
    }
    std::printf("model        : %s\n", model_path.c_str());

    const llama_vocab* vocab = llama_model_get_vocab(model);

    // --- create context -----------------------------------------------------
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx           = 2048;   // milestone 03 default; ATC turns are short
    cparams.n_batch         = 512;
    cparams.n_ubatch        = 512;
    cparams.n_threads       = 4;
    cparams.n_threads_batch = 4;
    cparams.no_perf         = true;   // we measure ourselves

    llama_context* ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        std::fprintf(stderr, "context init failed\n");
        llama_model_free(model);
        llama_backend_free();
        return 4;
    }

    // --- sampler chain: top_k → top_p → temp → dist -------------------------
    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    sparams.no_perf = true;
    llama_sampler* smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.9f, /*min_keep=*/1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.3f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(/*seed=*/1234));

    // --- format prompt via chat template ------------------------------------
    const std::string formatted = apply_chat_template(model, turns);
    if (formatted.empty()) {
        std::fprintf(stderr, "chat template formatting failed\n");
        llama_sampler_free(smpl);
        llama_free(ctx);
        llama_model_free(model);
        llama_backend_free();
        return 5;
    }

    auto prompt_tokens = tokenize(vocab, formatted, /*add_special=*/true);
    if (prompt_tokens.empty()) {
        std::fprintf(stderr, "tokenization failed\n");
        llama_sampler_free(smpl);
        llama_free(ctx);
        llama_model_free(model);
        llama_backend_free();
        return 6;
    }

    // --- warm-up: short generation, results discarded -----------------------
    {
        std::string warm_text;
        GenStats    warm_stats;
        // Re-use the formatted prompt so KV layout matches the real run, but
        // cap the warm-up to a handful of tokens to keep startup brisk.
        if (!generate(ctx, smpl, vocab, prompt_tokens, /*max_new_tokens=*/8, warm_text, warm_stats)) {
            std::fprintf(stderr, "warm-up generation failed\n");
            llama_sampler_free(smpl);
            llama_free(ctx);
            llama_model_free(model);
            llama_backend_free();
            return 7;
        }
        // Reset for the measured run.
        llama_memory_clear(llama_get_memory(ctx), /*data=*/true);
        llama_sampler_reset(smpl);
    }

    // --- measured run -------------------------------------------------------
    std::string  text;
    GenStats     stats;
    if (!generate(ctx, smpl, vocab, prompt_tokens, /*max_new_tokens=*/256, text, stats)) {
        llama_sampler_free(smpl);
        llama_free(ctx);
        llama_model_free(model);
        llama_backend_free();
        return 8;
    }

    const auto t_total_end = Clock::now();

    // --- output -------------------------------------------------------------
    std::printf("\n--- response ---\n%s\n", text.c_str());

    const double tok_per_s = (stats.n_generated > 1 && stats.generation_ms > 0.0)
        ? (double(stats.n_generated - 1) * 1000.0 / stats.generation_ms)
        : 0.0;

    std::printf("\n--- timings ---\n");
    std::printf("model load   : %8.1f ms\n", elapsed_ms(t_load_start, t_load_end));
    std::printf("prompt eval  : %8.1f ms  (%d tokens)\n", stats.prompt_eval_ms, stats.n_prompt);
    std::printf("first token  : %8.1f ms  (TTFT incl. prompt eval)\n", stats.first_token_ms);
    std::printf("generation   : %8.1f ms  (%d tokens after the first)\n",
                stats.generation_ms, std::max(0, stats.n_generated - 1));
    std::printf("throughput   : %8.1f tok/s\n", tok_per_s);
    std::printf("wall total   : %8.1f ms\n", elapsed_ms(t_total_start, t_total_end));
    std::printf("threads      : %d\n", cparams.n_threads);

    std::printf("\n--- memory ---\n");
    std::printf("peak rss     : %8.1f MB\n", peak_rss_mb());

    llama_sampler_free(smpl);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
