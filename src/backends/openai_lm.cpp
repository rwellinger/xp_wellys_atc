/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/openai_lm.hpp"

#include "core/logging.hpp"

#include <curl/curl.h>
#include <json.hpp>

#include <utility>

namespace backends {

namespace {
constexpr const char *kBackendTag = "LM-OPENAI";

size_t write_to_string(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *response = static_cast<std::string *>(userdata);
  const size_t bytes = size * nmemb;
  response->append(ptr, bytes);
  return bytes;
}
} // namespace

OpenAiLm::OpenAiLm(std::string api_key, std::string model, std::string base_url)
    : api_key_(std::move(api_key)), model_(std::move(model)),
      base_url_(std::move(base_url)) {}

std::string OpenAiLm::respond(const std::string &system_prompt,
                              const std::string &user_text) {
  return call(system_prompt, user_text, /*json_mode=*/false);
}

std::string OpenAiLm::respond_constrained(const std::string &system_prompt,
                                          const std::string &user_text,
                                          const std::string &grammar_gbnf) {
  // GBNF is a llama.cpp construct; OpenAI has no equivalent. The
  // caller's contract is "JSON-shaped output" — we honor that via
  // response_format=json_object and rely on the system_prompt to
  // describe the schema. The unused grammar is intentionally dropped.
  (void)grammar_gbnf;
  return call(system_prompt, user_text, /*json_mode=*/true);
}

std::string OpenAiLm::call(const std::string &system_prompt,
                           const std::string &user_text, bool json_mode) {
  if (api_key_.empty()) {
    logging::error("[%s] No API key configured", kBackendTag);
    return {};
  }

  const std::string key_tail = openai_common::last4(api_key_);
  logging::info(
      "[%s] POST /v1/chat/completions, model %s, json_mode=%s, key sk-...%s",
      kBackendTag, model_.c_str(), json_mode ? "true" : "false",
      key_tail.c_str());

  nlohmann::json body = {
      {"model", model_},
      {"messages",
       {{{"role", "system"}, {"content", system_prompt}},
        {{"role", "user"}, {"content", user_text}}}},
      {"max_tokens", 150},
      {"temperature", json_mode ? 0.0 : 0.7},
  };
  if (json_mode)
    body["response_format"] = {{"type", "json_object"}};
  const std::string body_str = body.dump();

  CURL *curl = curl_easy_init();
  if (!curl) {
    logging::error("[%s] curl_easy_init failed", kBackendTag);
    return {};
  }

  const std::string url = base_url_ + "/v1/chat/completions";
  const std::string auth = "Authorization: Bearer " + api_key_;
  struct curl_slist *headers = curl_slist_append(nullptr, auth.c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");

  std::string response_body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
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
    logging::error("[%s] HTTP %ld: %s", kBackendTag, http_code,
                   response_body.c_str());
    return {};
  }

  try {
    const auto j = nlohmann::json::parse(response_body);
    if (!j.contains("choices") || !j["choices"].is_array() ||
        j["choices"].empty())
      return {};
    return j["choices"][0]["message"].value("content", std::string{});
  } catch (const std::exception &e) {
    logging::error("[%s] JSON parse error: %s", kBackendTag, e.what());
    return {};
  }
}

} // namespace backends
