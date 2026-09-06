/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/loader.hpp"

#include "backends/manager.hpp"
#include "backends/mistral_lm.hpp"
#include "backends/mistral_stt.hpp"
#include "backends/mistral_tts.hpp"
#include "backends/openai_lm.hpp"
#include "backends/openai_stt.hpp"
#include "backends/openai_tts.hpp"
#include "core/cross_country_log.hpp"
#include "core/logging.hpp"
#include "persistence/model_paths.hpp"
#include "persistence/models_catalog.hpp"
#include "persistence/settings.hpp"

// Metal STT/LM backends (issue #69): gated on XPWELLYS_USE_LOCAL_INFERENCE.
#ifdef XPWELLYS_USE_LOCAL_INFERENCE
#include "backends/llama_lm.hpp"
#include "backends/whisper_stt.hpp"
#endif
// CPU Piper TTS backend: gated on its own flag so the hybrid mode (cloud
// STT/LM + local German voice) works on the cloud-only slices too (#69/#70).
#ifdef XPWELLYS_USE_LOCAL_TTS
#include "backends/piper_tts.hpp"
#endif

// Full local inference (Metal STT/LM) always implies local TTS at build time
// (CMake forces XPWELLYS_USE_LOCAL_TTS ON whenever XPWELLYS_USE_LOCAL_INFERENCE
// is), so "any local backend compiled in" is INFERENCE || TTS. The shared
// model verification + status helpers below use this; the Piper-only pieces
// (g_piper, ensure_piper_init, load_piper_voice, the hybrid override, the
// Piper voice rows) are gated on XPWELLYS_USE_LOCAL_TTS alone.
#if defined(XPWELLYS_USE_LOCAL_INFERENCE) || defined(XPWELLYS_USE_LOCAL_TTS)
#define XPWELLYS_HAS_LOCAL_MODELS 1
#endif

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
// MSVC ships the S_IF* flags but not the POSIX S_ISREG/S_ISDIR test macros.
#if !defined(S_ISREG)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#if !defined(S_ISDIR)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#include <thread>
#include <unordered_set>
#include <utility>

#if defined(_WIN32) && defined(XPWELLYS_USE_LOCAL_TTS)
#include <windows.h>
#endif

