/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/openai_stt.hpp"

#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <curl/curl.h>
#include <json.hpp>

#include <utility>

namespace backends {

namespace {
constexpr const char *kBackendTag = "STT-OPENAI";

size_t write_to_string(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *response = static_cast<std::string *>(userdata);
  const size_t bytes = size * nmemb;
  response->append(ptr, bytes);
  return bytes;
}
} // namespace

OpenAiStt::OpenAiStt(std::string api_key, std::string model,
                     std::string base_url)
    : api_key_(std::move(api_key)), model_(std::move(model)),
      base_url_(std::move(base_url)) {}

std::string OpenAiStt::transcribe(const std::vector<float> &pcm_16k_mono,
                                  const std::string &airport_context) {
  if (api_key_.empty()) {
    logging::error("[%s] No API key configured", kBackendTag);
    return {};
  }
  if (pcm_16k_mono.empty())
    return {};

  // Resolve the language per request from settings rather than freezing
  // it at construction time. Lets the user flip the ATC profile (and
  // thereby backend_language()) at runtime without reloading the cloud
  // backends. Whisper API accepts any ISO-639-1 code and falls back to
  // auto-detect on empty input.
  std::string language = settings::backend_language();
  if (language.empty())
    language = "en";

  std::vector<uint8_t> wav = openai_common::pcm_float32_to_wav(pcm_16k_mono);
  const std::string key_tail = openai_common::last4(api_key_);
  logging::info("[%s] POST /v1/audio/transcriptions, %zu samples, model %s, "
                "lang=%s, key sk-...%s",
                kBackendTag, pcm_16k_mono.size(), model_.c_str(),
                language.c_str(), key_tail.c_str());

  CURL *curl = curl_easy_init();
  if (!curl) {
    logging::error("[%s] curl_easy_init failed", kBackendTag);
    return {};
  }

  curl_mime *mime = curl_mime_init(curl);

  curl_mimepart *part = curl_mime_addpart(mime);
  curl_mime_name(part, "file");
  curl_mime_data(part, reinterpret_cast<const char *>(wav.data()), wav.size());
  curl_mime_filename(part, "audio.wav");
  curl_mime_type(part, "audio/wav");

  part = curl_mime_addpart(mime);
  curl_mime_name(part, "model");
  curl_mime_data(part, model_.c_str(), CURL_ZERO_TERMINATED);

  part = curl_mime_addpart(mime);
  curl_mime_name(part, "language");
  curl_mime_data(part, language.c_str(), CURL_ZERO_TERMINATED);

  part = curl_mime_addpart(mime);
  curl_mime_name(part, "response_format");
  curl_mime_data(part, "json", CURL_ZERO_TERMINATED);

  if (!airport_context.empty()) {
    const std::string prompt = "Airport: " + airport_context;
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "prompt");
    curl_mime_data(part, prompt.c_str(), CURL_ZERO_TERMINATED);
  }

  const std::string url = base_url_ + "/v1/audio/transcriptions";
  const std::string auth = "Authorization: Bearer " + api_key_;
  struct curl_slist *headers = curl_slist_append(nullptr, auth.c_str());

  std::string response_body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_mime_free(mime);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    logging::error("[%s] curl error: %s", kBackendTag, curl_easy_strerror(rc));
    return {};
  }
  if (http_code != 200) {
    logging::error("[%s] HTTP %ld: %s", kBackendTag, http_code,
                   response_body.c_str());
    return {};
  }

  try {
    const auto j = nlohmann::json::parse(response_body);
    return j.value("text", std::string{});
  } catch (const std::exception &e) {
    logging::error("[%s] JSON parse error: %s", kBackendTag, e.what());
    return {};
  }
}

} // namespace backends
