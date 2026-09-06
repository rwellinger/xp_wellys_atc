/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/manager.hpp"

#include "audio/pcm_resample.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <curl/curl.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace backends {

// ── Registered backends (owned here) ─────────────────────────────────
//
// The plugin registers concrete instances after model verification.
// Headless tools (atc_repl, tests) never register and rely on the
// short-circuit paths below.
namespace {

std::mutex g_backend_mtx;
std::unique_ptr<ISpeechToText> g_stt;
std::unique_ptr<ILanguageModel> g_lm;
std::unique_ptr<ITextToSpeech> g_tts;

// One mutex per stage covers both the call into the model (which is
// not thread-safe inside whisper.cpp / llama.cpp / Piper) and the
// teardown path. We do not parallelise multiple inferences of the same
// stage; the user-facing pipeline is sequential.
std::mutex g_stt_call_mtx;
std::mutex g_lm_call_mtx;
std::mutex g_tts_call_mtx;

// Pending main-thread callbacks. Worker threads enqueue here; the
// X-Plane flight loop drains via drain_callback_queue().
std::mutex g_cb_mtx;
std::deque<std::function<void()>> g_callbacks;

// Live worker thread count. stop() waits until this hits zero so
// joined threads do not race the unload.
std::atomic<int> g_active_workers{0};

// Last observed inference durations per stage, in milliseconds. 0
// means "not yet measured". Updated by worker threads after each
// call into the model.
std::atomic<uint32_t> g_last_stt_ms{0};
std::atomic<uint32_t> g_last_lm_ms{0};
std::atomic<uint32_t> g_last_tts_ms{0};

void enqueue_callback(std::function<void()> fn) {
  std::lock_guard<std::mutex> lk(g_cb_mtx);
  g_callbacks.emplace_back(std::move(fn));
}

// Spawn a detached worker. The atomic counter tracks lifetime so
// stop() can wait for them; we don't keep std::thread handles around.
//
// Workers are deprioritized against X-Plane's renderer thread. Whisper STT and
// Llama LM both drive the GPU (Metal on macOS, Vulkan on Windows) — without a
// scheduling hint they compete equally with X-Plane and FPS drops during
// landing-phase ATC calls. TTS is CPU-only, so the hint is harmless there.
template <class Fn> void spawn_worker(Fn &&fn) {
  g_active_workers.fetch_add(1, std::memory_order_relaxed);
  std::thread t([fn = std::forward<Fn>(fn)]() mutable {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(_WIN32)
    // Windows has no QoS class equivalent; thread priority is the lever.
    // BELOW_NORMAL rather than LOWEST: these workers must still finish
    // promptly (the pilot is waiting on the reply), they just must not
    // preempt the renderer. This used to be a deliberate no-op while the
    // Windows slice was cloud-only with nothing to contend for — local
    // inference (whisper/llama on Vulkan) makes it real (#23).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    fn();
    g_active_workers.fetch_sub(1, std::memory_order_relaxed);
  });
  t.detach();
}

// Whisper performs better on transcripts when seeded with relevant
// proper nouns (airport names, callsigns). For now we squash the
// hint into the head of the audio buffer's prompt indirectly — the
// concrete WhisperStt implementation handles the hint as its second
// argument and decides whether to feed it to whisper_full_params.
// Manager just passes it through.

// Trim a string of leading/trailing whitespace + surrounding
// punctuation. Used to normalise classifier output.
std::string trim(const std::string &in) {
  size_t b = 0, e = in.size();
  while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\n' ||
                   in[b] == '\r' || in[b] == '"' || in[b] == '\''))
    ++b;
  while (e > b && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\n' ||
                   in[e - 1] == '\r' || in[e - 1] == '"' || in[e - 1] == '\'' ||
                   in[e - 1] == '.' || in[e - 1] == ','))
    --e;
  return in.substr(b, e - b);
}

} // namespace

// ── Lifecycle ────────────────────────────────────────────────────────

void init() {
  // libcurl global init must run before any thread calls
  // curl_easy_init(). On macOS the lazy auto-init is not thread-safe
  // and has been observed to crash on the first call from a worker
  // thread. Calling curl_global_init from the main thread at plugin
  // startup avoids the race. Idempotent: second calls return OK.
  CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (rc != CURLE_OK) {
    // Not fatal: downloads will fail with a clear curl error later if
    // libcurl is broken, but the rest of the plugin still works (e.g.
    // a user with manually-dropped models bypasses the downloader).
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "[xp_wellys_vfr_atc][ERROR] curl_global_init failed: %s\n",
                  curl_easy_strerror(rc));
    // logging::set_sink may not be wired yet at this exact call site
    // depending on init order; fall back to fprintf which the plugin
    // is allowed to use during init.
    std::fputs(buf, stderr);
  }
}