namespace backends::loader {

namespace {

// ── Shared state ────────────────────────────────────────────────────
//
// `g_status` is read from the main thread (UI snapshot) and written
// by the verification worker. A single mutex covers the whole struct
// — updates are coarse-grained (per state transition) so contention
// is negligible.
std::mutex g_mtx;
Status g_status; // protected by g_mtx

// `g_running` ensures only one verification worker exists at a time.
// `g_should_exit` lets stop() signal the worker to bail mid-SHA256.
std::atomic<bool> g_running{false};
std::atomic<bool> g_should_exit{false};
std::thread g_worker;

// When non-empty the worker only processes the manifest entry whose
// entry_key matches. Read by run_worker() exactly once; cleared on
// completion so subsequent full sweeps are not accidentally narrowed.
std::mutex g_single_mtx;
std::string g_single_key; // protected by g_single_mtx

#ifdef XPWELLYS_USE_LOCAL_TTS
// One ITextToSpeech survives across loader runs so newly-downloaded
// optional voices can be hot-loaded. Created on first run. Local Piper
// only — the cloud TTS backends are owned by the manager.
std::shared_ptr<PiperTts> g_piper;
#endif

#ifdef XPWELLYS_HAS_LOCAL_MODELS
void update_state(const model_manifest::Entry &entry, FileState s,
                  std::string message = {}) {
  std::lock_guard<std::mutex> lk(g_mtx);
  for (auto &f : g_status.files) {
    if (f.kind == entry.kind && f.voice_id == entry.voice_id &&
        f.language == entry.language) {
      f.state = s;
      f.message = std::move(message);
      return;
    }
  }
}
#endif

void seed_status_locked() {
  // Initial layout, mirrors the manifest order. Idempotent: only
  // overwritten on a fresh start() — re-runs preserve any Ready
  // state that was already established.
  if (!g_status.files.empty())
    return;
  for (const auto &e : model_manifest::all()) {
    g_status.files.push_back(
        {e.kind, e.voice_id, e.language, FileState::NotChecked, {}});
  }
}

// True if the corresponding inference backend is currently registered
// with the manager. For voice entries this asks PiperTts whether the
// specific voice_id is loaded.
bool entry_loaded(const model_manifest::Entry &e) {
  switch (e.kind) {
  case model_manifest::Kind::WhisperModel:
    return backends::stt_ready();
  case model_manifest::Kind::LlamaModel:
    return backends::lm_ready();
  case model_manifest::Kind::PiperVoice:
  case model_manifest::Kind::PiperVoiceConfig:
#ifdef XPWELLYS_USE_LOCAL_TTS
    return g_piper && g_piper->has_voice(e.voice_id);
#else
    return false;
#endif
  }
  return false;
}

#ifdef XPWELLYS_USE_LOCAL_TTS
bool dir_exists(const std::string &path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0)
    return false;
  return S_ISDIR(st.st_mode);
}
#endif

#ifdef XPWELLYS_HAS_LOCAL_MODELS
// The set of voice_ids the user currently wants loaded — i.e. the
// four roles' assignments. Optional voices not assigned to any role
// are not loaded into memory but still verified (so the UI can show
// them as Ready).
std::unordered_set<std::string> assigned_voice_ids() {
  using R = model_manifest::VoiceRole;
  std::unordered_set<std::string> v;
  for (auto role : model_manifest::all_roles())
    v.insert(settings::voice_for_role(role));
  (void)R{};
  return v;
}
#endif

#ifdef XPWELLYS_HAS_LOCAL_MODELS
// Verify a single manifest entry. Updates `g_status` to reflect the
// terminal state (Missing / SizeMismatch / HashMismatch / Verified).
// Returns true iff the entry reached Verified.
bool verify_one(const model_manifest::Entry &e) {
  if (g_should_exit.load())
    return false;
  std::string full_path = model_paths::models_dir() + "/" + e.filename;

  if (!model_manifest::size_matches(e, full_path)) {
    struct stat st{};
    if (stat(full_path.c_str(), &st) != 0) {
      update_state(e, FileState::Missing, "File not found at " + full_path);
    } else {
      update_state(e, FileState::SizeMismatch,
                   "Size mismatch (have " +
                       std::to_string(static_cast<uint64_t>(st.st_size)) +
                       ", expected " + std::to_string(e.size_bytes) +
                       "). Likely a partial download - re-download to fix.");
    }
    return false;
  }

  update_state(e, FileState::Verifying, "Computing SHA256...");
  const bool debug = settings::debug_logging();
  if (debug)
    logging::info("verify: hashing %s (%llu bytes)", e.filename.c_str(),
                  static_cast<unsigned long long>(e.size_bytes));
  std::string actual = model_manifest::sha256_file(full_path);

  if (g_should_exit.load())
    return false;

  if (actual.empty()) {
    update_state(e, FileState::Missing, "Failed to read " + full_path);
    return false;
  }
  if (actual != e.sha256_hex) {
    update_state(e, FileState::HashMismatch,
                 "SHA256 mismatch (file is corrupt or modified). "
                 "Delete and re-download.");
    return false;
  }
  update_state(e, FileState::Verified, {});
  if (debug)
    logging::info("verify: %s OK (sha256 matches)", e.filename.c_str());
  return true;
}
#endif // XPWELLYS_HAS_LOCAL_MODELS

#ifdef XPWELLYS_USE_LOCAL_INFERENCE
// Walks the manifest, fast-checks size, computes SHA256 only for
// files that pass the size check. Updates `g_status` as it goes so
// the UI can reflect "Verifying… llama-3.2-3B (45%)" etc.
//
// Returns true if all *required* entries (Whisper, Llama, plus the
// four assigned voices' .onnx + .json) reach Verified. Optional
// voices that are missing do not count against this gate.
bool verify_files() {
  bool all_required_ok = true;
  auto wanted_voices = assigned_voice_ids();
  const std::string active_lang = settings::backend_language();
  const bool debug = settings::debug_logging();

  size_t verified_count = 0;
  size_t missing_count = 0;
  size_t size_mismatch_count = 0;
  size_t hash_mismatch_count = 0;

  for (const auto &e : model_manifest::all()) {
    if (g_should_exit.load())
      return false;

    // Verify EVERY entry's FileState — the size check is cheap (one
    // stat), the SHA256 only runs after a size match (which a missing
    // file fails). This way a DE-profile user who toggles "Show all
    // languages" still sees the EN optional voices as Missing and can
    // click Download, instead of staring at a permanent "Busy" button.
    const bool foreign_language =
        !e.language.empty() && e.language != active_lang;
    // Optional AI models (Llama) are NOT part of the readiness gate: the
    // engine runs fully on the rule parser without the LM, and a missing
    // Llama must not stop Whisper/Piper from loading (load_backends only
    // runs when verify_files() returns true).
    bool is_required = !e.optional && !foreign_language &&
                       !model_manifest::is_optional_ai_model(e.kind);
    bool is_assigned_voice =
        (e.kind == model_manifest::Kind::PiperVoice ||
         e.kind == model_manifest::Kind::PiperVoiceConfig) &&
        wanted_voices.count(e.voice_id) > 0;
    // Foreign-language entries never count toward the readiness gate
    // (they will not be loaded into the active backend anyway), but
    // we still walk them so the UI can show their on-disk state.
    bool counts_against_gate =
        (is_required || is_assigned_voice) && !foreign_language;

    if (debug)
      logging::info("verify: %s%s (%s)", e.filename.c_str(),
                    counts_against_gate ? " [gating]" : " [non-gating]",
                    e.display_name.c_str());

    bool ok = verify_one(e);
    if (ok) {
      ++verified_count;
    } else {
      // Inspect the freshly written state for the summary tally.
      std::lock_guard<std::mutex> lk(g_mtx);
      for (const auto &f : g_status.files) {
        if (f.kind == e.kind && f.voice_id == e.voice_id &&
            f.language == e.language) {
          switch (f.state) {
          case FileState::Missing:
            ++missing_count;
            break;
          case FileState::SizeMismatch:
            ++size_mismatch_count;
            break;
          case FileState::HashMismatch:
            ++hash_mismatch_count;
            break;
          default:
            break;
          }
          if (counts_against_gate)
            logging::error("verify: %s -> %s (gates readiness)",
                           e.filename.c_str(),
                           f.message.empty() ? "not ready" : f.message.c_str());
          break;
        }
      }
    }
    if (!ok && counts_against_gate)
      all_required_ok = false;
  }

  logging::info(
      "verify_files complete: %zu verified, %zu missing, %zu size_mismatch, "
      "%zu hash_mismatch (gate %s)",
      verified_count, missing_count, size_mismatch_count, hash_mismatch_count,
      all_required_ok ? "OK" : "FAIL");
  return all_required_ok;
}

// Open the three concrete backends in sequence. Whisper goes first
// because its model load is fastest and surfaces obvious problems
// (path errors, broken Metal cache) before we commit to the
// 2 GB llama load.
// Forward declarations — Piper helpers are defined after load_backends
// for readability but used inside it.
void load_piper_voice(const std::string &voice_id);
bool ensure_piper_init();

void load_whisper(const model_manifest::Entry &whisper_entry,
                  const std::string &lang) {
  if (backends::stt_ready()) {
    // Backend already loaded (e.g. from a prior targeted download). A
    // full re-run's verify_files() downgraded the row to Verified; restore
    // Ready so the readiness gate doesn't block on an already-loaded model.
    update_state(whisper_entry, FileState::Ready, {});
    return;
  }
  update_state(whisper_entry, FileState::Loading,
               "Loading whisper.cpp context...");
  auto stt = std::make_unique<backends::WhisperStt>();
  std::string p = model_paths::models_dir() + "/" + whisper_entry.filename;
  if (stt->open(p, lang)) {
    backends::register_stt(std::move(stt));
    update_state(whisper_entry, FileState::Ready, {});
    logging::info("STT backend ready (whisper.cpp, lang=%s)", lang.c_str());
  } else {
    update_state(whisper_entry, FileState::LoadError,
                 "whisper.cpp rejected the model file. Try re-downloading.");
    logging::error("Whisper open failed for %s", p.c_str());
  }
}

void load_llama(const model_manifest::Entry &llama_entry) {
  if (backends::lm_ready()) {
    update_state(llama_entry, FileState::Ready, {});
    return;
  }
  update_state(llama_entry, FileState::Loading,
               "Loading llama.cpp context (this can take a few seconds)...");
  auto lm = std::make_unique<backends::LlamaLm>();
  std::string p = model_paths::models_dir() + "/" + llama_entry.filename;
  if (lm->open(p)) {
    backends::register_lm(std::move(lm));
    update_state(llama_entry, FileState::Ready, {});
    logging::info("LM backend ready (llama.cpp)");
  } else {
    update_state(llama_entry, FileState::LoadError,
                 "llama.cpp rejected the model file. Try re-downloading.");
    logging::error("Llama open failed for %s", p.c_str());
  }
}

void load_backends() {
  using K = model_manifest::Kind;

  // Whisper — pick the variant that matches the active language
  // (EN-only ggml-small.en vs. multilingual ggml-small).
  {
    const std::string lang = settings::backend_language();
    const auto &whisper_entry =
        model_manifest::get_for_language(K::WhisperModel, lang);
    load_whisper(whisper_entry, lang);
  }

  if (g_should_exit.load())
    return;

  // Llama — optional low-confidence fallback. Only load it when the file
  // is actually on disk; its absence leaves the LM backend unregistered
  // (engine falls back to the rule parser) instead of logging a scary
  // "open failed" for a file the user deliberately did not download.
  {
    const auto &llama = model_manifest::get(K::LlamaModel);
    const std::string llama_path =
        model_paths::models_dir() + "/" + llama.filename;
    if (model_manifest::size_matches(llama, llama_path))
      load_llama(llama);
    else
      logging::info("LM (llama.cpp) not present - optional, running "
                    "rule-parser-only classification.");
  }

  if (g_should_exit.load())
    return;

  // Piper voices — load every voice currently assigned to a role
  // (voice files for those roles are required to be Verified;
  // verify_files() ensured this is the case before we got here).
  auto wanted_voices = assigned_voice_ids();
  for (const std::string &voice_id : wanted_voices) {
    if (g_should_exit.load())
      return;
    load_piper_voice(voice_id);
  }
}
#endif // XPWELLYS_USE_LOCAL_INFERENCE

#ifdef XPWELLYS_USE_LOCAL_TTS
// Shim that lets the manager own the Piper instance via a unique_ptr
// while we keep a shared_ptr to it for hot-loading new voices.
struct PiperShim final : ITextToSpeech {
  std::shared_ptr<PiperTts> inner;
  explicit PiperShim(std::shared_ptr<PiperTts> p) : inner(std::move(p)) {}
  bool load_voice(const std::string &voice_id,
                  const std::string &voice_onnx_path,
                  const std::string &voice_json_path) override {
    return inner->load_voice(voice_id, voice_onnx_path, voice_json_path);
  }
  void unload_voice(const std::string &voice_id) override {
    inner->unload_voice(voice_id);
  }
  bool has_voice(const std::string &voice_id) const override {
    return inner->has_voice(voice_id);
  }
  std::vector<int16_t> synthesize(const std::string &voice_id,
                                  const std::string &text, float length_scale,
                                  uint32_t &sample_rate_hz,
                                  TtsFailure &out_failure) override {
    return inner->synthesize(voice_id, text, length_scale, sample_rate_hz,
                             out_failure);
  }
  std::string default_voice_for(model_manifest::VoiceRole role) const override {
    return inner->default_voice_for(role);
  }
};

#if defined(_WIN32)
// piper.dll is delay-loaded (see the /DELAYLOAD in CMakeLists) so a cloud-only
// user never pulls in Piper or onnxruntime. The catch: the delay-load helper
// resolves the DLL at the FIRST piper call using the PROCESS search path —
// X-Plane.exe's directory, System32, PATH. Our win_x64/ directory is not on it;
// that one is only consulted for the .xpl's own load-time imports. So the
// helper cannot find piper.dll and raises 0xC06D007E (ERROR_MOD_NOT_FOUND),
// which without a guard kills X-Plane in the middle of piper_create.
//
// Load it explicitly by absolute path before any piper symbol is touched.
// LOAD_WITH_ALTERED_SEARCH_PATH makes piper.dll's own dependency
// (onnxruntime.dll) resolve from that same directory. Afterwards the delay-load
// thunk asks for "piper.dll" by name and gets the already-loaded module.
bool preload_piper_dll() {
  static bool attempted = false;
  static bool loaded = false;
  if (attempted)
    return loaded;
  attempted = true;

  // Derive the directory from THIS module's own path rather than rebuilding it
  // from model_paths::plugin_root(). Two reasons: the XPLM path arrives with
  // mixed separators (C:\...\plugin/win_x64/...), and LoadLibraryEx with
  // LOAD_WITH_ALTERED_SEARCH_PATH wants a clean fully-qualified Win32 path;
  // and the .xpl already sits in win_x64 next to piper.dll, so there is nothing
  // to reconstruct in the first place.
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&preload_piper_dll),
                          &self)) {
    logging::error("piper.dll preload: GetModuleHandleEx failed (%lu)",
                   static_cast<unsigned long>(GetLastError()));
    return false;
  }

  wchar_t self_path[MAX_PATH * 4] = {};
  constexpr DWORD kSelfPathCap =
      static_cast<DWORD>(sizeof(self_path) / sizeof(self_path[0]));
  const DWORD n = GetModuleFileNameW(self, self_path, kSelfPathCap);
  if (n == 0 || n >= kSelfPathCap) {
    logging::error("piper.dll preload: GetModuleFileName failed (%lu)",
                   static_cast<unsigned long>(GetLastError()));
    return false;
  }

  // Strip the .xpl filename, keep the trailing separator, append piper.dll.
  std::wstring dll(self_path, self_path + n);
  const size_t sep = dll.find_last_of(L"\\/");
  if (sep == std::wstring::npos) {
    logging::error("piper.dll preload: unexpected module path");
    return false;
  }
  dll.resize(sep + 1);
  dll += L"piper.dll";

  // Narrow copy for logging only — XPLMDebugString is ASCII-only anyway.
  std::string dll_utf8(dll.size(), '\0');
  for (size_t i = 0; i < dll.size(); ++i)
    dll_utf8[i] = (dll[i] >= 0x20 && dll[i] < 0x7F) ? static_cast<char>(dll[i])
                                                    : '?';

  if (!LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
    logging::error("piper.dll preload failed for %s (GetLastError=%lu)",
                   dll_utf8.c_str(), static_cast<unsigned long>(GetLastError()));
    return false;
  }
  logging::info("piper.dll preloaded from %s", dll_utf8.c_str());
  loaded = true;
  return true;
}
#endif

