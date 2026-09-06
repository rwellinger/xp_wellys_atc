// spike_e2e — milestone 05 end-to-end CLI: WAV (pilot) → STT → LLM → TTS → WAV.
//
// Loads whisper, llama, and Piper model handles once at startup; then either
// processes a single (in.wav, out.wav) pair or reads repeated `<in> <out>`
// pairs from stdin so warm-state latencies can be measured without process
// restart cost. Per-stage timings (load + transcribe / respond / synthesize /
// write) and total wall-clock are printed for every request.
//
// Deliberately minimal: sequential stage execution, no streaming, no VAD,
// no resampling. Parallelising LLM token-stream into TTS is a milestone-06
// lever, not a spike-stage lever.

#include "i_language_model.hpp"
#include "i_speech_to_text.hpp"
#include "i_text_to_speech.hpp"
#include "llama_lm.hpp"
#include "piper_tts.hpp"
#include "wav.hpp"
#include "whisper_stt.hpp"

#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// CLI flag parsing. Order-independent, long-form only — readable from a
// shell loop without quoting gymnastics.
struct Args {
    std::string whisper_model;
    std::string llama_model;
    std::string piper_voice;
    std::string piper_config;
    std::string piper_espeak_data;
    std::string system_prompt_path;
    std::string single_in;
    std::string single_out;
};

bool parse_args(int argc, char** argv, Args& a) {
    a.piper_espeak_data = SPIKE_E2E_ESPEAKNG_DATA_DIR;

    auto need = [&](int i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", flag);
            return nullptr;
        }
        return argv[i + 1];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        if      (f == "--whisper-model")     { auto v = need(i, "--whisper-model");     if (!v) return false; a.whisper_model      = v; ++i; }
        else if (f == "--llama-model")       { auto v = need(i, "--llama-model");       if (!v) return false; a.llama_model        = v; ++i; }
        else if (f == "--piper-voice")       { auto v = need(i, "--piper-voice");       if (!v) return false; a.piper_voice        = v; ++i; }
        else if (f == "--piper-config")      { auto v = need(i, "--piper-config");      if (!v) return false; a.piper_config       = v; ++i; }
        else if (f == "--piper-espeak-data") { auto v = need(i, "--piper-espeak-data"); if (!v) return false; a.piper_espeak_data  = v; ++i; }
        else if (f == "--system-prompt")     { auto v = need(i, "--system-prompt");     if (!v) return false; a.system_prompt_path = v; ++i; }
        else if (f == "--single") {
            if (i + 2 >= argc) {
                std::fprintf(stderr, "--single needs <in.wav> <out.wav>\n");
                return false;
            }
            a.single_in  = argv[i + 1];
            a.single_out = argv[i + 2];
            i += 2;
        } else {
            std::fprintf(stderr, "unknown flag: %s\n", f.c_str());
            return false;
        }
    }

    if (a.whisper_model.empty() || a.llama_model.empty()
        || a.piper_voice.empty() || a.piper_config.empty()
        || a.system_prompt_path.empty()) {
        std::fprintf(stderr,
            "usage: %s --whisper-model <bin> --llama-model <gguf> "
            "--piper-voice <onnx> --piper-config <json> "
            "[--piper-espeak-data <dir>] --system-prompt <txt> "
            "[--single <in.wav> <out.wav>]\n"
            "\n"
            "If --single is not given, reads `<in.wav> <out.wav>` pairs from "
            "stdin until EOF.\n",
            argv[0]);
        return false;
    }
    return true;
}

struct StageTimes {
    double stt_ms      = 0.0;
    double llm_ms      = 0.0;
    double tts_ms      = 0.0;
    double wav_io_ms   = 0.0;  // WAV read + write combined
    double total_ms    = 0.0;
    int    samples_in  = 0;
    int    samples_out = 0;
    double audio_in_s  = 0.0;
    double audio_out_s = 0.0;
};

bool run_one(const std::string&            in_path,
             const std::string&            out_path,
             const std::string&            system_prompt,
             spike_e2e::ISpeechToText&     stt,
             spike_e2e::ILanguageModel&    llm,
             spike_e2e::ITextToSpeech&     tts,
             StageTimes&                   times,
             std::string&                  transcript_out,
             std::string&                  reply_out) {
    const auto t_total_start = Clock::now();

    // --- read pilot WAV ---------------------------------------------------
    const auto t_wav_in_start = Clock::now();
    spike_e2e::WavInput wav;
    std::string err;
    if (!spike_e2e::load_wav_pcm16_mono_16k(in_path, wav, err)) {
        std::fprintf(stderr, "wav read failed: %s\n", err.c_str());
        return false;
    }
    const auto t_wav_in_end = Clock::now();
    times.samples_in = int(wav.samples.size());
    times.audio_in_s = wav.duration_s;

    // --- stage 1: STT -----------------------------------------------------
    const auto t_stt_start = Clock::now();
    transcript_out = stt.transcribe(wav.samples);
    const auto t_stt_end = Clock::now();
    times.stt_ms = elapsed_ms(t_stt_start, t_stt_end);
    if (transcript_out.empty()) {
        std::fprintf(stderr, "stt produced empty transcript\n");
        return false;
    }

    // --- stage 2: LLM -----------------------------------------------------
    const auto t_llm_start = Clock::now();
    reply_out = llm.respond(system_prompt, transcript_out);
    const auto t_llm_end = Clock::now();
    times.llm_ms = elapsed_ms(t_llm_start, t_llm_end);
    if (reply_out.empty()) {
        std::fprintf(stderr, "llm produced empty reply\n");
        return false;
    }

    // --- stage 3: TTS -----------------------------------------------------
    const auto t_tts_start = Clock::now();
    uint32_t sr = 0;
    auto pcm = tts.synthesize(reply_out, sr);
    const auto t_tts_end = Clock::now();
    times.tts_ms = elapsed_ms(t_tts_start, t_tts_end);
    if (pcm.empty() || sr == 0) {
        std::fprintf(stderr, "tts produced no audio\n");
        return false;
    }
    times.samples_out = int(pcm.size());
    times.audio_out_s = double(pcm.size()) / double(sr);

    // --- stage 4: write WAV ----------------------------------------------
    const auto t_wav_out_start = Clock::now();
    if (!spike_e2e::write_wav_pcm16_mono(out_path, pcm, sr, err)) {
        std::fprintf(stderr, "wav write failed: %s\n", err.c_str());
        return false;
    }
    const auto t_wav_out_end = Clock::now();

    times.wav_io_ms = elapsed_ms(t_wav_in_start,  t_wav_in_end)
                    + elapsed_ms(t_wav_out_start, t_wav_out_end);
    times.total_ms  = elapsed_ms(t_total_start,  t_wav_out_end);
    return true;
}

