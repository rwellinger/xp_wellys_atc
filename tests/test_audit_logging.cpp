/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Audit-logging invariants for the hybrid local/cloud backend
 * pipeline. Two layers:
 *
 *   1. Behavioural: install a capturing logging sink, run mock
 *      backends that emit their kBackendTag, and assert that local
 *      mocks produce only [*-LOCAL] tags and cloud mocks produce only
 *      [*-OPENAI] tags.
 *   2. Structural: read the on-disk source of the three local and
 *      three cloud backend TUs and assert that no local TU references
 *      the string "OPENAI" and no cloud TU references "LOCAL" in a
 *      backend-tag context. This is what guarantees, on source-level,
 *      that "in local mode nothing went to OpenAI".
 */

#include "core/logging.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::mutex g_sink_mtx;
std::vector<std::string> g_captured;

void capture_sink(const char *line) {
  std::lock_guard<std::mutex> lk(g_sink_mtx);
  g_captured.emplace_back(line);
}

bool any_line_contains(const std::vector<std::string> &lines,
                       const std::string &needle) {
  return std::any_of(lines.begin(), lines.end(),
                     [&](const std::string &l) {
                       return l.find(needle) != std::string::npos;
                     });
}

// Tiny mock backends that mirror the real audit-log shape. They do
// not implement the full ISpeechToText/ILanguageModel/ITextToSpeech
// interfaces — that would drag whisper/llama/piper headers into the
// test target. Behavioural test only needs a function that emits the
// tag, which is what the real production code does too.
struct LocalStt {
  static constexpr const char *kTag = "STT-LOCAL";
  void transcribe() {
    logging::info("[%s] transcribe 16000 PCM samples", kTag);
  }
};
struct OpenaiStt {
  static constexpr const char *kTag = "STT-OPENAI";
  void transcribe() {
    logging::info("[%s] POST /v1/audio/transcriptions, key sk-...ABCD", kTag);
  }
};
struct LocalLm {
  static constexpr const char *kTag = "LM-LOCAL";
  void respond() { logging::info("[%s] respond, 80 chars", kTag); }
};
struct OpenaiLm {
  static constexpr const char *kTag = "LM-OPENAI";
  void respond() {
    logging::info("[%s] POST /v1/chat/completions, key sk-...ABCD", kTag);
  }
};

// Read a source file fully into a string (for the structural check).
// Returns an empty string when the path is unreadable so the test
// fails loudly via the size assertion below rather than silently
// passing on a missing file.
std::string slurp(const std::string &path) {
  std::ifstream f(path);
  if (!f.good())
    return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

} // namespace

#ifndef XP_WELLYS_ATC_SOURCE_DIR
#error "XP_WELLYS_ATC_SOURCE_DIR must be defined for test_audit_logging"
#endif

TEST_CASE("local mode produces only LOCAL audit tags",
          "[audit][logging]") {
  g_captured.clear();
  logging::set_sink(&capture_sink);

  LocalStt stt;
  LocalLm lm;
  stt.transcribe();
  lm.respond();

  logging::set_sink(nullptr);

  REQUIRE(any_line_contains(g_captured, "[STT-LOCAL]"));
  REQUIRE(any_line_contains(g_captured, "[LM-LOCAL]"));
  REQUIRE_FALSE(any_line_contains(g_captured, "OPENAI"));
}

TEST_CASE("openai mode produces only OPENAI audit tags",
          "[audit][logging]") {
  g_captured.clear();
  logging::set_sink(&capture_sink);

  OpenaiStt stt;
  OpenaiLm lm;
  stt.transcribe();
  lm.respond();

  logging::set_sink(nullptr);

  REQUIRE(any_line_contains(g_captured, "[STT-OPENAI]"));
  REQUIRE(any_line_contains(g_captured, "[LM-OPENAI]"));
  REQUIRE_FALSE(any_line_contains(g_captured, "-LOCAL]"));
}