void stop() {
  // Wait for any in-flight worker to finish before tearing down. The
  // wait is bounded by the longest possible inference (~1 s for
  // llama.cpp on M1); cap at 5 s and drop the backend regardless so a
  // wedged worker cannot block plugin unload indefinitely.
  using clock = std::chrono::steady_clock;
  auto deadline = clock::now() + std::chrono::seconds(5);
  while (g_active_workers.load() > 0 && clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  {
    std::lock_guard<std::mutex> lk(g_backend_mtx);
    g_stt.reset();
    g_lm.reset();
    g_tts.reset();
  }
  {
    std::lock_guard<std::mutex> lk(g_cb_mtx);
    g_callbacks.clear();
  }

  // Pair with the init-time curl_global_init. Safe even if init
  // failed — curl_global_cleanup is documented as no-op when the
  // library was not initialised.
  curl_global_cleanup();
}

void register_stt(std::unique_ptr<ISpeechToText> stt) {
  std::lock_guard<std::mutex> lk(g_backend_mtx);
  g_stt = std::move(stt);
}
void register_lm(std::unique_ptr<ILanguageModel> lm) {
  std::lock_guard<std::mutex> lk(g_backend_mtx);
  g_lm = std::move(lm);
}
void register_tts(std::unique_ptr<ITextToSpeech> tts) {
  std::lock_guard<std::mutex> lk(g_backend_mtx);
  g_tts = std::move(tts);
}

bool stt_ready() {
  std::lock_guard<std::mutex> lk(g_backend_mtx);
  return g_stt != nullptr;
}
bool lm_ready() {
  std::lock_guard<std::mutex> lk(g_backend_mtx);
  return g_lm != nullptr;
}
bool tts_ready() {
  std::lock_guard<std::mutex> lk(g_backend_mtx);
  return g_tts != nullptr;
}

uint32_t last_stt_ms() { return g_last_stt_ms.load(); }
uint32_t last_lm_ms() { return g_last_lm_ms.load(); }
uint32_t last_tts_ms() { return g_last_tts_ms.load(); }

void drain_callback_queue() {
  // Drain under the lock-free pattern: swap into a local deque, run
  // outside the lock, so callbacks that re-enqueue (uncommon but
  // legal) do not deadlock.
  std::deque<std::function<void()>> local;
  {
    std::lock_guard<std::mutex> lk(g_cb_mtx);
    local.swap(g_callbacks);
  }
  for (auto &cb : local) {
    if (cb)
      cb();
  }
}

// ── STT ──────────────────────────────────────────────────────────────

namespace stt {

void transcribe_async(std::vector<int16_t> pcm16, uint32_t sample_rate_hz,
                      std::function<void(TranscriptResult)> callback,
                      std::string airport_context) {
  if (!callback)
    return;

  if (!stt_ready()) {
    enqueue_callback([cb = std::move(callback)]() {
      TranscriptResult r;
      r.success = false;
      r.text = "STT backend not loaded";
      cb(std::move(r));
    });
    return;
  }

  spawn_worker([pcm16 = std::move(pcm16), sample_rate_hz,
                airport_context = std::move(airport_context),
                cb = std::move(callback)]() mutable {
    // Convert outside the call mutex so concurrent callers get the
    // most parallelism we can offer without serialising on whisper.
    //
    // Anything other than 16 kHz means the capture layer could not give
    // us the target rate and pcm_resample has to band-limit in software.
    // That is the degraded path (issue #85) -- say so in Log.txt rather
    // than leaving it to be reconstructed from sample counts.
    if (sample_rate_hz != 0 && sample_rate_hz != pcm_resample::kTargetRateHz)
      logging::info("[STT] capture rate %u Hz != 16000 Hz, resampling in "
                    "software (fallback path)",
                    static_cast<unsigned>(sample_rate_hz));
    std::vector<float> pcm32 =
        pcm_resample::to_float_16k(pcm16, sample_rate_hz);

    std::string transcript;
    {
      std::lock_guard<std::mutex> lk(g_stt_call_mtx);
      ISpeechToText *stt_ptr = nullptr;
      {
        std::lock_guard<std::mutex> lk2(g_backend_mtx);
        stt_ptr = g_stt.get();
      }
      if (stt_ptr) {
        auto t0 = std::chrono::steady_clock::now();
        transcript = stt_ptr->transcribe(pcm32, airport_context);
        auto t1 = std::chrono::steady_clock::now();
        g_last_stt_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                .count());
      }
    }

    TranscriptResult r;
    if (transcript.empty()) {
      r.success = false;
      r.text = "Transcription failed";
      r.quality = 0.0f;
    } else {
      r.success = true;
      r.text = std::move(transcript);
      // Quality scoring lived in the OpenAI client where Whisper's
      // logprobs were not surfaced anyway. Until we extract token
      // logprobs from whisper.cpp, treat everything that came back as
      // a confident transcript. The engine still applies its own
      // length / blacklist filters for noise.
      r.quality = 1.0f;
    }
    enqueue_callback(
        [cb = std::move(cb), r = std::move(r)]() mutable { cb(std::move(r)); });
  });
}

} // namespace stt

