/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "core/logging.hpp"

#include <cstdarg>
#include <cstdio>

namespace logging {

static void default_sink(const char *line) { std::fprintf(stderr, "%s", line); }

static Sink sink_ = &default_sink;

void set_sink(Sink s) { sink_ = (s != nullptr) ? s : &default_sink; }

static void emit(const char *level, const char *fmt, va_list ap) {
  char msg[1024];
  std::vsnprintf(msg, sizeof(msg), fmt, ap);

  char line[1152];
  std::snprintf(line, sizeof(line), "[xp_wellys_vfr_atc]%s %s\n", level, msg);

  sink_(line);
}

void debug(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emit("[DEBUG]", fmt, ap);
  va_end(ap);
}

void info(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emit("", fmt, ap);
  va_end(ap);
}

void error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emit("[ERROR]", fmt, ap);
  va_end(ap);
}

} // namespace logging
