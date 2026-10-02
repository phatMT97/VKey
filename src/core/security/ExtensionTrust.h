// VKey - Extension Trust & Binary Integrity Verification
// SPDX-License-Identifier: GPL-3.0-only
//
// Zero-trust verification for optional companion binaries (Watchdog, BrowserHost)
// to prevent loading tampered or malicious executables in open-source builds.

#pragma once

#include <cstdint>
#include <string>

namespace NextKey::Security {

enum class ExtensionTrustResult : uint8_t {
    Trusted = 0,
    FileNotFound,
    CannotAccess,
    SizeOutOfRange,
    SignatureInvalid,
    UntrustedPublisher,
    DevBypass
};

/// Verify an extension executable binary (.exe) before launching or registering it.
/// Checks:
/// 1. File existence and accessibility (FILE_SHARE_READ).
/// 2. Bounded file size (> 0 and < 32 MiB).
/// 3. Valid Authenticode digital signature chaining to a trusted root CA.
/// 4. Publisher pin matching official release signers (SignPath Foundation or project cert).
///
/// In Debug builds or when compiled with VKEY_ALLOW_UNSIGNED_EXTENSIONS,
/// unsigned binaries are allowed for local developer testing with a log notice.
///
/// In official Release builds, invalid or untrusted signatures will return false.
[[nodiscard]] ExtensionTrustResult VerifyExtensionBinary(
    const std::wstring& filePath,
    std::wstring* reasonOut = nullptr) noexcept;

/// Convenience boolean check: returns true if result == Trusted or DevBypass.
[[nodiscard]] bool IsExtensionTrusted(const std::wstring& filePath) noexcept;

}  // namespace NextKey::Security