// ── LM ───────────────────────────────────────────────────────────────

namespace lm {

void respond_async(std::string system_prompt, std::string user_text,
                   std::function<void(std::string, bool)> callback) {
  if (!callback)
    return;
  if (!lm_ready()) {
    enqueue_callback([cb = std::move(callback)]() { cb({}, false); });
    return;
  }

  spawn_worker([system_prompt = std::move(system_prompt),
                user_text = std::move(user_text),
                cb = std::move(callback)]() mutable {
    std::string reply;
    {
      std::lock_guard<std::mutex> lk(g_lm_call_mtx);
      ILanguageModel *lm_ptr = nullptr;
      {
        std::lock_guard<std::mutex> lk2(g_backend_mtx);
        lm_ptr = g_lm.get();
      }
      if (lm_ptr) {
        auto t0 = std::chrono::steady_clock::now();
        reply = lm_ptr->respond(system_prompt, user_text);
        auto t1 = std::chrono::steady_clock::now();
        g_last_lm_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                .count());
      }
    }
    bool ok = !reply.empty();
    enqueue_callback([cb = std::move(cb), reply = std::move(reply),
                      ok]() mutable { cb(std::move(reply), ok); });
  });
}

// Build a GBNF grammar that forces the model output into a strict
// JSON shape with the `intent` field constrained to one of the
// supplied valid_intents (plus "_INVALID"). Whitespace is permitted
// only as single spaces between elements; the model has no room to
// emit prose around the JSON.
//
// Example for valid_intents={"READBACK","REQUEST_TAXI"}:
//   root   ::= "{" ws "\"intent\":" ws intent ws "," ws "\"repaired\":" ws
//   repaired ws "," ws "\"whisper_fix\":" ws bool ws "}" intent ::=
//   "\"READBACK\"" | "\"REQUEST_TAXI\"" | "\"_INVALID\"" repaired ::= "\""
//   char* "\"" bool   ::= "true" | "false" ws     ::= " "? char   ::= [^"\\] |
//   "\\" ["\\bfnrt/]
// Currently unused — grammar-constrained generation is disabled in
// classify_with_repair_async (see comment there). Kept compiled so
// re-enablement is one switch in the worker, no code resurrection.
[[maybe_unused]] static std::string
build_classify_grammar(const std::vector<std::string> &valid_intents) {
  std::string g;
  g += "root ::= \"{\" ws \"\\\"intent\\\":\" ws intent ws \",\" ws "
       "\"\\\"repaired\\\":\" ws repaired ws \",\" ws \"\\\"whisper_fix\\\":\" "
       "ws bool ws \"}\"\n";
  g += "intent ::= ";
  bool first = true;
  for (const auto &v : valid_intents) {
    if (!first)
      g += " | ";
    g += "\"\\\"" + v + "\\\"\"";
    first = false;
  }
  if (!first)
    g += " | ";
  g += "\"\\\"_INVALID\\\"\"\n";
  g += "repaired ::= \"\\\"\" char* \"\\\"\"\n";
  g += "bool ::= \"true\" | \"false\"\n";
  g += "ws ::= \" \"?\n";
  // Allow most printable ASCII inside the repaired-string value but
  // require backslash-escaping for the JSON-special chars. Keeps the
  // model from emitting unescaped quotes that would break parsing.
  g += "char ::= [^\"\\\\] | \"\\\\\" [\"\\\\bfnrt/]\n";
  return g;
}

