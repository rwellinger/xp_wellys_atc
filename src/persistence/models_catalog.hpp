/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef PERSISTENCE_MODELS_CATALOG_HPP
#define PERSISTENCE_MODELS_CATALOG_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace models_catalog {

// One selectable cloud model or voice preset. `id` is the wire value
// passed to the API; `label` is the human-readable UI string (defaults
// to `id` when the JSON omits it).
struct Option {
  std::string id;
  std::string label;
};

// Local Piper voice entry pulled from the catalog. Mirrors the fields
// the loader/downloader need without exposing model_manifest types
// (which would create a circular include).
struct PiperVoiceEntry {
  std::string voice_id;
  std::string url_subpath; // relative to local._piper_base
  uint64_t onnx_size = 0;
  std::string onnx_sha256;
  uint64_t json_size = 0;
  std::string json_sha256;
  bool optional = false;
  std::string language; // "en", "de"
};

// Local whisper.cpp / llama.cpp model entry from the catalog.
struct LocalFileEntry {
  std::string filename;
  uint64_t size_bytes = 0;
  std::string sha256;
  std::string url;
  std::string display_name;
  std::string language; // empty for Llama
};

// Load <data_dir>/models_catalog.json into the in-memory catalog. Safe
// to call multiple times; later calls replace the cached contents.
// Returns true if the file was found and parsed; false if it was
// missing or malformed (callers fall back to the built-in defaults
// loaded by ensure_defaults_locked()). Always leaves the catalog in a
// usable state — never throws across the call boundary.
bool init(const std::string &data_dir);

// Accessors used by the Settings UI. The catalog guarantees each
// returned vector contains at least one option; on a parse failure the
// defaults match the pre-catalog hardcoded lists. `label` is filled
// (falling back to `id`) so callers never have to handle empty labels.
const std::vector<Option> &openai_stt_options();
const std::vector<Option> &openai_lm_options();
const std::vector<Option> &openai_tts_options();
const std::vector<Option> &openai_voice_options();

const std::vector<Option> &mistral_stt_options();
const std::vector<Option> &mistral_lm_options();
const std::vector<Option> &mistral_tts_options();
const std::vector<Option> &mistral_voice_options();

// Local-section accessors consumed by model_manifest::manifest() to
// build its Entry list. The Piper voice array is ordered required
// first (in role order: Atis, Tower, Ground, Unicom), optional last —
// the same ordering model_manifest::default_voice_for() relies on.
const std::vector<LocalFileEntry> &local_whisper_entries();
const std::vector<LocalFileEntry> &local_llama_entries();
const std::vector<PiperVoiceEntry> &local_piper_voices();
const std::string &piper_base_url();

} // namespace models_catalog

#endif // PERSISTENCE_MODELS_CATALOG_HPP
