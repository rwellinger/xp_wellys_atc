/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Windows build gate (issue #18). The macOS AVFoundation permission
 * flow lives in mic_permission.mm, which the Windows build excludes.
 * Windows has no per-application microphone authorization dialog
 * comparable to macOS TCC, so access is reported as granted here. If
 * the system-wide "Microphone access" privacy toggle is off, capture
 * simply yields silence — surfacing that to the user is part of the
 * mic-capture port (issue #21) / UI helpers (issue #22).
 */

#include "audio/mic_permission.hpp"

#include <XPLMUtilities.h>

namespace mic_permission {

bool check_and_request() {
  XPLMDebugString(
      "[xp_wellys_vfr_atc] Microphone permission: not gated on Windows\n");
  return true;
}

} // namespace mic_permission