// Initialise the shared Piper instance lazily, register the manager
// TTS shim on first creation. Returns false (and tags every voice row
// LoadError) if espeak-ng-data is missing.
bool ensure_piper_init() {
  using K = model_manifest::Kind;
  if (g_piper)
    return true;

#if defined(_WIN32)
  if (!preload_piper_dll()) {
    const std::string msg =
        "piper.dll could not be loaded from the plugin's win_x64 directory. "
        "Local TTS unavailable.";
    for (const auto &e : model_manifest::all()) {
      if (e.kind == K::PiperVoice || e.kind == K::PiperVoiceConfig)
        update_state(e, FileState::LoadError, msg);
    }
    logging::error("%s", msg.c_str());
    return false;
  }
#endif

  const std::string &espeak_dir = model_paths::espeakng_data_dir();
  if (!dir_exists(espeak_dir)) {
    const std::string msg = "espeak-ng-data missing at " + espeak_dir +
                            ". Reinstall the plugin bundle.";
    for (const auto &e : model_manifest::all()) {
      if (e.kind == K::PiperVoice || e.kind == K::PiperVoiceConfig)
        update_state(e, FileState::LoadError, msg);
    }
    logging::error("%s", msg.c_str());
    return false;
  }

  g_piper = std::make_shared<PiperTts>();
  if (!g_piper->init(espeak_dir)) {
    logging::error("PiperTts init failed");
    g_piper.reset();
    return false;
  }
  backends::register_tts(std::make_unique<PiperShim>(g_piper));
  logging::info("TTS backend ready (Piper)");
  return true;
}

