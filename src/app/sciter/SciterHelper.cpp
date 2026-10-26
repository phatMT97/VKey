// VKey - Sciter Window Helper Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "SciterHelper.h"
#include "system/DarkModeHelper.h"
#include <dwmapi.h>
#include <windowsx.h>
#include "sciter-x.h"

#pragma comment(lib, "dwmapi.lib")

namespace NextKey {
namespace SciterHelper {

void enableWindowBlur(HWND hwnd, BlurMode mode) noexcept {
    if (!hwnd) return;

    const bool isBlur = (mode == BlurMode::Blur);
    const bool isWin11 = DarkModeHelper::IsWindows11OrGreater();

    // Win10 blur artifact: AccentState::BlurBehind applies DWM blur to the entire HWND
    // rectangle, including transparent pixels from border-radius corners and box-shadow area.
    // Win10 also doesn't support DWMWCP_ROUND, so DWM can't clip blur to the rounded shape.
    // Result: visible blur halo at the bottom edge of the window.
    //
    // Fix: skip blur on Win10 entirely. CSS --bg-glass (rgba(255,255,255,1) light /
    // rgba(5,5,8,0.92) dark) provides a solid-enough background without blur.
    // Reference: OpenKey ModernMenu.cpp — "Win10: No transparency (GDI+/acrylic unreliable)"
    if (!isWin11 && isBlur) {
        return;
    }

    // 1. Set WS_EX_LAYERED to allow alpha/blur composition
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);

    // 2. Apply SetWindowCompositionAttribute for blur effect (undocumented API)
    HMODULE hUser = GetModuleHandle(L"user32.dll");
    if (hUser) {
        using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttribData*);
        auto SetWindowCompositionAttribute = reinterpret_cast<SetWindowCompositionAttributeFn>(
            GetProcAddress(hUser, "SetWindowCompositionAttribute"));

        if (SetWindowCompositionAttribute) {
            AccentPolicy policy{};
            if (isBlur) {
                // Use BlurBehind (3) - most stable across Win10/11 versions
                // (AcrylicBlurBehind=4 has compatibility issues)
                policy.AccentState = static_cast<int>(AccentState::BlurBehind);
                policy.AccentFlags = 0;
                policy.GradientColor = 0;
            } else {
                policy.AccentState = static_cast<int>(AccentState::Disabled);
            }

            WindowCompositionAttribData data{};
            data.Attrib = DwmConstants::WCA_ACCENT_POLICY;
            data.pvData = &policy;
            data.cbData = sizeof(policy);

            SetWindowCompositionAttribute(hwnd, &data);
        }
    }

    // 3. Enable rounded corners on Windows 11 (Win10 ignores this silently)
    int cornerPreference = DwmConstants::DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DwmConstants::DWMWA_WINDOW_CORNER_PREFERENCE,
                          &cornerPreference, sizeof(cornerPreference));
}

LRESULT handleWindowDrag(HWND hwnd, LPARAM lParam, int titleHeight, int buttonsWidth) noexcept {
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ScreenToClient(hwnd, &pt);

    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);

    // Win10: .container margin + border pushes title bar down and buttons inward.
    // Extend drag zone to compensate.
    if (!DarkModeHelper::IsWindows11OrGreater()) {
        titleHeight += Win10Frame::FRAME_OFFSET;
        buttonsWidth += Win10Frame::FRAME_OFFSET;
    }

    // Scale coordinates based on window DPI to fix drag zone at > 100% scale
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = 96;

    int scaledTitleHeight = MulDiv(titleHeight, dpi, 96);
    int scaledButtonsWidth = MulDiv(buttonsWidth, dpi, 96);

    // Drag zone: top area excluding buttons on the right
    const int buttonsZone = clientRect.right - scaledButtonsWidth;

    if (pt.y >= 0 && pt.y < scaledTitleHeight && pt.x >= 0 && pt.x < buttonsZone) {
        return HTCAPTION;
    }

    return HTCLIENT;
}

void ForceTaskbarPresence(HWND hwnd, int iconId) noexcept {
    if (!hwnd) return;

    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    ex &= ~WS_EX_TOOLWINDOW;
    ex |= WS_EX_APPWINDOW;
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    style |= WS_MINIMIZEBOX | WS_SYSMENU;
    SetWindowLongW(hwnd, GWL_STYLE, style);

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);

    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(iconId));
    if (icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
}

bool GuardTaskbarStyle(WPARAM wParam, LPARAM lParam) noexcept {
    if (wParam == GWL_EXSTYLE) {
        auto* pss = reinterpret_cast<STYLESTRUCT*>(lParam);
        pss->styleNew &= ~WS_EX_TOOLWINDOW;
        pss->styleNew |= WS_EX_APPWINDOW;
        return true;
    }
    if (wParam == GWL_STYLE) {
        auto* pss = reinterpret_cast<STYLESTRUCT*>(lParam);
        pss->styleNew |= WS_MINIMIZEBOX | WS_SYSMENU;
        return true;
    }
    return false;
}

bool ConsumeRestoreFromMinimized(UINT msg, WPARAM wParam, bool& wasMinimized) noexcept {
    if (msg == WM_SIZE && wParam == SIZE_MINIMIZED) {
        wasMinimized = true;
        return false;
    }
    const bool restoring = (msg == WM_SIZE && wParam == SIZE_RESTORED)
                        || (msg == WM_SHOWWINDOW && wParam == TRUE);
    if (!restoring || !wasMinimized) return false;
    wasMinimized = false;
    return true;
}

void RepaintAfterRestore(HWND hwnd) {
    sciter::dom::element rootEl = sciter::dom::element::root_element(hwnd);
    if (rootEl.is_valid()) {
        rootEl.set_attribute("force-paint", L"1");
        rootEl.update(false);
        rootEl.remove_attribute("force-paint");
        rootEl.update(false);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

}  // namespace SciterHelper
}  // namespace NextKey
