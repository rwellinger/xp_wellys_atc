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

#ifndef UI_STRINGS_HPP
#define UI_STRINGS_HPP

namespace ui_strings {

void init();
void stop();
void reload();

// Look up the localized UI string for `key`. Returns the value from
// the loaded JSON if present; otherwise returns `key` itself so the
// UI stays readable even when a key is missing or the file fails to
// load. The returned pointer is owned by the internal map and stays
// valid until the next reload() call -- every UI draw path resolves
// tr() per frame, so this is safe in practice.
const char *tr(const char *key);

} // namespace ui_strings

#endif // UI_STRINGS_HPP
