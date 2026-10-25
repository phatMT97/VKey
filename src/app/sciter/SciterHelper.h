// VKey - Sciter Window Helper
// SPDX-License-Identifier: GPL-3.0-only
//
// Utilities for Sciter window effects: DWM blur, acrylic, window dragging.
// Dark mode utilities are in system/DarkModeHelper.h

#pragma once
#include <windows.h>

namespace NextKey {

/// Blur mode for Sciter windows
enum class BlurMode {
    Solid = 0,   // No blur effect
    Blur = 1     // DWM blur/acrylic effect
};

/// DWM accent state values (undocumented Windows API)
enum class AccentState : int {
    Disabled = 0,
    BlurBehind = 3,
    AcrylicBlurBehind = 4,
    HostBackdrop = 5
};

/// DWM accent policy structure (for SetWindowCompositionAttribute)
struct AccentPolicy {
    int AccentState;
    int AccentFlags;
    int GradientColor;
    int AnimationId;
};

/// DWM window composition attribute data
struct WindowCompositionAttribData {
    int Attrib;
    void* pvData;
    size_t cbData;
};

/// Undocumented DWM constants
namespace DwmConstants {
    constexpr int WCA_ACCENT_POLICY = 19;
    constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    constexpr int DWMWCP_ROUND = 2;
}

/// Default dimensions for title bar drag handling
namespace TitleBarDefaults {
    constexpr int HEIGHT = 40;
    constexpr int BUTTONS_WIDTH = 80;
}

/// Win10 CSS frame geometry.
/// `.container` on Win10 (see shared/theme.css) adds `margin: 12px` + `border: 1px`
/// to define the window edge (Win11 uses DWMWCP_ROUND instead). Window-sizing
/// code must compensate so the HWND covers the shadow+border; drag-zone code
/// must shift to skip the margin. One definition here — don't re-hardcode.
namespace Win10Frame {
    constexpr int CSS_MARGIN = 12;   // .container margin on each side
    constexpr int CSS_BORDER = 1;    // .container 1px border
    constexpr int FRAME_OFFSET = CSS_MARGIN + CSS_BORDER;   // margin + border (per side)
    constexpr int FRAME_SIZE_ADD = 2 * CSS_MARGIN;          // width/height padding total
}

namespace SciterHelper {

    /**
     * Enable DWM blur/acrylic effect on a window.
     *
     * @param hwnd Window handle
     * @param mode BlurMode::Solid (no effect) or BlurMode::Blur (acrylic)
     */
    void enableWindowBlur(HWND hwnd, BlurMode mode) noexcept;

    /**
     * Handle WM_NCHITTEST for window dragging via title bar area.
     *
     * Call this from your SubclassProc when handling WM_NCHITTEST.
     * Returns HTCAPTION if mouse is in drag zone, HTCLIENT otherwise.
     *
     * @param hwnd Window handle
     * @param lParam LPARAM from WM_NCHITTEST (contains mouse position)
     * @param titleHeight Height of the title bar drag zone (default 40px)
     * @param buttonsWidth Width of buttons area to exclude from drag zone (default 80px)
     * @return HTCAPTION for drag zone, HTCLIENT for content area
     */
    [[nodiscard]] LRESULT handleWindowDrag(
        HWND hwnd,
        LPARAM lParam,
        int titleHeight = TitleBarDefaults::HEIGHT,
        int buttonsWidth = TitleBarDefaults::BUTTONS_WIDTH) noexcept;

    /**
     * Force a Sciter popup/main window to appear on the taskbar with the app icon.
     * Removes WS_EX_TOOLWINDOW, adds WS_EX_APPWINDOW, ensures WS_MINIMIZEBOX and
     * WS_SYSMENU styles so clicking the taskbar button minimizes/restores the window,
     * applies SWP_FRAMECHANGED, and sets WM_SETICON from IDI_APP. Call AFTER all expand() calls.
     *
     * @param hwnd Window handle
     * @param iconId Resource ID for the window icon (e.g. IDI_APP)
     */
    void ForceTaskbarPresence(HWND hwnd, int iconId) noexcept;

    /**
     * Filter WM_STYLECHANGING to prevent Sciter from re-adding WS_EX_TOOLWINDOW
     * and ensure WS_MINIMIZEBOX / WS_SYSMENU are retained.
     * Call from SubclassProc. Modifies styleNew in-place; caller should fall through
     * to DefSubclassProc to propagate the corrected style.
     *
     * @param wParam wParam from WM_STYLECHANGING
     * @param lParam lParam from WM_STYLECHANGING (pointer to STYLESTRUCT)
     * @return true if the style was modified (caller should still fall through)
     */
    [[nodiscard]] bool GuardTaskbarStyle(WPARAM wParam, LPARAM lParam) noexcept;

}  // namespace SciterHelper

}  // namespace NextKey
