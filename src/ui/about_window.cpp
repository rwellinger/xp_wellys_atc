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

#include "ui/about_window.hpp"

#include <wellys_about.hpp>

namespace {

#ifdef XP_WELLYS_ATC_VERSION
constexpr const char *kPluginVersion = XP_WELLYS_ATC_VERSION;
#else
constexpr const char *kPluginVersion = "SNAPSHOT";
#endif

// Mirrors the at-a-glance table in THIRD_PARTY.md: what ships with the
// plugin (build- and test-only deps such as Catch2 are left out).
const WellysAbout::PluginInfo &plugin_info() {
  static const WellysAbout::PluginInfo info{
      "xp_wellys_vfr_atc",
      kPluginVersion,
      "Copyright (C) 2026 thWelly",
      "Free software under the GNU General Public License v3 or later, "
      "provided without any warranty.",
      {
          {"whisper.cpp", "MIT"},
          {"llama.cpp / ggml", "MIT"},
          {"Piper (libpiper)", "GPL-3.0"},
          {"espeak-ng", "GPL-3.0-or-later"},
          {"onnxruntime", "MIT"},
          {"Dear ImGui", "MIT"},
          {"nlohmann/json", "MIT"},
          {"miniaudio", "Public Domain / MIT-0"},
          {"libcurl", "curl license"},
          {"zlib", "zlib license"},
          {"X-Plane SDK", "Laminar Research SDK License"},
      },
  };
  return info;
}

} // namespace

namespace about_window {

void draw(bool &open) {
  WellysAbout::draw_window(open, plugin_info(), WellysAbout::photo_texture());
}

void release() { WellysAbout::release_photo(); }

} // namespace about_window
