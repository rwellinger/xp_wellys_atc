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

#include <XPLMDataAccess.h>
#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include "atc/atc_session.hpp"
#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/atis_generator.hpp"
#include "atc/flight_phase.hpp"
#include "atc/initial_call_conformance.hpp"
#include "atc/phraseology_hints.hpp"
#include "atc/traffic_dialog.hpp"
#include "audio/audio_player.hpp"
#include "audio/audio_recorder.hpp"
#include "audio/ptt_input.hpp"
#include "backends/downloader.hpp"
#include "backends/loader.hpp"
#include "backends/manager.hpp"
#include "core/cross_country_log.hpp"
#include "core/logging.hpp"
#include "core/xplane_context.hpp"
#include "data/airport_vrps.hpp"
#include "data/airspace_db.hpp"
#include "data/openair_db.hpp"
#include "data/traffic_context.hpp"
#include "persistence/model_paths.hpp"
#include "persistence/settings.hpp"
#include "ui/atc_ui.hpp"
#include "ui/ui_strings.hpp"

static XPLMMenuID menu_id = nullptr;
static int menu_container_idx = -1;
static XPLMCommandRef cmd_atc_panel_ = nullptr;
static XPLMCommandRef cmd_transcript_ = nullptr;

static void menu_handler(void *, void *item_ref) {
  intptr_t idx = reinterpret_cast<intptr_t>(item_ref);
  if (idx == 0)
    atc_ui::toggle();
  else if (idx == 1)
    atc_ui::toggle_atc_panel();
  else if (idx == 2)
    atc_ui::reset_window_position();
  else if (idx == 3)
    atc_ui::toggle_transcript_window();
}

static int atc_panel_cmd_handler(XPLMCommandRef, XPLMCommandPhase phase,
                                 void *) {
  if (phase == xplm_CommandBegin)
    atc_ui::toggle_atc_panel();
  return 0;
}

static int transcript_cmd_handler(XPLMCommandRef, XPLMCommandPhase phase,
                                  void *) {
  if (phase == xplm_CommandBegin)
    atc_ui::toggle_transcript_window();
  return 0;
}

static int atis_check_counter_ = 0;
static int traffic_check_counter_ = 0;
static float last_elapsed_ = 0.0f;

static XPLMDataRef dr_atc_verbose_ = nullptr;
static XPLMDataRef dr_atc_show_hist_ = nullptr;

static float flight_loop_cb(float, float, int, void *) {
  // Catch all exceptions inside the flight loop — X-Plane is C and
  // any std::exception that propagates out is a guaranteed crash.
  // Drain-callback-queue, ImGui draw paths inside atc_session::update
  // and worker callbacks could all throw under unexpected error
  // conditions; we'd rather log + skip a frame than terminate.
  try {
    float now = XPLMGetElapsedTime();
    float dt = (last_elapsed_ > 0.0f) ? (now - last_elapsed_) : (1.0f / 60.0f);
    last_elapsed_ = now;

    xplane_context::update();
    flight_phase::update(xplane_context::get(), dt);
    // Check ATIS for updates ~1/s (every 60 frames)
    if (++atis_check_counter_ % 60 == 0)
      atis_generator::check_for_update(xplane_context::get());
    // Refresh traffic snapshot at ~2 Hz (every 30 frames at 60 FPS).
    // Reading 8 TCAS arrays + computing distance/bearing for up to 63
    // slots is cheap, but doing it per-frame is wasted work — UI is
    // gated behind debug_traffic and the engine doesn't consume the
    // snapshot at frame rate.
    if (++traffic_check_counter_ % 30 == 0)
      traffic_context::update();
    ptt_input::update();
    backends::drain_callback_queue();
    atc_session::update();

    if (settings::disable_default_atc()) {
      if (dr_atc_verbose_ && XPLMGetDatai(dr_atc_verbose_) != 0)
        XPLMSetDatai(dr_atc_verbose_, 0);
      if (dr_atc_show_hist_ && XPLMGetDatai(dr_atc_show_hist_) != 0)
        XPLMSetDatai(dr_atc_show_hist_, 0);
    }
  } catch (const std::exception &e) {
    logging::error("flight_loop_cb threw: %s", e.what());
  } catch (...) {
    logging::error("flight_loop_cb threw an unknown exception");
  }

  return -1.0f; // called every frame
}