// Load one Piper voice into the shared instance. Both the .onnx and
// the .onnx.json row flip to Ready/LoadError together. No-op when the
// voice is already loaded.
void load_piper_voice(const std::string &voice_id) {
  using K = model_manifest::Kind;
  if (voice_id.empty())
    return;
  if (!ensure_piper_init())
    return;
  if (g_piper->has_voice(voice_id)) {
    // Already loaded — a full re-run's verify_files() downgraded both rows
    // to Verified; restore Ready so the readiness gate (which requires every
    // assigned voice in Ready) doesn't block on an already-loaded voice.
    if (const auto *o = model_manifest::get_voice(K::PiperVoice, voice_id))
      update_state(*o, FileState::Ready, {});
    if (const auto *j =
            model_manifest::get_voice(K::PiperVoiceConfig, voice_id))
      update_state(*j, FileState::Ready, {});
    return;
  }

  const auto *onnx = model_manifest::get_voice(K::PiperVoice, voice_id);
  const auto *json = model_manifest::get_voice(K::PiperVoiceConfig, voice_id);
  if (!onnx || !json) {
    logging::error("Manifest mismatch: voice %s not found in catalog",
                   voice_id.c_str());
    return;
  }
  update_state(*onnx, FileState::Loading, "Loading Piper voice...");
  update_state(*json, FileState::Loading, "Loading Piper voice config...");

  const std::string voice_path =
      model_paths::models_dir() + "/" + onnx->filename;
  const std::string config_path =
      model_paths::models_dir() + "/" + json->filename;

  if (g_piper->load_voice(voice_id, voice_path, config_path)) {
    update_state(*onnx, FileState::Ready, {});
    update_state(*json, FileState::Ready, {});
    logging::info("Piper voice loaded: %s", voice_id.c_str());
  } else {
    const std::string msg = "Piper rejected " + voice_id;
    update_state(*onnx, FileState::LoadError, msg);
    update_state(*json, FileState::LoadError, msg);
    logging::error("%s", msg.c_str());
  }
}
#endif // XPWELLYS_USE_LOCAL_TTS

