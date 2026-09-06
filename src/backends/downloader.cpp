/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/downloader.hpp"

#include "backends/loader.hpp"
#include "core/logging.hpp"
#include "persistence/model_manifest.hpp"
#include "persistence/model_paths.hpp"
#include "persistence/settings.hpp"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
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

namespace backends::downloader {

namespace {

namespace fs = std::filesystem;

// ── Per-entry state, shared between worker and UI ────────────────────
//
// Each manifest entry has one Progress slot, looked up by entry_key
// (kind + voice_id). UI calls snapshot() to copy the vector out under
// g_mtx.
std::mutex g_mtx;
std::vector<Progress> g_progress; // protected by g_mtx, mirrors manifest order

// FIFO of pending downloads. Holds entry_key strings so the worker
// can look up the live Entry on demand.
std::deque<std::string> g_queue; // protected by g_mtx

// Worker lifecycle.
std::thread g_worker;
std::atomic<bool> g_worker_alive{false};
std::condition_variable g_wake;

// `g_active_key` is the entry_key the worker is currently
// downloading. `g_cancel_active` is checked from libcurl's progress
// callback; the callback returning non-zero aborts the transfer.
std::atomic<bool> g_cancel_active{false};
// Used to signal the worker to exit altogether (vs. just cancelling
// the current download).
std::atomic<bool> g_should_stop{false};

// Live-updated by libcurl's xferinfo callback — read by the UI on
// every frame to feed a progress bar without locking.
std::atomic<uint64_t> g_active_bytes_total{0};
std::atomic<uint64_t> g_active_bytes_downloaded{0};
std::atomic<int> g_active_index{-1}; // -1 = no active download

void seed_progress_locked() {
  if (!g_progress.empty())
    return;
  for (const auto &e : model_manifest::all()) {
    g_progress.push_back(
        {e.kind, e.voice_id, e.language, State::Idle, e.size_bytes, 0, {}});
  }
}

int find_index_locked(const std::string &key) {
  for (size_t i = 0; i < g_progress.size(); ++i) {
    const auto &p = g_progress[i];
    model_manifest::Entry tmp{};
    tmp.kind = p.kind;
    tmp.voice_id = p.voice_id;
    tmp.language = p.language;
    if (model_manifest::entry_key(tmp) == key)
      return static_cast<int>(i);
  }
  return -1;
}

const model_manifest::Entry *find_entry_by_key(const std::string &key) {
  for (const auto &e : model_manifest::all()) {
    if (model_manifest::entry_key(e) == key)
      return &e;
  }
  return nullptr;
}

void set_state(const std::string &key, State s, std::string msg = {},
               uint64_t bytes_done = UINT64_MAX) {
  std::lock_guard<std::mutex> lk(g_mtx);
  int idx = find_index_locked(key);
  if (idx < 0)
    return;
  auto &p = g_progress[static_cast<size_t>(idx)];
  p.state = s;
  p.error_message = std::move(msg);
  if (bytes_done != UINT64_MAX)
    p.bytes_downloaded = bytes_done;
}

uint64_t file_size(const std::string &path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0)
    return 0;
  if (!S_ISREG(st.st_mode))
    return 0;
  return static_cast<uint64_t>(st.st_size);
}

// libcurl write callback — append straight to the open .part file.
size_t write_to_file(char *buf, size_t size, size_t nmemb, void *user) {
  auto *fp = static_cast<std::FILE *>(user);
  return std::fwrite(buf, size, nmemb, fp);
}

// libcurl xferinfo callback — runs on the transfer thread (our worker
// here).
int xferinfo_cb(void * /*user*/, curl_off_t dltotal, curl_off_t dlnow,
                curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  if (dltotal > 0)
    g_active_bytes_total = static_cast<uint64_t>(dltotal);
  if (dlnow >= 0)
    g_active_bytes_downloaded = static_cast<uint64_t>(dlnow);
  if (g_cancel_active.load() || g_should_stop.load())
    return 1;
  return 0;
}

// One-shot download attempt for a single manifest entry. Returns true
// on success (file present at final path, SHA256-verified).
bool download_one(const model_manifest::Entry &e) {
  const std::string key = model_manifest::entry_key(e);
  const std::string final_path = model_paths::models_dir() + "/" + e.filename;
  const std::string part_path = final_path + ".part";

  // If the final file already exists with the right size + hash, we
  // are done.
  if (model_manifest::size_matches(e, final_path)) {
    if (model_manifest::sha256_file(final_path) == e.sha256_hex) {
      set_state(key, State::Done, {}, e.size_bytes);
      return true;
    }
    // size matches but hash doesn't — fall through to redownload.
  }

  // Disk-space precheck.
  uint64_t resume_from = file_size(part_path);
  if (resume_from > e.size_bytes) {
    std::error_code ec;
    fs::remove(part_path, ec);
    resume_from = 0;
  }
  uint64_t need = e.size_bytes - resume_from + (1024ULL * 1024); // 1 MB slack

  std::error_code sec;
  fs::space_info si = fs::space(model_paths::models_dir(), sec);
  if (!sec) {
    if (si.available < need) {
      char msg[160];
      std::snprintf(msg, sizeof(msg),
                    "Need %.1f MB free; only %.1f MB available on this volume.",
                    static_cast<double>(need) / 1024.0 / 1024.0,
                    static_cast<double>(si.available) / 1024.0 / 1024.0);
      set_state(key, State::InsufficientDisk, msg, resume_from);
      return false;
    }
  }

  std::FILE *fp = std::fopen(part_path.c_str(), resume_from > 0 ? "ab" : "wb");
  if (!fp) {
    set_state(key, State::Failed, "Cannot open " + part_path + " for writing",
              resume_from);
    return false;
  }

  set_state(key, State::Downloading, {}, resume_from);

  CURL *curl = curl_easy_init();
  if (!curl) {
    std::fclose(fp);
    set_state(key, State::Failed, "curl_easy_init() failed", resume_from);
    return false;
  }

  g_active_bytes_total = e.size_bytes;
  g_active_bytes_downloaded = resume_from;
  g_cancel_active = false;

  curl_easy_setopt(curl, CURLOPT_URL, e.url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
  if (resume_from > 0)
    curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                     static_cast<curl_off_t>(resume_from));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  std::fclose(fp);

  if (rc == CURLE_ABORTED_BY_CALLBACK) {
    set_state(key, State::Cancelled, "Cancelled by user", file_size(part_path));
    return false;
  }
  if (rc != CURLE_OK) {
    char msg[256];
    std::snprintf(msg, sizeof(msg), "Download failed (%s; HTTP %ld)",
                  curl_easy_strerror(rc), http_code);
    set_state(key, State::Failed, msg, file_size(part_path));
    return false;
  }

  uint64_t got = file_size(part_path);
  if (got != e.size_bytes) {
    char msg[160];
    std::snprintf(
        msg, sizeof(msg),
        "Wrong size after download (got %llu, expected %llu) - partial "
        "transfer; click Download again to resume.",
        static_cast<unsigned long long>(got),
        static_cast<unsigned long long>(e.size_bytes));
    set_state(key, State::Failed, msg, got);
    return false;
  }

  set_state(key, State::Verifying, {}, got);
  std::string actual = model_manifest::sha256_file(part_path);
  if (actual != e.sha256_hex) {
    std::error_code ec;
    fs::remove(part_path, ec);
    set_state(key, State::Failed,
              "SHA256 mismatch after download - file deleted. If this keeps "
              "happening, the upstream URL may have changed; check the "
              "README's manual-fallback table.",
              0);
    return false;
  }

  std::error_code ec;
  fs::rename(part_path, final_path, ec);
  if (ec) {
    set_state(key, State::Failed,
              "Could not rename .part to final filename: " + ec.message(), got);
    return false;
  }

  set_state(key, State::Done, {}, e.size_bytes);
  logging::info("Downloaded + verified %s", e.filename.c_str());
  return true;
}

void worker_loop() {
  try {
    while (true) {
      std::string next_key;
      {
        std::unique_lock<std::mutex> lk(g_mtx);
        g_wake.wait(lk,
                    []() { return !g_queue.empty() || g_should_stop.load(); });
        if (g_should_stop.load())
          break;
        next_key = g_queue.front();
        g_queue.pop_front();

        g_active_index = find_index_locked(next_key);
      }

      const model_manifest::Entry *entry = find_entry_by_key(next_key);
      if (!entry) {
        // Manifest changed since enqueue; skip silently.
        g_active_index = -1;
        continue;
      }

      try {
        download_one(*entry);
      } catch (const std::exception &e) {
        set_state(next_key, State::Failed,
                  std::string("Internal error: ") + e.what(),
                  file_size(model_paths::models_dir() + "/" + entry->filename +
                            ".part"));
        logging::error("downloader: download_one threw: %s", e.what());
      } catch (...) {
        set_state(next_key, State::Failed, "Internal error: unknown exception",
                  file_size(model_paths::models_dir() + "/" + entry->filename +
                            ".part"));
        logging::error("downloader: download_one threw an unknown exception");
      }

      g_active_index = -1;
      g_active_bytes_total = 0;
      g_active_bytes_downloaded = 0;

      // Re-verify ONLY the file we just finished — the full sweep
      // would re-hash every model on disk (2 GB Llama incl.), which
      // is what made the post-download wait painful pre-v3.1.
      try {
        backends::loader::start(model_manifest::entry_key(*entry));
      } catch (const std::exception &e) {
        logging::error("downloader: loader::start(key) threw: %s", e.what());
      } catch (...) {
        logging::error("downloader: loader::start(key) threw unknown");
      }
    }
  } catch (const std::exception &e) {
    logging::error("downloader: worker_loop threw at outer level: %s",
                   e.what());
  } catch (...) {
    logging::error("downloader: worker_loop threw unknown at outer level");
  }
  g_worker_alive = false;
}

void ensure_worker() {
  bool expected = false;
  if (!g_worker_alive.compare_exchange_strong(expected, true)) {
    g_wake.notify_one();
    return;
  }
  g_should_stop = false;
  if (g_worker.joinable())
    g_worker.join();
  g_worker = std::thread(worker_loop);
}

} // namespace