PLUGIN_API int XPluginStart(char *name, char *sig, char *desc) {
  // Required for X-Plane installs on external volumes — without it the SDK
  // returns HFS paths that lose the /Volumes/<name>/ mount prefix when
  // naively converted to POSIX, causing all file I/O to fail.
  XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

#ifdef XP_WELLYS_ATC_VERSION
  std::snprintf(name, 256, "Welly's ATC v%s", XP_WELLYS_ATC_VERSION);
#else
  std::snprintf(name, 256, "Welly's ATC");
#endif
  std::snprintf(sig, 256, "ch.thWelly.wellys_devfr_atc");
  std::snprintf(desc, 256, "AI-powered ATC voice communication for VFR");

  logging::set_sink(&XPLMDebugString);
  logging::info("Plugin started");

  settings::init();
  // Per-flight ATC logbook lives under <X-Plane>/Output/xp_wellys_devfr_atc/
  // flightlog so it survives plugin re-installs. Created lazily on first write.
  cross_country_log::set_dir(settings::output_dir() + "/flightlog");
  atc_templates::init();
  initial_call_conformance::init();
  phraseology_hints::init();
  ui_strings::init();
  airport_vrps::init();
  {
    char raw[2048] = {};
    XPLMGetSystemPath(raw);
    std::string sys(raw);
#if defined(__APPLE__)
    if (sys.find(':') != std::string::npos &&
        sys.find('/') == std::string::npos) {
      auto colon = sys.find(':');
      std::string posix = sys.substr(colon + 1);
      for (char &c : posix)
        if (c == ':')
          c = '/';
      sys = "/" + posix;
    }
#endif
    if (!sys.empty() && sys.back() != '/')
      sys += '/';
    airspace_db::init(sys + "Custom Data/1200 atc data/Earth nav data/atc.dat");
  }
  // Real 3-D airspace DB (openair_db): bundled DE starter set + optional
  // user override (a full openaip.net Germany OpenAir export dropped at
  // <prefs>/airspace.txt). Detection only for now — see issue #29.
  openair_db::init(settings::airspaces_data_path(),
                   settings::user_prefs_dir() + "/airspace.txt");
  xplane_context::init();
  traffic_context::init();
  flight_phase::init();
  atis_generator::init();
  audio_recorder::init();
  audio_player::init();
  backends::init();
  // Resolve <plugin>/Resources/{models,espeak-ng-data} paths once.
  // Both the loader (model SHA256 + backend instantiation) and the
  // downloader (P5) read these. Initialised before the loader since
  // the loader walks models_dir() on startup.
  model_paths::init();
  atc_state_machine::init();
  traffic_dialog::init();
  atc_ui::init();

  // DataRefs for silencing X-Plane's default ATC
  dr_atc_verbose_ = XPLMFindDataRef("sim/atc/atc_verbose");
  dr_atc_show_hist_ = XPLMFindDataRef("sim/atc/atc_show_hist");

  // Flight loop
  XPLMCreateFlightLoop_t loop_params{};
  loop_params.structSize = sizeof(loop_params);
  loop_params.phase = xplm_FlightLoop_Phase_AfterFlightModel;
  loop_params.callbackFunc = flight_loop_cb;
  XPLMFlightLoopID loop_id = XPLMCreateFlightLoop(&loop_params);
  XPLMScheduleFlightLoop(loop_id, -1.0f, true);

  // ATC Panel command (bindable via X-Plane keyboard/joystick settings)
  cmd_atc_panel_ = XPLMCreateCommand("xp_wellys_devfr_atc/atc_panel",
                                     "Toggle ATC Commands Panel");
  XPLMRegisterCommandHandler(cmd_atc_panel_, atc_panel_cmd_handler, 1, nullptr);

  // Transcript window command (bindable; toggles the radio-log / debug-text
  // floating window).
  cmd_transcript_ = XPLMCreateCommand("xp_wellys_devfr_atc/transcript",
                                      "Toggle Transcript Window");
  XPLMRegisterCommandHandler(cmd_transcript_, transcript_cmd_handler, 1,
                             nullptr);

  // Menu
  menu_container_idx =
      XPLMAppendMenuItem(XPLMFindPluginsMenu(), "Welly's ATC", nullptr, 0);
  menu_id = XPLMCreateMenu("Welly's ATC", XPLMFindPluginsMenu(),
                           menu_container_idx, menu_handler, nullptr);
  XPLMAppendMenuItem(menu_id, "Open / Close", nullptr, 0);
  // NOLINTBEGIN(performance-no-int-to-ptr)
  XPLMAppendMenuItem(menu_id, "ATC Commands",
                     reinterpret_cast<void *>(uintptr_t{1}), 0);
  XPLMAppendMenuItem(menu_id, "Transcript",
                     reinterpret_cast<void *>(uintptr_t{3}), 0);
  XPLMAppendMenuItem(menu_id, "Reset Window Position",
                     reinterpret_cast<void *>(uintptr_t{2}), 0);
  // NOLINTEND(performance-no-int-to-ptr)

  return 1;
}

