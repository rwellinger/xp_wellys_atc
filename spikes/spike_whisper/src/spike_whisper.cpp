// spike_whisper — milestone 02 latency / memory probe for whisper.cpp.
//
// Usage: spike_whisper <model.bin> <audio.wav>
//
// Loads a GGUF whisper model, transcribes a 16 kHz / mono / 16-bit PCM WAV,
// prints the transcript plus a timing breakdown (model load, inference,
// wall clock total) and peak resident memory.
//
// Deliberately minimal: no resampling, no streaming, no VAD. The fixture is
// expected to already match whisper's native 16 kHz mono PCM format.

#include "whisper.h"

#include <sys/resource.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
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

// -----------------------------------------------------------------------------
// Minimal WAV reader: PCM 16-bit, 16 kHz, mono only. Anything else → error.
// We avoid a third-party WAV lib to keep the spike's dependency surface tiny.
// -----------------------------------------------------------------------------

struct WavData {
    std::vector<float> samples;  // normalised to [-1, 1]
    uint32_t           sample_rate;
    uint16_t           channels;
    double             duration_s;
};

bool read_u32_le(std::ifstream& f, uint32_t& out) {
    uint8_t b[4];
    if (!f.read(reinterpret_cast<char*>(b), 4)) return false;
    out = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}

bool read_u16_le(std::ifstream& f, uint16_t& out) {
    uint8_t b[2];
    if (!f.read(reinterpret_cast<char*>(b), 2)) return false;
    out = uint16_t(b[0]) | (uint16_t(b[1]) << 8);
    return true;
}

bool load_wav_pcm16_mono_16k(const std::string& path, WavData& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }

    char riff[4]; f.read(riff, 4);
    uint32_t riff_size; read_u32_le(f, riff_size);
    char wave[4]; f.read(wave, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file"; return false;
    }

    uint16_t fmt_tag = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    std::vector<uint8_t> pcm_bytes;
    bool got_fmt = false, got_data = false;

    while (f && !(got_fmt && got_data)) {
        char     id[4];
        uint32_t size = 0;
        if (!f.read(id, 4)) break;
        if (!read_u32_le(f, size)) break;

        if (std::memcmp(id, "fmt ", 4) == 0) {
            uint16_t block_align = 0;
            uint32_t byte_rate   = 0;
            read_u16_le(f, fmt_tag);
            read_u16_le(f, channels);
            read_u32_le(f, sample_rate);
            read_u32_le(f, byte_rate);     // ignored
            read_u16_le(f, block_align);   // ignored
            read_u16_le(f, bits);
            (void)block_align;
            (void)byte_rate;
            // skip any extra fmt bytes (for non-PCM formats)
            if (size > 16) f.seekg(size - 16, std::ios::cur);
            got_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            pcm_bytes.resize(size);
            f.read(reinterpret_cast<char*>(pcm_bytes.data()), size);
            got_data = true;
        } else {
            f.seekg(size, std::ios::cur);  // skip unknown chunk
        }
    }

    if (!got_fmt || !got_data)        { err = "fmt or data chunk missing";      return false; }
    if (fmt_tag != 1)                 { err = "only PCM (fmt_tag=1) supported"; return false; }
    if (channels != 1)                { err = "only mono supported";            return false; }
    if (sample_rate != WHISPER_SAMPLE_RATE) {
        err = "expected " + std::to_string(WHISPER_SAMPLE_RATE) + " Hz, got " + std::to_string(sample_rate);
        return false;
    }
    if (bits != 16)                   { err = "only 16-bit PCM supported";      return false; }

    const size_t n = pcm_bytes.size() / 2;
    out.samples.resize(n);
    const auto* s16 = reinterpret_cast<const int16_t*>(pcm_bytes.data());
    for (size_t i = 0; i < n; ++i) {
        out.samples[i] = static_cast<float>(s16[i]) / 32768.0f;
    }
    out.sample_rate = sample_rate;
    out.channels    = channels;
    out.duration_s  = double(n) / double(sample_rate);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <model.bin> <audio.wav>\n", argv[0]);
        return 1;
    }
    const std::string model_path = argv[1];
    const std::string audio_path = argv[2];

    const auto t_total_start = Clock::now();

    // --- load audio ---------------------------------------------------------
    WavData wav;
    std::string err;
    if (!load_wav_pcm16_mono_16k(audio_path, wav, err)) {
        std::fprintf(stderr, "audio load failed: %s\n", err.c_str());
        return 2;
    }
    std::printf("audio        : %s (%.2f s, %u Hz, %u ch)\n",
                audio_path.c_str(), wav.duration_s, wav.sample_rate, wav.channels);

    // --- load model ---------------------------------------------------------
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu    = true;   // Metal backend
    cparams.flash_attn = false;  // small model, default kernels are fine

    const auto t_load_start = Clock::now();
    whisper_context* ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    const auto t_load_end = Clock::now();
    if (!ctx) {
        std::fprintf(stderr, "model load failed: %s\n", model_path.c_str());
        return 3;
    }
    std::printf("model        : %s\n", model_path.c_str());

    // --- transcribe ---------------------------------------------------------
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language         = "en";
    wparams.translate        = false;
    wparams.no_context       = true;
    wparams.single_segment   = false;
    wparams.print_progress   = false;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.print_special    = false;
    // Half the perf cores; whisper.cpp scales sub-linearly above 4 on M1.
    const unsigned hw = std::thread::hardware_concurrency();
    wparams.n_threads = hw == 0 ? 4 : std::min(8u, std::max(2u, hw / 2));

    const auto t_inf_start = Clock::now();
    if (whisper_full(ctx, wparams, wav.samples.data(), int(wav.samples.size())) != 0) {
        std::fprintf(stderr, "whisper_full failed\n");
        whisper_free(ctx);
        return 4;
    }
    const auto t_inf_end = Clock::now();

    std::string transcript;
    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char* seg = whisper_full_get_segment_text(ctx, i);
        if (seg) transcript += seg;
    }

    const auto t_total_end = Clock::now();

    std::printf("\n--- transcript ---\n%s\n", transcript.c_str());

    std::printf("\n--- timings ---\n");
    std::printf("model load   : %8.1f ms\n", elapsed_ms(t_load_start, t_load_end));
    std::printf("inference    : %8.1f ms  (audio %.2f s → RTF %.3f)\n",
                elapsed_ms(t_inf_start, t_inf_end),
                wav.duration_s,
                elapsed_ms(t_inf_start, t_inf_end) / 1000.0 / wav.duration_s);
    std::printf("wall total   : %8.1f ms\n", elapsed_ms(t_total_start, t_total_end));
    std::printf("threads      : %d\n", wparams.n_threads);

    std::printf("\n--- memory ---\n");
    std::printf("peak rss     : %8.1f MB\n", peak_rss_mb());

    whisper_free(ctx);
    return 0;
}