void print_stage_times(const StageTimes& t,
                       const std::string& transcript,
                       const std::string& reply) {
    std::printf("\n--- transcript (pilot) ---\n%s\n", transcript.c_str());
    std::printf("\n--- response (atc) ---\n%s\n",     reply.c_str());

    std::printf("\n--- timings ---\n");
    std::printf("stage 1: stt    : %8.1f ms  (audio %.2f s)\n", t.stt_ms,    t.audio_in_s);
    std::printf("stage 2: llm    : %8.1f ms\n",                 t.llm_ms);
    std::printf("stage 3: tts    : %8.1f ms  (audio %.2f s)\n", t.tts_ms,    t.audio_out_s);
    std::printf("stage 4: wav io : %8.1f ms\n",                 t.wav_io_ms);
    std::printf("total wall      : %8.1f ms  %s\n",
                t.total_ms,
                t.total_ms < 3000.0 ? "(< 3 s target: PASS)" : "(< 3 s target: FAIL)");
    std::printf("peak rss        : %8.1f MB\n", peak_rss_mb());
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 1;

    const std::string system_prompt = read_file(args.system_prompt_path);
    if (system_prompt.empty()) {
        std::fprintf(stderr, "system prompt empty or unreadable: %s\n",
                     args.system_prompt_path.c_str());
        return 2;
    }

    // --- model loads (once) -----------------------------------------------
    std::printf("loading models...\n");
    spike_e2e::WhisperStt stt;
    spike_e2e::LlamaLm    llm;
    spike_e2e::PiperTts   tts;

    const auto t_stt_load_start = Clock::now();
    if (!stt.open(args.whisper_model)) {
        std::fprintf(stderr, "failed to load whisper model: %s\n",
                     args.whisper_model.c_str());
        return 3;
    }
    const auto t_stt_load_end = Clock::now();

    const auto t_llm_load_start = Clock::now();
    if (!llm.open(args.llama_model)) {
        std::fprintf(stderr, "failed to load llama model: %s\n",
                     args.llama_model.c_str());
        return 4;
    }
    const auto t_llm_load_end = Clock::now();

    const auto t_tts_load_start = Clock::now();
    if (!tts.open(args.piper_voice, args.piper_config, args.piper_espeak_data)) {
        std::fprintf(stderr, "failed to load piper voice: %s\n",
                     args.piper_voice.c_str());
        return 5;
    }
    const auto t_tts_load_end = Clock::now();

    std::printf("\n--- model load (cold, once) ---\n");
    std::printf("stt (whisper)   : %8.1f ms\n", elapsed_ms(t_stt_load_start, t_stt_load_end));
    std::printf("llm (llama)     : %8.1f ms\n", elapsed_ms(t_llm_load_start, t_llm_load_end));
    std::printf("tts (piper)     : %8.1f ms\n", elapsed_ms(t_tts_load_start, t_tts_load_end));
    std::printf("peak rss        : %8.1f MB\n", peak_rss_mb());
    std::fflush(stdout);

    // --- single shot or stdin loop ---------------------------------------
    if (!args.single_in.empty()) {
        StageTimes  times;
        std::string transcript;
        std::string reply;
        if (!run_one(args.single_in, args.single_out, system_prompt,
                     stt, llm, tts, times, transcript, reply)) {
            return 6;
        }
        print_stage_times(times, transcript, reply);
        return 0;
    }

    std::printf("\n--- stdin loop: enter `<in.wav> <out.wav>` per line, EOF to quit ---\n");
    std::fflush(stdout);

    std::string line;
    int run_idx = 0;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string in_path, out_path;
        if (!(iss >> in_path >> out_path)) {
            std::fprintf(stderr, "expected `<in.wav> <out.wav>`, got: %s\n", line.c_str());
            continue;
        }

        ++run_idx;
        std::printf("\n=== run %d ===\nin:  %s\nout: %s\n",
                    run_idx, in_path.c_str(), out_path.c_str());

        StageTimes  times;
        std::string transcript;
        std::string reply;
        if (run_one(in_path, out_path, system_prompt,
                    stt, llm, tts, times, transcript, reply)) {
            print_stage_times(times, transcript, reply);
        }
        std::fflush(stdout);
    }
    return 0;
}