PLUGIN_API void XPluginStop() {
  if (menu_id) {
    XPLMDestroyMenu(menu_id);
    menu_id = nullptr;
  }

  atc_ui::stop();
  atc_state_machine::stop();
  traffic_dialog::stop();
  // backends::stop() joins worker threads + drops registered backends.
  // Must run before audio_player::stop() so an in-flight TTS callback
  // does not hand a buffer to a torn-down audio path.
  backends::stop();
  audio_player::stop();
  audio_recorder::stop();
  atis_generator::stop();
  flight_phase::stop();
  traffic_context::stop();
  xplane_context::stop();
  openair_db::stop();
  airspace_db::stop();
  airport_vrps::stop();
  ui_strings::stop();
  phraseology_hints::stop();
  atc_templates::stop();
  settings::stop();

  logging::info("Plugin stopped");
}

PLUGIN_API int XPluginEnable() {
  try {
    ptt_input::init();
    atc_session::init();
    // Kick off the verification + load worker. SHA256 of the 2 GB
    // llama model takes a few seconds on M1; running this in the
    // foreground would freeze the X-Plane main thread, so the loader
    // runs on its own std::thread and writes status into a struct the
    // UI snapshots each frame. PTT is gated by `Status::all_ready()`,
    // so the user sees a "models not loaded" message until the worker
    // succeeds.
    backends::loader::start();
    return 1;
  } catch (const std::exception &e) {
    logging::error("XPluginEnable threw: %s", e.what());
    return 0;
  } catch (...) {
    logging::error("XPluginEnable threw an unknown exception");
    return 0;
  }
}

PLUGIN_API void XPluginDisable() {
  ptt_input::stop();
  atc_session::stop();
  // Stop downloader first — its worker may call loader::start()
  // when a download finishes; doing it the other way round risks
  // re-spawning the loader thread between loader::stop() and the
  // downloader's last enqueue.
  backends::downloader::stop();
  // Joins the verification / load worker before the .xpl unloads —
  // matches the no-threads-survive-XPluginDisable rule.
  backends::loader::stop();
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int inMessage, void *) {
  // A new airport loaded (user repositioned / picked a new start location) or
  // the user aircraft was reloaded means a fresh flight — start a new logbook
  // file. On the very first load no flight is open yet, so this is a no-op.
  if (inMessage == XPLM_MSG_AIRPORT_LOADED ||
      inMessage == XPLM_MSG_PLANE_LOADED) {
    cross_country_log::begin_new_flight();
    // Fresh flight → fresh ATIS session state (clears a stale cooldown and
    // any in-flight TTS-retry counter from the previous flight).
    atc_session::reset_atis_cooldown();
    // Full ATC state-machine reset for the new flight — but only when the
    // guard inside confirms we are safely between flights (on the ground in
    // IDLE/GROUND_CONTACT). A spurious non-reposition message while taxiing
    // or airborne is a strict no-op, so an in-progress flight is never
    // wiped. This subsumes the old set_was_airborne(false) (reset() clears
    // the flag as part of the sweep) and additionally clears the state,
    // history, runway/callsign locks and cached clearance that would
    // otherwise leak from the previous flight. on_ground comes from the
    // latest flight-loop snapshot (may read stale-airborne when
    // repositioning out of flight; the guard treats that as "do not reset").
    atc_state_machine::begin_fresh_flight(xplane_context::get().on_ground);
  }
}
