/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/openai_tts.hpp"

#include "core/logging.hpp"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <utility>

namespace backends {

namespace {
constexpr const char *kBackendTag = "TTS-OPENAI";

bool is_valid_openai_voice(const std::string &v) {
  return v == "alloy" || v == "echo" || v == "fable" || v == "onyx" ||
         v == "nova" || v == "shimmer";
}

size_t write_to_vec(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *buf = static_cast<std::vector<uint8_t> *>(userdata);
  const size_t bytes = size * nmemb;
  buf->insert(buf->end(), reinterpret_cast<uint8_t *>(ptr),
              reinterpret_cast<uint8_t *>(ptr) + bytes);
  return bytes;
}
} // namespace

OpenAiTts::OpenAiTts(std::string api_key, std::string model,
                     std::string base_url)
    : api_key_(std::move(api_key)), model_(std::move(model)),
      base_url_(std::move(base_url)) {}

bool OpenAiTts::load_voice(const std::string &voice_id, const std::string &,
                           const std::string &) {
  if (!is_valid_openai_voice(voice_id)) {
    logging::error("[%s] Unknown OpenAI voice id: %s", kBackendTag,
                   voice_id.c_str());
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_voices_.insert(voice_id);
  return true;
}

void OpenAiTts::unload_voice(const std::string &voice_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_voices_.erase(voice_id);
}

bool OpenAiTts::has_voice(const std::string &voice_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_voices_.count(voice_id) > 0;
}

std::string OpenAiTts::default_voice_for(model_manifest::VoiceRole role) const {
  using R = model_manifest::VoiceRole;
  switch (role) {
  case R::Atis:
    return "onyx";
  case R::Tower:
    return "echo";
  case R::Ground:
    return "alloy";
  case R::Unicom:
    return "echo";
  }
  return "onyx";
}

std::vector<int16_t> OpenAiTts::synthesize(const std::string &voice_id,
                                           const std::string &text,
                                           float length_scale,
                                           uint32_t &sample_rate_hz,
                                           TtsFailure &out_failure) {
  sample_rate_hz = 0;
  // Default to Transient: every early failure return is retryable unless
  // the moderation path below upgrades it to ContentBlocked.
  out_failure = TtsFailure::Transient;
  if (api_key_.empty()) {
    logging::error("[%s] No API key configured", kBackendTag);
    return {};
  }
  if (!is_valid_openai_voice(voice_id)) {
    logging::error("[%s] Unknown OpenAI voice id: %s", kBackendTag,
                   voice_id.c_str());
    return {};
  }
  if (text.empty())
    return {};

  // Piper's length_scale > 1 means "slower". OpenAI's speed > 1 means
  // "faster". Invert and clamp to OpenAI's accepted range [0.25, 4.0].
  const float raw_speed = (length_scale > 0.0f) ? (1.0f / length_scale) : 1.0f;
  const float speed = std::max(0.25f, std::min(4.0f, raw_speed));

  const std::string key_tail = openai_common::last4(api_key_);
  logging::info("[%s] POST /v1/audio/speech, voice %s, %zu chars, speed %.2f, "
                "key sk-...%s",
                kBackendTag, voice_id.c_str(), text.size(), speed,
                key_tail.c_str());

  nlohmann::json body = {
      {"model", model_},          {"input", text},
      {"voice", voice_id},        {"speed", speed},
      {"response_format", "wav"},
  };
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
    // OpenAI rejects disallowed content with an HTTP 400 whose body names
    // a content-policy / moderation violation. That is deterministic, so
    // flag it non-retryable for the session's fail-fast path (issue #62).
    if (http_code == 400 && (err.find("content_policy") != std::string::npos ||
                             err.find("moderation") != std::string::npos ||
                             err.find("invalid_prompt") != std::string::npos))
      out_failure = TtsFailure::ContentBlocked;
    return {};
  }

  std::vector<int16_t> pcm =
      openai_common::wav_to_pcm_int16(wav_bytes, sample_rate_hz);
  if (pcm.empty() || sample_rate_hz == 0) {
    logging::error("[%s] WAV decode failed (%zu bytes received)", kBackendTag,
                   wav_bytes.size());
    return {};
  }
  out_failure = TtsFailure::None;
  return pcm;
}

} // namespace backends
