/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Win32 counterpart of the macOS NSPasteboard bridge (clipboard.mm).
 * Reads/writes the system clipboard via the classic Win32 clipboard API
 * (issue #22). ImGui reaches only its internal clipboard in this plugin
 * (no platform backend installed — input comes from X-Plane), so the
 * [Paste] buttons need these helpers to see what the user copied from
 * outside the sim. CF_UNICODETEXT is UTF-16; we transcode to/from UTF-8
 * to match the rest of the codebase. Never throws.
 */

#include "ui/clipboard.hpp"

#include <windows.h>

#include <vector>

namespace ui::clipboard {

std::string read_system_text() {
  if (!OpenClipboard(nullptr))
    return {};

  std::string result;
  HANDLE handle = GetClipboardData(CF_UNICODETEXT);
  if (handle != nullptr) {
    const wchar_t *wide = static_cast<const wchar_t *>(GlobalLock(handle));
    if (wide != nullptr) {
      // -1 length: the source is null-terminated, so the computed length
      // (and the resulting string) includes the terminator.
      const int len =
          WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr,
                              nullptr);
      if (len > 1) {
        std::vector<char> utf8(static_cast<size_t>(len));
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), len, nullptr,
                            nullptr);
        result.assign(utf8.data()); // stops at the embedded null terminator
      }
      GlobalUnlock(handle);
    }
  }
  CloseClipboard();
  return result;
}

void write_system_text(const std::string &text) {
  if (text.empty())
    return;
  if (!OpenClipboard(nullptr))
    return;

  EmptyClipboard();

  const int wlen = MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  // +1 for the null terminator CF_UNICODETEXT requires. GMEM_MOVEABLE is
  // mandatory: SetClipboardData rejects fixed handles.
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE,
                            (static_cast<size_t>(wlen) + 1) * sizeof(wchar_t));
  if (mem != nullptr) {
    wchar_t *dst = static_cast<wchar_t *>(GlobalLock(mem));
    if (dst != nullptr) {
      MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                          static_cast<int>(text.size()), dst, wlen);
      dst[wlen] = L'\0';
      GlobalUnlock(mem);
      // On success the clipboard owns the block; only free it on failure.
      if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr)
        GlobalFree(mem);
    } else {
      GlobalFree(mem);
    }
  }
  CloseClipboard();
}

} // namespace ui::clipboard