#ifdef XPWELLYS_HAS_LOCAL_MODELS
// Per-entry verify-then-load used by the targeted start(key) path.
// Verifies the entry; if Verified, loads the matching backend
// (Whisper / Llama / Piper voice). Piper voices pull their sibling
// entry through automatically so the voice ends up usable from a
// single Re-verify click. Available whenever any local backend is
// compiled in — the Models-tab voice re-verify uses it in hybrid mode.
void process_one(const model_manifest::Entry &e) {
  using K = model_manifest::Kind;
  if (e.kind == K::PiperVoice || e.kind == K::PiperVoiceConfig) {
#ifdef XPWELLYS_USE_LOCAL_TTS
    const auto *onnx = model_manifest::get_voice(K::PiperVoice, e.voice_id);
    const auto *json =
        model_manifest::get_voice(K::PiperVoiceConfig, e.voice_id);
    if (!onnx || !json)
      return;
    bool onnx_ok = verify_one(*onnx);
    bool json_ok = verify_one(*json);
    if (onnx_ok && json_ok)
      load_piper_voice(e.voice_id);
#endif
    return;
  }
#ifdef XPWELLYS_USE_LOCAL_INFERENCE
  if (!verify_one(e))
    return;
  if (e.kind == K::WhisperModel)
    load_whisper(e, e.language.empty() ? settings::backend_language()
                                       : e.language);
  else if (e.kind == K::LlamaModel)
    load_llama(e);
#endif
}
#endif // XPWELLYS_HAS_LOCAL_MODELS

#ifdef XPWELLYS_USE_LOCAL_TTS
// Hybrid TTS bring-up (issue #66). Called AFTER a cloud loader has already
// registered STT+LM+TTS, when settings::tts_backend_override() == "local".
// Verifies the assigned Piper voice(s) and, only if at least one verifies,
// inits Piper + loads them — ensure_piper_init() registers the PiperShim
// and thereby OVERWRITES the cloud TTS slot. If nothing verifies we leave
// the cloud TTS untouched (graceful fallback, no voiceless shim). Touches
// no whisper/llama models; voice_for_role() already yields Piper ids
// because the override is active. Returns true iff a Piper voice is loaded.
bool bring_up_local_tts_override() {
  using K = model_manifest::Kind;
  std::unordered_set<std::string> verified;
  for (const auto &vid : assigned_voice_ids()) {
    const auto *onnx = model_manifest::get_voice(K::PiperVoice, vid);
    const auto *json = model_manifest::get_voice(K::PiperVoiceConfig, vid);
    if (onnx && json && verify_one(*onnx) && verify_one(*json))
      verified.insert(vid);
  }
  if (verified.empty())
    return false;
  bool any_ready = false;
  for (const auto &vid : verified) {
    load_piper_voice(vid);
    if (g_piper && g_piper->has_voice(vid))
      any_ready = true;
  }
  return any_ready;
}

// Verify the on-disk state of every Piper voice (both the .onnx weights
// and the .onnx.json config sibling) WITHOUT loading anything into
// memory. Cloud modes skip verify_files() entirely, so without this the
// Models-tab voice rows stay stuck at NotChecked and render a disabled
// "..." button instead of a Download button — the user could never start
// a voice download (issue #78). Purely updates g_status so the UI can
// resolve each row to Missing (-> Download) or Verified (-> Ready).
// Cheap for the common "nothing downloaded yet" case: a missing file
// fails the size check and reports Missing without hashing.
void verify_local_voice_files() {
  using K = model_manifest::Kind;
  for (const auto &e : model_manifest::all()) {
    if (g_should_exit.load())
      return;
    if (e.kind == K::PiperVoice || e.kind == K::PiperVoiceConfig)
      verify_one(e);
  }
}
#endif // XPWELLYS_USE_LOCAL_TTS

// Construct the three OpenAI cloud backends and register them with the
// manager. Skips the local-model verification entirely: no files on
// disk, only an API key in the Keychain. On a missing/empty key we
// log the situation and bail — the UI's "Backend Mode" banner will
// surface that state to the user and PTT stays disabled via
// all_ready().
void load_openai_backends() {
  std::string api_key = settings::load_api_key();
  if (api_key.empty()) {
    logging::error("[xp_wellys_vfr_atc] OpenAI mode active but no API key in "
                   "Keychain. Open Settings to paste a key.");
    return;
  }

  auto stt = std::make_unique<OpenAiStt>(api_key, settings::openai_stt_model());
  auto lm = std::make_unique<OpenAiLm>(api_key, settings::openai_lm_model());
  auto tts = std::make_unique<OpenAiTts>(api_key, settings::openai_tts_model());

  // Pre-register the three configured OpenAI voices. load_voice() on
  // the cloud TTS only validates the voice id (alloy / echo / fable /
  // onyx / nova / shimmer) — no model file to fetch, so this is
  // instant.
  tts->load_voice(settings::openai_tts_voice_atis(), {}, {});
  tts->load_voice(settings::openai_tts_voice_tower(), {}, {});
  tts->load_voice(settings::openai_tts_voice_ground(), {}, {});

  backends::register_stt(std::move(stt));
  backends::register_lm(std::move(lm));
  backends::register_tts(std::move(tts));
  logging::info("STT/LM/TTS backends ready (OpenAI Cloud)");
}

