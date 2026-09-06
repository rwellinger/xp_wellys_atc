/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "ui/atc_ui.hpp"
#include "atc/atc_session.hpp"
#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/atis_generator.hpp"
#include "atc/de_phraseology.hpp"
#include "atc/en_phraseology.hpp"
#include "atc/flight_phase.hpp"
#include "atc/flows/ground_operations.hpp"
#include "atc/intent_parser.hpp"
#include "atc/phraseology_hints.hpp"
#include "audio/audio_player.hpp"
#include "audio/audio_recorder.hpp"
#include "backends/downloader.hpp"
#include "backends/loader.hpp"
#include "backends/manager.hpp"
#include "core/cross_country_log.hpp"
#include "core/logging.hpp"
#include "core/xplane_context.hpp"
#include "data/airport_vrps.hpp"
#include "data/airspace_db.hpp"
#include "data/traffic_context.hpp"
#include "persistence/model_manifest.hpp"
#include "persistence/model_paths.hpp"
#include "persistence/models_catalog.hpp"
#include "persistence/settings.hpp"
#include "ui/clipboard.hpp"
#include "ui/ui_strings.hpp"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/task_info.h>
#elif defined(_WIN32)
#include <windows.h>
// psapi.h must follow windows.h.
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

#include <XPLMDataAccess.h>
#include <XPLMDisplay.h>
#include <XPLMGraphics.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <imgui.h>
#include <imgui_impl_opengl2.h>

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <vector>

namespace atc_ui {

// ── State ────────────────────────────────────────────────────────
// The XPLM window is full-screen and invisible (DecorationNone).
// It exists only to capture mouse/keyboard events and feed them to ImGui.
// ImGui draws its own window on top.
static XPLMWindowID window_id = nullptr;
static bool visible = false;
static bool atc_panel_visible_ = false;
static bool transcript_window_visible_ = false;

// ImGui persistent buffers
static char callsign_raw_buf[64] = {};
static bool buffers_initialized = false;

static const char *pattern_dir_names[] = {"left", "right"};
static int pattern_dir_selection = 0; // default: left

// Planned VFR flight type for the departure phraseology hints (NfL 1.4.7
// a/b). Keys persisted to settings; labels are display-only. Destination
// buffer feeds "VFR nach <dest>" for the cross-country case.
static const char *vfr_flight_type_names[] = {"pattern", "cross_country"};
static int vfr_flight_type_selection = 0; // default: pattern (Platzrunde)
static char vfr_destination_buf[64] = {};

// Cockpit start mode — drives the initial ATCState the state machine
// adopts at plugin boot. Display labels are user-friendly; the keys
// stored in settings.json are snake_case (cold_and_dark, etc.).
static const char *start_mode_keys[] = {"cold_and_dark", "engines_running",
                                        "ready_for_takeoff"};
static const char *start_mode_labels[] = {"Cold and Dark", "Engines Running",
                                          "Ready for Takeoff"};
static int start_mode_selection = 1; // default: engines_running

// ATC phraseology language — "de" (NfL DACH-VFR) or "en" (ICAO-VFR).
// Keys match settings::atc_language(); labels are shown in the combo.
// A restart is required for the profile bundles / voices to reload.
static const char *language_keys[] = {"de", "en"};
static int language_selection = 0; // default: de

// Interface (UI-chrome) language — "de" or "en" (Issue #56). Decoupled from
// the ATC phraseology language above; reuses language_keys. Switches live via
// ui_strings::reload() (no restart).
static int ui_language_selection = 1; // default: en

// AI backend selection — local (whisper.cpp + llama.cpp + Piper), the
// OpenAI cloud pipeline, or the Mistral cloud pipeline. The arm64
// slice offers all three; the x86_64 slice has no local backends
// compiled in and offers only the two cloud providers.
#ifdef XPWELLYS_USE_LOCAL_INFERENCE
static const char *backend_mode_keys[] = {"local", "openai", "mistral"};
static const char *backend_mode_labels[] = {"Local (whisper + llama + Piper)",
                                            "OpenAI Cloud",
                                            "Mistral Cloud (Voxtral)"};
static constexpr int kBackendModeCount = 3;
#else
static const char *backend_mode_keys[] = {"openai", "mistral"};
static const char *backend_mode_labels[] = {"OpenAI Cloud",
                                            "Mistral Cloud (Voxtral)"};
static constexpr int kBackendModeCount = 2;
#endif
static int backend_mode_selection = 0;

// Status-panel facility label. AFIS must read distinctly from uncontrolled —
// it is exam-relevant (AFIS mandatory reports vs. self-announce). UNKNOWN ->
// no label (the cache is not ready / no airport resolved yet).
static const char *
facility_status_label(const xplane_context::XPlaneContext &c) {
  switch (c.facility_type) {
  case xplane_context::FacilityType::TOWERED:
    return c.tower_only ? ui_strings::tr("status.tower_only")
                        : ui_strings::tr("status.towered");
  case xplane_context::FacilityType::AFIS:
    return ui_strings::tr("status.afis");
  case xplane_context::FacilityType::UNCONTROLLED:
    return ui_strings::tr("status.uncontrolled");
  case xplane_context::FacilityType::UNKNOWN:
    return "";
  }
  return "";
}

// OpenAI / Mistral model + voice combos. Slugs and voice presets are
// pulled from data/models_catalog.json at boot — the static arrays
// they replaced lived here pre-v3.1. Edit the JSON to add a new slug,
// no recompile needed.

// API key TextInput buffer (password-masked at the ImGui call site).
// Cleared when the user clicks Save Key so the in-memory copy doesn't
// stick around. 256 bytes covers the longest cloud token format with
// generous headroom. Mistral keys live in a parallel buffer so the
// two providers' state never bleeds across.
static char api_key_buf[256] = {};
static float api_key_feedback_timer = 0.0f;
static char api_key_feedback_msg[128] = {};

static char mistral_key_buf[256] = {};
static float mistral_key_feedback_timer = 0.0f;
static char mistral_key_feedback_msg[128] = {};

// ── Catalog-driven Combo helper ──────────────────────────────────
//
// ImGui::Combo wants `const char *const *items`; the catalog returns
// std::vector<Option>. Builds an interim const-char* array on the
// stack (max 64 options — way past any sensible model list) and
// finds the currently-selected index by id match.
static bool
combo_from_catalog(const char *label,
                   const std::vector<models_catalog::Option> &options,
                   const std::string &current,
                   const std::function<void(const std::string &)> &on_change) {
  constexpr int kMaxOptions = 64;
  if (options.empty()) {
    ImGui::TextDisabled("%s: (catalog empty)", label);
    return false;
  }
  const char *labels[kMaxOptions];
  int n = 0;
  int sel = 0;
  for (size_t i = 0; i < options.size() && n < kMaxOptions; ++i, ++n) {
    labels[n] = options[i].label.empty() ? options[i].id.c_str()
                                         : options[i].label.c_str();
    if (options[i].id == current)
      sel = n;
  }
  if (ImGui::Combo(label, &sel, labels, n)) {
    on_change(options[static_cast<size_t>(sel)].id);
    settings::save();
    return true;
  }
  return false;
}

// ── Helpers: format bytes, resident memory, model state strings ──

static std::string format_bytes(uint64_t b) {
  // SI-style for sizes the user reads next to disk-space numbers; the
  // exact base does not matter for a UI hint — picking 1024-based
  // (binary) kept consistency with `du -h` output the user sees in
  // Terminal during the manual-fallback download path.
  char buf[32];
  if (b < 1024ULL) {
    std::snprintf(buf, sizeof(buf), "%llu B",
                  static_cast<unsigned long long>(b));
  } else if (b < 1024ULL * 1024) {
    std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(b) / 1024.0);
  } else if (b < 1024ULL * 1024 * 1024) {
    std::snprintf(buf, sizeof(buf), "%.1f MB",
                  static_cast<double>(b) / 1024.0 / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f GB",
                  static_cast<double>(b) / 1024.0 / 1024.0 / 1024.0);
  }
  return buf;
}

// Resident set size in bytes via Mach. Polled at most once a second
// from the Models tab — the call itself is cheap (microseconds) but
// not worth running every frame.
[[maybe_unused]] static uint64_t resident_bytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    return info.resident_size;
  }
  return 0;
#elif defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return pmc.WorkingSetSize;
  }
  return 0;
#else
  return 0;
#endif
}

[[maybe_unused]] static const char *file_state_label(backends::loader::FileState s) {
  using FS = backends::loader::FileState;
  switch (s) {
  case FS::NotChecked:
    return ui_strings::tr("filestate.not_checked");
  case FS::Missing:
    return ui_strings::tr("filestate.missing");
  case FS::SizeMismatch:
    return ui_strings::tr("filestate.wrong_size");
  case FS::Verifying:
    return ui_strings::tr("filestate.verifying");
  case FS::HashMismatch:
    return ui_strings::tr("filestate.corrupt");
  case FS::Verified:
    return ui_strings::tr("filestate.verified");
  case FS::Loading:
    return ui_strings::tr("filestate.loading");
  case FS::Ready:
    return ui_strings::tr("filestate.ready");
  case FS::LoadError:
    return ui_strings::tr("filestate.load_error");
  }
  return ui_strings::tr("filestate.unknown");
}

static ImVec4 file_state_color(backends::loader::FileState s) {
  using FS = backends::loader::FileState;
  switch (s) {
  case FS::Ready:
    return ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // green
  case FS::Verifying:
  case FS::Loading:
    return ImVec4(1.0f, 0.8f, 0.2f, 1.0f); // amber
  case FS::Missing:
  case FS::SizeMismatch:
  case FS::HashMismatch:
  case FS::LoadError:
    return ImVec4(1.0f, 0.4f, 0.2f, 1.0f); // red
  default:
    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f); // grey
  }
}

static const char *download_state_label(backends::downloader::State s) {
  using DS = backends::downloader::State;
  switch (s) {
  case DS::Idle:
    return "";
  case DS::Queued:
    return ui_strings::tr("dlstate.queued");
  case DS::Downloading:
    return ui_strings::tr("dlstate.downloading");
  case DS::Verifying:
    return ui_strings::tr("dlstate.verifying");
  case DS::Done:
    return ui_strings::tr("dlstate.done");
  case DS::Failed:
    return ui_strings::tr("dlstate.failed");
  case DS::Cancelled:
    return ui_strings::tr("dlstate.cancelled");
  case DS::InsufficientDisk:
    return ui_strings::tr("dlstate.no_disk_space");
  }
  return "";
}

// ── Time ─────────────────────────────────────────────────────────
static double last_frame_time_ = 0.0;
static double get_xp_time() {
  static XPLMDataRef dr = nullptr;
  if (!dr)
    dr = XPLMFindDataRef("sim/time/total_running_time_sec");
  return dr ? static_cast<double>(XPLMGetDataf(dr)) : 0.0;
}

static size_t last_transcript_count_ = 0;
static bool window_pos_reset_pending_ = false;
static float geometry_save_timer_ = 0.0f;
static constexpr float kGeometrySaveDelay = 0.5f; // save 0.5s after last change

// ── Nearby airports panel ────────────────────────────────────────

static constexpr double kNearbyAirportsRangeNm = 40.0;
static constexpr size_t kNearbyAirportsMax = 10;

static std::vector<xplane_context::NearbyAirport> nearby_cache_;
static std::chrono::steady_clock::time_point nearby_last_refresh_{};

static void draw_nearby_airports() {
  const auto &ctx = xplane_context::get();
  const std::string &locked = xplane_context::locked_airport();

  // Throttle refresh to ~1 Hz.
  auto now = std::chrono::steady_clock::now();
  if (nearby_cache_.empty() ||
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now - nearby_last_refresh_)
              .count() >= 1000) {
    nearby_cache_ = xplane_context::find_nearby_airports(kNearbyAirportsRangeNm,
                                                         kNearbyAirportsMax);
    nearby_last_refresh_ = now;
  }

  ImGui::Text(ui_strings::tr("nearby.header_format"), kNearbyAirportsRangeNm);
  if (!locked.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton(ui_strings::tr("btn.unlock"))) {
      xplane_context::unlock_airport();
      nearby_cache_.clear();
    }
  }

  // If locked airport is outside the nearby window, show it as a pinned row.
  bool locked_in_list = false;
  if (!locked.empty()) {
    for (const auto &na : nearby_cache_) {
      if (na.icao == locked) {
        locked_in_list = true;
        break;
      }
    }
  }

  // Header row for the facility badges. Disambiguates A=ATIS vs APP and
  // makes the X/- columns underneath self-explanatory. Spacing matches the
  // %-30s gap before the badges in render_row below.
  ImGui::TextDisabled(ui_strings::tr("nearby.col_header_format"),
                      ui_strings::tr("nearby.col_icao"),
                      ui_strings::tr("nearby.col_name"),
                      ui_strings::tr("nearby.col_dist"),
                      ui_strings::tr("nearby.col_facilities"));

  auto render_row = [&](const std::string &icao, const std::string &name,
                        double dist_nm, bool has_atis, bool has_ground,
                        bool has_tower, bool has_afis, bool has_unicom,
                        bool is_locked) {
    auto mark = [](bool present) -> const char * {
      return present ? "X" : "-";
    };
    char label[256];
    std::snprintf(label, sizeof(label),
                  "%s %-4s  %-24s  %5.1f NM   %s    %s   %s    %s    %s##nb_%s",
                  is_locked ? ">" : " ", // lock marker
                  icao.c_str(), name.empty() ? "" : name.substr(0, 24).c_str(),
                  dist_nm, mark(has_atis), mark(has_ground), mark(has_tower),
                  mark(has_afis), mark(has_unicom), icao.c_str());
    if (is_locked) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
    }
    bool clicked = ImGui::Selectable(label, is_locked);
    if (is_locked)
      ImGui::PopStyleColor();

    if (clicked) {
      xplane_context::lock_airport(icao);
      // Tune standby to the most useful freq of the picked airport:
      // ATIS if available, otherwise Tower, otherwise Unicom, otherwise the
      // AFIS Info/Radio frequency (German info fields have none of the above).
      const auto &cur_ctx = xplane_context::get();
      uint32_t target_khz = 0;
      for (const auto &f : cur_ctx.airport_freqs.all) {
        if (f.type == xplane_context::FrequencyType::ATIS) {
          target_khz = f.freq_khz;
          break;
        }
      }
      if (target_khz == 0) {
        for (const auto &f : cur_ctx.airport_freqs.all) {
          if (f.type == xplane_context::FrequencyType::TOWER) {
            target_khz = f.freq_khz;
            break;
          }
        }
      }
      if (target_khz == 0) {
        for (const auto &f : cur_ctx.airport_freqs.all) {
          if (f.type == xplane_context::FrequencyType::UNICOM) {
            target_khz = f.freq_khz;
            break;
          }
        }
      }
      if (target_khz == 0) {
        for (const auto &f : cur_ctx.airport_freqs.all) {
          if (f.type == xplane_context::FrequencyType::INFO ||
              f.type == xplane_context::FrequencyType::RADIO) {
            target_khz = f.freq_khz;
            break;
          }
        }
      }
      if (target_khz != 0 && cur_ctx.com_radio_powered)
        xplane_context::set_standby_freq(target_khz);
      nearby_cache_.clear();
    }
  };

  if (!locked.empty() && !locked_in_list) {
    // Compute distance for pinned locked row.
    double dist_nm = 0.0;
    if (ctx.airport_lat != 0.0 || ctx.airport_lon != 0.0) {
      // Reuse ctx.airport_lat/lon which was populated from the lock.
      const double kDeg2Rad = 3.14159265358979323846 / 180.0;
      double dlat = (ctx.airport_lat - ctx.latitude) * kDeg2Rad;
      double dlon = (ctx.airport_lon - ctx.longitude) * kDeg2Rad;
      double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                 std::cos(ctx.latitude * kDeg2Rad) *
                     std::cos(ctx.airport_lat * kDeg2Rad) * std::sin(dlon / 2) *
                     std::sin(dlon / 2);
      double dm =
          6371000.0 * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
      dist_nm = dm / 1852.0;
    }
    using FT = xplane_context::FrequencyType;
    render_row(
        locked, ctx.nearest_airport_name, dist_nm,
        ctx.airport_freqs.has(FT::ATIS), ctx.airport_freqs.has(FT::GROUND),
        ctx.airport_freqs.has(FT::TOWER),
        ctx.airport_freqs.has(FT::INFO) || ctx.airport_freqs.has(FT::RADIO),
        ctx.airport_freqs.has(FT::UNICOM) || ctx.airport_freqs.has(FT::CTAF),
        true);
  }

  if (nearby_cache_.empty()) {
    ImGui::TextDisabled("%s", ui_strings::tr("nearby.empty"));
  } else {
    for (const auto &na : nearby_cache_) {
      render_row(na.icao, na.name, na.distance_nm, na.has_atis, na.has_ground,
                 na.has_tower, na.has_afis, na.has_unicom, na.icao == locked);
    }
  }
}

// ── Tab drawing ──────────────────────────────────────────────────