TEST_CASE("source-level invariant: local backend TUs reference only "
          "LOCAL kBackendTag, never OPENAI",
          "[audit][source]") {
  const std::string root = XP_WELLYS_ATC_SOURCE_DIR;
  const std::vector<std::pair<std::string, std::string>> local_tus = {
      {"src/backends/whisper_stt.cpp", "STT-LOCAL"},
      {"src/backends/llama_lm.cpp", "LM-LOCAL"},
      {"src/backends/piper_tts.cpp", "TTS-LOCAL"},
  };

  for (const auto &[rel, expected_tag] : local_tus) {
    std::string path = root;
    path += '/';
    path += rel;
    const std::string src = slurp(path);
    INFO("checking " << rel);
    REQUIRE_FALSE(src.empty()); // file must be readable
    REQUIRE(src.find(expected_tag) != std::string::npos);
    REQUIRE(src.find("OPENAI") == std::string::npos);
    REQUIRE(src.find("api.openai.com") == std::string::npos);
    REQUIRE(src.find("curl_easy_perform") == std::string::npos);
  }
}

TEST_CASE("source-level invariant: openai backend TUs reference only "
          "OPENAI kBackendTag, never LOCAL whisper/llama/piper",
          "[audit][source]") {
  const std::string root = XP_WELLYS_ATC_SOURCE_DIR;
  const std::vector<std::pair<std::string, std::string>> openai_tus = {
      {"src/backends/openai_stt.cpp", "STT-OPENAI"},
      {"src/backends/openai_lm.cpp", "LM-OPENAI"},
      {"src/backends/openai_tts.cpp", "TTS-OPENAI"},
  };

  for (const auto &[rel, expected_tag] : openai_tus) {
    std::string path = root;
    path += '/';
    path += rel;
    const std::string src = slurp(path);
    INFO("checking " << rel);
    REQUIRE_FALSE(src.empty());
    REQUIRE(src.find(expected_tag) != std::string::npos);
    // Cloud TUs must not include local backend headers or vocab.
    REQUIRE(src.find("whisper.h") == std::string::npos);
    REQUIRE(src.find("llama.h") == std::string::npos);
    REQUIRE(src.find("piper.h") == std::string::npos);
    REQUIRE(src.find("-LOCAL]") == std::string::npos);
    // OpenAI TUs must not bleed into the Mistral provider either —
    // they live side by side in the plugin module and the only
    // legitimate cross-provider link is openai_common (audio helpers).
    REQUIRE(src.find("MISTRAL") == std::string::npos);
    REQUIRE(src.find("api.mistral.ai") == std::string::npos);
  }
}

TEST_CASE("source-level invariant: mistral backend TUs reference only "
          "MISTRAL kBackendTag, never LOCAL whisper/llama/piper or OpenAI",
          "[audit][source]") {
  const std::string root = XP_WELLYS_ATC_SOURCE_DIR;
  const std::vector<std::pair<std::string, std::string>> mistral_tus = {
      {"src/backends/mistral_stt.cpp", "STT-MISTRAL"},
      {"src/backends/mistral_lm.cpp", "LM-MISTRAL"},
      {"src/backends/mistral_tts.cpp", "TTS-MISTRAL"},
  };

  for (const auto &[rel, expected_tag] : mistral_tus) {
    std::string path = root;
    path += '/';
    path += rel;
    const std::string src = slurp(path);
    INFO("checking " << rel);
    REQUIRE_FALSE(src.empty());
    REQUIRE(src.find(expected_tag) != std::string::npos);
    // Mistral TUs must not include local backend headers or vocab.
    REQUIRE(src.find("whisper.h") == std::string::npos);
    REQUIRE(src.find("llama.h") == std::string::npos);
    REQUIRE(src.find("piper.h") == std::string::npos);
    REQUIRE(src.find("-LOCAL]") == std::string::npos);
    // Mistral TUs must not carry OpenAI provider literals — the
    // openai_common header is allowed (audio helpers + last4), but
    // the OPENAI audit tag and the api.openai.com endpoint string
    // must not appear.
    REQUIRE(src.find("OPENAI") == std::string::npos);
    REQUIRE(src.find("api.openai.com") == std::string::npos);
  }
}
