/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/openai_common.hpp"

#include "core/logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace backends::openai_common {

std::string last4(const std::string &api_key) {
  if (api_key.size() <= 4)
    return api_key;
  return api_key.substr(api_key.size() - 4);
}

namespace {

// Write little-endian primitives into a byte vector.
void write_u32_le(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}
void write_u16_le(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

uint32_t read_u32_le(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t read_u16_le(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

} // namespace

std::vector<uint8_t>
pcm_float32_to_wav(const std::vector<float> &pcm_16k_mono) {
  constexpr uint32_t sample_rate = 16000;
  constexpr uint16_t channels = 1;
  constexpr uint16_t bits_per_sample = 16;
  const uint32_t data_bytes =
      static_cast<uint32_t>(pcm_16k_mono.size() * sizeof(int16_t));

  std::vector<uint8_t> wav;
  wav.reserve(44 + data_bytes);

  // RIFF header
  wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
  write_u32_le(wav, 36 + data_bytes); // file size - 8
  wav.insert(wav.end(), {'W', 'A', 'V', 'E'});

  // fmt chunk
  wav.insert(wav.end(), {'f', 'm', 't', ' '});
  write_u32_le(wav, 16); // fmt chunk size
  write_u16_le(wav, 1);  // PCM format
  write_u16_le(wav, channels);
  write_u32_le(wav, sample_rate);
  write_u32_le(wav, sample_rate * channels * bits_per_sample / 8); // byte rate
  write_u16_le(wav, channels * bits_per_sample / 8); // block align
  write_u16_le(wav, bits_per_sample);

  // data chunk
  wav.insert(wav.end(), {'d', 'a', 't', 'a'});
  write_u32_le(wav, data_bytes);

  // float [-1, 1] -> int16 with clipping
  for (float f : pcm_16k_mono) {
    const float clamped = std::max(-1.0f, std::min(1.0f, f));
    const int32_t s = static_cast<int32_t>(std::lround(clamped * 32767.0f));
    const int16_t i16 = static_cast<int16_t>(
        std::max<int32_t>(-32768, std::min<int32_t>(32767, s)));
    wav.push_back(static_cast<uint8_t>(i16 & 0xff));
    wav.push_back(static_cast<uint8_t>((i16 >> 8) & 0xff));
  }
  return wav;
}

namespace {

// Clamp a 32-bit signed integer into the int16 range without UB on
// overflow. Used by every PCM/Float -> int16 conversion path below.
inline int16_t clamp_to_i16(int32_t v) {
  if (v > 32767)
    return 32767;
  if (v < -32768)
    return -32768;
  return static_cast<int16_t>(v);
}

// Convert a single multi-byte PCM sample at `p` (channels interleaved)
// to a mono int16, mixing stereo channels by averaging. Supports
// integer PCM at 8 / 16 / 24 / 32 bits.
inline int16_t decode_pcm_sample(const uint8_t *p, uint16_t bits,
                                 uint16_t channels) {
  const size_t bytes_per_sample = bits / 8u;
  int64_t acc = 0;
  for (uint16_t c = 0; c < channels; ++c) {
    const uint8_t *sp = p + static_cast<size_t>(c) * bytes_per_sample;
    int32_t s = 0;
    switch (bits) {
    case 8:
      // Unsigned PCM 8 — center at 128.
      s = (static_cast<int32_t>(sp[0]) - 128) << 8;
      break;
    case 16:
      s = static_cast<int16_t>(static_cast<uint16_t>(sp[0]) |
                               (static_cast<uint16_t>(sp[1]) << 8));
      break;
    case 24: {
      // Signed 24-bit little-endian; promote to int32 with sign-extend
      // then truncate the bottom byte for int16 output.
      uint32_t u = static_cast<uint32_t>(sp[0]) |
                   (static_cast<uint32_t>(sp[1]) << 8) |
                   (static_cast<uint32_t>(sp[2]) << 16);
      if (u & 0x800000u)
        u |= 0xff000000u;
      s = static_cast<int32_t>(u) >> 8;
      break;
    }
    case 32: {
      uint32_t u = static_cast<uint32_t>(sp[0]) |
                   (static_cast<uint32_t>(sp[1]) << 8) |
                   (static_cast<uint32_t>(sp[2]) << 16) |
                   (static_cast<uint32_t>(sp[3]) << 24);
      s = static_cast<int32_t>(u) >> 16;
      break;
    }
    default:
      return 0;
    }
    acc += s;
  }
  return clamp_to_i16(static_cast<int32_t>(acc / channels));
}

// Convert a single IEEE-754 float sample (32-bit) at `p` to int16,
// downmixing channels by averaging in the float domain to avoid
// clipping before the final clamp.
inline int16_t decode_float32_sample(const uint8_t *p, uint16_t channels) {
  float acc = 0.0f;
  for (uint16_t c = 0; c < channels; ++c) {
    uint32_t u = static_cast<uint32_t>(p[c * 4 + 0]) |
                 (static_cast<uint32_t>(p[c * 4 + 1]) << 8) |
                 (static_cast<uint32_t>(p[c * 4 + 2]) << 16) |
                 (static_cast<uint32_t>(p[c * 4 + 3]) << 24);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    acc += f;
  }
  const float avg = acc / static_cast<float>(channels);
  const float scaled = std::max(-1.0f, std::min(1.0f, avg)) * 32767.0f;
  return static_cast<int16_t>(std::lround(scaled));
}

} // namespace

std::vector<int16_t> wav_to_pcm_int16(const std::vector<uint8_t> &wav,
                                      uint32_t &sample_rate_hz) {
  sample_rate_hz = 0;
  if (wav.size() < 44)
    return {};
  if (std::memcmp(wav.data(), "RIFF", 4) != 0 ||
      std::memcmp(wav.data() + 8, "WAVE", 4) != 0) {
    // Dump the first 16 bytes so we can identify the actual payload
    // — providers occasionally return raw PCM, MP3 (49 44 33 / ff fb),
    // Ogg (4f 67 67 53), or FLAC (66 4c 61 43) despite a "wav"
    // response_format request.
    char hex[80] = {};
    size_t n = wav.size() < 16 ? wav.size() : 16;
    for (size_t i = 0; i < n; ++i) {
      std::snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02x ",
                    static_cast<unsigned>(wav[i]));
    }
    logging::error("[wav_decode] not a RIFF/WAVE container (%zu bytes, "
                   "first 16 bytes: %s)",
                   wav.size(), hex);
    return {};
  }

  // Walk sub-chunks until we find fmt + data. Standard PCM WAVs put
  // both chunks back-to-back, but the spec allows extra chunks (LIST,
  // fact, etc.) before data, and many TTS providers ship a fact
  // chunk for non-PCM formats. We don't assume canonical layout.
  size_t pos = 12;
  uint16_t format = 0;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  uint16_t actual_format = 0; // unwrapped from WAVE_FORMAT_EXTENSIBLE
  bool fmt_ok = false;

  while (pos + 8 <= wav.size()) {
    char id[5] = {static_cast<char>(wav[pos]), static_cast<char>(wav[pos + 1]),
                  static_cast<char>(wav[pos + 2]),
                  static_cast<char>(wav[pos + 3]), 0};
    uint32_t chunk_size = read_u32_le(wav.data() + pos + 4);
    size_t chunk_start = pos + 8;

    if (std::memcmp(id, "fmt ", 4) == 0 && chunk_size >= 16 &&
        chunk_start + 16 <= wav.size()) {
      format = read_u16_le(wav.data() + chunk_start);
      channels = read_u16_le(wav.data() + chunk_start + 2);
      sample_rate = read_u32_le(wav.data() + chunk_start + 4);
      bits_per_sample = read_u16_le(wav.data() + chunk_start + 14);
      // WAVE_FORMAT_EXTENSIBLE (0xFFFE): the actual format sits in
      // bytes 24-25 of the fmt chunk as a SubFormat GUID, but the
      // first two bytes of that GUID match the legacy format tag —
      // good enough for the PCM (0x0001) vs. IEEE-Float (0x0003)
      // split we care about. Streaming TTS providers often wrap PCM
      // in extensible to ship a bigger fmt block with a channel mask.
      if (format == 0xFFFE && chunk_size >= 40 &&
          chunk_start + 26 <= wav.size()) {
        actual_format = read_u16_le(wav.data() + chunk_start + 24);
      } else {
        actual_format = format;
      }
      fmt_ok = true;
    } else if (std::memcmp(id, "data", 4) == 0 && fmt_ok) {
      if (chunk_start + chunk_size > wav.size())
        chunk_size = static_cast<uint32_t>(wav.size() - chunk_start);
      const bool is_pcm = (actual_format == 1) &&
                          (bits_per_sample == 8 || bits_per_sample == 16 ||
                           bits_per_sample == 24 || bits_per_sample == 32);
      const bool is_float = (actual_format == 3) && (bits_per_sample == 32);
      if ((!is_pcm && !is_float) || channels < 1 || channels > 8) {
        logging::error("[wav_decode] unsupported format=%u bits=%u channels=%u "
                       "sample_rate=%u",
                       static_cast<unsigned>(actual_format),
                       static_cast<unsigned>(bits_per_sample),
                       static_cast<unsigned>(channels),
                       static_cast<unsigned>(sample_rate));
        return {};
      }
      const uint32_t frame_size =
          static_cast<uint32_t>(channels) * (bits_per_sample / 8u);
      if (frame_size == 0)
        return {};
      const size_t frame_count = chunk_size / frame_size;
      std::vector<int16_t> out;
      out.reserve(frame_count);
      const uint8_t *base = wav.data() + chunk_start;
      for (size_t i = 0; i < frame_count; ++i) {
        const uint8_t *frame = base + i * frame_size;
        out.push_back(
            is_float ? decode_float32_sample(frame, channels)
                     : decode_pcm_sample(frame, bits_per_sample, channels));
      }
      sample_rate_hz = sample_rate;
      return out;
    }
    pos = chunk_start + chunk_size;
    // Chunks are word-aligned in the spec.
    if (chunk_size & 1u)
      ++pos;
  }
  logging::error("[wav_decode] no data chunk found after fmt_ok=%d (%zu bytes)",
                 fmt_ok ? 1 : 0, wav.size());
  return {};
}

} // namespace backends::openai_common