std::vector<Progress> snapshot() {
  std::lock_guard<std::mutex> lk(g_mtx);
  seed_progress_locked();

  int active = g_active_index.load();
  if (active >= 0 && active < static_cast<int>(g_progress.size())) {
    auto &p = g_progress[static_cast<size_t>(active)];
    if (p.state == State::Downloading) {
      p.bytes_total = g_active_bytes_total.load();
      p.bytes_downloaded = g_active_bytes_downloaded.load();
    }
  }
  return g_progress;
}

uint64_t free_space_bytes() {
  std::error_code ec;
  fs::space_info si = fs::space(model_paths::models_dir(), ec);
  if (ec)
    return 0;
  return si.available;
}

uint64_t bytes_still_required(bool include_all_languages) {
  uint64_t total = 0;
  const std::string active_lang = settings::backend_language();
  for (const auto &e : model_manifest::all()) {
    if (e.optional)
      continue;
    // Optional AI models (Llama) are downloaded on explicit request only;
    // they must not inflate the language-pack "still required" total.
    if (model_manifest::is_optional_ai_model(e.kind))
      continue;
    if (!include_all_languages && !e.language.empty() &&
        e.language != active_lang)
      continue;
    const std::string final_path = model_paths::models_dir() + "/" + e.filename;
    uint64_t have = file_size(final_path);
    if (have == 0)
      have = file_size(final_path + ".part");
    if (have >= e.size_bytes)
      continue;
    total += e.size_bytes - have;
  }
  return total;
}

