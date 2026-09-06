#include "wav.hpp"

#include <cstring>
#include <fstream>

namespace spike_e2e {

namespace {

bool read_u32_le(std::ifstream& f, uint32_t& out) {
    uint8_t b[4];
    if (!f.read(reinterpret_cast<char*>(b), 4)) return false;
    out = uint32_t(b[0]) | (uint32_t(b[1]) << 8)
        | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}

bool read_u16_le(std::ifstream& f, uint16_t& out) {
    uint8_t b[2];
    if (!f.read(reinterpret_cast<char*>(b), 2)) return false;
    out = uint16_t(b[0]) | (uint16_t(b[1]) << 8);
    return true;
}

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

constexpr uint32_t kWhisperSampleRate = 16000;

}  // namespace

bool load_wav_pcm16_mono_16k(const std::string& path,
                             WavInput&          out,
                             std::string&       err) {
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
            read_u32_le(f, byte_rate);
            read_u16_le(f, block_align);
            read_u16_le(f, bits);
            (void)block_align;
            (void)byte_rate;
            if (size > 16) f.seekg(size - 16, std::ios::cur);
            got_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            pcm_bytes.resize(size);
            f.read(reinterpret_cast<char*>(pcm_bytes.data()), size);
            got_data = true;
        } else {
            f.seekg(size, std::ios::cur);
        }
    }

    if (!got_fmt || !got_data)        { err = "fmt or data chunk missing";      return false; }
    if (fmt_tag != 1)                 { err = "only PCM (fmt_tag=1) supported"; return false; }
    if (channels != 1)                { err = "only mono supported";            return false; }
    if (sample_rate != kWhisperSampleRate) {
        err = "expected " + std::to_string(kWhisperSampleRate)
            + " Hz, got " + std::to_string(sample_rate);
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

bool write_wav_pcm16_mono(const std::string&          path,
                          const std::vector<int16_t>& samples,
                          uint32_t                    sample_rate,
                          std::string&                err) {
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
    write_u16_le(f, 1);
    write_u16_le(f, channels);
    write_u32_le(f, sample_rate);
    write_u32_le(f, byte_rate);
    write_u16_le(f, block_align);
    write_u16_le(f, 16);
    f.write("data", 4); write_u32_le(f, data_size);
    f.write(reinterpret_cast<const char*>(samples.data()), data_size);
    return f.good();
}

}  // namespace spike_e2e
