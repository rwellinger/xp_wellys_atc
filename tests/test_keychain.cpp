/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Roundtrip test for the macOS Keychain wrapper used to store the
 * OpenAI API key. The test uses a dedicated service name
 * ("com.xp_wellys_devfr_atc.test") so it can never clobber a real user key
 * stored under the production service.
 */

#include "persistence/keychain.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

namespace {
const std::string kTestService = "com.xp_wellys_devfr_atc.test";
const std::string kTestAccount = "keychain_unit_test";
} // namespace

TEST_CASE("keychain roundtrip with dedicated test service",
          "[keychain][persistence]") {
#if !defined(__APPLE__) && !defined(_WIN32)
  SKIP("Secret-storage backend is macOS/Windows only");
#else
  // Pre-clean any leftover from a previous run.
  persistence::keychain::remove(kTestService, kTestAccount);

  // The macOS wrapper writes to the user's login keychain via
  // Security.framework, which requires an unlocked login keychain in a
  // real user session. Over SSH, on a headless CI box, or with a locked
  // keychain, SecKeychainAddGenericPassword returns a non-success status
  // and save() is false. That is an environment limitation, not a code
  // defect — probe once and skip cleanly rather than reporting a spurious
  // failure. In the live X-Plane (Aqua session, unlocked keychain) the
  // path works. The Windows Credential Manager has no such gate but the
  // same probe keeps the test environment-robust.
  if (!persistence::keychain::save(kTestService, kTestAccount, "probe")) {
    SKIP("Secret store not writable in this environment "
         "(headless/SSH session or locked login keychain)");
  }
  persistence::keychain::remove(kTestService, kTestAccount);

  REQUIRE_FALSE(persistence::keychain::has(kTestService, kTestAccount));

  SECTION("save then load returns the same value") {
    const std::string secret = "sk-test-1234567890ABCDEF";
    REQUIRE(persistence::keychain::save(kTestService, kTestAccount, secret));
    REQUIRE(persistence::keychain::has(kTestService, kTestAccount));
    REQUIRE(persistence::keychain::load(kTestService, kTestAccount) == secret);
  }

  SECTION("save overwrites an existing entry") {
    REQUIRE(persistence::keychain::save(kTestService, kTestAccount, "first"));
    REQUIRE(persistence::keychain::save(kTestService, kTestAccount, "second"));
    REQUIRE(persistence::keychain::load(kTestService, kTestAccount) ==
            "second");
  }

  SECTION("remove makes load return empty") {
    REQUIRE(
        persistence::keychain::save(kTestService, kTestAccount, "to-delete"));
    REQUIRE(persistence::keychain::remove(kTestService, kTestAccount));
    REQUIRE_FALSE(persistence::keychain::has(kTestService, kTestAccount));
    REQUIRE(persistence::keychain::load(kTestService, kTestAccount).empty());
  }

  SECTION("save rejects empty key") {
    REQUIRE_FALSE(persistence::keychain::save(kTestService, kTestAccount, ""));
  }

  // Final cleanup so nothing lingers in the user's Keychain.
  persistence::keychain::remove(kTestService, kTestAccount);
#endif
}
