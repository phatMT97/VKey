// VKey - Extension Installer & Downloader
// SPDX-License-Identifier: GPL-3.0-only
//
// Manages on-demand download and verification of optional companion executables
// (Watchdog, BrowserHost) from GitHub Releases.

#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace NextKey {

enum class ExtensionType : uint8_t {
    Watchdog = 0,
    BrowserHost = 1
};

class ExtensionInstaller {
public:
    [[nodiscard]] static std::wstring GetExtensionExeName(ExtensionType type) noexcept;
    [[nodiscard]] static std::wstring GetExtensionPath(ExtensionType type) noexcept;
    [[nodiscard]] static std::wstring GetExtensionDownloadUrl(ExtensionType type);
    [[nodiscard]] static std::wstring GetReleasePageUrl();

    /// Returns true if the companion executable exists and passes cryptographic integrity verification.
    [[nodiscard]] static bool IsInstalledAndTrusted(ExtensionType type) noexcept;

#ifdef _WIN32
    /// Checks if the extension is present and trusted. If not, prompts the user to
    /// download from official GitHub Releases, downloads to staging, verifies signature,
    /// and places it into the application directory.
    /// Returns true if the extension is ready and trusted to execute.
    [[nodiscard]] static bool EnsureInstalledWithUi(ExtensionType type, HWND parentHwnd) noexcept;
#endif
};

}  // namespace NextKey
