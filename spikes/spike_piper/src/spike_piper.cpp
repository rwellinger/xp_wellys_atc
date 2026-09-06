// spike_piper — milestone 04 latency / memory probe for Piper TTS.
//
// Usage: spike_piper <voice.onnx> <voice.json> <text-or-textfile> <out.wav>
//
// Loads a Piper voice (ONNX model + JSON config), synthesizes the input text
// to a 16-bit PCM WAV at the voice's native sample rate (22.05 kHz for the
// medium voices), and prints synthesis time, real-time factor, and peak
// resident memory.
//
// Deliberately minimal: no streaming output, no resampling, no multi-speaker
// handling. The plugin's audio path will pick up resampling in milestone 06.
//
// Note: piper_create needs the path to espeak-ng-data. We bake the build-tree
// path in via SPIKE_PIPER_ESPEAKNG_DATA_DIR, but allow override via the
// PIPER_ESPEAKNG_DATA_DIR env var so the binary stays portable.

#include "piper.h"

#include <sys/resource.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock    = std::chrono::steady_clock;
using millisec = std::chrono::duration<double, std::milli>;

double elapsed_ms(Clock::time_point a, Clock::time_point b) {
    return millisec(b - a).count();
}

// Peak resident set size in MB. macOS reports ru_maxrss in bytes (Linux: KB),
// but this spike is Apple-only so we treat it as bytes.
double peak_rss_mb() {
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
    return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
}

// If `arg` names an existing regular file, return its contents; otherwise
// treat `arg` itself as the text to synthesize. This mirrors the convention
// in the milestone task.
std::string read_text_arg(const std::string& arg) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_regular_file(arg, ec)) {
        std::ifstream f(arg);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    return arg;
}

// Naive word counter: split on whitespace. Good enough for an RTF report.
size_t word_count(const std::string& s) {
    size_t n = 0;
    bool   in_word = false;
    for (char c : s) {
        const bool ws = std::isspace(static_cast<unsigned char>(c));
        if (!ws && !in_word) ++n;
        in_word = !ws;
    }
    return n;
}

// -----------------------------------------------------------------------------
// Minimal WAV writer: PCM 16-bit, mono, sample rate from the synthesizer.
// We avoid a third-party WAV lib to keep the spike's dependency surface tiny.
// -----------------------------------------------------------------------------

void write_u32_le(std::ostream& o, uint32_t v) {
    uint8_t b[4] = {
        uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF),
        uint8_t((v >> 16) & 0xFF), uint8_t((v >> 24) & 0xFF),
    };
    o.write(reinterpret_cast<const char*>(b), 4);
}

void write_u16_le(std::ostream& o, uint16_t v) {
    uint8_t b[2] = { uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF) };
    o.write(reinterpret_cast<const char*>(b), 2);
}

bool write_wav_pcm16_mono(const std::string& path,
                          const std::vector<int16_t>& samples,
                          uint32_t sample_rate,
                          std::string& err) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path + " for writing"; return false; }

    const uint32_t bytes_per_sample = 2;
    const uint32_t channels         = 1;
    const uint32_t byte_rate        = sample_rate * channels * bytes_per_sample;
    const uint32_t block_align      = channels * bytes_per_sample;
    const uint32_t data_size        = uint32_t(samples.size() * sizeof(int16_t));
    const uint32_t riff_size        = 36 + data_size;

    f.write("RIFF", 4); write_u32_le(f, riff_size); f.write("WAVE", 4);
    f.write("fmt ", 4); write_u32_le(f, 16);
    write_u16_le(f, 1);             // PCM
    write_u16_le(f, channels);
    write_u32_le(f, sample_rate);
    write_u32_le(f, byte_rate);
    write_u16_le(f, block_align);
    write_u16_le(f, 16);            // bits per sample
    f.write("data", 4); write_u32_le(f, data_size);
    f.write(reinterpret_cast<const char*>(samples.data()), data_size);
    return f.good();
}

