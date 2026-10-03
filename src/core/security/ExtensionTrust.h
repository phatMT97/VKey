// VKey - Extension Trust & Binary Integrity Verification
// SPDX-License-Identifier: GPL-3.0-only
//
// Version-bound integrity verification for optional companions and VKeyTSF.dll.

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

enum class ExtensionBinary : uint8_t { Watchdog, BrowserHost, TsfDll };

/// Verify a companion executable or VKeyTSF.dll before VKey launches, loads,
/// registers, or activates it.
/// Checks:
/// 1. File existence and accessibility (FILE_SHARE_READ).
/// 2. Bounded file size (> 0 and < 32 MiB).
/// 3. Exact SHA-256 match to this VKey build's binary or the pinned release
///    companion. Classic watchdogs and TSF DLLs signed after compilation also
///    require the current version and a valid Foundation signature.
///
/// In Debug builds or when compiled with VKEY_ALLOW_UNSIGNED_EXTENSIONS,
/// unsigned binaries are allowed for local developer testing with a log notice.
///
/// In official Release builds, any other binary is rejected.
[[nodiscard]] ExtensionTrustResult VerifyExtensionBinary(
    const std::wstring& filePath,
    ExtensionBinary binary,
    std::wstring* reasonOut = nullptr) noexcept;

/// Convenience boolean check: returns true if result == Trusted or DevBypass.
[[nodiscard]] bool IsExtensionTrusted(const std::wstring& filePath,
                                      ExtensionBinary binary) noexcept;

}  // namespace NextKey::Security