// Mirror of load_openai_backends() for the Mistral cloud provider.
// Same lifecycle: read the key from the Mistral Keychain entry, build
// the three concrete clients, register them with the manager. Voice
// ids are free strings — empty entries skip the load_voice() call so
// the user sees a clean "voice id not configured" error path rather
// than a spurious load failure at startup.
void load_mistral_backends() {
  std::string api_key = settings::load_mistral_api_key();
  if (api_key.empty()) {
    logging::error(
        "[xp_wellys_vfr_atc] Mistral mode active but no API key in "
        "Keychain. Open Settings to paste a Mistral key.");
    return;
  }

  auto stt =
      std::make_unique<MistralStt>(api_key, settings::mistral_stt_model());
  auto lm = std::make_unique<MistralLm>(api_key, settings::mistral_lm_model());
  auto tts =
      std::make_unique<MistralTts>(api_key, settings::mistral_tts_model());

  // Track the three role voices. load_voice() silently skips empty
  // ids — the user can paste them later from the Settings tab and a
  // backend restart will pick them up.
  tts->load_voice(settings::mistral_tts_voice_atis(), {}, {});
  tts->load_voice(settings::mistral_tts_voice_tower(), {}, {});
  tts->load_voice(settings::mistral_tts_voice_ground(), {}, {});

  backends::register_stt(std::move(stt));
  backends::register_lm(std::move(lm));
  backends::register_tts(std::move(tts));
  logging::info("STT/LM/TTS backends ready (Mistral Cloud)");
}

// Log the concrete model / voice ids that are about to serve this
// backend mode. Gives Log.txt a single authoritative line tying each
// inference run's [STT|LM|TTS]-[LOCAL|OPENAI|MISTRAL] tag back to the
// exact model the user selected — emitted at startup and again on every
// Settings-driven backend restart, so a switch is always visible.
void log_active_models(const std::string &mode) {
  if (mode == "openai") {
    logging::info("[xp_wellys_vfr_atc] ACTIVE MODELS [OpenAI]: STT=%s LM=%s "
                  "TTS=%s voices[atis=%s tower=%s ground=%s]",
                  settings::openai_stt_model().c_str(),
                  settings::openai_lm_model().c_str(),
                  settings::openai_tts_model().c_str(),
                  settings::openai_tts_voice_atis().c_str(),
                  settings::openai_tts_voice_tower().c_str(),
                  settings::openai_tts_voice_ground().c_str());
    return;
  }
  if (mode == "mistral") {
    logging::info("[xp_wellys_vfr_atc] ACTIVE MODELS [Mistral]: STT=%s LM=%s "
                  "TTS=%s voices[atis=%s tower=%s ground=%s]",
                  settings::mistral_stt_model().c_str(),
                  settings::mistral_lm_model().c_str(),
                  settings::mistral_tts_model().c_str(),
                  settings::mistral_tts_voice_atis().c_str(),
                  settings::mistral_tts_voice_tower().c_str(),
                  settings::mistral_tts_voice_ground().c_str());
    return;
  }
#ifdef XPWELLYS_USE_LOCAL_INFERENCE
  if (mode == "local") {
    const auto &whisper = models_catalog::local_whisper_entries();
    const auto &llama = models_catalog::local_llama_entries();
    const auto &voices = models_catalog::local_piper_voices();
    logging::info(
        "[xp_wellys_vfr_atc] ACTIVE MODELS [Local]: STT=%s LM=%s TTS=%s",
        whisper.empty() ? "(none)" : whisper.front().filename.c_str(),
        llama.empty() ? "(none)" : llama.front().filename.c_str(),
        voices.empty() ? "(none)" : voices.front().voice_id.c_str());
  }
#endif
}

