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

#include "audio/ptt_input.hpp"
#include "atc/atc_session.hpp"

#include <XPLMUtilities.h>

namespace ptt_input {

// ── X-Plane Command ─────────────────────────────────────────────

static XPLMCommandRef ptt_cmd_ = nullptr;

static int ptt_command_handler(XPLMCommandRef, XPLMCommandPhase phase, void *) {
  if (phase == xplm_CommandBegin) {
    atc_session::on_ptt_pressed();
  } else if (phase == xplm_CommandEnd) {
    atc_session::on_ptt_released();
  }
  return 0;
}

// ── Lifecycle ───────────────────────────────────────────────────

void init() {
  ptt_cmd_ =
      XPLMCreateCommand("xp_wellys_devfr_atc/ptt", "Welly's ATC: Push-to-Talk");
  XPLMRegisterCommandHandler(ptt_cmd_, ptt_command_handler, 1, nullptr);

  XPLMDebugString(
      "[xp_wellys_vfr_atc] PTT input initialized (X-Plane command)\n");
}

void stop() {
  if (ptt_cmd_) {
    XPLMUnregisterCommandHandler(ptt_cmd_, ptt_command_handler, 1, nullptr);
    ptt_cmd_ = nullptr;
  }
}

void update() {}

} // namespace ptt_input