static void draw_status_tab() {
  // Models-not-ready banner. PTT is hard-gated on all three backends
  // being registered, so the user sees nothing happen if they hit
  // the key before models load. Surface that explicitly here and
  // point at the Models tab (local mode) or the Settings tab
  // (cloud modes — API key missing).
  {
    auto status = backends::loader::snapshot();
    if (!status.all_ready()) {
      const std::string mode = settings::backend_mode();
      const bool is_cloud = (mode == "openai" || mode == "mistral");
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.4f, 0.1f, 0.1f, 0.6f));
      ImGui::BeginChild(
          "##model_banner",
          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2 + 8), true);
      if (is_cloud) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           ui_strings::tr("status.banner_openai_key_missing"));
        ImGui::TextDisabled("%s", ui_strings::tr("status.banner_openai_hint"));
      } else {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           ui_strings::tr("status.banner_models_not_ready"));
        ImGui::TextDisabled("%s", ui_strings::tr("status.banner_models_hint"));
      }
      ImGui::EndChild();
      ImGui::PopStyleColor();
      ImGui::Spacing();
    }
  }

  // PTT State
  auto ptt = atc_session::ptt_state();
  std::string label = atc_session::ptt_state_label();
  if (ptt == atc_session::PTTState::RECORDING) {
    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "%s", label.c_str());
  } else if (ptt == atc_session::PTTState::PROCESSING) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", label.c_str());
  } else if (ptt == atc_session::PTTState::PLAYING) {
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "%s", label.c_str());
  } else {
    ImGui::Text("%s", label.c_str());
  }

  // Flight Phase + ATC State (+ departure type when in DEPARTURE_CLEARED)
  ImGui::SameLine();
  // After step 3b state_name() carries the flow qualifier
  // ("Pattern/DEPARTURE_CLEARED" / "XC/DEPARTURE_CLEARED"); no separate
  // departure-type tag needed any more.
  ImGui::Text("   %s | %s", flight_phase::phase_name(flight_phase::get()),
              atc_state_machine::state_name(atc_state_machine::get_state()));

  // Last recording info
  float dur = atc_session::last_recording_duration();
  size_t samples = atc_session::last_recording_samples();
  size_t wav_bytes = atc_session::last_wav_bytes();
  if (samples > 0) {
    ImGui::Text(ui_strings::tr("status.last_recording_format"), dur, samples);
    ImGui::Text(ui_strings::tr("status.wav_buffer_format"), wav_bytes);
  }

  ImGui::Separator();

  const auto &ctx = xplane_context::get();

  {
    std::string apt_display =
        ctx.nearest_airport_id.empty()
            ? "---"
            : ctx.nearest_airport_id + (ctx.nearest_airport_name.empty()
                                            ? ""
                                            : " " + ctx.nearest_airport_name);
    ImGui::Text(ui_strings::tr("status.airport_format"), apt_display.c_str(),
                ctx.nearest_airport_id.empty() ? ""
                                               : facility_status_label(ctx));
    if (!ctx.geometric_nearest_id.empty() &&
        ctx.geometric_nearest_id != ctx.nearest_airport_id) {
      const char *reason = xplane_context::locked_airport().empty()
                               ? ui_strings::tr("status.active_via_freq")
                               : ui_strings::tr("status.locked_by_picker");
      ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                         ui_strings::tr("status.geometric_nearest_format"),
                         reason, ctx.geometric_nearest_id.c_str());
    }
  }

  // Runway info
  if (!ctx.active_runway.empty()) {
    std::string rwy_list;
    for (const auto &rwy : ctx.runways) {
      if (!rwy_list.empty())
        rwy_list += ", ";
      rwy_list += rwy.end1.number + "/" + rwy.end2.number;
    }
    ImGui::Text(ui_strings::tr("status.runway_active_format"),
                ctx.active_runway.c_str(), rwy_list.c_str());
  } else if (!ctx.runways.empty()) {
    std::string rwy_list;
    for (const auto &rwy : ctx.runways) {
      if (!rwy_list.empty())
        rwy_list += ", ";
      rwy_list += rwy.end1.number + "/" + rwy.end2.number;
    }
    ImGui::Text(ui_strings::tr("status.runways_format"), rwy_list.c_str());
  } else {
    ImGui::TextDisabled("%s", ui_strings::tr("status.no_runway"));
  }

  // Wind info
  if (ctx.wind_speed_kt < 3.0f) {
    ImGui::Text("%s", ui_strings::tr("status.wind_calm"));
  } else {
    ImGui::Text(ui_strings::tr("status.wind_format"), ctx.wind_direction_deg,
                ctx.wind_speed_kt);
  }

  // ATIS info
  {
    static const char *letter_names[] = {
        "Alpha",  "Bravo",    "Charlie", "Delta",  "Echo",    "Foxtrot",
        "Golf",   "Hotel",    "India",   "Juliet", "Kilo",    "Lima",
        "Mike",   "November", "Oscar",   "Papa",   "Quebec",  "Romeo",
        "Sierra", "Tango",    "Uniform", "Victor", "Whiskey", "X-ray",
        "Yankee", "Zulu"};
    char letter = atis_generator::current_letter();
    if (ctx.atis_freq_mhz > 100.0f) {
      ImGui::Text(ui_strings::tr("status.atis_format"),
                  letter_names[letter - 'A'], ctx.atis_freq_mhz);
    } else {
      ImGui::Text(ui_strings::tr("status.atis_no_freq_format"),
                  letter_names[letter - 'A']);
    }
  }

  float active_freq =
      (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
  ImGui::Text(ui_strings::tr("status.com_format"), ctx.active_com, active_freq,
              xplane_context::frequency_type_name(ctx.frequency_type));
  if (ctx.tower_only) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ui_strings::tr("status.tower_only"));
  }

  ImGui::Text(ui_strings::tr("status.frequencies_format"),
              ctx.airport_freqs.all.size());
  ImGui::SameLine();
  if (ImGui::SmallButton(ui_strings::tr("btn.atc_panel"))) {
    atc_panel_visible_ = !atc_panel_visible_;
  }

  ImGui::Separator();
  ImGui::Text(ui_strings::tr("status.position_format"), ctx.latitude,
              ctx.longitude);
  ImGui::Text(ui_strings::tr("status.altitude_format"), ctx.altitude_ft_msl);
  ImGui::Text(ui_strings::tr("status.gs_ias_format"), ctx.groundspeed_kts,
              ctx.indicated_airspeed_kts);
  ImGui::Text(ui_strings::tr("status.vs_hdg_format"), ctx.vertical_speed_fpm,
              ctx.heading_true);
  ImGui::Text(ui_strings::tr("status.on_ground_format"),
              ctx.on_ground ? ui_strings::tr("status.yes")
                            : ui_strings::tr("status.no"));
  ImGui::Text(ui_strings::tr("status.aircraft_format"),
              ctx.aircraft_icao.empty() ? "---" : ctx.aircraft_icao.c_str());

  // Last Parsed Intent
  ImGui::Separator();
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("status.last_intent_header"));

  const auto &pm = atc_session::last_pilot_message();
  if (!pm.raw_transcript.empty()) {
    ImGui::Text(ui_strings::tr("status.intent_format"),
                intent_parser::intent_name(pm.intent));
    ImGui::Text(ui_strings::tr("status.confidence_format"), pm.confidence);
    if (!pm.callsign.empty())
      ImGui::Text(ui_strings::tr("status.callsign_format"),
                  pm.callsign.c_str());
    if (!pm.runway.empty())
      ImGui::Text(ui_strings::tr("status.runway_label_format"),
                  pm.runway.c_str());
    ImGui::TextWrapped(ui_strings::tr("status.transcript_format"),
                       pm.raw_transcript.c_str());
  } else {
    ImGui::TextDisabled("%s", ui_strings::tr("status.no_transcript"));
  }

  // Session stats
  ImGui::Separator();
  ImGui::Text(ui_strings::tr("status.session_format"),
              atc_session::total_transcriptions(),
              atc_session::total_api_calls());
}

// Models tab — lifecycle UI for the three local inference models.
// Shows per-file state (Missing / Verifying / Ready / etc.), the
// expected size + SHA256 hint, and a single contextual action
// button per row whose verb depends on the current state.
//
// RAM usage and per-stage inference latency live at the bottom for
// live tuning during dev sessions; both are read-only.
[[maybe_unused]] static void draw_models_tab() {
  // Cloud modes never download the full local pipeline (Whisper STT +
  // Llama LM). Wherever Piper TTS is compiled in (Apple Silicon, and with
  // #69 also x86_64-macOS / Windows) we still render a trimmed "voice only"
  // view so the hybrid TTS override (issue #66) can fetch its local Piper
  // voice right here, without a detour through Local mode. On the
  // cloud-only slices there is nothing local to manage, so short-circuit.
  bool voice_only = false;
  {
    const std::string mode = settings::backend_mode();
    if (mode == "openai" || mode == "mistral") {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                         ui_strings::tr("models.openai_active"));
#ifdef XPWELLYS_USE_LOCAL_TTS
      // Hybrid TTS (issue #66): only the local Piper voice is downloadable
      // here — STT+LM stay on the cloud backend, so Whisper/Llama are
      // hidden. Fall through to the per-voice rows below instead of
      // returning.
      voice_only = true;
      ImGui::TextDisabled("%s", ui_strings::tr("models.hybrid_voice_section"));
      ImGui::Spacing();
      ImGui::Separator();
#else
      ImGui::TextDisabled("%s", ui_strings::tr("models.openai_no_models"));
      ImGui::Spacing();
      ImGui::TextDisabled("%s", ui_strings::tr("models.openai_hint"));
      return;
#endif
    }
  }

  auto loader_status = backends::loader::snapshot();
  auto downloads = backends::downloader::snapshot();

  // DE-only build: every catalog entry is German or language-agnostic
  // (Llama), so there is nothing to filter — the old "show all languages"
  // toggle is gone.
  const std::string active_lang = settings::backend_language();

  // The whole-pipeline summary (disk + still-required) and the "Download
  // language pack" bulk button count Whisper via bytes_still_required /
  // enqueue_all_missing — neither is meaningful in the hybrid voice-only
  // view, so both are skipped there. The per-voice rows below carry their
  // own Download/Re-verify buttons and cover the voice-only case fully.
  if (!voice_only) {
    // ── Top summary: where files live + free disk + still-required ──
    ImGui::TextDisabled("%s", ui_strings::tr("models.location"));
    uint64_t free_b = backends::downloader::free_space_bytes();
    uint64_t need_b = backends::downloader::bytes_still_required(false);
    if (need_b == 0) {
      ImGui::Text(ui_strings::tr("models.all_present_format"),
                  format_bytes(free_b).c_str());
    } else {
      bool low_disk = free_b < need_b + (50ULL * 1024 * 1024); // 50 MB slack
      if (low_disk) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                           ui_strings::tr("models.low_disk_format"),
                           format_bytes(need_b).c_str(),
                           format_bytes(free_b).c_str());
      } else {
        ImGui::Text(ui_strings::tr("models.still_need_format"),
                    format_bytes(need_b).c_str(), format_bytes(free_b).c_str());
      }
    }
    ImGui::Spacing();

    // ── Bulk action: download all missing ──
    bool any_pending_or_active = false;
    for (const auto &d : downloads) {
      using DS = backends::downloader::State;
      if (d.state == DS::Queued || d.state == DS::Downloading ||
          d.state == DS::Verifying) {
        any_pending_or_active = true;
        break;
      }
    }
    // Two top-row buttons: Download language pack (left) + Re-verify All
    // (right). The pack button fetches only what the active language needs
    // (Whisper + required voice + any assigned optional voices) — it does
    // NOT pull the large optional Llama, which has its own per-row Download
    // in the "Optional AI models" section. `need_b` already excludes Llama.
    // Re-verify All stays visible even when something is still missing — it
    // lets the user retrigger a check after they manually copied a model
    // file into Resources/models/ without a download.
    if (need_b > 0) {
      if (any_pending_or_active) {
        ImGui::BeginDisabled();
        ImGui::Button(ui_strings::tr("btn.download_language_pack"));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ui_strings::tr("models.download_progress"));
      } else {
        if (ImGui::Button(ui_strings::tr("btn.download_language_pack"))) {
          backends::downloader::enqueue_all_missing(false);
        }
      }
      ImGui::SameLine();
    }
    if (ImGui::Button(ui_strings::tr("btn.reverify_all"))) {
      backends::loader::start();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ui_strings::tr("models.bandwidth_hint"));

    ImGui::Separator();
  }

  // ── Per-file rows (accordion sections) ──
  // Three CollapsingHeaders separate what the user actually needs from
  // the large optional download, so nobody accidentally pulls the
  // ~1.9 GB Llama when they only wanted a ~60 MB language voice:
  //   0 "Language pack"  — the multilingual Whisper STT + the required
  //                        voice for the active language. Open by default.
  //   1 "Optional voices"— extra voices for the active language. Folded.
  //   2 "Optional AI models" — Llama (download-on-demand). Folded, with a
  //                        note that the sim runs without it.
  //
  // Sections are rendered order-INDEPENDENTLY: we first bucket the
  // visible manifest indices by section, then draw each non-empty
  // section exactly once in a fixed order. The catalog interleaves
  // voices of different sections/languages (e.g. a required EN voice
  // sits between optional DE voices), so a "emit a header whenever the
  // section changes vs. the previous row" scheme would re-open the same
  // section twice in one frame and ImGui would assert "2 visible items
  // with conflicting ID". Bucketing first makes that impossible.
  const auto &manifest = model_manifest::all();
  enum Section { SecLanguagePack = 0, SecOptionalVoices, SecAiModels };
  std::vector<size_t> sec_indices[3];
  for (size_t i = 0; i < manifest.size(); ++i) {
    const auto &m = manifest[i];

    // Fold the .onnx.json config sibling into its voice's .onnx row so
    // each voice shows as ONE row, not two near-identical entries. The
    // tiny config file always travels with the .onnx; the row's buttons
    // act on both.
    if (m.kind == model_manifest::Kind::PiperVoiceConfig)
      continue;

    // Only offer models/voices of the active language. Whisper + Llama
    // carry an empty language and are always kept. Applies to OPTIONAL
    // voices too — otherwise foreign-language voices leak into the list.
    if (!m.language.empty() && m.language != active_lang)
      continue;

    // Hybrid voice-only view (cloud mode on Apple Silicon): STT+LM stay in
    // the cloud, so only the local Piper voice is downloadable here — hide
    // Whisper and Llama entirely.
    if (voice_only && m.kind != model_manifest::Kind::PiperVoice)
      continue;

    if (model_manifest::is_optional_ai_model(m.kind))
      sec_indices[SecAiModels].push_back(i); // Llama — separate & optional
    else if (m.kind == model_manifest::Kind::WhisperModel || !m.optional)
      sec_indices[SecLanguagePack].push_back(i); // Whisper + required voice
    else
      sec_indices[SecOptionalVoices].push_back(i);
  }

  struct SectionDef {
    const char *label_key;
    const char *id_suffix; // stable ###-ID so a translated label change
                           // never flips the header's ImGui ID.
    ImGuiTreeNodeFlags flags;
  };
  const SectionDef section_defs[3] = {
      {"models.section_language_pack", "###sec_language_pack",
       ImGuiTreeNodeFlags_DefaultOpen},
      // Optional voices + AI models folded — most users never touch these.
      {"models.section_optional_voices", "###sec_optional_voices", 0},
      {"models.section_ai_models", "###sec_ai_models", 0},
  };

  for (int s = 0; s < 3; ++s) {
    if (sec_indices[s].empty())
      continue; // no visible rows -> no header for this section

    std::string header = std::string(ui_strings::tr(section_defs[s].label_key));
    // Name the active language on the language-pack header so it is clear
    // which voices these are (e.g. "Language pack · English (ICAO-VFR)").
    if (s == SecLanguagePack)
      header += std::string(" - ") +
                ui_strings::tr(active_lang == "en" ? "settings.language_en"
                                                   : "settings.language_de");
    header += section_defs[s].id_suffix;
    if (!ImGui::CollapsingHeader(header.c_str(), section_defs[s].flags))
      continue; // user has the section folded away

    // Optional AI models get a one-line "you don't need this" note so the
    // large download reads as deliberate, not mandatory.
    if (s == SecAiModels)
      ImGui::TextDisabled("%s", ui_strings::tr("models.ai_model_note"));

    for (size_t i : sec_indices[s]) {
      const auto &m = manifest[i];
      backends::loader::FileStatus loader_fs{
          m.kind, m.voice_id, m.language,
          backends::loader::FileState::NotChecked, ""};
      for (const auto &fs : loader_status.files) {
        // Match on the full (kind, voice_id, language) triple — two
        // Whisper rows share the same kind/voice_id but differ by
        // language.
        if (fs.kind == m.kind && fs.voice_id == m.voice_id &&
            fs.language == m.language) {
          loader_fs = fs;
          break;
        }
      }
      backends::downloader::Progress dl{};
      dl.kind = m.kind;
      dl.voice_id = m.voice_id;
      // Downloader Progress vector mirrors model_manifest::all() order
      // exactly, so the same index `i` is the authoritative lookup.
      if (i < downloads.size() && downloads[i].kind == m.kind &&
          downloads[i].voice_id == m.voice_id) {
        dl = downloads[i];
      }

      // Piper voices: fold in the .onnx.json config sibling's on-disk +
      // download state so this single row's buttons manage both files.
      const model_manifest::Entry *cfg_entry = nullptr;
      backends::loader::FileStatus cfg_fs{};
      backends::downloader::Progress cfg_dl{};
      if (m.kind == model_manifest::Kind::PiperVoice) {
        cfg_entry = model_manifest::get_voice(
            model_manifest::Kind::PiperVoiceConfig, m.voice_id);
        for (const auto &fs : loader_status.files) {
          if (fs.kind == model_manifest::Kind::PiperVoiceConfig &&
              fs.voice_id == m.voice_id && fs.language == m.language) {
            cfg_fs = fs;
            break;
          }
        }
        for (const auto &p : downloads) {
          if (p.kind == model_manifest::Kind::PiperVoiceConfig &&
              p.voice_id == m.voice_id) {
            cfg_dl = p;
            break;
          }
        }
      }

      ImGui::PushID(static_cast<int>(i));

      // Row header: filename + state badge + language tag
      ImGui::Text("%s", m.display_name.c_str());
      ImGui::SameLine();
      ImGui::TextColored(file_state_color(loader_fs.state), "[%s]",
                         file_state_label(loader_fs.state));
      if (!m.language.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", m.language.c_str());
      }

      // Live download line (if active)
      using DS = backends::downloader::State;
      if (dl.state == DS::Downloading) {
        float frac = dl.bytes_total > 0
                         ? static_cast<float>(dl.bytes_downloaded) /
                               static_cast<float>(dl.bytes_total)
                         : 0.0f;
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%s / %s",
                      format_bytes(dl.bytes_downloaded).c_str(),
                      format_bytes(dl.bytes_total).c_str());
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
      } else if (dl.state == DS::Queued) {
        ImGui::TextDisabled("%s", ui_strings::tr("models.queued"));
      } else if (dl.state == DS::Verifying) {
        ImGui::TextDisabled("%s", ui_strings::tr("models.verifying"));
      } else if (dl.state == DS::Failed || dl.state == DS::InsufficientDisk) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "%s: %s",
                           download_state_label(dl.state),
                           dl.error_message.c_str());
      }

      // Compact metadata: size + first 8 hex of SHA256
      ImGui::TextDisabled(ui_strings::tr("models.metadata_format"),
                          format_bytes(m.size_bytes).c_str(),
                          m.sha256_hex.substr(0, 8).c_str(),
                          m.filename.c_str());

      // Loader-side messages (verify errors, load errors)
      if (!loader_fs.message.empty() &&
          loader_fs.state != backends::loader::FileState::Verifying &&
          loader_fs.state != backends::loader::FileState::Loading) {
        ImGui::TextWrapped("   %s", loader_fs.message.c_str());
      }

      // Action buttons — act on the voice as a whole (.onnx + .onnx.json).
      // The per-row "Re-verify" + "Force re-download" skip the 2 GB Llama
      // hash entirely. For voices the buttons fan out to both files so a
      // single row drives the whole voice.
      using FS = backends::loader::FileState;
      auto is_missing = [](FS s) {
        return s == FS::Missing || s == FS::SizeMismatch ||
               s == FS::HashMismatch;
      };
      auto is_settled = [](FS s) {
        return s == FS::Ready || s == FS::Verified || s == FS::LoadError;
      };
      auto dl_active = [](DS s) {
        return s == DS::Downloading || s == DS::Queued || s == DS::Verifying;
      };
      const bool busy_dl =
          dl_active(dl.state) || (cfg_entry && dl_active(cfg_dl.state));
      const bool needs_dl = is_missing(loader_fs.state) ||
                            (cfg_entry && is_missing(cfg_fs.state));
      const bool settled = is_settled(loader_fs.state) &&
                           (!cfg_entry || is_settled(cfg_fs.state));
      const std::string entry_key = model_manifest::entry_key(m);
      if (busy_dl) {
        if (ImGui::Button(ui_strings::tr("btn.cancel"))) {
          backends::downloader::cancel(m);
          if (cfg_entry)
            backends::downloader::cancel(*cfg_entry);
        }
      } else if (needs_dl) {
        if (ImGui::Button(ui_strings::tr("btn.download"))) {
          backends::downloader::enqueue(m);
          if (cfg_entry)
            backends::downloader::enqueue(*cfg_entry);
        }
      } else if (settled) {
        if (ImGui::Button(ui_strings::tr("btn.reverify"))) {
          // process_one() pulls the .onnx.json sibling automatically.
          backends::loader::start(entry_key);
        }
        ImGui::SameLine();
        if (ImGui::Button(ui_strings::tr("btn.force_redownload"))) {
          backends::downloader::force_redownload(m);
          if (cfg_entry)
            backends::downloader::force_redownload(*cfg_entry);
        }
      } else {
        ImGui::BeginDisabled();
        ImGui::Button(ui_strings::tr("btn.busy"));
        ImGui::EndDisabled();
      }

      ImGui::PopID();
      ImGui::Separator();
    }
  }

  // The hybrid voice-only view ends here. The footer below reports the full
  // local pipeline (STT/LM/TTS readiness, RAM, per-stage latency), which is
  // misleading in a cloud mode where STT+LM never run locally.
  if (voice_only)
    return;

  // ── Footer: backend readiness + RAM + per-stage latency ──
  ImGui::Spacing();
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("models.backend_status_header"));
  auto badge = [](const char *name, bool ready) {
    ImGui::Text("%s", name);
    ImGui::SameLine();
    if (ready)
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s",
                         ui_strings::tr("status.ready"));
    else
      ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s",
                         ui_strings::tr("status.not_loaded"));
  };
  badge(ui_strings::tr("models.stt_label"), backends::stt_ready());
  badge(ui_strings::tr("models.lm_label"), backends::lm_ready());
  badge(ui_strings::tr("models.tts_label"), backends::tts_ready());

  // If the readiness gate is failing, list exactly why right here.
  // Previously the tab title showed "(!)" but the user had to guess
  // which row was the holdout — now we print it verbatim.
  auto blockers = loader_status.readiness_blockers();
  if (!blockers.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "%s",
                       ui_strings::tr("models.blockers_header"));
    for (const auto &b : blockers) {
      ImGui::BulletText("%s", b.description.c_str());
    }
  }

  ImGui::Spacing();
  // RAM polling: cache for 1 s so we don't tax mach every frame.
  static uint64_t cached_rss_ = 0;
  static double rss_last_refresh_ = -1.0;
  double now_sec = static_cast<double>(ImGui::GetTime());
  if (rss_last_refresh_ < 0 || (now_sec - rss_last_refresh_) > 1.0) {
    cached_rss_ = resident_bytes();
    rss_last_refresh_ = now_sec;
  }
  ImGui::Text(ui_strings::tr("models.ram_format"),
              format_bytes(cached_rss_).c_str());

  // Per-stage latency: 0 while no inference has run yet.
  uint32_t stt_ms = backends::last_stt_ms();
  uint32_t lm_ms = backends::last_lm_ms();
  uint32_t tts_ms = backends::last_tts_ms();
  ImGui::Text(ui_strings::tr("models.last_inference_format"), stt_ms, lm_ms,
              tts_ms);
}