void run_worker() {
  // Guard against any std::filesystem / whisper.cpp / llama.cpp /
  // Piper exception escaping into std::thread destructor and
  // terminating X-Plane. We log + leave g_running false so a
  // subsequent start() can retry.
  try {
    // Pull the single-key target (if any) for this run. Cleared
    // immediately so a later start() / start(other_key) starts clean.
    std::string single_key;
    {
      std::lock_guard<std::mutex> lk(g_single_mtx);
      single_key = std::move(g_single_key);
      g_single_key.clear();
    }
#ifdef XPWELLYS_HAS_LOCAL_MODELS
    if (!single_key.empty()) {
      // Targeted: only one file (plus Piper sibling). Cloud modes
      // have nothing to verify on disk, so single-key requests are
      // ignored there — the cloud loader runs full anyway. Available in
      // hybrid TTS-only builds too, for the Models-tab voice re-verify.
      logging::info("[xp_wellys_vfr_atc] LOADER: targeted verify for key %s",
                    single_key.c_str());
      const model_manifest::Entry *target = nullptr;
      for (const auto &e : model_manifest::all()) {
        if (model_manifest::entry_key(e) == single_key) {
          target = &e;
          break;
        }
      }
      if (target)
        process_one(*target);
      else
        logging::error("loader: unknown single_key %s", single_key.c_str());
      g_running = false;
      return;
    }
#else
    (void)single_key;
#endif
    std::string mode = settings::backend_mode();
#ifndef XPWELLYS_USE_LOCAL_INFERENCE
    // Cloud-only slice: settings.json may still say "local" if the
    // user previously ran the arm64 slice on the same Mac. Force
    // OpenAI silently so the cockpit comes up usable, and persist
    // so the next launch starts clean.
    if (mode == "local") {
      logging::info(
          "[xp_wellys_vfr_atc] Local inference not compiled into this "
          "build; switching backend_mode to openai.");
      settings::set_backend_mode("openai");
      settings::save();
      mode = "openai";
    }
#endif
    log_active_models(mode);
    // Record the active backend label for the cross-country session log.
    // The loader is the one place that legitimately knows the mode; engine
    // code never inspects backend_mode (Backend Adapter Rule) and only
    // reads this label back through cross_country_log.
    cross_country_log::set_lm_backend(mode);
    if (mode == "openai") {
      logging::info(
          "[xp_wellys_vfr_atc] BACKEND MODE: OPENAI (api.openai.com). "
          "Audio + transcripts will be sent to OpenAI.");
      load_openai_backends();
    } else if (mode == "mistral") {
      logging::info(
          "[xp_wellys_vfr_atc] BACKEND MODE: MISTRAL (api.mistral.ai). "
          "Audio + transcripts will be sent to Mistral.");
      load_mistral_backends();
    } else {
#ifdef XPWELLYS_USE_LOCAL_INFERENCE
      logging::info("[xp_wellys_vfr_atc] BACKEND MODE: LOCAL (whisper.cpp + "
                    "llama.cpp + Piper). No network traffic to AI APIs.");
      bool all_files_verified = verify_files();
      if (g_should_exit.load()) {
        g_running = false;
        return;
      }
      if (all_files_verified) {
        load_backends();
      } else {
        logging::info(
            "One or more model files are missing or corrupt - backends not "
            "loaded; open the plugin window to download.");
      }
#else
      // Cloud-only slice (the x86_64 half of the macOS universal binary) but
      // settings still ask for Local. Unreachable in practice — the rewrite
      // above already forced mode to "openai" — but kept so a future build
      // that drops that rewrite fails loudly instead of silently.
      logging::error(
          "[xp_wellys_vfr_atc] BACKEND MODE: LOCAL requested but this "
          "build has no local-inference backends. Switch to OpenAI or "
          "Mistral in Settings (Local needs Apple Silicon or Windows x64).");
#endif
    }
#ifdef XPWELLYS_USE_LOCAL_TTS
    // Hybrid (issue #66/#78): a cloud run never touched the local Piper
    // voice files above, so their Models-tab rows would stay at NotChecked
    // and show a disabled "..." button — no way to start a voice download
    // (issue #78). Verify their on-disk state here so each row resolves to
    // Missing (-> Download) or Verified (-> Ready). Independent of the TTS
    // override so the voices are downloadable even before it is enabled.
    if (mode == "openai" || mode == "mistral") {
      verify_local_voice_files();
      if (g_should_exit.load()) {
        g_running = false;
        return;
      }
    }

    // Hybrid (issue #66): keep STT+LM on the cloud backend just registered,
    // but speak with the local Piper voice (native German, no US accent).
    // Overwrites the cloud TTS slot if a Piper voice is available; falls
    // back to the cloud TTS otherwise. Gated on the Piper-TTS flag so it
    // also runs on the x86_64-macOS / Windows slices (#69/#70).
    if ((mode == "openai" || mode == "mistral") &&
        settings::tts_backend_override() == "local") {
      if (g_should_exit.load()) {
        g_running = false;
        return;
      }
      if (bring_up_local_tts_override()) {
        logging::info("[xp_wellys_vfr_atc] TTS OVERRIDE: LOCAL Piper voice "
                      "active (STT+LM stay on %s).",
                      mode.c_str());
      } else {
        logging::info(
            "[xp_wellys_vfr_atc] TTS override 'local' requested but no Piper "
            "voice is verified - keeping %s TTS. Download the voice in the "
            "Models tab.",
            mode.c_str());
      }
    }
#endif
  } catch (const std::exception &e) {
    logging::error("loader: run_worker threw: %s", e.what());
  } catch (...) {
    logging::error("loader: run_worker threw an unknown exception");
  }
  g_running = false;
}

} // namespace

bool Status::all_ready() const { return readiness_blockers().empty(); }

std::vector<ReadinessBlocker> Status::readiness_blockers() const {
  std::vector<ReadinessBlocker> out;
  const std::string mode = settings::backend_mode();
  if (!backends::stt_ready())
    out.push_back({ReadinessBlocker::Source::SttBackend,
                   {},
                   "STT backend not registered"});
  // The LM is the primary classifier for the cloud modes but only an
  // optional low-confidence fallback in local mode (Llama). Do not gate
  // local-mode readiness on it — a user who skipped the 1.9 GB download
  // is still fully operational on the rule parser.
  if (mode != "local" && !backends::lm_ready())
    out.push_back(
        {ReadinessBlocker::Source::LmBackend, {}, "LM backend not registered"});
  if (!backends::tts_ready())
    out.push_back({ReadinessBlocker::Source::TtsBackend,
                   {},
                   "TTS backend not registered"});

  // Cloud modes have no model files on disk to gate against — backend
  // registration alone is the readiness signal.
  if (mode == "openai" || mode == "mistral")
    return out;

  std::unordered_set<std::string> wanted;
  for (auto role : model_manifest::all_roles())
    wanted.insert(settings::voice_for_role(role));
  const std::string active_lang = settings::backend_language();

  // Lookup helper: human label for a manifest entry without rebuilding
  // it ad-hoc per row.
  auto label_for = [](const FileStatus &f) {
    for (const auto &e : model_manifest::all()) {
      if (e.kind == f.kind && e.voice_id == f.voice_id &&
          e.language == f.language)
        return e.display_name;
    }
    // Fallback — should not happen because Status.files mirrors
    // model_manifest::all().
    if (!f.voice_id.empty())
      return std::string("Voice ") + f.voice_id;
    return std::string("model");
  };

  static const auto state_word = [](FileState s) -> const char * {
    switch (s) {
    case FileState::NotChecked:
      return "not checked yet";
    case FileState::Missing:
      return "missing";
    case FileState::SizeMismatch:
      return "wrong size on disk";
    case FileState::Verifying:
      return "verifying";
    case FileState::HashMismatch:
      return "SHA256 mismatch";
    case FileState::Verified:
      return "verified but not yet loaded";
    case FileState::Loading:
      return "loading";
    case FileState::Ready:
      return "ready";
    case FileState::LoadError:
      return "load error";
    }
    return "unknown";
  };

  for (const auto &f : files) {
    if (!f.language.empty() && f.language != active_lang)
      continue;
    if (model_manifest::is_optional_ai_model(f.kind))
      continue; // Llama is download-on-demand; never a readiness blocker
    bool is_voice_kind = (f.kind == model_manifest::Kind::PiperVoice ||
                          f.kind == model_manifest::Kind::PiperVoiceConfig);
    if (is_voice_kind && wanted.count(f.voice_id) == 0)
      continue; // optional voice not assigned anywhere
    if (f.state == FileState::Ready)
      continue;
    out.push_back({ReadinessBlocker::Source::File, f,
                   label_for(f) + " - " + state_word(f.state)});
  }
  return out;
}