// Tiny tolerant JSON extractor: pulls the value of a single top-level
// key from a JSON-ish object string. The grammar guarantees the
// shape, but we still keep the parsing defensive in case the model
// emits stray whitespace or escapes the grammar drops on edge cases.
static bool json_extract_string(const std::string &s, const std::string &key,
                                std::string *out) {
  std::string needle = "\"" + key + "\"";
  auto p = s.find(needle);
  if (p == std::string::npos)
    return false;
  p = s.find(':', p + needle.size());
  if (p == std::string::npos)
    return false;
  // Skip whitespace
  ++p;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n'))
    ++p;
  if (p >= s.size() || s[p] != '"')
    return false;
  ++p;
  std::string value;
  while (p < s.size() && s[p] != '"') {
    if (s[p] == '\\' && p + 1 < s.size()) {
      char c = s[p + 1];
      switch (c) {
      case 'n':
        value += '\n';
        break;
      case 't':
        value += '\t';
        break;
      case 'r':
        value += '\r';
        break;
      default:
        // '"', '\\', '/' and unknown escapes: append literal char.
        value += c;
        break;
      }
      p += 2;
      continue;
    }
    value += s[p];
    ++p;
  }
  *out = value;
  return true;
}

static bool json_extract_bool(const std::string &s, const std::string &key,
                              bool *out) {
  std::string needle = "\"" + key + "\"";
  auto p = s.find(needle);
  if (p == std::string::npos)
    return false;
  p = s.find(':', p + needle.size());
  if (p == std::string::npos)
    return false;
  ++p;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n'))
    ++p;
  if (s.compare(p, 4, "true") == 0) {
    *out = true;
    return true;
  }
  if (s.compare(p, 5, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

void classify_with_repair_async(std::string transcript,
                                std::string system_prompt,
                                const std::vector<std::string> &valid_intents,
                                std::function<void(ClassifyResult)> callback) {
  if (!callback)
    return;
  if (!lm_ready()) {
    enqueue_callback([cb = std::move(callback)]() {
      ClassifyResult r;
      r.intent_name = "_INVALID";
      r.success = false;
      cb(std::move(r));
    });
    return;
  }

  // Grammar-constrained generation is currently disabled (Nov 2026):
  // a live X-Plane crash was reported on the first call after enabling
  // GBNF + top_k + low temp in respond_constrained. Without a stack
  // trace the most likely cause is the grammar sampler interacting
  // poorly with top_k filtering when no grammar-allowed tokens remain
  // in the top_k window. Until that is debugged, we run the prompt
  // through plain respond() (the proven path used by classify_intent
  // for months) and rely on the JSON-strict prompt + tolerant parser
  // + post-hoc enum validation against valid_intents to keep the
  // model on the rails. The grammar code path remains in
  // LlamaLm::respond_constrained() for future re-enablement.
  // Snapshot valid_intents into a set so the worker can validate
  // the model output without holding any reference to the caller.
  std::vector<std::string> valid_set = valid_intents;

  spawn_worker([transcript = std::move(transcript),
                system_prompt = std::move(system_prompt),
                valid_set = std::move(valid_set),
                cb = std::move(callback)]() mutable {
    std::string raw;
    {
      std::lock_guard<std::mutex> lk(g_lm_call_mtx);
      ILanguageModel *lm_ptr = nullptr;
      {
        std::lock_guard<std::mutex> lk2(g_backend_mtx);
        lm_ptr = g_lm.get();
      }
      if (lm_ptr) {
        auto t0 = std::chrono::steady_clock::now();
        raw = lm_ptr->respond(system_prompt, transcript);
        auto t1 = std::chrono::steady_clock::now();
        g_last_lm_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                .count());
      }
    }

    ClassifyResult r;
    if (raw.empty()) {
      r.intent_name = "_INVALID";
      r.success = false;
    } else {
      std::string intent;
      std::string repaired;
      bool whisper_fix = false;
      bool ok_intent = json_extract_string(raw, "intent", &intent);
      json_extract_string(raw, "repaired", &repaired);
      json_extract_bool(raw, "whisper_fix", &whisper_fix);

      // Without a grammar constraint the model can return anything.
      // Validate the intent against the supplied enum and downgrade
      // to _INVALID on any mismatch. _INVALID itself is always
      // acceptable. This is the safety net the grammar would have
      // provided at the token level.
      bool intent_in_enum =
          intent == "_INVALID" || std::find(valid_set.begin(), valid_set.end(),
                                            intent) != valid_set.end();

      if (!ok_intent || intent.empty() || !intent_in_enum) {
        r.intent_name = "_INVALID";
        r.success = false;
      } else {
        r.intent_name = std::move(intent);
        r.repaired_transcript = std::move(repaired);
        r.whisper_fix = whisper_fix;
        r.success = true;
      }
    }
    enqueue_callback(
        [cb = std::move(cb), r = std::move(r)]() mutable { cb(std::move(r)); });
  });
}

void classify_intent_async(std::string transcript, std::string system_prompt,
                           std::function<void(std::string, bool)> callback) {
  if (!callback)
    return;
  if (!lm_ready()) {
    enqueue_callback(
        [cb = std::move(callback)]() { cb(std::string("_INVALID"), false); });
    return;
  }

  spawn_worker([transcript = std::move(transcript),
                system_prompt = std::move(system_prompt),
                cb = std::move(callback)]() mutable {
    std::string raw;
    {
      std::lock_guard<std::mutex> lk(g_lm_call_mtx);
      ILanguageModel *lm_ptr = nullptr;
      {
        std::lock_guard<std::mutex> lk2(g_backend_mtx);
        lm_ptr = g_lm.get();
      }
      if (lm_ptr) {
        // Tighter prompt — classifiers should respond with the intent
        // name verbatim. We trim downstream.
        auto t0 = std::chrono::steady_clock::now();
        raw = lm_ptr->respond(system_prompt, transcript);
        auto t1 = std::chrono::steady_clock::now();
        g_last_lm_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                .count());
      }
    }
    std::string intent = trim(raw);
    // Take only the first whitespace-delimited token; small models
    // sometimes append " (because ...)" even when told not to.
    auto sp = intent.find_first_of(" \t\n\r");
    if (sp != std::string::npos)
      intent = intent.substr(0, sp);

    bool ok = !intent.empty();
    if (!ok)
      intent = "_INVALID";
    enqueue_callback([cb = std::move(cb), intent = std::move(intent),
                      ok]() mutable { cb(std::move(intent), ok); });
  });
}

} // namespace lm