// Serialize the radio history into a plain-text block for the system
// clipboard, mirroring the on-screen Transcript formatting (ASCII only).
// Backs the [Copy] button so the pilot can paste the log into notes or
// verification tooling outside the sim.
static std::string serialize_transcript() {
  const auto &entries = atc_session::transcript_entries();
  std::string out;
  char ts[24];
  for (const auto &entry : entries) {
    int mins = static_cast<int>(entry.sim_time) / 60;
    int secs = static_cast<int>(entry.sim_time) % 60;
    if (entry.kind == atc_session::TranscriptKind::System) {
      std::snprintf(ts, sizeof(ts), "[%02d:%02d] -- ", mins, secs);
      out += ts;
      out += entry.text;
      out += " --\n";
      continue;
    }
    std::snprintf(ts, sizeof(ts), "[%02d:%02d", mins, secs);
    out += ts;
    if (!entry.frequency.empty()) {
      out += ' ';
      out += entry.frequency;
    }
    out += "] ";
    if (entry.kind == atc_session::TranscriptKind::Pilot) {
      out += "You: ";
    } else {
      const auto &cx = xplane_context::get();
      std::string apt = !cx.nearest_airport_name.empty()
                            ? cx.nearest_airport_name
                            : cx.nearest_airport_id;
      out += apt.empty() ? "ATC" : apt + " ATC";
      out += ": ";
    }
    out += entry.text;
    out += '\n';
  }
  return out;
}

static void draw_transcript_tab() {
  if (ImGui::Button(ui_strings::tr("btn.clear"))) {
    atc_session::clear_transcript();
    last_transcript_count_ = 0;
  }

  // [Copy] dumps the whole radio history to the system clipboard. Brief
  // "Kopiert!" label swap as visual confirmation (the X-Plane ImGui
  // backend has no toast); the timer is sim-frame driven via the static
  // counter below so it survives across draws without a clock call.
  ImGui::SameLine();
  static int copied_feedback_frames = 0;
  const bool has_entries = !atc_session::transcript_entries().empty();
  ImGui::BeginDisabled(!has_entries);
  if (ImGui::Button(copied_feedback_frames > 0 ? ui_strings::tr("btn.copied")
                                               : ui_strings::tr("btn.copy"))) {
    ui::clipboard::write_system_text(serialize_transcript());
    copied_feedback_frames = 90; // ~1.5 s at 60 fps
  }
  ImGui::EndDisabled();
  if (copied_feedback_frames > 0)
    --copied_feedback_frames;

  ImGui::Separator();

  // Reserve space at the bottom for the optional Debug-Texteingabe row
  // when the master switch is on — InputText + Send button + spacing.
  // Without the reserved height, BeginChild(ImVec2(0,0)) would consume
  // the whole remaining area and push the input row off-screen.
  const bool text_input_active = settings::debug_text_input();
  float input_row_h =
      text_input_active ? (ImGui::GetFrameHeightWithSpacing() + 6.0f) : 0.0f;
  // Also reserve space for the network-error notice + retry button so it
  // is never pushed off-screen below the transcript (Issue #46).
  if (atc_session::tts_network_error())
    input_row_h += ImGui::GetTextLineHeightWithSpacing() +
                   ImGui::GetFrameHeightWithSpacing() + 8.0f;
  // No HorizontalScrollbar flag — long Tower / VRP-rich responses wrap
  // at the window edge instead (see PushTextWrapPos below). Same idiom
  // as the VRP list (line ~2050) and the phraseology hints panel.
  ImGui::BeginChild("TranscriptScroll", ImVec2(0, -input_row_h), false);

  ImGui::PushTextWrapPos(0.0f); // wrap at window edge

  const auto &entries = atc_session::transcript_entries();
  for (const auto &entry : entries) {
    int mins = static_cast<int>(entry.sim_time) / 60;
    int secs = static_cast<int>(entry.sim_time) % 60;
    std::string freq_tag = entry.frequency.empty() ? "" : " " + entry.frequency;
    switch (entry.kind) {
    case atc_session::TranscriptKind::Pilot:
      ImGui::Text("[%02d:%02d%s] You: %s", mins, secs, freq_tag.c_str(),
                  entry.text.c_str());
      break;
    case atc_session::TranscriptKind::Tower: {
      const auto &cx = xplane_context::get();
      std::string apt = !cx.nearest_airport_name.empty()
                            ? cx.nearest_airport_name
                            : cx.nearest_airport_id;
      std::string prefix = apt.empty() ? "ATC" : apt + " ATC";
      ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "[%02d:%02d%s] %s: %s",
                         mins, secs, freq_tag.c_str(), prefix.c_str(),
                         entry.text.c_str());
      break;
    }
    case atc_session::TranscriptKind::System:
      // Plugin-side notice (e.g. radio glitch / TTS failure). Distinct
      // dim-amber colour so the user reads "this is the plugin
      // talking" instead of "ATC just said something".
      ImGui::TextColored(ImVec4(0.85f, 0.65f, 0.20f, 1.0f),
                         "[%02d:%02d] -- %s --", mins, secs,
                         entry.text.c_str());
      break;
    }
  }

  ImGui::PopTextWrapPos();

  // Auto-scroll on new entries
  if (entries.size() != last_transcript_count_) {
    ImGui::SetScrollHereY(1.0f);
    last_transcript_count_ = entries.size();
  }

  ImGui::EndChild();

  // Network-error notice + manual retry (Issue #46). Shown when the
  // interactive TTS exchange gave up after its auto-retries — the pilot
  // otherwise waits silently for a reply that never comes.
  if (atc_session::tts_network_error()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                       ui_strings::tr("transcript.network_error"));
    const bool can_retry =
        atc_session::ptt_state() == atc_session::PTTState::IDLE;
    ImGui::BeginDisabled(!can_retry);
    if (ImGui::Button(ui_strings::tr("btn.retry")))
      atc_session::retry_last_transmission();
    ImGui::EndDisabled();
  } else if (atc_session::tts_blocked()) {
    // Provider content-moderation guardrail rejected the reply (issue #62).
    // Retrying the identical text can never succeed, so show an honest
    // notice and NO retry button — the text is still in the transcript.
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "%s",
                       ui_strings::tr("transcript.tts_blocked"));
  }

  // Debug-Texteingabe: typed pilot transmission injected directly into
  // engine::process_transcript via atc_session::submit_text — bypasses
  // STT but keeps LM + TTS + state machine identical to the voice path.
  if (text_input_active) {
    static char debug_input_buf[256] = {};
    // Sticky "give the input field focus on the next frame where the
    // pipeline is IDLE again" flag. After Enter/Send, the ATC pipeline
    // transitions to PROCESSING/PLAYING, the InputText becomes disabled
    // and ImGui parks the keyboard focus on nothing. Without this, the
    // user has to click back into the field after every reply. Setting
    // the flag here and consuming it after the IDLE check below keeps
    // the chat-style typing UX without re-focusing while the tower is
    // still speaking.
    static bool refocus_input = false;
    const bool can_send =
        atc_session::ptt_state() == atc_session::PTTState::IDLE;
    // InputText shrinks to leave room for [Paste] + [Send] on the right.
    // Cmd+V into an X-Plane ImGui InputText is unreliable (the sim's
    // command bindings swallow the event), so the Paste button reads
    // the system pasteboard via ui::clipboard::read_system_text() —
    // same pattern as the OpenAI API-key field.
    ImGui::BeginDisabled(!can_send);
    if (refocus_input && can_send) {
      ImGui::SetKeyboardFocusHere();
      refocus_input = false;
    }
    ImGui::PushItemWidth(-180.0f);
    bool submit = ImGui::InputText("##debug_text", debug_input_buf,
                                   sizeof(debug_input_buf),
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button(ui_strings::tr("btn.paste"))) {
      std::string clip = ui::clipboard::read_system_text();
      if (!clip.empty()) {
        std::strncpy(debug_input_buf, clip.c_str(),
                     sizeof(debug_input_buf) - 1);
        debug_input_buf[sizeof(debug_input_buf) - 1] = '\0';
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(ui_strings::tr("btn.send"))) {
      submit = true;
    }
    ImGui::EndDisabled();
    if (submit && can_send && debug_input_buf[0] != '\0') {
      atc_session::submit_text(debug_input_buf);
      debug_input_buf[0] = '\0';
      refocus_input = true;
    }
  }
}

// ── Audio test state ─────────────────────────────────────────────

enum class AudioTestState { IDLE, RECORDING, PLAYING };
static AudioTestState audio_test_state_ = AudioTestState::IDLE;
static float audio_test_timer_ = 0.0f;
static std::vector<uint8_t> audio_test_wav_;
static constexpr float kTestRecordDuration = 3.0f;

