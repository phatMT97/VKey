// VKey - System Configuration
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <climits>
#include <cstdint>

namespace NextKey {

/// Icon style for tray/language bar
enum class IconStyle : uint8_t {
    Color = 0,   // Default colored icons (V=pink, E=blue)
    Dark = 1,    // White icons (for dark taskbar)
    Light = 2,   // Black icons (for light taskbar)
    Custom = 3,  // User-selected custom colors
    Auto = 4     // Follow Windows taskbar theme (SystemUsesLightTheme)
};

/// Default icon colors (COLORREF format: 0x00BBGGRR)
/// V: RGB(232, 17, 35)  = #E81123
/// E: RGB(0, 120, 215)  = #0078D7
inline constexpr uint32_t DEFAULT_ICON_COLOR_V = 0x002311E8;  // BGR
inline constexpr uint32_t DEFAULT_ICON_COLOR_E = 0x00D77800;  // BGR

/// System-level configuration (EXE-only, not shared with DLL)
/// Stored in [system] section of config.toml
struct SystemConfig {
    bool runAtStartup = false;     // Register to run on Windows logon
    bool runAsAdmin = false;       // Run with elevated privileges (Task Scheduler)
    bool showOnStartup = true;     // Open settings dialog on app startup
    bool desktopShortcut = false;  // Desktop shortcut exists

    // Theme override
    bool forceLightTheme = false;  // Always use light theme (ignore Windows dark mode)

    uint8_t language = 0;


    // Icon customization
    uint8_t iconStyle = 0;         // IconStyle enum (0=Color, 1=Dark, 2=Light, 3=Custom, 4=Auto)
    uint32_t customColorV = 0;     // Custom V color (COLORREF, 0 = use default)
    uint32_t customColorE = 0;     // Custom E color (COLORREF, 0 = use default)

    // TSF "T" tray indicator. When false (default) the tray shows the normal
    // V/E icon even in TSF apps; when true it shows a colored "T" to signal the
    // focused app is handled by the TSF engine (issue #209 — opt-in: testers
    // preferred plain V/E, "the main point is E/V").
    bool showTsfIndicator = false;

    // Floating V/E icon overlay (draggable)
    bool showFloatingIcon = false;     // Show floating V/E indicator
    int32_t floatingIconX = INT32_MIN; // Saved X position (INT32_MIN = default)
    int32_t floatingIconY = INT32_MIN; // Saved Y position (INT32_MIN = default)

    // Startup mode
    uint8_t startupMode = 0;   // 0=Vietnamese, 1=English, 2=Remember (Smart Switch persist)

    // Auto-update
    bool autoCheckUpdate = true;   // Check for updates on startup

    // Watchdog (auto-restart on crash) — opt-in. When true, VKeyWatchdog.exe
    // is registered with Task Scheduler at logon and respawns VKey on crash.
    bool watchdogEnabled = false;

    // Browser extension integration — opt-in. When true, native-messaging
    // manifests are registered for Chrome, Edge, Firefox, etc.
    bool browserExtensionEnabled = false;

    /// Get effective V color (default if custom not set)
    [[nodiscard]] uint32_t GetEffectiveColorV() const noexcept {
        return customColorV != 0 ? customColorV : DEFAULT_ICON_COLOR_V;
    }

    /// Get effective E color (default if custom not set)
    [[nodiscard]] uint32_t GetEffectiveColorE() const noexcept {
        return customColorE != 0 ? customColorE : DEFAULT_ICON_COLOR_E;
    }

    /// Check if UI language is English (derived from language field)
    [[nodiscard]] bool IsEnglishUI() const noexcept { return language == 1; }

    SystemConfig() = default;
};

}  // namespace NextKey
