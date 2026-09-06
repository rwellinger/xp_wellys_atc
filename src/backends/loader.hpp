/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_LOADER_HPP
#define BACKENDS_LOADER_HPP

#include "persistence/model_manifest.hpp"

#include <string>
#include <vector>

namespace backends::loader {

enum class FileState {
  NotChecked,   // initial; verification has not run yet
  Missing,      // file is not on disk
  SizeMismatch, // present but wrong size — likely partial download
  Verifying,    // SHA256 in progress on the worker thread
  HashMismatch, // size OK, hash differs from manifest — corrupt
  Verified,     // ready to be loaded into the inference backend
  Loading,      // backend is opening the model file
  Ready,        // backend registered with the manager
  LoadError,    // file OK on disk but the inference lib rejected it
};

struct FileStatus {
  model_manifest::Kind kind;
  // Non-empty for Piper entries — pairs with `kind` to uniquely
  // identify which voice this row refers to. Empty for Whisper/Llama.
  std::string voice_id;
  // ISO-639-1 language tag of the manifest entry ("en", "de"), or
  // empty for language-agnostic entries (Llama). Lets all_ready()
  // skip the wrong-language Whisper/voice rows.
  std::string language;
  FileState state = FileState::NotChecked;
  // Last error / informational message — surfaced verbatim by the UI.
  std::string message;
};

// Why the readiness gate is failing right now. Mirrors all_ready()
// logic so the UI can tell the user exactly which file or backend is
// the holdout, instead of just "Local models not ready".
struct ReadinessBlocker {
  enum class Source {
    SttBackend, // backends::stt_ready() == false
    LmBackend,  // backends::lm_ready()  == false
    TtsBackend, // backends::tts_ready() == false
    File,       // one FileStatus row is not Ready
  };
  Source source;
  // Populated when source == File; describes which row is blocking.
  FileStatus file{};
  // Human-readable one-liner — surface verbatim in the UI.
  std::string description;
};

struct Status {
  std::vector<FileStatus> files; // mirrors model_manifest::all() order

  // True only when STT + LM + the four currently-assigned voices are
  // Ready. Optional voices not in any role's slot are ignored. This
  // is the gate the PTT path consults.
  bool all_ready() const;

  // Enumerate every reason all_ready() would currently return false.
  // Empty vector when everything is green. Cheap (walks `files` once),
  // safe to call every UI frame.
  std::vector<ReadinessBlocker> readiness_blockers() const;
};

// Snapshot the current status. Safe to call from the main thread on
// every frame — internally locks a mutex shared with the worker.
Status snapshot();

// Kick off verification + load on a worker thread. Idempotent: a
// second call while a verification is already in flight is ignored.
// Re-runs are how the downloader signals "this file is now on disk,
// please re-check" — the downloader (P5) calls this after a
// successful download finishes.
void start();

// Targeted variant: only verify (and, if Verified, load) the single
// manifest entry whose key matches `single_entry_key`. For Piper
// voices the matching .onnx + .onnx.json sibling are processed
// together so the voice is fully usable. Useful from the Models tab's
// per-row "Re-verify" button — avoids the 2 GB Llama SHA256 that the
// full sweep would re-do. No-op when the key is unknown.
void start(const std::string &single_entry_key);

// Reset the loader's FileState for `single_entry_key` (and its Piper
// sibling) back to NotChecked and unload any registered backend that
// owns it. Used by "Force re-download" before deleting the file on
// disk so the UI does not flash "Ready" while the download runs.
void clear_file_state(const std::string &single_entry_key);

// Block until the worker thread (if any) has exited. Called from
// XPluginDisable. Capped internally at a few seconds in case a
// SHA256 of the 2 GB llama model is mid-stream.
void stop();

// Plugin-relative path for the espeak-ng-data directory used by
// Piper. Returned even if the directory does not exist (caller
// should verify). Resolved once via model_paths::init().
std::string espeakng_data_dir_for_piper();

} // namespace backends::loader

#endif // BACKENDS_LOADER_HPP
