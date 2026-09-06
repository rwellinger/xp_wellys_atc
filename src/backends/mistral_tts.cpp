/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/mistral_tts.hpp"

#include "backends/openai_common.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <curl/curl.h>
#include <json.hpp>

#include <cstring>
#include <utility>

namespace backends {

namespace {
constexpr const char *kBackendTag = "TTS-MISTRAL";

size_t write_to_vec(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *buf = static_cast<std::vector<uint8_t> *>(userdata);
  const size_t bytes = size * nmemb;
  buf->insert(buf->end(), reinterpret_cast<uint8_t *>(ptr),
              reinterpret_cast<uint8_t *>(ptr) + bytes);
  return bytes;
}

// Standard RFC-4648 base64 decode. Returns an empty vector on
// unrecoverable input. Skips whitespace and padding so JSON-escaped
// payloads (`\n`, `\r`) round-trip cleanly.
int b64_value(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

std::vector<uint8_t> base64_decode(const std::string &s) {
  std::vector<uint8_t> out;
  out.reserve(s.size() * 3 / 4);
  uint32_t buf = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
      continue;
    int v = b64_value(c);
    if (v < 0)
      continue;
    buf = (buf << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buf >> bits) & 0xffu));
    }
  }
  return out;
}
} // namespace

MistralTts::MistralTts(std::string api_key, std::string model,
                       std::string base_url)
    : api_key_(std::move(api_key)), model_(std::move(model)),
      base_url_(std::move(base_url)) {}