static void draw_audio_tab() {
  // ── Microphone / Input ──────────────────────────────────────────
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("audio.mic_header"));
  ImGui::TextDisabled("%s", ui_strings::tr("audio.input_device"));
  ImGui::Spacing();

  float delta = ImGui::GetIO().DeltaTime;

  if (audio_test_state_ == AudioTestState::RECORDING) {
    audio_test_timer_ += delta;
    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                       ui_strings::tr("audio.recording_format"),
                       audio_test_timer_, kTestRecordDuration);
    if (audio_test_timer_ >= kTestRecordDuration) {
      audio_recorder::stop_recording();
      audio_test_wav_ = audio_recorder::encode_wav();
      audio_test_timer_ = 0.0f;
      if (!audio_test_wav_.empty()) {
        char log[128];
        std::snprintf(
            log, sizeof(log),
            "[xp_wellys_vfr_atc] Audio test playback - volume: %.2f, "
            "wav: %zu bytes\n",
            settings::volume(), audio_test_wav_.size());
        XPLMDebugString(log);

        // Save WAV to disk for debugging. Resolve the OS temp dir at
        // runtime (std::filesystem is C++17, platform-neutral) instead of
        // hardcoding /tmp — Windows has no /tmp. Fall back to a relative
        // filename if temp_directory_path() throws (no TMPDIR/%TEMP%).
        if (settings::debug_logging()) {
          std::error_code ec;
          std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
          std::filesystem::path wav_path =
              (ec ? std::filesystem::path() : dir) /
              "xp_wellys_vfr_atc_test.wav";
          std::string path = wav_path.string();
          FILE *f = std::fopen(path.c_str(), "wb");
          if (f) {
            std::fwrite(audio_test_wav_.data(), 1, audio_test_wav_.size(), f);
            std::fclose(f);
            char dbg[256];
            std::snprintf(dbg, sizeof(dbg),
                          "[xp_wellys_vfr_atc] Debug: test WAV saved to %s\n",
                          path.c_str());
            XPLMDebugString(dbg);
          }
        }

        audio_test_state_ = AudioTestState::PLAYING;
        audio_player::play_wav(audio_test_wav_, settings::volume());
      } else {
        audio_test_state_ = AudioTestState::IDLE;
        XPLMDebugString(
            "[xp_wellys_vfr_atc] Audio test: WAV encode returned empty "
            "- mic may not be working\n");
      }
    }
  } else if (audio_test_state_ == AudioTestState::PLAYING) {
    if (audio_player::is_playing()) {
      ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "%s",
                         ui_strings::tr("audio.playing"));
    } else {
      audio_test_state_ = AudioTestState::IDLE;
    }
  } else {
    if (ImGui::Button(ui_strings::tr("btn.record_test"))) {
      audio_test_state_ = AudioTestState::RECORDING;
      audio_test_timer_ = 0.0f;
      audio_test_wav_.clear();
      XPLMDebugString(
          "[xp_wellys_vfr_atc] Audio test: starting 3s mic recording\n");
      audio_recorder::start_recording();
    }
    ImGui::TextDisabled("%s", ui_strings::tr("audio.test_hint"));
  }

  ImGui::Separator();

  // ── Output / Speaker ────────────────────────────────────────────
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("audio.speaker_header"));
  ImGui::Spacing();

  // Volume
  float vol = settings::volume();
  if (ImGui::SliderFloat(ui_strings::tr("audio.volume"), &vol, 0.0f, 1.0f)) {
    settings::set_volume(vol);
    settings::save();
  }

  ImGui::TextDisabled("%s", ui_strings::tr("audio.output_hint"));

  if (ImGui::Button(ui_strings::tr("btn.test_speaker"))) {
    audio_player::play_ptt_click();
  }

  ImGui::Separator();

  // ── TTS ─────────────────────────────────────────────────────────
  // Voice selection is no longer per-frequency: the plugin ships with
  // a single Piper voice (en_US-lessac-medium). ATIS speaks slower via
  // length_scale; tower/ground use the same voice at normal rate.
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("audio.tts_header"));
  ImGui::TextDisabled("%s", ui_strings::tr("audio.tts_voice_info"));
}

static void draw_settings_tab() {
  // One-time init of buffers from settings (callsign now inits in the
  // Flugvorbereitung tab, see draw_flightprep_tab).
  if (!buffers_initialized) {
    std::string pdir = settings::pattern_direction();
    pattern_dir_selection = (pdir == "right") ? 1 : 0;
    std::string sm = settings::start_mode();
    start_mode_selection = 1; // engines_running default
    for (size_t i = 0; i < sizeof(start_mode_keys) / sizeof(start_mode_keys[0]);
         ++i) {
      if (sm == start_mode_keys[i]) {
        start_mode_selection = static_cast<int>(i);
        break;
      }
    }
    {
      std::string bm = settings::backend_mode();
      backend_mode_selection = 0;
      for (int i = 0; i < kBackendModeCount; ++i) {
        if (bm == backend_mode_keys[i]) {
          backend_mode_selection = i;
          break;
        }
      }
    }
    {
      std::string lang = settings::atc_language();
      language_selection = 0; // de default
      for (size_t i = 0; i < sizeof(language_keys) / sizeof(language_keys[0]);
           ++i) {
        if (lang == language_keys[i]) {
          language_selection = static_cast<int>(i);
          break;
        }
      }
    }
    {
      std::string ui_lang = settings::ui_language();
      ui_language_selection = 1; // en default
      for (size_t i = 0; i < sizeof(language_keys) / sizeof(language_keys[0]);
           ++i) {
        if (ui_lang == language_keys[i]) {
          ui_language_selection = static_cast<int>(i);
          break;
        }
      }
    }
    // Mistral model slugs no longer live in InputText buffers — the
    // Combo widgets pull current values straight from settings each
    // frame. Nothing to seed here.
    buffers_initialized = true;
  }

  // ── AI Backend ─────────────────────────────────────────────────
  // Sits at the top because it is the highest-impact choice in
  // this tab: it picks the entire inference pipeline (Local vs.
  // OpenAI Cloud) and a change forces a backend reload.
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("settings.backend_header"));
#ifndef XPWELLYS_USE_LOCAL_INFERENCE
  ImGui::TextDisabled("%s", ui_strings::tr("settings.backend_x86_hint"));
#endif
  if (ImGui::Combo(ui_strings::tr("settings.backend_combo"),
                   &backend_mode_selection, backend_mode_labels,
                   kBackendModeCount)) {
    settings::set_backend_mode(backend_mode_keys[backend_mode_selection]);
    settings::save();
    // Tear down whatever was registered and re-run the loader so the
    // newly-selected pipeline comes up. loader::stop() unregisters
    // the backend pointers; start() reads settings::backend_mode()
    // and instantiates accordingly.
    backends::loader::stop();
    backends::loader::start();
    // A backend switch is an explicit "test the new pipeline" signal —
    // drop the ATIS cooldown so the next tune-in plays immediately
    // instead of waiting up to 120 s on the previous backend's timer.
    atc_session::reset_atis_cooldown();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", ui_strings::tr("tooltip.backend"));
  }
  const std::string active_backend_key =
      backend_mode_keys[backend_mode_selection];
  const bool show_openai_controls = (active_backend_key == "openai");
  const bool show_mistral_controls = (active_backend_key == "mistral");
  // Only read inside the hybrid-TTS blocks below (compiled wherever Piper
  // TTS is available).
  [[maybe_unused]] const bool is_cloud_mode =
      show_openai_controls || show_mistral_controls;
  // Hybrid TTS (issue #66): local Piper voice for speech while STT+LM stay
  // on the cloud backend. When active, the cloud TTS model/voice combos are
  // hidden and the Piper voice combos are shown instead — so the
  // speech-output controls always match the active TTS. Gated on the
  // Piper-TTS flag so it also lights up on x86_64-macOS / Windows (#69/#70).
#ifdef XPWELLYS_USE_LOCAL_TTS
  const bool tts_local_override =
      is_cloud_mode && settings::tts_backend_override() == "local";
#else
  const bool tts_local_override = false;
#endif

  if (show_openai_controls) {
    // OpenAI mode — show the key + model + voice controls. None of
    // these fields are visible in Local mode.
    ImGui::Indent();

    // API key: password-masked TextInput, plus three action buttons.
    bool has_key = settings::api_key_saved();
    if (has_key) {
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s",
                         ui_strings::tr("settings.api_key_saved"));
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s",
                         ui_strings::tr("settings.api_key_missing"));
    }
    ImGui::InputText(ui_strings::tr("settings.api_key_label"), api_key_buf,
                     sizeof(api_key_buf), ImGuiInputTextFlags_Password);
    // Explicit Paste button — Cmd+V into a password-flagged InputText
    // is unreliable inside the X-Plane ImGui context (key events get
    // intercepted by the sim's command bindings before ImGui sees
    // them), AND ImGui::GetClipboardText() in this plugin only sees
    // ImGui's internal buffer, not the system pasteboard (no
    // platform backend is wired up). Read NSPasteboard directly via
    // ui::clipboard::read_system_text().
    if (ImGui::Button(ui_strings::tr("btn.paste"))) {
      std::string clip = ui::clipboard::read_system_text();
      if (!clip.empty()) {
        std::strncpy(api_key_buf, clip.c_str(), sizeof(api_key_buf) - 1);
        api_key_buf[sizeof(api_key_buf) - 1] = '\0';
        std::snprintf(api_key_feedback_msg, sizeof(api_key_feedback_msg),
                      ui_strings::tr("settings.api_pasted_format"),
                      std::strlen(api_key_buf));
        api_key_feedback_timer = 3.0f;
      } else {
        std::snprintf(api_key_feedback_msg, sizeof(api_key_feedback_msg), "%s",
                      ui_strings::tr("settings.clipboard_empty"));
        api_key_feedback_timer = 3.0f;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(ui_strings::tr("btn.save_key"))) {
      if (api_key_buf[0] != '\0' && settings::save_api_key(api_key_buf)) {
        std::snprintf(api_key_feedback_msg, sizeof(api_key_feedback_msg), "%s",
                      ui_strings::tr("settings.api_saved"));
        api_key_feedback_timer = 3.0f;
        // Wipe the in-memory buffer — the key now lives in the
        // Keychain only.
        std::memset(api_key_buf, 0, sizeof(api_key_buf));
        backends::loader::stop();
        backends::loader::start();
      } else {
        std::snprintf(api_key_feedback_msg, sizeof(api_key_feedback_msg), "%s",
                      ui_strings::tr("settings.api_save_failed"));
        api_key_feedback_timer = 3.0f;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(ui_strings::tr("btn.delete_key"))) {
      settings::delete_api_key();
      std::memset(api_key_buf, 0, sizeof(api_key_buf));
      std::snprintf(api_key_feedback_msg, sizeof(api_key_feedback_msg), "%s",
                    ui_strings::tr("settings.api_deleted"));
      api_key_feedback_timer = 3.0f;
      backends::loader::stop();
      backends::loader::start();
    }
    if (api_key_feedback_timer > 0.0f) {
      ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                         api_key_feedback_msg);
      api_key_feedback_timer -= ImGui::GetIO().DeltaTime;
    }

    // Model + voice combos — driven by data/models_catalog.json so
    // the user can add new slugs without recompiling.
    combo_from_catalog(
        ui_strings::tr("settings.stt_model"),
        models_catalog::openai_stt_options(), settings::openai_stt_model(),
        [](const std::string &v) { settings::set_openai_stt_model(v); });
    combo_from_catalog(
        ui_strings::tr("settings.lm_model"),
        models_catalog::openai_lm_options(), settings::openai_lm_model(),
        [](const std::string &v) { settings::set_openai_lm_model(v); });
    // TTS model + voices are hidden when the local Piper TTS override is
    // active (issue #66) — the Piper voice combos below take their place.
    if (!tts_local_override) {
      combo_from_catalog(
          ui_strings::tr("settings.tts_model"),
          models_catalog::openai_tts_options(), settings::openai_tts_model(),
          [](const std::string &v) { settings::set_openai_tts_model(v); });

      combo_from_catalog(
          ui_strings::tr("settings.atis_voice"),
          models_catalog::openai_voice_options(),
          settings::openai_tts_voice_atis(),
          [](const std::string &v) { settings::set_openai_tts_voice_atis(v); });
      combo_from_catalog(
          ui_strings::tr("settings.tower_voice"),
          models_catalog::openai_voice_options(),
          settings::openai_tts_voice_tower(),
          [](const std::string &v) { settings::set_openai_tts_voice_tower(v); });
      combo_from_catalog(ui_strings::tr("settings.ground_voice"),
                         models_catalog::openai_voice_options(),
                         settings::openai_tts_voice_ground(),
                         [](const std::string &v) {
                           settings::set_openai_tts_voice_ground(v);
                         });
      ImGui::TextDisabled("%s", ui_strings::tr("settings.voice_tip"));
    }

    ImGui::Unindent();
  }

  if (show_mistral_controls) {
    // Mistral mode — separate Keychain entry + free-text model and
    // voice ids. Mirrors the OpenAI panel structurally so the user
    // sees the same Paste / Save / Delete affordances.
    ImGui::Indent();

    bool has_mistral_key = settings::mistral_api_key_saved();
    if (has_mistral_key) {
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s",
                         "Mistral API key saved");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s",
                         "No Mistral API key saved");
    }
    ImGui::InputText("Mistral API key", mistral_key_buf,
                     sizeof(mistral_key_buf), ImGuiInputTextFlags_Password);
    if (ImGui::Button("Paste##mistral")) {
      std::string clip = ui::clipboard::read_system_text();
      if (!clip.empty()) {
        std::strncpy(mistral_key_buf, clip.c_str(),
                     sizeof(mistral_key_buf) - 1);
        mistral_key_buf[sizeof(mistral_key_buf) - 1] = '\0';
        std::snprintf(mistral_key_feedback_msg,
                      sizeof(mistral_key_feedback_msg),
                      "Pasted %zu characters from clipboard",
                      std::strlen(mistral_key_buf));
        mistral_key_feedback_timer = 3.0f;
      } else {
        std::snprintf(mistral_key_feedback_msg,
                      sizeof(mistral_key_feedback_msg), "%s",
                      "Clipboard is empty");
        mistral_key_feedback_timer = 3.0f;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Key##mistral")) {
      if (mistral_key_buf[0] != '\0' &&
          settings::save_mistral_api_key(mistral_key_buf)) {
        std::snprintf(mistral_key_feedback_msg,
                      sizeof(mistral_key_feedback_msg), "%s",
                      "Mistral key saved to Keychain");
        mistral_key_feedback_timer = 3.0f;
        std::memset(mistral_key_buf, 0, sizeof(mistral_key_buf));
        backends::loader::stop();
        backends::loader::start();
      } else {
        std::snprintf(mistral_key_feedback_msg,
                      sizeof(mistral_key_feedback_msg), "%s",
                      "Failed to save Mistral key");
        mistral_key_feedback_timer = 3.0f;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Key##mistral")) {
      settings::delete_mistral_api_key();
      std::memset(mistral_key_buf, 0, sizeof(mistral_key_buf));
      std::snprintf(mistral_key_feedback_msg, sizeof(mistral_key_feedback_msg),
                    "%s", "Mistral key removed");
      mistral_key_feedback_timer = 3.0f;
      backends::loader::stop();
      backends::loader::start();
    }
    if (mistral_key_feedback_timer > 0.0f) {
      ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                         mistral_key_feedback_msg);
      mistral_key_feedback_timer -= ImGui::GetIO().DeltaTime;
    }

    // Model + voice combos — same data/models_catalog.json that drives
    // the OpenAI combos above. Adding new slugs (e.g. when Mistral
    // ships a new Voxtral snapshot) is a JSON edit + restart, no
    // recompile.
    combo_from_catalog(
        "STT model##mistral", models_catalog::mistral_stt_options(),
        settings::mistral_stt_model(),
        [](const std::string &v) { settings::set_mistral_stt_model(v); });
    combo_from_catalog(
        "LM model##mistral", models_catalog::mistral_lm_options(),
        settings::mistral_lm_model(),
        [](const std::string &v) { settings::set_mistral_lm_model(v); });
    // TTS model + voices are hidden when the local Piper TTS override is
    // active (issue #66) — the Piper voice combos below take their place.
    if (!tts_local_override) {
      combo_from_catalog(
          "TTS model##mistral", models_catalog::mistral_tts_options(),
          settings::mistral_tts_model(),
          [](const std::string &v) { settings::set_mistral_tts_model(v); });

      combo_from_catalog(
          "ATIS voice##mistral", models_catalog::mistral_voice_options(),
          settings::mistral_tts_voice_atis(),
          [](const std::string &v) { settings::set_mistral_tts_voice_atis(v); });
      combo_from_catalog("Tower voice##mistral",
                         models_catalog::mistral_voice_options(),
                         settings::mistral_tts_voice_tower(),
                         [](const std::string &v) {
                           settings::set_mistral_tts_voice_tower(v);
                         });
      combo_from_catalog("Ground voice##mistral",
                         models_catalog::mistral_voice_options(),
                         settings::mistral_tts_voice_ground(),
                         [](const std::string &v) {
                           settings::set_mistral_tts_voice_ground(v);
                         });
      ImGui::TextDisabled(
          "%s",
          "Voxtral preset voices - \"EN-GB Oliver (neutral)\" reads closest to "
          "ICAO ATC. Edit data/models_catalog.json to add custom clones.");
    }

    ImGui::Unindent();
  }
  ImGui::Separator();

#ifdef XPWELLYS_USE_LOCAL_TTS
  // Hybrid TTS toggle (issue #66): only offered in a cloud mode (in local
  // mode TTS is already Piper), and only enabled once a Piper voice for the
  // active language is verified — otherwise it would silently fall back to
  // the accented cloud voice. The Piper voice combos appear below when on.
  // Gated on the Piper-TTS flag so it also appears on the x86_64-macOS /
  // Windows slices (#69/#70), not just Apple Silicon.
  if (is_cloud_mode) {
    // Gate on the voice files being present on disk (a cheap stat via
    // size_matches), NOT on the loader snapshot state: in a cloud mode the
    // loader never verifies Piper files, so their snapshot state stays
    // NotChecked even when the voice is downloaded. Enabling the toggle
    // restarts the loader, which SHA-verifies + loads the voice for real
    // (and falls back to cloud TTS if that fails).
    bool piper_ready = false;
    {
      using K = model_manifest::Kind;
      const std::string vlang = settings::backend_language();
      const std::string &dir = model_paths::models_dir();
      for (const auto &id : model_manifest::voice_ids()) {
        if (model_manifest::voice_language(id) != vlang)
          continue;
        const auto *onnx = model_manifest::get_voice(K::PiperVoice, id);
        const auto *json = model_manifest::get_voice(K::PiperVoiceConfig, id);
        if (onnx && json &&
            model_manifest::size_matches(*onnx, dir + "/" + onnx->filename) &&
            model_manifest::size_matches(*json, dir + "/" + json->filename)) {
          piper_ready = true;
          break;
        }
      }
    }
    bool hybrid = settings::tts_backend_override() == "local";
    // Allow turning it OFF even if the voice went missing; only block
    // turning it ON when no voice is ready.
    ImGui::BeginDisabled(!piper_ready && !hybrid);
    if (ImGui::Checkbox(ui_strings::tr("settings.tts_local_override"),
                        &hybrid)) {
      settings::set_tts_backend_override(hybrid ? "local" : "");
      settings::save();
      backends::loader::stop();
      backends::loader::start();
      atc_session::reset_atis_cooldown();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", ui_strings::tr("tooltip.tts_local_override"));
    if (!piper_ready)
      ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s",
                         ui_strings::tr("settings.tts_local_override_hint"));
    ImGui::Separator();
  }
