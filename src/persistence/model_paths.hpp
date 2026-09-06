/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef PERSISTENCE_MODEL_PATHS_HPP
#define PERSISTENCE_MODEL_PATHS_HPP

#include <string>

namespace model_paths {

// Resolve the plugin's bundle root by walking up from the .xpl
// returned by XPLMGetPluginInfo. Returns an absolute POSIX path with
// no trailing slash. Must be called only after the plugin has fully
// loaded (i.e. inside or after XPluginStart). The path resolution is
// done once and cached — it is safe to call from any thread once
// init() has run.
//
// We resolve relative to the plugin (not $HOME, not CWD) so the .xpl
// works on internal SSDs, external USB-SSDs / Thunderbolt drives, and
// X-Plane installs whose path contains spaces or non-ASCII characters.
void init();

// `<plugin>/` (no trailing slash). Empty string before init().
const std::string &plugin_root();

// `<plugin>/Resources/models/` — where downloadable model files live.
// Created on first access if it does not yet exist.
const std::string &models_dir();

// `<plugin>/Resources/espeak-ng-data/` — Piper's espeak-ng dictionary
// directory, shipped with the plugin (no SHA256, no download).
const std::string &espeakng_data_dir();

} // namespace model_paths

#endif // PERSISTENCE_MODEL_PATHS_HPP
