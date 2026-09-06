/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_DOWNLOADER_HPP
#define BACKENDS_DOWNLOADER_HPP

#include "persistence/model_manifest.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace backends::downloader {

enum class State {
  Idle,             // no download active for this entry
  Queued,           // waiting for the worker to pick it up
  Downloading,      // libcurl is streaming to <file>.part
  Verifying,        // SHA256 in progress on the freshly written file
  Done,             // .part renamed to final filename, loader notified
  Failed,           // see error_message; .part may still exist for resume
  Cancelled,        // user cancelled; .part kept for later resume
  InsufficientDisk, // pre-flight disk-space check rejected the download
};

struct Progress {
  // (kind, voice_id, language) jointly identify the manifest entry.
  // voice_id is empty for Whisper/Llama; language is empty for
  // language-agnostic entries (Llama).
  model_manifest::Kind kind;
  std::string voice_id;
  std::string language;
  State state = State::Idle;
  // Total expected size and bytes already on disk (.part + already-
  // resumed). The UI feeds these directly into a progress bar.
  uint64_t bytes_total = 0;
  uint64_t bytes_downloaded = 0;
  std::string error_message;
};

// Snapshot of every manifest entry's download state — same length and
// order as model_manifest::all(). Safe to call from the main thread
// every frame.
std::vector<Progress> snapshot();

// Free space in bytes at <plugin>/Resources/models/. Used by the UI
// to warn the user before they kick off a 2.3 GB download. Returns 0
// on stat failure.
uint64_t free_space_bytes();

// Sum of (entry.size_bytes - bytes_already_present) across every
// manifest entry that is not yet Verified. Optional voices are
// excluded — they only get pulled when the user explicitly enqueues
// them. By default only entries matching the active backend language
// (settings::backend_language()) count; pass `include_all_languages =
// true` to sum every language (used by the "Show all languages"
// toggle in the Models tab).
uint64_t bytes_still_required(bool include_all_languages = false);

// Queue a single manifest entry for download. If the file is already
// present + size-matched, this is a no-op (state goes Done
// immediately). If a download for that entry is already queued or in
// flight, also no-op. The Entry pointer must outlive the queue (in
// practice manifest entries are static).
void enqueue(const model_manifest::Entry &entry);

// Queue every required manifest entry that is currently
// Missing/SizeMismatch/HashMismatch in `backends::loader`. Optional
// voices are skipped — call enqueue() per voice for those. By default
// only entries matching the active backend language are queued; pass
// `include_all_languages = true` to fetch every language at once.
size_t enqueue_all_missing(bool include_all_languages = false);

// Cancel a single in-flight or queued download. The .part file is
// preserved on disk so a future enqueue() can resume from where the
// transfer stopped.
void cancel(const model_manifest::Entry &entry);

// Force a fresh download of `entry`: cancels any in-flight transfer
// for it, deletes the final file and any stale `.part`, asks the
// loader to drop the FileState back to NotChecked, and enqueues the
// download. Use from the Models tab "Force re-download" button when
// the file is on disk but suspect (LoadError, hash mismatch, or just
// "I want to grab it again").
void force_redownload(const model_manifest::Entry &entry);

// Cancel everything + join the worker thread.
void stop();

} // namespace backends::downloader

#endif // BACKENDS_DOWNLOADER_HPP