#endif

  // PTT — bound via X-Plane settings
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("settings.ptt_header"));
  ImGui::TextDisabled("%s", ui_strings::tr("settings.ptt_bind_hint"));
  ImGui::TextDisabled("%s", ui_strings::tr("settings.ptt_command"));
  ImGui::Separator();

  // Pilot callsign (Kennzeichen) moved to the "Flugvorbereitung" tab in the
  // ATC Commands Panel — it is per-flight identity, grouped with flight type
  // and destination, not buried in config.

  // Pattern direction (left/right hand traffic). The names array doubles
  // as persistence key, so we keep it English ("left"/"right") and build
  // a separate translated label array for the combo display.
  const char *pattern_dir_labels[2] = {
      ui_strings::tr("pattern.left"),
      ui_strings::tr("pattern.right"),
  };
  if (ImGui::Combo(ui_strings::tr("settings.pattern_dir_label"),
                   &pattern_dir_selection, pattern_dir_labels, 2)) {
    settings::set_pattern_direction(pattern_dir_names[pattern_dir_selection]);
    settings::save();
  }

  // VFR flight type + destination moved to the "Flugvorbereitung" tab in the
  // ATC Commands Panel (see draw_flightprep_tab) so it sits with the operative
  // flow rather than buried in Settings.

  // Interface (UI-chrome) language — DE or EN (Issue #56). Decoupled from
  // the ATC phraseology language below; only ui_strings.json follows it.
  // Switches live via ui_strings::reload() — no restart needed. Plain
  // language labels (no NfL/ICAO annotation — that belongs to phraseology).
  const char *ui_language_labels_tr[2] = {
      ui_strings::tr("settings.ui_language_de"),
      ui_strings::tr("settings.ui_language_en"),
  };
  if (ImGui::Combo(ui_strings::tr("settings.ui_language_label"),
                   &ui_language_selection, ui_language_labels_tr, 2)) {
    settings::set_ui_language(language_keys[ui_language_selection]);
    settings::save();
    ui_strings::reload();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", ui_strings::tr("tooltip.ui_language"));
  }

  // ATC phraseology language — DE (NfL DACH-VFR) or EN (ICAO-VFR). Governs
  // the spoken phraseology, ATC responses and hint contents. Applies on the
  // next plugin start: the profile bundle and the local TTS voice are loaded
  // during init. Labels carry the NfL/ICAO annotation here.
  const char *language_labels_tr[2] = {
      ui_strings::tr("settings.language_de"),
      ui_strings::tr("settings.language_en"),
  };
  if (ImGui::Combo(ui_strings::tr("settings.language_label"),
                   &language_selection, language_labels_tr, 2)) {
    settings::set_atc_language(language_keys[language_selection]);
    settings::save();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", ui_strings::tr("tooltip.language"));
  }

  // Cockpit start mode — applies on next plugin enable / sim start.
  const char *start_mode_labels_tr[3] = {
      ui_strings::tr("startmode.cold_dark"),
      ui_strings::tr("startmode.engines_running"),
      ui_strings::tr("startmode.ready_takeoff"),
  };
  (void)start_mode_labels; // English fallback array; UI uses tr() now.
  if (ImGui::Combo(ui_strings::tr("settings.start_mode_label"),
                   &start_mode_selection, start_mode_labels_tr, 3)) {
    settings::set_start_mode(start_mode_keys[start_mode_selection]);
    settings::save();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", ui_strings::tr("tooltip.start_mode"));
  }

  // Debug logging
  bool debug = settings::debug_logging();
  if (ImGui::Checkbox(ui_strings::tr("settings.debug_log"), &debug)) {
    settings::set_debug_logging(debug);
  }

  // Debug text input — replaces PTT/STT with a typed transcript in the
  // Transcript tab. Useful on laptops without a headset, or to isolate
  // ATC-logic bugs from STT misrecognitions. PTT stays functional in
  // parallel.
  bool text_in = settings::debug_text_input();
  if (ImGui::Checkbox(ui_strings::tr("settings.debug_text_input"), &text_in)) {
    settings::set_debug_text_input(text_in);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", ui_strings::tr("tooltip.debug_text_input"));
  }

  // Skip radio power check (workaround for exotic aircraft)
  bool skip_power = settings::skip_radio_power_check();
  if (ImGui::Checkbox(ui_strings::tr("settings.skip_radio_power"),
                      &skip_power)) {
    settings::set_skip_radio_power_check(skip_power);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", ui_strings::tr("tooltip.skip_radio_power"));
  }

  // ATC state recovery timing
  float acf = settings::auto_correction_factor();
  char acf_label[32];
  std::snprintf(acf_label, sizeof(acf_label),
                ui_strings::tr("settings.recovery_format"),
                30.0f * acf); // show base recovery time (30s * factor)
  if (ImGui::SliderFloat(ui_strings::tr("settings.recovery_label"), &acf, 0.5f,
                         2.0f, acf_label)) {
    settings::set_auto_correction_factor(acf);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", ui_strings::tr("tooltip.recovery"));
  }

  // Phraseology hints toggle
  bool hints = settings::show_phraseology_hints();
  if (ImGui::Checkbox(ui_strings::tr("settings.show_hints"), &hints)) {
    settings::set_show_phraseology_hints(hints);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", ui_strings::tr("tooltip.show_hints"));
  }

  // Disable Default X-Plane ATC
  bool disable_xp_atc = settings::disable_default_atc();
  if (ImGui::Checkbox(ui_strings::tr("settings.disable_default_atc"),
                      &disable_xp_atc)) {
    settings::set_disable_default_atc(disable_xp_atc);
  }

  // Traffic features master switch. Drives all advisory / sequencing /
  // go-around logic in one place. The plugin reads the standard X-Plane
  // TCAS dataRefs (filled by LiveTraffic, xPilot, swift, X-IvAp or the
  // native AI traffic), so there is no reliable way to auto-detect a
  // specific provider — turning the whole subsystem off is the explicit
  // opt-out for users who don't want traffic-driven controller chatter.
  bool traffic_on = settings::traffic_features_enabled();
  if (ImGui::Checkbox(ui_strings::tr("settings.enable_traffic"), &traffic_on)) {
    settings::set_traffic_features_enabled(traffic_on);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", ui_strings::tr("tooltip.enable_traffic"));
  }

  // BZF-Strict-Mode toggle. The corrective tower behaviour is anchored
  // in NfL Sprechfunk 2024 §25 b) Nr. 1 (mandatory readback elements) —
  // German BZF exam didactics with no ICAO 1:1 counterpart. Both engine
  // hooks (apply_bzf_strict_check / apply_initial_call_conformance) gate on
  // atc_profile()=="DE", so the toggle is inert under the EN/ICAO profile;
  // hide it there rather than offer a dead control (#42, Option A).
  if (settings::atc_profile() == "DE") {
    bool strict = settings::bzf_strict_mode();
    if (ImGui::Checkbox(ui_strings::tr("settings.bzf_strict_mode"), &strict)) {
      settings::set_bzf_strict_mode(strict);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", ui_strings::tr("tooltip.bzf_strict_mode"));
    }
    // STT-quality caveat: strict readback checking is only as good as
    // the transcription feeding it. Local and Mistral STT garble
    // spelled-out callsigns, so strict mode can reject otherwise
    // correct readbacks. Surface this inline whenever strict is on.
    if (strict) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                         ui_strings::tr("settings.bzf_strict_stt_warning"));
      ImGui::PopTextWrapPos();
    }
  }

  // ── Voices per ATC role ─────────────────────────────────────────
  // Each role gets a dropdown listing every voice that is currently
  // Ready (loaded into Piper). The defaults are seeded by
  // settings::default_config() so a fresh install always plays sound;
  // selecting a different voice triggers loader::start() which loads
  // the new voice on a worker thread (~300 ms blip on M1, then
  // permanent).
  //
  // Local mode only — both cloud providers ship their own voice
  // catalog with dedicated combos above. Rendering both blocks at
  // once would clash on the identical ImGui labels ("ATIS Voice"
  // etc.) and leak Piper voice ids into the cloud synthesize() path,
  // which silently fails (cloud has_voice() rejects local ids).
  // Shown in Local mode, and in a cloud mode when the local Piper TTS
  // override is active (issue #66) — in both cases speech comes from Piper,
  // so these are the voice controls that actually apply.
  if ((!show_openai_controls && !show_mistral_controls) || tts_local_override) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                       ui_strings::tr("settings.voices_header"));
    {
      auto loader_status = backends::loader::snapshot();
      const std::string voice_lang = settings::backend_language();
      // Build list of "Ready" voice ids — these are the only ones we
      // expose in the dropdown because picking an un-Ready voice would
      // silently fall back to the manifest default at speak time.
      // Also language-filtered: picking a voice from the other
      // language slot would be reset by the next region migration.
      std::vector<std::string> ready_ids;
      for (const auto &id : model_manifest::voice_ids()) {
        if (model_manifest::voice_language(id) != voice_lang)
          continue;
        bool onnx_ready = false, json_ready = false;
        for (const auto &fs : loader_status.files) {
          if (fs.voice_id != id)
            continue;
          if (fs.kind == model_manifest::Kind::PiperVoice &&
              fs.state == backends::loader::FileState::Ready)
            onnx_ready = true;
          if (fs.kind == model_manifest::Kind::PiperVoiceConfig &&
              fs.state == backends::loader::FileState::Ready)
            json_ready = true;
        }
        if (onnx_ready && json_ready)
          ready_ids.push_back(id);
      }

      auto draw_role_combo = [&](const char *label,
                                 model_manifest::VoiceRole role) {
        std::string current = settings::voice_for_role(role);
        // If the configured voice is not Ready, still show it (greyed
        // out implicitly via the "(loading)" marker) so the user knows
        // why the dropdown does not match what they picked.
        if (ImGui::BeginCombo(label, current.c_str())) {
          for (const auto &id : ready_ids) {
            bool selected = (id == current);
            if (ImGui::Selectable(id.c_str(), selected)) {
              settings::set_voice_for_role(role, id);
              settings::save();
              // No loader action: the combo only lists voices that are
              // already Ready (loaded into Piper), and the manager picks
              // the voice by id at synthesize time. A full loader::start()
              // here would needlessly re-hash every model (incl. the 2 GB
              // Llama), flicker the whole backend to "not ready", and light
              // up the Models tab with a spurious "loading" error.
            }
            if (selected)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
      };
      draw_role_combo(ui_strings::tr("settings.atis_voice"),
                      model_manifest::VoiceRole::Atis);
      draw_role_combo(ui_strings::tr("settings.tower_voice"),
                      model_manifest::VoiceRole::Tower);
      draw_role_combo(ui_strings::tr("settings.ground_voice"),
                      model_manifest::VoiceRole::Ground);
      draw_role_combo(ui_strings::tr("settings.unicom_voice"),
                      model_manifest::VoiceRole::Unicom);
      // German ships one required voice (Thorsten) that covers all four
      // roles out of the box; three more German voices are optional
      // downloads (Models tab) so each role can sound distinct.
      const size_t expected_voices = (voice_lang == "de") ? 1u : 4u;
      if (ready_ids.size() < expected_voices) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s",
                           ui_strings::tr("settings.voices_tip"));
      }
    }
  } // local mode (Piper voice block)

  ImGui::Separator();
  if (ImGui::Button(ui_strings::tr("btn.save_settings"))) {
    settings::save();
  }

  if (settings::disable_default_atc()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                       ui_strings::tr("settings.conflicts_header"));
    ImGui::TextWrapped("%s", ui_strings::tr("settings.conflicts_body"));
    ImGui::BulletText("sim/operation/contact_atc_ptt");
    ImGui::BulletText("sim/operation/contact_atc");
    ImGui::BulletText("sim/operation/atc_readback");
    ImGui::BulletText("sim/operation/toggle_auto_checkin");
    ImGui::BulletText("sim/operation/toggle_auto_readback");
    ImGui::BulletText("sim/operation/toggle_taxi_arrows");
  }

  // Flight logbook — manual backstop for splitting flights when the
  // automatic heuristic / X-Plane reposition message does not catch a
  // transition. Starts a fresh data/flightlog/*.json on the next call.
  ImGui::Separator();
  ImGui::TextDisabled("%s", ui_strings::tr("flightlog.header"));
  if (ImGui::Button(ui_strings::tr("btn.new_flight"))) {
    cross_country_log::begin_new_flight();
    // Mirror the airport/plane-reload path: disarm was_airborne so a manual
    // "new flight" is treated as fresh-spawn-IDLE, not post-landing-IDLE.
    // Flag only — not a full reset() (see main.cpp XPluginReceiveMessage).
    atc_state_machine::set_was_airborne(false);
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", ui_strings::tr("flightlog.new_flight_tip"));

  // About section
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextDisabled("%s", ui_strings::tr("settings.about_header"));
#ifdef XP_WELLYS_ATC_VERSION
  ImGui::Text(ui_strings::tr("about.title_format"), XP_WELLYS_ATC_VERSION);
#else
  ImGui::Text("%s", ui_strings::tr("about.title_dev"));
#endif
  ImGui::Text("%s", ui_strings::tr("about.tagline"));
  ImGui::TextDisabled("%s", ui_strings::tr("about.repo"));
}

// ── ATC Commands Panel ──────────────────────────────────────────

static const char *intent_display_label(const std::string &key) {
  static const std::pair<const char *, const char *> labels[] = {
      {"INITIAL_CALL_GROUND", "intent.initial_call_ground"},
      {"INITIAL_CALL_TOWER", "intent.initial_call_tower"},
      {"INITIAL_CALL_INBOUND", "intent.initial_call_inbound"},
      {"REQUEST_TAXI", "intent.request_taxi"},
      {"REQUEST_TAXI_PARKING", "intent.request_taxi_parking"},
      {"READY_FOR_DEPARTURE", "intent.ready_for_departure"},
      {"REPORT_POSITION", "intent.report_position"},
      {"REPORT_POSITION_DOWNWIND", "intent.report_downwind"},
      {"REPORT_POSITION_BASE", "intent.report_base"},
      {"REPORT_POSITION_FINAL", "intent.report_final"},
      {"REQUEST_LANDING", "intent.request_landing"},
      {"REQUEST_TOUCH_AND_GO", "intent.request_touch_and_go"},
      {"GO_AROUND", "intent.go_around"},
      {"RUNWAY_VACATED", "intent.runway_vacated"},
      {"READBACK", "intent.readback"},
      {"REQUEST_FREQUENCY", "intent.request_frequency"},
      {"RADIO_CHECK", "intent.radio_check"},
      {"SELF_ANNOUNCE", "intent.self_announce"},
      {"SELF_ANNOUNCE_DEPARTURE", "intent.self_announce_departure"},
  };
  for (const auto &p : labels)
    if (key == p.first)
      return ui_strings::tr(p.second);
  return key.c_str();
}

static const char *freq_type_label(xplane_context::FrequencyType ft) {
  switch (ft) {
  case xplane_context::FrequencyType::ATIS:
    return "ATIS";
  case xplane_context::FrequencyType::DELIVERY:
    return "DEL";
  case xplane_context::FrequencyType::GROUND:
    return "GND";
  case xplane_context::FrequencyType::TOWER:
    return "TWR";
  case xplane_context::FrequencyType::UNICOM:
    return "UNICOM";
  case xplane_context::FrequencyType::CTAF:
    return "CTAF";
  case xplane_context::FrequencyType::INFO:
    return "INFO";
  case xplane_context::FrequencyType::RADIO:
    return "RADIO";
  default:
    return "???";
  }
}

// ── Shared pilot action buttons (used by both tabs) ─────────────

