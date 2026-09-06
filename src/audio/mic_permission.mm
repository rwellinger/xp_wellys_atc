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

#include "audio/mic_permission.hpp"

#include <XPLMUtilities.h>

#import <AVFoundation/AVFoundation.h>

namespace mic_permission {

bool check_and_request() {
  AVAuthorizationStatus status =
      [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];

  switch (status) {
  case AVAuthorizationStatusAuthorized:
    XPLMDebugString("[xp_wellys_vfr_atc] Microphone permission: Authorized\n");
    return true;

  case AVAuthorizationStatusNotDetermined:
    XPLMDebugString(
        "[xp_wellys_vfr_atc] Microphone permission: Not determined - requesting "
        "access (restart X-Plane after granting)\n");
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) {
                               if (granted) {
                                 XPLMDebugString(
                                     "[xp_wellys_vfr_atc] Microphone permission "
                                     "granted - please restart X-Plane\n");
                               } else {
                                 XPLMDebugString(
                                     "[xp_wellys_vfr_atc] Microphone permission "
                                     "denied by user\n");
                               }
                             }];
    return false;

  case AVAuthorizationStatusDenied:
    XPLMDebugString(
        "[xp_wellys_vfr_atc] ERROR: Microphone permission DENIED. Enable in: "
        "System Settings > Privacy & Security > Microphone > X-Plane\n");
    return false;

  case AVAuthorizationStatusRestricted:
    XPLMDebugString(
        "[xp_wellys_vfr_atc] ERROR: Microphone access restricted by policy\n");
    return false;

  default:
    return false;
  }
}

} // namespace mic_permission
