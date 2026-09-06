/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef PERSISTENCE_KEYCHAIN_HPP
#define PERSISTENCE_KEYCHAIN_HPP

#include <string>

namespace persistence::keychain {

// macOS Keychain wrapper for the OpenAI API key. The single-argument
// overloads use the production service/account
// ("com.xp_wellys_devfr_atc.openai" / "default"); the explicit overloads
// take a service+account pair and exist so unit tests can target a
// dedicated test entry without clobbering the user's real key.
//
// load() returns an empty string when no entry exists or when the
// Keychain call fails — callers must treat empty as "no key".
// On non-macOS builds all calls are stubbed and report no key.

bool save(const std::string &api_key);
std::string load();
bool remove();
bool has();

bool save(const std::string &service, const std::string &account,
          const std::string &api_key);
std::string load(const std::string &service, const std::string &account);
bool remove(const std::string &service, const std::string &account);
bool has(const std::string &service, const std::string &account);

} // namespace persistence::keychain

#endif // PERSISTENCE_KEYCHAIN_HPP