// Convert float [-1, 1] to int16 with clipping. Piper sometimes emits samples
// slightly above 1.0 in transients; clamp instead of relying on UB on cast.
int16_t f32_to_i16(float x) {
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<int16_t>(x * 32767.0f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s <voice.onnx> <voice.json> <text-or-textfile> <out.wav>\n",
                     argv[0]);
        return 1;
    }

    const std::string model_path = argv[1];
    const std::string cfg_path   = argv[2];
    const std::string text       = read_text_arg(argv[3]);
    const std::string out_path   = argv[4];

    // Resolve espeak-ng-data directory: env override > compile-time default.
    const char* env_espeak = std::getenv("PIPER_ESPEAKNG_DATA_DIR");
    const std::string espeak_dir = env_espeak && *env_espeak
                                       ? std::string(env_espeak)
                                       : std::string(SPIKE_PIPER_ESPEAKNG_DATA_DIR);

    const auto t_total_start = Clock::now();

    // --- load voice ---------------------------------------------------------
    const auto t_load_start = Clock::now();
    piper_synthesizer* synth = piper_create(model_path.c_str(),
                                            cfg_path.c_str(),
                                            espeak_dir.c_str());
    const auto t_load_end = Clock::now();
    if (!synth) {
        std::fprintf(stderr, "piper_create failed\n");
        std::fprintf(stderr, "  model     : %s\n", model_path.c_str());
        std::fprintf(stderr, "  config    : %s\n", cfg_path.c_str());
        std::fprintf(stderr, "  espeak    : %s\n", espeak_dir.c_str());
        return 2;
    }

    std::printf("voice        : %s\n", model_path.c_str());
    std::printf("config       : %s\n", cfg_path.c_str());
    std::printf("espeak data  : %s\n", espeak_dir.c_str());
    std::printf("text         : %zu words, %zu chars\n", word_count(text), text.size());

    // --- synthesize ---------------------------------------------------------
    piper_synthesize_options opts = piper_default_synthesize_options(synth);
    // Defaults from the voice config are good for a single-speaker model.
    // length_scale, noise_scale, noise_w_scale stay at the config's values.

    const auto t_synth_start = Clock::now();
    if (piper_synthesize_start(synth, text.c_str(), &opts) != PIPER_OK) {
        std::fprintf(stderr, "piper_synthesize_start failed\n");
        piper_free(synth);
        return 3;
    }

    std::vector<int16_t> pcm;
    pcm.reserve(size_t(text.size()) * 1024);   // rough overshoot, avoids re-alloc
    uint32_t sample_rate = 0;

    piper_audio_chunk chunk{};
    int rc;
    while ((rc = piper_synthesize_next(synth, &chunk)) != PIPER_DONE) {
        if (rc != PIPER_OK) {
            std::fprintf(stderr, "piper_synthesize_next failed: %d\n", rc);
            piper_free(synth);
            return 4;
        }
        if (sample_rate == 0) sample_rate = uint32_t(chunk.sample_rate);
        const float* s = chunk.samples;
        for (size_t i = 0; i < chunk.num_samples; ++i) {
            pcm.push_back(f32_to_i16(s[i]));
        }
    }
    // Final chunk on PIPER_DONE may still carry samples.
    if (chunk.num_samples > 0) {
        if (sample_rate == 0) sample_rate = uint32_t(chunk.sample_rate);
        const float* s = chunk.samples;
        for (size_t i = 0; i < chunk.num_samples; ++i) {
            pcm.push_back(f32_to_i16(s[i]));
        }
    }
    const auto t_synth_end = Clock::now();

    if (sample_rate == 0) {
        std::fprintf(stderr, "no audio produced\n");
        piper_free(synth);
        return 5;
    }

    // --- write WAV ----------------------------------------------------------
    std::string err;
    if (!write_wav_pcm16_mono(out_path, pcm, sample_rate, err)) {
        std::fprintf(stderr, "wav write failed: %s\n", err.c_str());
        piper_free(synth);
        return 6;
    }

    const auto t_total_end = Clock::now();

    const double audio_s = double(pcm.size()) / double(sample_rate);
    const double synth_ms = elapsed_ms(t_synth_start, t_synth_end);
    const double rtf_audio_over_synth = audio_s / (synth_ms / 1000.0);

    std::printf("output       : %s (%.2f s, %u Hz, mono, 16-bit)\n",
                out_path.c_str(), audio_s, sample_rate);

    std::printf("\n--- timings ---\n");
    std::printf("voice load   : %8.1f ms\n", elapsed_ms(t_load_start, t_load_end));
    std::printf("synthesis    : %8.1f ms  (audio %.2f s → RTF %.2fx realtime)\n",
                synth_ms, audio_s, rtf_audio_over_synth);
    std::printf("wall total   : %8.1f ms\n", elapsed_ms(t_total_start, t_total_end));

    std::printf("\n--- memory ---\n");
    std::printf("peak rss     : %8.1f MB\n", peak_rss_mb());

    piper_free(synth);
    return 0;
}