Status snapshot() {
  std::lock_guard<std::mutex> lk(g_mtx);
  return g_status;
}

void start() {
  // If a worker is already running, leave it alone — start() is a
  // hint, not a force.
  bool expected = false;
  if (!g_running.compare_exchange_strong(expected, true))
    return;

  g_should_exit = false;
  {
    // Explicit: a no-arg start() is a full sweep, never a stale
    // targeted run carried over from a prior start(key).
    std::lock_guard<std::mutex> lk(g_single_mtx);
    g_single_key.clear();
  }
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    seed_status_locked();
    // Reset transient states; preserve Ready entries whose backend
    // is still registered so a re-run after a download doesn't
    // spuriously reload the others.
    for (auto &f : g_status.files) {
      // Find the corresponding manifest entry to feed entry_loaded.
      // Three-way key (kind, voice_id, language) — two Whisper rows
      // share the same kind/voice_id but differ by language.
      const model_manifest::Entry *e = nullptr;
      for (const auto &cand : model_manifest::all()) {
        if (cand.kind == f.kind && cand.voice_id == f.voice_id &&
            cand.language == f.language) {
          e = &cand;
          break;
        }
      }
      if (e && f.state == FileState::Ready && entry_loaded(*e))
        continue;
      f.state = FileState::NotChecked;
      f.message.clear();
    }
  }

  // Join any prior thread before launching a new one.
  if (g_worker.joinable())
    g_worker.join();
  g_worker = std::thread(run_worker);
}

void start(const std::string &single_entry_key) {
  if (single_entry_key.empty()) {
    start();
    return;
  }
  // If a worker is already running, fall back to a full re-run later
  // — the user will not lose data, just convenience. We do NOT queue
  // single-key requests behind an in-flight worker; that would create
  // a thread of pending verifies the user cannot cancel.
  bool expected = false;
  if (!g_running.compare_exchange_strong(expected, true))
    return;

  g_should_exit = false;
  {
    std::lock_guard<std::mutex> lk(g_single_mtx);
    g_single_key = single_entry_key;
  }
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    seed_status_locked();
  }

  if (g_worker.joinable())
    g_worker.join();
  g_worker = std::thread(run_worker);
}

void clear_file_state(const std::string &single_entry_key) {
  if (single_entry_key.empty())
    return;
  // Find the matching manifest entry and (for Piper voices) the
  // sibling so both rows flip back to NotChecked together.
  const model_manifest::Entry *target = nullptr;
  for (const auto &e : model_manifest::all()) {
    if (model_manifest::entry_key(e) == single_entry_key) {
      target = &e;
      break;
    }
  }
  if (!target)
    return;
  auto reset = [](model_manifest::Kind k, const std::string &voice_id,
                  const std::string &lang) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto &f : g_status.files) {
      if (f.kind == k && f.voice_id == voice_id && f.language == lang) {
        f.state = FileState::NotChecked;
        f.message.clear();
        return;
      }
    }
  };
  reset(target->kind, target->voice_id, target->language);
#ifdef XPWELLYS_HAS_LOCAL_MODELS
  using K = model_manifest::Kind;
  if (target->kind == K::PiperVoice || target->kind == K::PiperVoiceConfig) {
    const K sibling =
        (target->kind == K::PiperVoice) ? K::PiperVoiceConfig : K::PiperVoice;
    reset(sibling, target->voice_id, target->language);
    // Unload from Piper so it is not still ready in memory after the
    // file disappears from disk.
    if (g_piper)
      g_piper->unload_voice(target->voice_id);
  }
#ifdef XPWELLYS_USE_LOCAL_INFERENCE
  else if (target->kind == K::WhisperModel) {
    backends::register_stt(nullptr);
  } else if (target->kind == K::LlamaModel) {
    backends::register_lm(nullptr);
  }
#endif
#endif // XPWELLYS_HAS_LOCAL_MODELS
}

void stop() {
  g_should_exit = true;
  if (g_worker.joinable()) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::seconds(6);
    while (g_running.load() && clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (g_worker.joinable())
      g_worker.join();
  }
  g_running = false;
  g_should_exit = false;
#ifdef XPWELLYS_USE_LOCAL_TTS
  g_piper.reset();
#endif
  // Drop the registered backend pointers so a subsequent start() —
  // typically the one fired by the UI when the user switches Backend
  // Mode — comes up against a clean slate. Without this the old
  // backend instance would linger (e.g. the 2 GB llama context would
  // stay in RAM after switching to OpenAI).
  backends::register_stt(nullptr);
  backends::register_lm(nullptr);
  backends::register_tts(nullptr);
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_status.files.clear();
  }
}

std::string espeakng_data_dir_for_piper() {
  return model_paths::espeakng_data_dir();
}

} // namespace backends::loader
