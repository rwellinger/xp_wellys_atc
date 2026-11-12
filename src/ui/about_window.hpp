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

#ifndef ABOUT_WINDOW_HPP
#define ABOUT_WINDOW_HPP

// The developer profile shared by all of thWelly's plugins (xp_wellys_about),
// completed with this plugin's own license notice. Opened from the plugin
// menu ("About..."); rendered inside atc_ui's ImGui frame.
namespace about_window {

// Draws the free-standing About window while `open` is true; its close
// button sets `open` to false. Needs X-Plane's GL context (the photo
// texture is uploaded on the first call), so call it from the draw callback.
void draw(bool &open);

// Frees the photo texture. Call before the ImGui OpenGL2 renderer shuts down.
void release();

} // namespace about_window

#endif // ABOUT_WINDOW_HPP
