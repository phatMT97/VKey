// VKey - DPI & Resolution Scale Helper
// SPDX-License-Identifier: GPL-3.0-only
//
// Utility for scaling dialog sizes based on DPI and screen resolution.
// Ensures UI looks correct across different monitors and DPI settings.

#pragma once
#include <windows.h>
#include <cmath>

namespace NextKey {

/**
 * Scale native window dimensions at the same DPI as Sciter's CSS pixels.
 * Resolution-based shrinking or a DPI cap would clip the rendered content.
 */
class ScaleHelper {
public:
    static constexpr int DEFAULT_DPI = 96;

    /**
     * Get per-window DPI scale factor. Uses GetDpiForWindow (Win10 1607+)
     * which returns the correct DPI for the monitor the HWND is currently on.
     * Critical for multi-monitor setups where monitors have different DPI.
     * Falls back to system DPI when hwnd is null or on older Windows.
     */
    [[nodiscard]] static double getDpiScaleForWindow(HWND hwnd) noexcept {
        if (hwnd) {
            static auto pfn = reinterpret_cast<UINT(WINAPI*)(HWND)>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
            if (pfn) {
                UINT dpi = pfn(hwnd);
                if (dpi > 0) return static_cast<double>(dpi) / DEFAULT_DPI;
            }
        }
        // Fallback for null hwnd or older Windows
        static auto pfnSys = reinterpret_cast<UINT(WINAPI*)()>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
        if (pfnSys) {
            return static_cast<double>(pfnSys()) / DEFAULT_DPI;
        }
        HDC hdc = GetDC(nullptr);
        if (!hdc) return 1.0;
        int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
        return static_cast<double>(dpiX) / DEFAULT_DPI;
    }

    /**
     * Scale a single dimension using per-window DPI (with optional padding before scale).
     * Falls back to system DPI when hwnd is null.
     */
    [[nodiscard]] static int scaleForWindow(HWND hwnd, int baseDimension, int pad = 0) noexcept {
        return static_cast<int>(std::lround((baseDimension + pad) * getDpiScaleForWindow(hwnd)));
    }
};

}  // namespace NextKey
