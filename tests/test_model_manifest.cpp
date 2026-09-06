#include "persistence/model_manifest.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <string>

using model_manifest::Kind;
using model_manifest::VoiceRole;

// ── Language-aware Whisper selection (M6) ────────────────────────────
// The catalog ships a single *multilingual* Whisper model. It is tagged
// language-agnostic (empty) so it is shared by both DE and EN — tagging
// it "de" would make get_for_language(WhisperModel,"en") abort and drop
// STT from EN local mode.

TEST_CASE("get_for_language routes multilingual Whisper for both languages",
          "[model_manifest][m6]") {
  const auto &de = model_manifest::get_for_language(Kind::WhisperModel, "de");
  const auto &en = model_manifest::get_for_language(Kind::WhisperModel, "en");
  REQUIRE(de.kind == Kind::WhisperModel);
  REQUIRE(en.kind == Kind::WhisperModel);
  REQUIRE(de.filename == "ggml-small-q5_1.bin");
  REQUIRE(en.filename == de.filename); // same single entry via fallback
  REQUIRE(de.language.empty());        // language-agnostic
}

TEST_CASE("is_optional_ai_model flags Llama but not Whisper/voices",
          "[model_manifest]") {
  REQUIRE(model_manifest::is_optional_ai_model(Kind::LlamaModel));
  REQUIRE_FALSE(model_manifest::is_optional_ai_model(Kind::WhisperModel));
  REQUIRE_FALSE(model_manifest::is_optional_ai_model(Kind::PiperVoice));
}

TEST_CASE("get_for_language falls back to language-agnostic Llama",
          "[model_manifest][m6]") {
  // Llama is multilingual — no language tag — so any language should
  // route to the same single entry via the empty-language fallback.
  const auto &en = model_manifest::get_for_language(Kind::LlamaModel, "en");
  const auto &de = model_manifest::get_for_language(Kind::LlamaModel, "de");
  REQUIRE(en.kind == Kind::LlamaModel);
  REQUIRE(de.kind == Kind::LlamaModel);
  REQUIRE(en.filename == de.filename);
  REQUIRE(en.language.empty());
}

// ── Voice catalog language tagging (M6) ──────────────────────────────

TEST_CASE("Thorsten voice is tagged 'de'", "[model_manifest][m6]") {
  REQUIRE(model_manifest::voice_language("de_DE-thorsten-medium") == "de");
}

TEST_CASE("voice_language returns empty for unknown ids",
          "[model_manifest][m6]") {
  REQUIRE(model_manifest::voice_language("does-not-exist").empty());
}

// ── default_voice_for(role, language) (M6) ───────────────────────────

TEST_CASE("German default voice covers every role", "[model_manifest][m6]") {
  // We ship a single German voice — Thorsten — and assign it to
  // every role at region-switch time.
  for (auto role : model_manifest::all_roles()) {
    REQUIRE(model_manifest::default_voice_for(role, "de") ==
            "de_DE-thorsten-medium");
  }
}

// ── English default voice (Issue #40) ────────────────────────────────

TEST_CASE("English default voice covers every role with an EN voice",
          "[model_manifest][en]") {
  // The EN profile ships en_GB-alan-medium as the primary (non-optional)
  // voice; the language default resolves to it for every role, with no
  // fall-back to a German voice.
  for (auto role : model_manifest::all_roles()) {
    const std::string v = model_manifest::default_voice_for(role, "en");
    REQUIRE(v == "en_GB-alan-medium");
    REQUIRE(model_manifest::voice_language(v) == "en");
  }
}

TEST_CASE("EN primary Piper voice is tagged 'en'", "[model_manifest][en]") {
  // Only the non-optional voice ships in the built-in fallback catalog the
  // unit tests exercise (the optional en_US-lessac-medium, like the optional
  // German voices, lives only in the full data/models_catalog.json).
  REQUIRE(model_manifest::voice_language("en_GB-alan-medium") == "en");
}

// ── SHA256 round-trip (issue #19) ────────────────────────────────────
// Known-answer tests pin the digest so the CommonCrypto (macOS) and CNG
// (Windows) backends stay bit-identical, and exercise the shared 1 MB
// streaming loop across a multi-chunk input. Runs on whichever platform
// builds the tests (macOS in CI); the Windows CNG path is the same
// standard SHA256 by construction.
TEST_CASE("sha256_file matches known digests", "[model_manifest][sha256]") {
  const std::string path = "sha256_roundtrip_test.tmp";
  auto write_file = [&](const std::string &content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
  };

  SECTION("empty file") {
    write_file("");
    REQUIRE(model_manifest::sha256_file(path) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b785"
            "2b855");
  }
  SECTION("abc") {
    write_file("abc");
    REQUIRE(model_manifest::sha256_file(path) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20"
            "015ad");
  }
  SECTION("multi-chunk (> 1 MB streaming buffer)") {
    write_file(std::string(2 * 1024 * 1024 + 123, 'x'));
    REQUIRE(model_manifest::sha256_file(path) ==
            "cfc6db13bd7e4bcc69c1675e27a9b3f7a820f94430245510a736bb68c01"
            "29f16");
  }
  SECTION("missing file yields empty string") {
    REQUIRE(model_manifest::sha256_file("does_not_exist.xyz").empty());
  }

  std::remove(path.c_str());
}
