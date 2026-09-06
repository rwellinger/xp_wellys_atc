/*
 * xp_wellys_vfr_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "persistence/keychain.hpp"

#include <cstring>

namespace persistence::keychain {

namespace {
constexpr const char *kProdService = "com.xp_wellys_devfr_atc.openai";
constexpr const char *kProdAccount = "default";
} // namespace

#if defined(__APPLE__)

// SecKeychain* APIs are deprecated in favor of SecItem*, but they remain
// fully functional on macOS 13+ and keep the implementation compact.
// Silence the warning locally rather than rewriting on the SecItem path.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <Security/Security.h>

bool save(const std::string &service, const std::string &account,
          const std::string &api_key) {
  if (api_key.empty())
    return false;

  // Overwrite semantics via delete-then-add rather than in-place
  // modify. A generic-password item created by a previously signed
  // build (e.g. the upstream plugin before the rename, or any earlier
  // ad-hoc-signed build) carries an ACL bound to that binary's
  // signature. SecKeychainItemModifyAttributesAndData is then denied
  // for the current binary, so "Save Key" silently failed until the
  // user deleted the entry by hand. Deleting first and adding a fresh
  // item sidesteps the ACL — the new item is owned by the current
  // binary. Delete failure is ignored (the item may not exist, or the
  // delete may be denied; the add below is what we check).
  SecKeychainItemRef existing = nullptr;
  if (SecKeychainFindGenericPassword(
          nullptr, static_cast<UInt32>(service.size()), service.c_str(),
          static_cast<UInt32>(account.size()), account.c_str(), nullptr,
          nullptr, &existing) == errSecSuccess &&
      existing) {
    SecKeychainItemDelete(existing);
    CFRelease(existing);
  }

  OSStatus status = SecKeychainAddGenericPassword(
      nullptr, static_cast<UInt32>(service.size()), service.c_str(),
      static_cast<UInt32>(account.size()), account.c_str(),
      static_cast<UInt32>(api_key.size()), api_key.c_str(), nullptr);
  return status == errSecSuccess;
}

std::string load(const std::string &service, const std::string &account) {
  UInt32 pw_len = 0;
  void *pw_data = nullptr;
  OSStatus status = SecKeychainFindGenericPassword(
      nullptr, static_cast<UInt32>(service.size()), service.c_str(),
      static_cast<UInt32>(account.size()), account.c_str(), &pw_len, &pw_data,
      nullptr);

  if (status == errSecSuccess && pw_data) {
    std::string result(static_cast<char *>(pw_data), pw_len);
    SecKeychainItemFreeContent(nullptr, pw_data);
    return result;
  }
  return {};
}

bool remove(const std::string &service, const std::string &account) {
  SecKeychainItemRef item = nullptr;
  OSStatus status = SecKeychainFindGenericPassword(
      nullptr, static_cast<UInt32>(service.size()), service.c_str(),
      static_cast<UInt32>(account.size()), account.c_str(), nullptr, nullptr,
      &item);

  if (status != errSecSuccess || !item)
    return false;

  status = SecKeychainItemDelete(item);
  CFRelease(item);
  return status == errSecSuccess;
}

bool has(const std::string &service, const std::string &account) {
  SecKeychainItemRef item = nullptr;
  OSStatus status = SecKeychainFindGenericPassword(
      nullptr, static_cast<UInt32>(service.size()), service.c_str(),
      static_cast<UInt32>(account.size()), account.c_str(), nullptr, nullptr,
      &item);
  if (status == errSecSuccess && item) {
    CFRelease(item);
    return true;
  }
  return false;
}

#pragma clang diagnostic pop

#elif defined(_WIN32)

#include <windows.h>
// wincred.h must follow windows.h.
#include <wincred.h>

namespace {
// Credential Manager target name: "<service>/<account>", e.g.
// "com.xp_wellys_devfr_atc.openai/default". The two production services
// (OpenAI / Mistral) map to two distinct targets and coexist, exactly
// like the two macOS Keychain service entries.
std::wstring target_name(const std::string &service,
                         const std::string &account) {
  const std::string t = service + "/" + account;
  const int len = MultiByteToWideChar(CP_UTF8, 0, t.c_str(),
                                      static_cast<int>(t.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, t.c_str(), static_cast<int>(t.size()),
                      w.data(), len);
  return w;
}
} // namespace

bool save(const std::string &service, const std::string &account,
          const std::string &api_key) {
  if (api_key.empty())
    return false;

  const std::wstring target = target_name(service, account);
  CREDENTIALW cred{};
  cred.Type = CRED_TYPE_GENERIC;
  cred.TargetName = const_cast<LPWSTR>(target.c_str());
  cred.CredentialBlobSize = static_cast<DWORD>(api_key.size());
  cred.CredentialBlob =
      reinterpret_cast<LPBYTE>(const_cast<char *>(api_key.data()));
  // LOCAL_MACHINE: survives logoff/restart (matches the Keychain's
  // persistence). The blob is DPAPI-encrypted per user under the hood,
  // so no plaintext secret ever hits disk.
  cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
  return CredWriteW(&cred, 0) == TRUE;
}

std::string load(const std::string &service, const std::string &account) {
  const std::wstring target = target_name(service, account);
  PCREDENTIALW cred = nullptr;
  if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) != TRUE)
    return {}; // miss == no key (contract)
  std::string result(reinterpret_cast<const char *>(cred->CredentialBlob),
                     cred->CredentialBlobSize);
  CredFree(cred);
  return result;
}

bool remove(const std::string &service, const std::string &account) {
  const std::wstring target = target_name(service, account);
  return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) == TRUE;
}

bool has(const std::string &service, const std::string &account) {
  const std::wstring target = target_name(service, account);
  PCREDENTIALW cred = nullptr;
  if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) == TRUE) {
    CredFree(cred);
    return true;
  }
  return false;
}

#else

bool save(const std::string &, const std::string &, const std::string &) {
  return false;
}
std::string load(const std::string &, const std::string &) { return {}; }
bool remove(const std::string &, const std::string &) { return false; }
bool has(const std::string &, const std::string &) { return false; }

#endif

// Production-default convenience overloads.

bool save(const std::string &api_key) {
  return save(kProdService, kProdAccount, api_key);
}
std::string load() { return load(kProdService, kProdAccount); }
bool remove() { return remove(kProdService, kProdAccount); }
bool has() { return has(kProdService, kProdAccount); }

} // namespace persistence::keychain
