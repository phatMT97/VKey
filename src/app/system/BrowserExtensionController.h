// VKey - Browser Extension Controller (Windows-only)
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#ifdef _WIN32

#include <Windows.h>
#include <atomic>

namespace NextKey {

struct SystemConfig;

class BrowserExtensionController {
public:
    BrowserExtensionController() = default;
    ~BrowserExtensionController() = default;

    BrowserExtensionController(const BrowserExtensionController&) = delete;
    BrowserExtensionController& operator=(const BrowserExtensionController&) = delete;

    /// Restore enabled state from systemConfig. If previously enabled,
    /// registers native-messaging manifests.
    void Init(const SystemConfig& systemConfig);

    /// Flip on/off in response to user click: if enabling and binary is missing,
    /// prompts user to download. Updates registry manifests and persists to TOML.
    void Toggle(HWND notifyHwnd);

    [[nodiscard]] bool IsEnabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> enabled_{false};
};

}  // namespace NextKey

#endif  // _WIN32