static void draw_pilot_actions(const xplane_context::XPlaneContext &ctx,
                               bool force_towered = false) {
  if (!settings::show_phraseology_hints())
    return;

  using FT = xplane_context::FrequencyType;
  using Facility = xplane_context::FacilityType;
  // Facility for the hint matrix. Preserve the legacy downgrade: a towered
  // field tuned to a UNICOM/CTAF frequency surfaces uncontrolled hints (matches
  // the former is_towered bool). AFIS/UNCONTROLLED/UNKNOWN pass through
  // unchanged.
  Facility facility = force_towered ? Facility::TOWERED : ctx.facility_type;
  if (facility == Facility::TOWERED &&
      (ctx.frequency_type == FT::UNICOM || ctx.frequency_type == FT::CTAF))
    facility = Facility::UNCONTROLLED;

  auto atc_state = atc_state_machine::get_state();
  std::string state_str = atc_state_machine::state_name(atc_state);
  auto phase = flight_phase::get();

  // post_landing: pilot is on the ground at the airport after a recent
  // landing or touch-and-go, with no new DEPARTURE_CLEARED in between.
  // History-driven so the window survives long stand times — pilot can
  // still pick up REQUEST_TAXI_PARKING after a long stop.
  bool post_landing = atc_state_machine::at_airport_after_landing(ctx);

  // Matrix lookup: deklarative State x Phase x Facility x Frequency rules
  // in data/atc_profiles/<region>/phraseology_hints.json.
  phraseology_hints::HintQuery query{};
  query.state = atc_state;
  query.phase = phase;
  query.facility = facility;
  query.frequency_type = ctx.frequency_type;
  query.tower_only = ctx.tower_only;
  query.post_landing = post_landing;
  std::vector<std::string> raw = phraseology_hints::lookup(query);
  std::vector<std::string> valid = raw;

  // Defense-in-depth: drop intents the matrix surfaced but that the
  // frequency rules or phase preconditions still reject. The matrix
  // SHOULD never let one of these through, but a JSON typo or schema
  // drift shouldn't render a wrong-frequency hint.
  std::vector<std::pair<std::string, std::string>> filtered;
  valid.erase(std::remove_if(
                  valid.begin(), valid.end(),
                  [&](const std::string &key) {
                    const char *reason = nullptr;
                    if (!flight_phase::is_intent_valid_for_frequency(
                            key, ctx.frequency_type)) {
                      if (!(ctx.tower_only && ctx.frequency_type == FT::TOWER))
                        reason = "frequency";
                    }
                    if (!reason &&
                        !flight_phase::check_precondition(key, phase).empty())
                      reason = "phase";
                    if (reason) {
                      filtered.emplace_back(key, reason);
                      return true;
                    }
                    return false;
                  }),
              valid.end());

  // Readback override: when ATC expects a readback, that's the ONLY
  // legitimate action regardless of what the matrix offers.
  bool readback_override = false;
  if (atc_state_machine::is_readback_pending()) {
    valid.clear();
    valid.push_back("READBACK");
    readback_override = true;
  }

  // ── Hint pipeline DEBUG dump ────────────────────────────────────
  // Throttle: only log when state/phase/freq/airport/readback changed,
  // so the per-frame UI redraw doesn't spam Log.txt. The fingerprint
  // captures the inputs that drive `valid` and the filter outcome.
  if (settings::debug_logging()) {
    static std::string last_fp;
    char fp[256];
    std::snprintf(fp, sizeof(fp), "%s|%s|%d|%s|%d|%d", state_str.c_str(),
                  flight_phase::phase_name(phase),
                  static_cast<int>(ctx.frequency_type),
                  ctx.nearest_airport_id.c_str(), readback_override ? 1 : 0,
                  static_cast<int>(valid.size()));
    if (last_fp != fp) {
      last_fp = fp;
      std::string raw_join, valid_join, filt_join;
      for (const auto &k : raw) {
        if (!raw_join.empty())
          raw_join += ",";
        raw_join += k;
      }
      for (const auto &k : valid) {
        if (!valid_join.empty())
          valid_join += ",";
        valid_join += k;
      }
      for (const auto &[k, r] : filtered) {
        if (!filt_join.empty())
          filt_join += ",";
        filt_join += k;
        filt_join += ":";
        filt_join += r;
      }
      logging::debug(
          "Hints: state=%s phase=%s freq=%s airport=%s facility=%s "
          "tower_only=%d on_ground=%d readback_pending=%d",
          state_str.c_str(), flight_phase::phase_name(phase),
          freq_type_label(ctx.frequency_type), ctx.nearest_airport_id.c_str(),
          xplane_context::facility_type_name(facility), ctx.tower_only ? 1 : 0,
          ctx.on_ground ? 1 : 0, readback_override ? 1 : 0);
      logging::debug("Hints raw [%zu]: %s", raw.size(),
                     raw_join.empty() ? "(none)" : raw_join.c_str());
      logging::debug("Hints filtered [%zu]: %s", filtered.size(),
                     filt_join.empty() ? "(none)" : filt_join.c_str());
      logging::debug("Hints final [%zu]: %s", valid.size(),
                     valid_join.empty() ? "(none)" : valid_join.c_str());
    }
  }

  // Button category for grouping. At tower-only airports the same
  // controller handles taxi clearances on the tower frequency, so collapse
  // ground-ops hints into the Tower category to avoid showing two
  // categories that route to the same controller.
  enum class BtnCat { GROUND_OPS, TOWER_OPS, PATTERN, GENERAL };
  bool collapse_ground = ctx.tower_only;
  auto intent_category = [collapse_ground](const std::string &key) -> BtnCat {
    if (key == "INITIAL_CALL_GROUND" || key == "REQUEST_TAXI" ||
        key == "REQUEST_TAXI_PARKING")
      return collapse_ground ? BtnCat::TOWER_OPS : BtnCat::GROUND_OPS;
    if (key == "INITIAL_CALL_TOWER" || key == "READY_FOR_DEPARTURE" ||
        key == "READY_FOR_DEPARTURE_VFR" || key == "RUNWAY_VACATED")
      return BtnCat::TOWER_OPS;
    if (key == "REPORT_POSITION" || key == "REPORT_POSITION_DOWNWIND" ||
        key == "REPORT_POSITION_BASE" || key == "REPORT_POSITION_FINAL" ||
        key == "REQUEST_LANDING" || key == "REQUEST_TOUCH_AND_GO" ||
        key == "GO_AROUND" || key == "INITIAL_CALL_INBOUND")
      return BtnCat::PATTERN;
    return BtnCat::GENERAL;
  };
  auto category_label = [](BtnCat cat) -> const char * {
    switch (cat) {
    case BtnCat::GROUND_OPS:
      return ui_strings::tr("category.ground_ops");
    case BtnCat::TOWER_OPS:
      return ui_strings::tr("category.tower_ops");
    case BtnCat::PATTERN:
      return ui_strings::tr("category.pattern");
    case BtnCat::GENERAL:
      return ui_strings::tr("category.general");
    }
    return "";
  };

  // Sort by category
  std::stable_sort(valid.begin(), valid.end(),
                   [&](const std::string &a, const std::string &b) {
                     return static_cast<int>(intent_category(a)) <
                            static_cast<int>(intent_category(b));
                   });

  // Phraseology header + Disregard button (always shown when hints enabled)
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("hints.header"));
  if (atc_state != atc_state_machine::ATCState::IDLE) {
    ImGui::SameLine();
    if (ImGui::SmallButton(ui_strings::tr("btn.disregard"))) {
      atc_state_machine::disregard(ctx, phase,
                                   static_cast<double>(XPLMGetElapsedTime()));
      XPLMDebugString("[xp_wellys_vfr_atc] Manual disregard\n");
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", ui_strings::tr("tooltip.disregard"));
    }
  }
  ImGui::TextDisabled(ui_strings::tr("hints.state_phase_format"),
                      state_str.c_str(), flight_phase::phase_name(phase));

  if (!valid.empty()) {
    bool radio_off = !ctx.com_radio_powered;

    // Build two var sets: short (display) and spoken (tooltip)
    intent_parser::PilotMessage dummy_msg{};
    dummy_msg.runway = atc_state_machine::effective_runway(ctx);

    // Display version: short callsign (e.g. "HBAKA")
    dummy_msg.callsign = settings::pilot_callsign_raw();
    auto vars_short = ground_ops::build_vars(dummy_msg, ctx);

    // Spoken version: full phonetic (e.g. "Hotel Bravo Alpha Kilo Alpha")
    dummy_msg.callsign = settings::pilot_callsign();
    auto vars_spoken = ground_ops::build_vars(dummy_msg, ctx);

    ImGui::PushTextWrapPos(0.0f); // wrap at window edge

    BtnCat last_cat = static_cast<BtnCat>(-1);
    for (const auto &key : valid) {
      BtnCat cat = intent_category(key);
      if (cat != last_cat) {
        if (last_cat != static_cast<BtnCat>(-1))
          ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                           category_label(cat));
        last_cat = cat;
      }

      // Show phraseology as text hint
      std::string phrase_tmpl = flight_phase::get_pilot_phraseology(key);
      if (!phrase_tmpl.empty()) {
        std::string display = atc_templates::fill(phrase_tmpl, vars_short);
        if (radio_off) {
          ImGui::TextDisabled("  %s", display.c_str());
        } else {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.0f, 0.5f, 1.0f));
          ImGui::Text("  %s", display.c_str());
          ImGui::PopStyleColor();
        }
        // Tooltip: full spoken phraseology with phonetic callsign
        if (ImGui::IsItemHovered()) {
          std::string spoken = atc_templates::fill(phrase_tmpl, vars_spoken);
          ImGui::SetTooltip(ui_strings::tr("hints.say_format"), spoken.c_str());
        }
      } else {
        const char *label = intent_display_label(key);
        ImGui::TextDisabled("  [%s]", label);
      }
    }

    ImGui::PopTextWrapPos();
  } else {
    // Context-aware empty state message — drives off real airport
    // facilities so a Tower-only field (e.g. LSZG) gets the right hint.
    bool has_twr = ctx.airport_freqs.has(FT::TOWER);
    bool has_gnd = ctx.airport_freqs.has(FT::GROUND);
    bool uncontrolled = !ctx.is_towered() && !has_twr;

    if (atc_state == atc_state_machine::ATCState::EN_ROUTE) {
      if (has_twr)
        ImGui::TextDisabled("%s", ui_strings::tr("hints.tune_tower_inbound"));
      else if (uncontrolled)
        ImGui::TextDisabled("%s", ui_strings::tr("hints.tune_unicom"));
      else
        ImGui::TextDisabled("%s", ui_strings::tr("hints.tune_tower"));
    } else if (ctx.frequency_type == FT::TOWER &&
               atc_state == atc_state_machine::ATCState::IDLE &&
               ctx.on_ground && has_gnd && !ctx.tower_only) {
      ImGui::TextDisabled("%s", ui_strings::tr("hints.tune_ground"));
    } else if (ctx.frequency_type == FT::ATIS ||
               ctx.frequency_type == FT::UNKNOWN) {
      if (uncontrolled)
        ImGui::TextDisabled("%s", ui_strings::tr("hints.tune_unicom"));
      else if (has_gnd)
        ImGui::TextDisabled("%s", ui_strings::tr("hints.tune_ground_or_tower"));
      else
        ImGui::TextDisabled("%s", ui_strings::tr("hints.tune_tower_simple"));
    }
  }
}

// ── En-Route tab state (notification hint) ──────────────────────

static size_t enroute_last_seen_count_ = 0;
static bool enroute_has_update_ = false;

// ── Airport tab content ─────────────────────────────────────────

static void draw_airport_tab(const xplane_context::XPlaneContext &ctx) {
  // Nearby airports picker
  if (ImGui::CollapsingHeader(ui_strings::tr("airport.nearby_header"),
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    draw_nearby_airports();
  }

  // VRP list (if airport has published visual reporting points)
  if (!ctx.nearest_airport_id.empty()) {
    const auto *vrp_data = airport_vrps::get(ctx.nearest_airport_id);
    if (vrp_data && !vrp_data->vrps.empty()) {
      std::string vrp_line = "VRPs: ";
      for (size_t i = 0; i < vrp_data->vrps.size(); ++i) {
        if (i > 0)
          vrp_line += " | ";
        vrp_line += vrp_data->vrps[i].name;
      }
      // TextWrapped so long VRP lists wrap at the current window width
      // instead of forcing the window wider. No TextColoredWrapped in
      // ImGui — emulate via PushStyleColor.
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 0.7f, 1.0f));
      ImGui::TextWrapped("%s", vrp_line.c_str());
      ImGui::PopStyleColor();
    }
  }

  ImGui::Separator();

  // Frequency list with clickable buttons
  if (!ctx.airport_freqs.all.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                       ui_strings::tr("airport.frequencies_header"));

    bool radio_off = !ctx.com_radio_powered;
    if (radio_off)
      ImGui::BeginDisabled();

    float active_freq =
        (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
    uint32_t active_khz =
        static_cast<uint32_t>(std::round(active_freq * 1000.0f));

    for (size_t i = 0; i < ctx.airport_freqs.all.size(); ++i) {
      const auto &af = ctx.airport_freqs.all[i];
      float freq_mhz = static_cast<float>(af.freq_khz) / 1000.0f;
      uint32_t diff = (active_khz > af.freq_khz) ? active_khz - af.freq_khz
                                                 : af.freq_khz - active_khz;
      bool is_active = (diff <= 1);

      if (is_active)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));

      char btn_label[96];
      const char *apt =
          ctx.nearest_airport_id.empty() ? "" : ctx.nearest_airport_id.c_str();
      std::snprintf(btn_label, sizeof(btn_label), "%s %-6s %.3f##pfreq%zu", apt,
                    freq_type_label(af.type), freq_mhz, i);

      if (ImGui::SmallButton(btn_label)) {
        xplane_context::set_standby_freq(af.freq_khz);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(ui_strings::tr("tooltip.set_standby_format"),
                          ctx.active_com, freq_mhz);
      }

      if (is_active)
        ImGui::PopStyleColor();
    }

    if (radio_off)
      ImGui::EndDisabled();
  }

  ImGui::Separator();

  // Pilot action buttons
  draw_pilot_actions(ctx);

  // Last ATC response — lives at the bottom of the Main tab so it stays
  // visible while the pilot picks actions or scans frequencies. Moved
  // here from the always-visible region below EndTabBar() to avoid
  // doubling the Tower line under the Transcript tab.
  ImGui::Separator();
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("panel.last_atc_header"));
  std::string last_atc = atc_session::last_atc_response();
  if (!last_atc.empty()) {
    ImGui::TextWrapped("%s", last_atc.c_str());
  } else {
    ImGui::TextDisabled("%s", ui_strings::tr("panel.no_atc_response"));
  }
}

// ── Flight-prep tab content ─────────────────────────────────────
//
// Pre-flight intent the first radio call carries: VFR flight type
// (Platzrunde / Ueberlandflug) plus the cross-country destination ICAO.
// The combo + field here are the pre-flight FALLBACK (settings::vfr_*),
// used only until the pilot declares the intent on the radio. A SPOKEN
// "VFR nach Echo Delta Mike Alfa" latches into the per-flight session
// state (atc_state_machine flight intent), which build_vars and the
// preview below then prefer — so a typed field is never required.
static void draw_flightprep_tab(const xplane_context::XPlaneContext &ctx) {
  (void)ctx;
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("flightprep.header"));
  ImGui::Spacing();

  // Pilot callsign (Kennzeichen) — raw registration input + phonetic preview.
  // Per-flight identity, so it lives here with flight type + destination.
  if (ImGui::InputText(ui_strings::tr("settings.callsign_label"),
                       callsign_raw_buf, sizeof(callsign_raw_buf))) {
    settings::set_pilot_callsign_raw(callsign_raw_buf);
  }
  // Persist once when editing finishes (Enter / focus-out). The handler above
  // updates the in-memory value per keystroke so the phonetic preview stays
  // live, but this tab has no "Save Settings" button (unlike the old Settings
  // tab this field was moved from), so without this the callsign is never
  // written to disk and is lost on restart.
  if (ImGui::IsItemDeactivatedAfterEdit())
    settings::save();
  // Keep the buffer in sync with settings when not actively typing, so it is
  // populated on first open without relying on the Settings tab init.
  if (!ImGui::IsItemActive()) {
    const std::string cur = settings::pilot_callsign_raw();
    if (cur != callsign_raw_buf) {
      std::strncpy(callsign_raw_buf, cur.c_str(), sizeof(callsign_raw_buf) - 1);
      callsign_raw_buf[sizeof(callsign_raw_buf) - 1] = '\0';
    }
  }
  {
    std::string phonetic = settings::pilot_callsign();
    if (!phonetic.empty())
      ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "  %s",
                         phonetic.c_str());
    else
      ImGui::TextDisabled("%s", ui_strings::tr("settings.callsign_hint"));
  }

  ImGui::Spacing();
  ImGui::Separator();

  // Combo reflects the pre-flight fallback field (settings::vfr_*). The
  // live, radio-declared intent is shown in the preview below, not here —
  // this control stays the user's manual default for silent flights.
  vfr_flight_type_selection =
      (settings::vfr_flight_type() == "cross_country") ? 1 : 0;
  const char *vfr_flight_type_labels[2] = {
      ui_strings::tr("settings.vfr_type_pattern"),
      ui_strings::tr("settings.vfr_type_cross_country"),
  };
  if (ImGui::Combo(ui_strings::tr("settings.vfr_type_label"),
                   &vfr_flight_type_selection, vfr_flight_type_labels, 2)) {
    settings::set_vfr_flight_type(
        vfr_flight_type_names[vfr_flight_type_selection]);
    settings::save();
  }

  if (vfr_flight_type_selection == 1) {
    if (ImGui::InputText(ui_strings::tr("settings.vfr_destination_label"),
                         vfr_destination_buf, sizeof(vfr_destination_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_CharsUppercase)) {
      settings::set_vfr_destination(vfr_destination_buf);
      settings::save();
    }
    // Refresh the field from the persisted settings fallback when the user
    // is not actively typing (keeps it populated on first open).
    if (!ImGui::IsItemActive()) {
      const std::string cur = settings::vfr_destination();
      if (cur != vfr_destination_buf) {
        std::strncpy(vfr_destination_buf, cur.c_str(),
                     sizeof(vfr_destination_buf) - 1);
        vfr_destination_buf[sizeof(vfr_destination_buf) - 1] = '\0';
      }
    }
    ImGui::TextDisabled("%s", ui_strings::tr("flightprep.dest_hint"));
  }

  ImGui::Spacing();
  ImGui::Separator();

  // Readonly preview of the intention element the first radio call will carry,
  // rendered exactly as the pilot should speak it (ICAO expanded phonetically).
  // Mirrors build_vars' precedence: a flight intent already declared on the
  // radio this flight (latched in the state machine) wins over the pre-flight
  // field above, so the preview matches what the tower will actually use.
  bool preview_xc;
  std::string preview_dest;
  if (atc_state_machine::flight_intent_declared()) {
    preview_xc = atc_state_machine::flight_intent_cross_country();
    preview_dest = atc_state_machine::flight_intent_destination();
  } else {
    preview_xc = (vfr_flight_type_selection == 1);
    preview_dest = settings::vfr_destination();
  }
  std::string preview;
  if (preview_xc) {
    preview = preview_dest.empty()
                  ? "VFR Ueberlandflug"
                  : "VFR nach " +
                        (settings::atc_profile() == "EN"
                             ? en_phraseology::expand_callsign_phonetic(
                                   preview_dest)
                             : de_phraseology::expand_callsign_phonetic(
                                   preview_dest));
  } else {
    preview = "VFR Platzrunde";
  }
  ImGui::TextDisabled("%s", ui_strings::tr("flightprep.preview_label"));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.0f, 0.5f, 1.0f));
  ImGui::Text("  %s", preview.c_str());
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
}