// ── TTS ──────────────────────────────────────────────────────────────

namespace tts {

void synthesize_async(std::string text, model_manifest::VoiceRole role,
                      float length_scale,
                      std::function<void(Audio, bool)> callback) {
  if (!callback)
    return;
  if (!tts_ready()) {
    enqueue_callback([cb = std::move(callback)]() { cb({}, false); });
    return;
  }

  // Resolve role → voice_id on the calling (main) thread so the worker
  // does not have to touch settings::cfg under its own lock. Falls back
  // to the manifest default if the user-selected voice is not actually
  // loaded — happens during cold-launch when an optional voice was
  // assigned but its file is still downloading.
  std::string voice_id = settings::voice_for_role(role);

  spawn_worker([text = std::move(text), voice_id = std::move(voice_id),
                length_scale, role, cb = std::move(callback)]() mutable {
    Audio a;
    {
      std::lock_guard<std::mutex> lk(g_tts_call_mtx);
      ITextToSpeech *tts_ptr = nullptr;
      {
        std::lock_guard<std::mutex> lk2(g_backend_mtx);
        tts_ptr = g_tts.get();
      }
      if (tts_ptr) {
        // Prefer the configured voice; if it is not loaded, retry
        // with the manifest default for the role. Keeps the user
        // unblocked when an optional voice has been assigned but is
        // still verifying.
        std::string id = voice_id;
        if (!tts_ptr->has_voice(id))
          id = tts_ptr->default_voice_for(role);
        auto t0 = std::chrono::steady_clock::now();
        TtsFailure fail = TtsFailure::None;
        a.pcm16 =
            tts_ptr->synthesize(id, text, length_scale, a.sample_rate_hz, fail);
        a.content_blocked = (fail == TtsFailure::ContentBlocked);
        auto t1 = std::chrono::steady_clock::now();
        g_last_tts_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                .count());
      }
    }
    bool ok = !a.pcm16.empty() && a.sample_rate_hz > 0;
    enqueue_callback([cb = std::move(cb), a = std::move(a), ok]() mutable {
      cb(std::move(a), ok);
    });
  });
}

} // namespace tts

} // namespace backends