bool MistralTts::load_voice(const std::string &voice_id, const std::string &,
                            const std::string &) {
  // Mistral has no client-side voice whitelist — accept any non-empty
  // id and remember it for has_voice() lookups. An empty id is a
  // configuration error (user did not paste a Voxtral voice id into
  // Settings yet); surface that as a load failure rather than caching
  // an empty entry that would later confuse synthesize().
  if (voice_id.empty()) {
    logging::info("[%s] load_voice called with empty voice id (set one in "
                  "Settings to enable TTS)",
                  kBackendTag);
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_voices_.insert(voice_id);
  return true;
}

void MistralTts::unload_voice(const std::string &voice_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_voices_.erase(voice_id);
}

bool MistralTts::has_voice(const std::string &voice_id) const {
  if (voice_id.empty())
    return false;
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_voices_.count(voice_id) > 0;
}

std::string
MistralTts::default_voice_for(model_manifest::VoiceRole role) const {
  using R = model_manifest::VoiceRole;
  switch (role) {
  case R::Atis:
    return settings::mistral_tts_voice_atis();
  case R::Tower:
    return settings::mistral_tts_voice_tower();
  case R::Ground:
    return settings::mistral_tts_voice_ground();
  case R::Unicom:
    return settings::mistral_tts_voice_tower();
  }
  return {};
}

std::vector<int16_t> MistralTts::synthesize(const std::string &voice_id,
                                            const std::string &text,
                                            float length_scale,
                                            uint32_t &sample_rate_hz,
                                            TtsFailure &out_failure) {
  // length_scale has no direct equivalent in Voxtral TTS — ATIS will
  // still synthesize, just at the model's native rate. We log the
  // value so it shows up once in Log.txt and the user understands
  // what was dropped.
  (void)length_scale;

  sample_rate_hz = 0;
  // Default to Transient: every early failure return below is retryable
  // unless the guardrail path (HTTP 403) upgrades it to ContentBlocked.
  out_failure = TtsFailure::Transient;
  if (api_key_.empty()) {
    logging::error("[%s] No API key configured", kBackendTag);
    return {};
  }
  if (voice_id.empty()) {
    logging::error("[%s] Empty voice id — open Settings and paste a Voxtral "
                   "voice id",
                   kBackendTag);
    return {};
  }
  if (text.empty())
    return {};

  const std::string key_tail = openai_common::last4(api_key_);
  logging::info("[%s] POST /v1/audio/speech, voice %s, %zu chars, "
                "key ...%s",
                kBackendTag, voice_id.c_str(), text.size(), key_tail.c_str());

  nlohmann::json body = {
      {"input", text},
      {"voice_id", voice_id},
      {"response_format", "wav"},
  };
  // Only include `model` when the user actually set one — Voxtral TTS
  // picks a sensible server-side default for the empty case, which
  // keeps the Settings UI tidy (one fewer string to paste).
  if (!model_.empty())
    body["model"] = model_;
  const std::string body_str = body.dump();

  CURL *curl = curl_easy_init();
  if (!curl) {
    logging::error("[%s] curl_easy_init failed", kBackendTag);
    return {};
  }

  const std::string url = base_url_ + "/v1/audio/speech";
  const std::string auth = "Authorization: Bearer " + api_key_;
  struct curl_slist *headers = curl_slist_append(nullptr, auth.c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");

  std::vector<uint8_t> wav_bytes;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vec);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wav_bytes);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    logging::error("[%s] curl error: %s", kBackendTag, curl_easy_strerror(rc));
    return {};
  }
  if (http_code != 200) {
    const std::string err(wav_bytes.begin(), wav_bytes.end());
    logging::error("[%s] HTTP %ld: %s", kBackendTag, http_code, err.c_str());
    // A 403 is Mistral's content-moderation guardrail rejecting the text
    // (issue #62). It is deterministic — retrying the identical input can
    // never succeed — so flag it non-retryable for the session's fail-fast
    // path.
    if (http_code == 403)
      out_failure = TtsFailure::ContentBlocked;
    return {};
  }

  // Mistral wraps the audio in a JSON envelope:
  //   {"audio_data": "<base64>", "sample_rate": 24000, "format": "wav"}
  // — independent of the response_format request body field, which
  // controls the encoding INSIDE the base64 payload. Detect via the
  // leading '{' byte; otherwise treat the body as raw container
  // bytes (defensive — in case Mistral ever ships the raw audio).
  std::vector<uint8_t> audio_bytes = std::move(wav_bytes);
  uint32_t json_sample_rate = 0;
  std::string json_format;

  if (!audio_bytes.empty() && audio_bytes.front() == '{') {
    try {
      const auto j =
          nlohmann::json::parse(audio_bytes.begin(), audio_bytes.end());
      std::string b64 = j.value("audio_data", std::string{});
      if (b64.empty())
        b64 = j.value("audio", std::string{});
      if (b64.empty()) {
        logging::error("[%s] JSON has no audio_data field", kBackendTag);
        return {};
      }
      json_sample_rate = j.value("sample_rate", 0u);
      json_format = j.value("format", std::string{});
      audio_bytes = base64_decode(b64);
      if (json_sample_rate == 0 && json_format.empty()) {
        logging::info("[%s] unwrapped JSON: %zu bytes (envelope had no "
                      "format/sample_rate metadata, decoding WAV container)",
                      kBackendTag, audio_bytes.size());
      } else {
        logging::info("[%s] unwrapped JSON: %zu bytes, format=%s, "
                      "sample_rate=%u",
                      kBackendTag, audio_bytes.size(),
                      json_format.empty() ? "?" : json_format.c_str(),
                      static_cast<unsigned>(json_sample_rate));
      }
    } catch (const std::exception &e) {
      logging::error("[%s] JSON parse error: %s", kBackendTag, e.what());
      return {};
    }
  }

  // If the JSON declared "pcm" with an explicit sample_rate, the
  // payload is raw little-endian int16 mono samples — skip the WAV
  // container parser entirely.
  if (json_format == "pcm" && json_sample_rate > 0) {
    if (audio_bytes.size() % 2 != 0) {
      logging::error("[%s] PCM byte count not even: %zu", kBackendTag,
                     audio_bytes.size());
      return {};
    }
    std::vector<int16_t> pcm(audio_bytes.size() / 2);
    std::memcpy(pcm.data(), audio_bytes.data(), audio_bytes.size());
    sample_rate_hz = json_sample_rate;
    out_failure = TtsFailure::None;
    return pcm;
  }

  std::vector<int16_t> pcm =
      openai_common::wav_to_pcm_int16(audio_bytes, sample_rate_hz);
  if (pcm.empty() || sample_rate_hz == 0) {
    logging::error("[%s] decode failed (%zu bytes after unwrap)", kBackendTag,
                   audio_bytes.size());
    return {};
  }
  // Plausibility guard: catch Mistral silently changing the inner
  // container's sample rate. Today the envelope omits sample_rate and
  // we trust the WAV header — out-of-range values would otherwise play
  // back at the wrong pitch with no log breadcrumb.
  if (sample_rate_hz < 8000 || sample_rate_hz > 48000) {
    logging::error("[%s] implausible sample_rate %u Hz from WAV container",
                   kBackendTag, static_cast<unsigned>(sample_rate_hz));
    return {};
  }
  out_failure = TtsFailure::None;
  return pcm;
}

} // namespace backends