// ── En-Route tab content ────────────────────────────────────────

static void draw_enroute_tab(const xplane_context::XPlaneContext &ctx) {
  // Airspace Controllers section
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                     ui_strings::tr("enroute.controllers_header"));

  if (!airspace_db::ready()) {
    ImGui::TextDisabled("%s", ui_strings::tr("enroute.loading"));
  } else if (!airspace_db::enabled()) {
    ImGui::TextDisabled("%s", ui_strings::tr("enroute.data_missing"));
  } else if (ctx.enclosing_airspaces.empty()) {
    ImGui::TextDisabled("%s", ui_strings::tr("enroute.outside_airspace"));
  } else {
    float active_freq =
        (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
    uint32_t active_khz =
        static_cast<uint32_t>(std::round(active_freq * 1000.0f));

    bool radio_off = !ctx.com_radio_powered;
    if (radio_off)
      ImGui::BeginDisabled();

    for (size_t ci = 0; ci < ctx.enclosing_airspaces.size(); ++ci) {
      const auto *c = ctx.enclosing_airspaces[ci];

      // Controller header line
      const char *role_label = airspace_db::role_name(c->role);
      ImGui::Text(ui_strings::tr("enroute.controller_format"), role_label,
                  c->name.c_str(), c->facility_id.c_str(), c->floor_ft,
                  c->ceiling_ft);

      // Clickable frequency buttons
      const char *type_short =
          (c->role == airspace_db::ControllerRole::TRACON) ? "APP" : "INFO";
      for (size_t fi = 0; fi < c->freqs_khz.size() && fi < 4; ++fi) {
        uint32_t freq = c->freqs_khz[fi];
        float freq_mhz = static_cast<float>(freq) / 1000.0f;
        uint32_t diff =
            (active_khz > freq) ? active_khz - freq : freq - active_khz;
        bool is_active = (diff <= 1);

        if (is_active)
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));

        char btn_label[96];
        std::snprintf(btn_label, sizeof(btn_label),
                      "%s %-4s %.3f##afreq%zu_%zu", c->facility_id.c_str(),
                      type_short, freq_mhz, ci, fi);

        if (ImGui::SmallButton(btn_label)) {
          xplane_context::set_standby_freq(freq);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(ui_strings::tr("tooltip.set_standby_format"),
                            ctx.active_com, freq_mhz);
        }

        if (is_active)
          ImGui::PopStyleColor();
      }
      if (c->freqs_khz.size() > 4)
        ImGui::TextDisabled(ui_strings::tr("enroute.more_freqs_format"),
                            c->freqs_khz.size() - 4);

      ImGui::Spacing();
    }

    if (radio_off)
      ImGui::EndDisabled();
  }

  ImGui::Separator();

  // Pilot action buttons (with force_towered when on approach freq)
  draw_pilot_actions(ctx);
}

// ── Traffic tab content (debug, gated on settings::debug_traffic) ───

static const char *traffic_phase_label(traffic_context::TrafficPhase p) {
  using P = traffic_context::TrafficPhase;
  switch (p) {
  case P::OnGround:
    return ui_strings::tr("trafficphase.ground");
  case P::Taxi:
    return ui_strings::tr("trafficphase.taxi");
  case P::Takeoff:
    return ui_strings::tr("trafficphase.takeoff");
  case P::Climb:
    return ui_strings::tr("trafficphase.climb");
  case P::Cruise:
    return ui_strings::tr("trafficphase.cruise");
  case P::Descend:
    return ui_strings::tr("trafficphase.descend");
  case P::Final:
    return ui_strings::tr("trafficphase.final");
  case P::Pattern:
    return ui_strings::tr("trafficphase.pattern");
  case P::Landed:
    return ui_strings::tr("trafficphase.landed");
  case P::Unknown:
  default:
    return ui_strings::tr("trafficphase.unknown");
  }
}

static void draw_traffic_tab() {
  const auto &snap = traffic_context::current();

  ImGui::Text(ui_strings::tr("traffic.targets_format"), snap.targets.size());
  if (snap.last_update_secs > 0.0) {
    ImGui::SameLine();
    ImGui::TextDisabled(ui_strings::tr("traffic.update_age_format"),
                        snap.last_update_secs);
  }

  if (snap.targets.empty()) {
    ImGui::TextDisabled("%s", ui_strings::tr("traffic.empty"));
    return;
  }

  ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_SizingFixedFit;
  if (!ImGui::BeginTable("traffic_table", 7, flags))
    return;

  ImGui::TableSetupColumn(ui_strings::tr("traffic.col_callsign"));
  ImGui::TableSetupColumn(ui_strings::tr("traffic.col_bearing"));
  ImGui::TableSetupColumn(ui_strings::tr("traffic.col_clock"));
  ImGui::TableSetupColumn(ui_strings::tr("traffic.col_dist"));
  ImGui::TableSetupColumn(ui_strings::tr("traffic.col_alt_diff"));
  ImGui::TableSetupColumn(ui_strings::tr("traffic.col_gs"));
  ImGui::TableSetupColumn(ui_strings::tr("traffic.col_phase"));
  ImGui::TableHeadersRow();

  const std::size_t row_count = std::min<std::size_t>(snap.targets.size(), 10);
  for (std::size_t i = 0; i < row_count; ++i) {
    const auto &t = snap.targets[i];
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(t.callsign.empty() ? ui_strings::tr("traffic.no_id")
                                              : t.callsign.c_str());
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%03.0f", t.bearing_from_user_deg);
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%2.0f", t.clock_position);
    ImGui::TableSetColumnIndex(3);
    ImGui::Text("%5.1f", t.distance_to_user_nm);
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("%+5.0f", t.altitude_diff_ft);
    ImGui::TableSetColumnIndex(5);
    ImGui::Text("%3.0f", t.groundspeed_kts);
    ImGui::TableSetColumnIndex(6);
    ImGui::TextUnformatted(traffic_phase_label(t.phase));
  }

  ImGui::EndTable();
}

// ── ATC Commands Panel (tabbed) ─────────────────────────────────

static void draw_atc_panel() {
  if (!atc_panel_visible_)
    return;

  ImGui::SetNextWindowSize(ImVec2(420, 620), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(280, 300), ImVec2(1200, 1000));

  bool open = atc_panel_visible_;
  if (ImGui::Begin(ui_strings::tr("window.atc_panel"), &open,
                   ImGuiWindowFlags_NoCollapse)) {
    const auto &ctx = xplane_context::get();

    // Airport header (always visible above tabs)
    {
      std::string apt_label =
          ctx.nearest_airport_id.empty()
              ? "---"
              : ctx.nearest_airport_id + (ctx.nearest_airport_name.empty()
                                              ? ""
                                              : " " + ctx.nearest_airport_name);
      const char *type_label = facility_status_label(ctx);
      bool tuned_elsewhere = !ctx.geometric_nearest_id.empty() &&
                             ctx.geometric_nearest_id != ctx.nearest_airport_id;
      if (tuned_elsewhere) {
        ImGui::Text("%s %s", apt_label.c_str(), type_label);
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                           ui_strings::tr("panel.tuned_format"),
                           ctx.geometric_nearest_id.c_str());
      } else {
        ImGui::Text("%s %s", apt_label.c_str(), type_label);
      }
    }

    // Pilot callsign / registration — prominent so the pilot does not
    // have to swap to Settings mid-call to remember it. Show both the
    // raw registration (e.g. HBAKA) and the spoken phonetic form
    // (Hotel Bravo Alpha Kilo Alpha) on a second line. Yellow highlight
    // matches the "Last ATC" / "Phraseology Hints" accent.
    {
      std::string raw_cs = settings::pilot_callsign_raw();
      std::string phonetic_cs = settings::pilot_callsign();
      if (raw_cs.empty() && phonetic_cs.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                           ui_strings::tr("panel.no_registration"));
      } else {
        std::string display = raw_cs.empty() ? phonetic_cs : raw_cs;
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                           ui_strings::tr("panel.registration_format"),
                           display.c_str());
        if (!raw_cs.empty() && !phonetic_cs.empty() && phonetic_cs != raw_cs) {
          ImGui::TextDisabled("  %s", phonetic_cs.c_str());
        }
      }
    }

    // Cross-country destination — shown like REG so the pilot sees the
    // declared target at a glance: raw ICAO plus its spoken phonetic form.
    if (settings::vfr_flight_type() == "cross_country") {
      const std::string dest = settings::vfr_destination();
      if (!dest.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                           ui_strings::tr("panel.destination_format"),
                           dest.c_str());
        ImGui::TextDisabled(
            "  %s",
            (settings::atc_profile() == "EN"
                 ? en_phraseology::expand_callsign_phonetic(dest)
                 : de_phraseology::expand_callsign_phonetic(dest))
                .c_str());
      }
    }

    // Radio power warning (only when unpowered)
    if (!ctx.com_radio_powered) {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
      ImGui::TextWrapped(ui_strings::tr("panel.radio_off_warning_format"),
                         ctx.active_com);
      ImGui::PopStyleColor();
      ImGui::Spacing();
    }

    // ATIS summary (always visible above tabs)
    {
      static const char *letter_names[] = {
          "Alpha",  "Bravo",    "Charlie", "Delta",  "Echo",    "Foxtrot",
          "Golf",   "Hotel",    "India",   "Juliet", "Kilo",    "Lima",
          "Mike",   "November", "Oscar",   "Papa",   "Quebec",  "Romeo",
          "Sierra", "Tango",    "Uniform", "Victor", "Whiskey", "X-ray",
          "Yankee", "Zulu"};
      char letter = atis_generator::current_letter();
      int qnh_hpa = static_cast<int>(std::round(ctx.qnh_inhg * 33.8639f));
      ImGui::Text(ui_strings::tr("panel.atis_format"),
                  letter_names[letter - 'A'],
                  ctx.active_runway.empty() ? "---" : ctx.active_runway.c_str(),
                  qnh_hpa);
      if (ctx.wind_speed_kt < 3.0f) {
        ImGui::Text("%s", ui_strings::tr("panel.wind_calm"));
      } else {
        ImGui::Text(ui_strings::tr("panel.wind_format"), ctx.wind_direction_deg,
                    ctx.wind_speed_kt);
      }
    }

    ImGui::Separator();

    // Track airspace changes for En-Route tab notification
    size_t current_airspace_count = ctx.enclosing_airspaces.size();
    if (current_airspace_count != enroute_last_seen_count_) {
      enroute_has_update_ = true;
    }

    // Tab bar
    if (ImGui::BeginTabBar("ATC_Tabs")) {
      // Main tab — airport overview, frequencies, pilot actions,
      // last ATC response. Renamed from "Airport" because the content
      // is broader than just the airport (see plan).
      if (ImGui::BeginTabItem(ui_strings::tr("tab.main"))) {
        draw_airport_tab(ctx);
        ImGui::EndTabItem();
      }

      // Flugvorbereitung tab — VFR flight type + cross-country destination.
      if (ImGui::BeginTabItem(ui_strings::tr("tab.flightprep"))) {
        draw_flightprep_tab(ctx);
        ImGui::EndTabItem();
      }

      // En-Route tab (with notification hint)
      bool hint_colored = false;
      if (enroute_has_update_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 1.0f, 1.0f));
        hint_colored = true;
      }
      if (ImGui::BeginTabItem(ui_strings::tr("tab.enroute"))) {
        // Mark notification as seen
        enroute_has_update_ = false;
        enroute_last_seen_count_ = current_airspace_count;
        if (hint_colored) {
          ImGui::PopStyleColor();
          hint_colored = false;
        }
        draw_enroute_tab(ctx);
        ImGui::EndTabItem();
      }
      if (hint_colored)
        ImGui::PopStyleColor();

      // Traffic tab — debug-only. Hidden unless `debug_traffic` is set.
      if (settings::debug_traffic() &&
          ImGui::BeginTabItem(ui_strings::tr("tab.traffic"))) {
        draw_traffic_tab();
        ImGui::EndTabItem();
      }

      // Transcript now lives in its own floating window
      // (draw_transcript_window) so the pilot can keep the radio history +
      // debug text input open alongside the operative panel while reading back
      // clearances.

      ImGui::EndTabBar();
    }
  }
  ImGui::End();

  if (!open)
    atc_panel_visible_ = false;
}

// ── Transcript window (independent floating dialog) ──────────────
//
// Same content as the former Transcript tab (scrollback + optional
// Debug-Texteingabe) but in its own window like the ATC Commands panel,
// so it can stay open next to it for type-and-read testing without voice.
static void draw_transcript_window() {
  if (!transcript_window_visible_)
    return;

  ImGui::SetNextWindowSize(ImVec2(440, 420), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(280, 200), ImVec2(1200, 1000));

  bool open = transcript_window_visible_;
  if (ImGui::Begin(ui_strings::tr("window.transcript"), &open,
                   ImGuiWindowFlags_NoCollapse)) {
    draw_transcript_tab();
  }
  ImGui::End();

  if (!open)
    transcript_window_visible_ = false;
}

// ── XPLM window callbacks (input capture only) ──────────────────

static void wnd_draw_cb(XPLMWindowID, void *) {
  // Nothing — rendering happens in the draw phase callback
}

// Pass-through helper: return 1 only when ImGui actually wants the mouse
// (cursor over an ImGui window). Otherwise return 0 so X-Plane processes the
// click for cockpit manipulators. WantCaptureMouse reflects the previous
// frame's hit-test, which is sufficient because draw_phase_cb feeds the mouse
// position every frame.
static bool imgui_wants_mouse_at(XPLMWindowID wnd, int x, int y) {
  int left, top, right, bottom;
  XPLMGetWindowGeometry(wnd, &left, &top, &right, &bottom);
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(static_cast<float>(x - left),
                      static_cast<float>(top - y));
  return io.WantCaptureMouse;
}

static int wnd_mouse_cb(XPLMWindowID wnd, int x, int y, XPLMMouseStatus status,
                        void *) {
  if (!imgui_wants_mouse_at(wnd, x, y))
    return 0; // pass through to X-Plane (cockpit manipulation)
  ImGuiIO &io = ImGui::GetIO();
  if (status == xplm_MouseDown)
    io.AddMouseButtonEvent(0, true);
  if (status == xplm_MouseUp)
    io.AddMouseButtonEvent(0, false);
  return 1;
}

static int wnd_rclick_cb(XPLMWindowID wnd, int x, int y, XPLMMouseStatus status,
                         void *) {
  if (!imgui_wants_mouse_at(wnd, x, y))
    return 0;
  ImGuiIO &io = ImGui::GetIO();
  if (status == xplm_MouseDown)
    io.AddMouseButtonEvent(1, true);
  if (status == xplm_MouseUp)
    io.AddMouseButtonEvent(1, false);
  return 1;
}

static int wnd_wheel_cb(XPLMWindowID wnd, int x, int y, int, int clicks,
                        void *) {
  if (!imgui_wants_mouse_at(wnd, x, y))
    return 0;
  ImGui::GetIO().AddMouseWheelEvent(0.0f, static_cast<float>(clicks));
  return 1;
}

static XPLMCursorStatus wnd_cursor_cb(XPLMWindowID, int, int, void *) {
  return xplm_CursorDefault;
}