void enqueue(const model_manifest::Entry &entry) {
  const std::string key = model_manifest::entry_key(entry);
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    seed_progress_locked();

    // De-dup: skip if this entry is already queued or actively
    // downloading.
    for (const auto &k : g_queue)
      if (k == key)
        return;
    int active = g_active_index.load();
    if (active >= 0 && active < static_cast<int>(g_progress.size())) {
      const auto &ap = g_progress[static_cast<size_t>(active)];
      model_manifest::Entry tmp{};
      tmp.kind = ap.kind;
      tmp.voice_id = ap.voice_id;
      tmp.language = ap.language;
      if (model_manifest::entry_key(tmp) == key)
        return;
    }

    g_queue.push_back(key);
    int idx = find_index_locked(key);
    if (idx >= 0) {
      auto &p = g_progress[static_cast<size_t>(idx)];
      p.state = State::Queued;
      p.error_message.clear();
    }
  }
  ensure_worker();
}

size_t enqueue_all_missing(bool include_all_languages) {
  size_t n = 0;
  const std::string active_lang = settings::backend_language();

  // Collect the four voice slots once so we can decide whether an
  // optional voice is actually needed: if the user assigned an
  // optional voice to ATIS, the previous "skip-all-optional" rule
  // would never download it and the Models tab would sit at
  // "Missing" forever.
  std::unordered_set<std::string> assigned_voices;
  for (auto role : model_manifest::all_roles())
    assigned_voices.insert(settings::voice_for_role(role));

  auto status = backends::loader::snapshot();
  for (const auto &fs : status.files) {
    using FS = backends::loader::FileState;
    if (fs.state != FS::Missing && fs.state != FS::SizeMismatch &&
        fs.state != FS::HashMismatch)
      continue;
    if (!include_all_languages && !fs.language.empty() &&
        fs.language != active_lang)
      continue;
    // Match on (kind, voice_id, language) — two Whisper entries share
    // the same kind/voice_id but differ by language.
    for (const auto &e : model_manifest::all()) {
      if (e.kind == fs.kind && e.voice_id == fs.voice_id &&
          e.language == fs.language) {
        // Never auto-pull large optional AI models (Llama, 1.9 GB) via
        // Download-All — they get an explicit per-row Download click.
        if (model_manifest::is_optional_ai_model(e.kind))
          break;
        if (e.optional) {
          // Optional Piper voice: only enqueue when the user picked
          // it for a role. Unassigned optional voices still need an
          // explicit per-row Download click — keeps "Download All"
          // from pulling all four optional voices unsolicited.
          if (e.voice_id.empty() || assigned_voices.count(e.voice_id) == 0)
            break;
        }
        enqueue(e);
        ++n;
        break;
      }
    }
  }
  return n;
}

