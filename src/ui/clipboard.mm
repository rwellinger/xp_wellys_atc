/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "ui/clipboard.hpp"

#import <AppKit/AppKit.h>

namespace ui::clipboard {

std::string read_system_text() {
  @autoreleasepool {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    if (pb == nil)
      return {};
    NSString *s = [pb stringForType:NSPasteboardTypeString];
    if (s == nil)
      return {};
    const char *utf8 = [s UTF8String];
    if (utf8 == nullptr)
      return {};
    return std::string(utf8);
  }
}

void write_system_text(const std::string &text) {
  if (text.empty())
    return;
  @autoreleasepool {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    if (pb == nil)
      return;
    NSString *s = [NSString stringWithUTF8String:text.c_str()];
    if (s == nil)
      return;
    [pb clearContents];
    [pb setString:s forType:NSPasteboardTypeString];
  }
}

} // namespace ui::clipboard