static void wnd_key_cb(XPLMWindowID, char key, XPLMKeyFlags flags, char vkey,
                       void *, int losing_focus) {
  if (losing_focus) {
    ImGui::GetIO().AddFocusEvent(false);
    return;
  }
  ImGuiIO &io = ImGui::GetIO();
  // Only consume keys when ImGui has an active text input.
  // Otherwise let X-Plane handle them (command key bindings, etc.)
  if (!io.WantTextInput)
    return;
  bool is_down = (flags & xplm_DownFlag) != 0;
  bool is_up = (flags & xplm_UpFlag) != 0;
  // Map special keys for both press and release so ImGui doesn't get stuck
  // with a "held" key (which would cause e.g. Backspace to keep deleting).
  ImGuiKey ikey = ImGuiKey_None;
  if (vkey == XPLM_VK_BACK)
    ikey = ImGuiKey_Backspace;
  else if (vkey == XPLM_VK_DELETE)
    ikey = ImGuiKey_Delete;
  else if (vkey == XPLM_VK_RETURN)
    ikey = ImGuiKey_Enter;
  else if (vkey == XPLM_VK_LEFT)
    ikey = ImGuiKey_LeftArrow;
  else if (vkey == XPLM_VK_RIGHT)
    ikey = ImGuiKey_RightArrow;
  else if (vkey == XPLM_VK_HOME)
    ikey = ImGuiKey_Home;
  else if (vkey == XPLM_VK_END)
    ikey = ImGuiKey_End;
  else if (vkey == XPLM_VK_TAB)
    ikey = ImGuiKey_Tab;
  if (ikey != ImGuiKey_None) {
    if (is_down)
      io.AddKeyEvent(ikey, true);
    if (is_up)
      io.AddKeyEvent(ikey, false);
  }
  if (is_down && key >= 32 && key < 127)
    io.AddInputCharacter(static_cast<unsigned>(key));
  if (is_down && vkey == XPLM_VK_ESCAPE) {
    visible = false;
    if (window_id) {
      XPLMSetWindowIsVisible(window_id, 0);
      XPLMTakeKeyboardFocus(nullptr);
    }
  }
}

// ── Draw phase callback (ImGui rendering) ────────────────────────

static int draw_phase_cb(XPLMDrawingPhase, int, void *) {
  if (!visible && !atc_panel_visible_ && !transcript_window_visible_)
    return 1;

  int gl, gt, gr, gb;
  XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);
  int sw = gr - gl;
  int sh = gt - gb;
  if (sw <= 0 || sh <= 0)
    return 1;

  // Keep capture window sized to full screen
  if (window_id) {
    int wl, wt, wr, wb;
    XPLMGetWindowGeometry(window_id, &wl, &wt, &wr, &wb);
    if (wl != gl || wb != gb || wr != gr || wt != gt)
      XPLMSetWindowGeometry(window_id, gl, gt, gr, gb);
  }

  // Save GL state
  GLint prev_viewport[4];
  glGetIntegerv(GL_VIEWPORT, prev_viewport);
  glPushAttrib(GL_TRANSFORM_BIT | GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT |
               GL_DEPTH_BUFFER_BIT | GL_SCISSOR_BIT | GL_TEXTURE_BIT);
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_LIGHTING);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // Use X-Plane's ACTUAL framebuffer viewport (in pixels), not the logical
  // boxel screen size. On Windows (X-Plane 12 renders via Vulkan, OpenGL is
  // a compat layer) and on Retina/HiDPI the framebuffer differs from the
  // logical size; drawing into (0,0,sw,sh) then paints ImGui into the wrong
  // sub-rectangle, so the visible window and the (logical) mouse hit-test
  // no longer overlap — dragging/closing/buttons all miss. The ortho stays
  // in logical units so the mouse feed (also logical) matches ImGui geometry.
  const int fb_w = prev_viewport[2];
  const int fb_h = prev_viewport[3];
  glViewport(0, 0, fb_w, fb_h);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, sw, sh, 0, -1, 1); // top-left origin for ImGui (logical units)
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  // ImGui frame setup
  ImGuiIO &io = ImGui::GetIO();
  double now = get_xp_time();
  io.DeltaTime = static_cast<float>(std::max(now - last_frame_time_, 0.001));
  last_frame_time_ = now;
  io.DisplaySize = ImVec2(static_cast<float>(sw), static_cast<float>(sh));
  io.DisplayFramebufferScale =
      ImVec2(sw > 0 ? static_cast<float>(fb_w) / static_cast<float>(sw) : 1.0f,
             sh > 0 ? static_cast<float>(fb_h) / static_cast<float>(sh) : 1.0f);

  // Track mouse position every frame (hover support)
  int gmx, gmy;
  XPLMGetMouseLocationGlobal(&gmx, &gmy);
  io.AddMousePosEvent(static_cast<float>(gmx - gl),
                      static_cast<float>(gt - gmy));

  ImGui_ImplOpenGL2_NewFrame();
  ImGui::NewFrame();

  // Re-claim keyboard focus whenever ImGui wants text input (prevents X-Plane
  // from stealing focus while the user is typing in an input field)
  if (ImGui::GetIO().WantTextInput && window_id) {
    XPLMTakeKeyboardFocus(window_id);
  }

  // Window position/size — load from settings or center
  if (window_pos_reset_pending_) {
    // Force re-center on next frame
    float def_w = 500.0f, def_h = 450.0f;
    ImGui::SetNextWindowPos(ImVec2((static_cast<float>(sw) - def_w) * 0.5f,
                                   (static_cast<float>(sh) - def_h) * 0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(def_w, def_h), ImGuiCond_Always);
    window_pos_reset_pending_ = false;
  } else {
    float sx = settings::window_x();
    float sy = settings::window_y();
    float sw_s = settings::window_w();
    float sh_s = settings::window_h();
    if (sx >= 0.0f && sy >= 0.0f) {
      ImGui::SetNextWindowPos(ImVec2(sx, sy), ImGuiCond_FirstUseEver);
    } else {
      float def_w = 500.0f, def_h = 450.0f;
      ImGui::SetNextWindowPos(ImVec2((static_cast<float>(sw) - def_w) * 0.5f,
                                     (static_cast<float>(sh) - def_h) * 0.5f),
                              ImGuiCond_FirstUseEver);
    }
    if (sw_s > 0.0f && sh_s > 0.0f) {
      ImGui::SetNextWindowSize(ImVec2(sw_s, sh_s), ImGuiCond_FirstUseEver);
    } else {
      ImGui::SetNextWindowSize(ImVec2(500.0f, 450.0f), ImGuiCond_FirstUseEver);
    }
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(400, 300), ImVec2(1920, 1080));

  // Main window (only when visible)
  bool open = visible;
  if (visible) {
    // Build window title each frame so a runtime region switch picks up
    // the localized "##main" id. Using a small thread-local buffer keeps
    // the string memory valid across the ImGui::Begin call.
    char window_title[128];
#ifdef XP_WELLYS_ATC_VERSION
    std::snprintf(window_title, sizeof(window_title),
                  ui_strings::tr("window.title_format"), XP_WELLYS_ATC_VERSION);
#else
    std::snprintf(window_title, sizeof(window_title), "%s",
                  ui_strings::tr("window.title_dev"));
#endif
    if (ImGui::Begin(window_title, &open, ImGuiWindowFlags_NoCollapse)) {
      if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem(ui_strings::tr("tab.status"))) {
          draw_status_tab();
          ImGui::EndTabItem();
        }
        // "Models" sits second so first-launch users see it
        // immediately after Status — they cannot use the plugin
        // until they download here.
        // Compiled in on any slice with a local backend: full Local (Metal
        // STT/LM, Apple Silicon) OR just the CPU Piper voice for hybrid TTS
        // (with #69 also x86_64-macOS / Windows). The pure cloud-only slices
        // never download models, so the whole tab is hidden there — nothing
        // to manage.
#ifdef XPWELLYS_USE_LOCAL_TTS
        // Highlight Models tab only in Local mode — in cloud modes
        // the user goes to Settings, not Models, when something is
        // missing (see the Status-tab banner above for the cloud
        // path).
        bool models_attention = !backends::loader::snapshot().all_ready() &&
                                settings::backend_mode() == "local";
        if (models_attention) {
          // Highlight the tab label so it's obvious where to go on
          // a fresh install.
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
        }
        // Stable "###" ID suffix: the visible label flips between
        // "Modelle" and "Modelle (!)" with readiness, but ImGui derives a
        // tab's identity from its label — without a fixed ID the selection
        // would be lost the moment attention toggles (e.g. a Force
        // re-download), bouncing the user back to the Status tab.
        std::string models_label =
            (models_attention ? ui_strings::tr("tab.models_attention")
                              : ui_strings::tr("tab.models"));
        models_label += "###models_tab";
        bool models_open = ImGui::BeginTabItem(models_label.c_str());
        if (models_attention) {
          ImGui::PopStyleColor();
        }
        if (models_open) {
          draw_models_tab();
          ImGui::EndTabItem();
        }
#endif // XPWELLYS_USE_LOCAL_TTS
        // Transcript-Tab lebt jetzt am ATC-Panel (siehe draw_atc_panel)
        // — Pilot soll Funkverlauf neben dem Status sehen, nicht im
        // Config-Fenster.
        if (ImGui::BeginTabItem(ui_strings::tr("tab.settings"))) {
          draw_settings_tab();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ui_strings::tr("tab.audio"))) {
          draw_audio_tab();
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }

      // Save window geometry when moved/resized (debounced)
      ImVec2 pos = ImGui::GetWindowPos();
      ImVec2 size = ImGui::GetWindowSize();
      float prev_x = settings::window_x();
      float prev_y = settings::window_y();
      float prev_w = settings::window_w();
      float prev_h = settings::window_h();
      if (pos.x != prev_x || pos.y != prev_y || size.x != prev_w ||
          size.y != prev_h) {
        settings::set_window_geometry(pos.x, pos.y, size.x, size.y);
        geometry_save_timer_ = kGeometrySaveDelay;
      }
      if (geometry_save_timer_ > 0.0f) {
        geometry_save_timer_ -= ImGui::GetIO().DeltaTime;
        if (geometry_save_timer_ <= 0.0f) {
          settings::save();
          geometry_save_timer_ = 0.0f;
        }
      }
    }
    ImGui::End();

    if (!open) {
      visible = false;
      if (window_id) {
        XPLMSetWindowIsVisible(window_id, 0);
        XPLMTakeKeyboardFocus(nullptr);
      }
    }
  } // end if (visible)

  // ATC Commands panel + Transcript window (both independent of main window)
  draw_atc_panel();
  draw_transcript_window();

  // If all windows are now closed, release the capture window so X-Plane
  // gets input back. Without this, the invisible full-screen capture window
  // swallows all mouse/keyboard events.
  if (!visible && !atc_panel_visible_ && !transcript_window_visible_ &&
      window_id) {
    XPLMSetWindowIsVisible(window_id, 0);
    XPLMTakeKeyboardFocus(nullptr);
  }

  // Dynamic XPLM keyboard focus management. The static
  // XPLMTakeKeyboardFocus calls in toggle() / toggle_atc_panel() only
  // run on window open/close — once a text-input widget appears later
  // (e.g. clicking into Debug-Texteingabe in the ATC panel after the
  // main window was closed), the capture window has no focus and the
  // keystrokes never reach wnd_key_cb. Mirror ImGui's per-frame
  // `WantTextInput` onto the XPLM focus state so the input field works
  // independently of which window is open, AND so X-Plane command
  // bindings (PTT, autopilot) keep firing when no text widget is
  // active.
  if (window_id &&
      (visible || atc_panel_visible_ || transcript_window_visible_)) {
    bool want_text = ImGui::GetIO().WantTextInput;
    bool have_focus = XPLMHasKeyboardFocus(window_id) != 0;
    if (want_text && !have_focus)
      XPLMTakeKeyboardFocus(window_id);
    else if (!want_text && have_focus)
      XPLMTakeKeyboardFocus(nullptr);
  }

  ImGui::Render();
  ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

  // Restore GL state
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glPopAttrib();
  glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2],
             prev_viewport[3]);

  return 1;
}

// ── Public lifecycle ─────────────────────────────────────────────

void init() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO &io = ImGui::GetIO();
  static std::string ini_path = settings::get_data_dir() + "/imgui.ini";
  io.IniFilename = ini_path.c_str();
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

  ImGui::StyleColorsDark();
  auto &style = ImGui::GetStyle();
  style.WindowRounding = 6.0f;
  style.FrameRounding = 3.0f;
  style.WindowPadding = ImVec2(8, 6);

  ImGui_ImplOpenGL2_Init();
  last_frame_time_ = get_xp_time();

  XPLMRegisterDrawCallback(draw_phase_cb, xplm_Phase_Window, 1, nullptr);
}

void stop() {
  XPLMUnregisterDrawCallback(draw_phase_cb, xplm_Phase_Window, 1, nullptr);

  if (window_id) {
    XPLMDestroyWindow(window_id);
    window_id = nullptr;
  }
  ImGui_ImplOpenGL2_Shutdown();
  ImGui::DestroyContext();

  buffers_initialized = false;
}

// Create the full-screen invisible input-capture window if it does not yet
// exist. Shared by all three window toggles (main / ATC panel / transcript).
static void ensure_capture_window() {
  if (window_id)
    return;
  int gl, gt, gr, gb;
  XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);

  XPLMCreateWindow_t p{};
  p.structSize = sizeof(p);
  p.left = gl;
  p.bottom = gb;
  p.right = gr;
  p.top = gt;
  p.visible = 1;
  p.drawWindowFunc = wnd_draw_cb;
  p.handleMouseClickFunc = wnd_mouse_cb;
  p.handleKeyFunc = wnd_key_cb;
  p.handleCursorFunc = wnd_cursor_cb;
  p.handleMouseWheelFunc = wnd_wheel_cb;
  p.handleRightClickFunc = wnd_rclick_cb;
  p.refcon = nullptr;
  p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
  p.layer = xplm_WindowLayerFloatingWindows;
  window_id = XPLMCreateWindowEx(&p);
}

void toggle() {
  visible = !visible;

  if (visible && !window_id) {
    // Create full-screen invisible capture window
    int gl, gt, gr, gb;
    XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);

    XPLMCreateWindow_t p{};
    p.structSize = sizeof(p);
    p.left = gl;
    p.bottom = gb;
    p.right = gr;
    p.top = gt;
    p.visible = 1;
    p.drawWindowFunc = wnd_draw_cb;
    p.handleMouseClickFunc = wnd_mouse_cb;
    p.handleKeyFunc = wnd_key_cb;
    p.handleCursorFunc = wnd_cursor_cb;
    p.handleMouseWheelFunc = wnd_wheel_cb;
    p.handleRightClickFunc = wnd_rclick_cb;
    p.refcon = nullptr;
    p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
    p.layer = xplm_WindowLayerFloatingWindows;
    window_id = XPLMCreateWindowEx(&p);

    if (settings::debug_logging()) {
      char dbg[256];
      std::snprintf(dbg, sizeof(dbg),
                    "[xp_wellys_vfr_atc] Capture window created: "
                    "bounds(%d,%d,%d,%d) wnd=%p\n",
                    gl, gt, gr, gb, static_cast<void *>(window_id));
      XPLMDebugString(dbg);
    }
  }

  if (window_id) {
    XPLMSetWindowIsVisible(window_id, visible ? 1 : 0);
    if (visible) {
      XPLMBringWindowToFront(window_id);
      XPLMTakeKeyboardFocus(window_id);
    } else {
      XPLMTakeKeyboardFocus(nullptr); // release focus
    }
  }
}

void draw() {
  // Rendering now handled by draw_phase_cb
}

void toggle_atc_panel() {
  atc_panel_visible_ = !atc_panel_visible_;

  // Ensure capture window exists for input events
  if (atc_panel_visible_ && !window_id && !visible) {
    // Will be created on next toggle() or when main window opens
    // For now, the panel renders but may not capture input without the
    // capture window. Open the capture window if needed.
    int gl, gt, gr, gb;
    XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);

    XPLMCreateWindow_t p{};
    p.structSize = sizeof(p);
    p.left = gl;
    p.bottom = gb;
    p.right = gr;
    p.top = gt;
    p.visible = 1;
    p.drawWindowFunc = wnd_draw_cb;
    p.handleMouseClickFunc = wnd_mouse_cb;
    p.handleKeyFunc = wnd_key_cb;
    p.handleCursorFunc = wnd_cursor_cb;
    p.handleMouseWheelFunc = wnd_wheel_cb;
    p.handleRightClickFunc = wnd_rclick_cb;
    p.refcon = nullptr;
    p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
    p.layer = xplm_WindowLayerFloatingWindows;
    window_id = XPLMCreateWindowEx(&p);
  }

  if (window_id) {
    if (atc_panel_visible_ || visible) {
      XPLMSetWindowIsVisible(window_id, 1);
      XPLMBringWindowToFront(window_id);
      // Only take keyboard focus if main window needs text input.
      // ATC panel alone has no text fields — taking focus would block
      // X-Plane command key bindings (including the toggle key itself).
      if (visible)
        XPLMTakeKeyboardFocus(window_id);
    } else {
      XPLMSetWindowIsVisible(window_id, 0);
      XPLMTakeKeyboardFocus(nullptr);
    }
  }
}

void toggle_transcript_window() {
  transcript_window_visible_ = !transcript_window_visible_;

  if (transcript_window_visible_)
    ensure_capture_window();

  if (window_id) {
    if (visible || atc_panel_visible_ || transcript_window_visible_) {
      XPLMSetWindowIsVisible(window_id, 1);
      XPLMBringWindowToFront(window_id);
      // Keyboard focus is granted per-frame by the WantTextInput logic in
      // draw_phase_cb when the Debug-Texteingabe field is clicked, so we do
      // not steal focus here (which would block X-Plane command bindings).
    } else {
      XPLMSetWindowIsVisible(window_id, 0);
      XPLMTakeKeyboardFocus(nullptr);
    }
  }
}

void reset_window_position() {
  settings::reset_window_geometry();
  window_pos_reset_pending_ = true;
  // Open the window so the user can see it
  if (!visible)
    toggle();
}

} // namespace atc_ui
