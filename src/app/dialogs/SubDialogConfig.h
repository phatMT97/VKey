// VKey - SubDialog Configuration
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <windows.h>

namespace NextKey {

/// Configuration for SciterSubDialog windows
struct SubDialogConfig {
    const wchar_t* htmlPath;          // e.g. "this://app/excludedapps/excludedapps.html"
    const wchar_t* windowTitle;       // e.g. "VKey - Excluded Apps"
    int baseWidth = 360;
    int baseHeight = 420;
    HWND parentHwnd = nullptr;        // Center on parent (or screen if null)
    bool topmost = false;             // HWND_TOPMOST (defaults to false; inherits from parent if pinned)
    int titleBarHeight = 36;          // Drag zone height
    int buttonsWidth = 40;            // Exclude from drag zone
    bool applyBackgroundOpacity = true; // Load UIConfig + call JS setBackgroundOpacity
};

}  // namespace NextKey