void cancel(const model_manifest::Entry &entry) {
  const std::string key = model_manifest::entry_key(entry);
  bool was_active = false;
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto it = g_queue.begin(); it != g_queue.end();) {
      if (*it == key) {
        it = g_queue.erase(it);
        int idx = find_index_locked(key);
        if (idx >= 0) {
          auto &p = g_progress[static_cast<size_t>(idx)];
          p.state = State::Cancelled;
          p.error_message = "Cancelled before start";
        }
      } else {
        ++it;
      }
    }

    int active = g_active_index.load();
    if (active >= 0 && active < static_cast<int>(g_progress.size())) {
      const auto &ap = g_progress[static_cast<size_t>(active)];
      model_manifest::Entry tmp{};
      tmp.kind = ap.kind;
      tmp.voice_id = ap.voice_id;
      if (model_manifest::entry_key(tmp) == key)
        was_active = true;
    }
  }
  if (was_active)
    g_cancel_active = true;
}

void force_redownload(const model_manifest::Entry &entry) {
  // 1. Cancel any in-flight or queued transfer for this entry.
  cancel(entry);

  // 2. Drop the loader's record of this file so the UI does not
  //    briefly flash "Ready" while the new download runs.
  const std::string key = model_manifest::entry_key(entry);
  try {
    backends::loader::clear_file_state(key);
  } catch (const std::exception &e) {
    logging::error("downloader: clear_file_state threw: %s", e.what());
  }

  // 3. Delete final + .part. Both are best-effort: a transient FS
  //    error here just means the enqueue() below downloads on top
  //    of whatever is there and the SHA256 verify catches the rest.
  const std::string final_path =
      model_paths::models_dir() + "/" + entry.filename;
  std::error_code ec;
  fs::remove(final_path, ec);
  fs::remove(final_path + ".part", ec);

  // 4. Reset our own Progress row so the UI immediately reflects 0%.
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    seed_progress_locked();
    int idx = find_index_locked(key);
    if (idx >= 0) {
      auto &p = g_progress[static_cast<size_t>(idx)];
      p.state = State::Idle;
      p.bytes_downloaded = 0;
      p.error_message.clear();
    }
  }

  // 5. Hand it back to the queue.
  enqueue(entry);
}

void stop() {
  g_should_stop = true;
  g_cancel_active = true;
  g_wake.notify_all();

  if (g_worker.joinable()) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::seconds(5);
    while (g_worker_alive.load() && clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (g_worker.joinable())
      g_worker.join();
  }
  g_worker_alive = false;
  g_should_stop = false;
  g_cancel_active = false;
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_queue.clear();
  }
}

} // namespace backends::downloader
