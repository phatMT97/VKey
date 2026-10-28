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
     * Get DPI scale factor (1.0 = 100%, 1.25 = 125%, 2.0 = 200%, etc.)
     * Uses GetDpiForSystem (Win10 1607+) which returns the correct value
     * for PerMonitorV2 apps. Falls back to GetDeviceCaps for older Windows.
     */
    [[nodiscard]] static double getDpiScale() noexcept {
        // GetDpiForSystem returns system DPI correctly even for PerMonitorV2 apps
        // (unlike GetDeviceCaps which returns 96 for PerMonitorV2)
        static auto pfn = reinterpret_cast<UINT(WINAPI*)()>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
        if (pfn) {
            return static_cast<double>(pfn()) / DEFAULT_DPI;
        }
        // Fallback for older Windows
        HDC hdc = GetDC(nullptr);
        if (!hdc) return 1.0;
        int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
        return static_cast<double>(dpiX) / DEFAULT_DPI;
    }

    /**
     * Get per-window DPI scale factor. Uses GetDpiForWindow (Win10 1607+)
     * which returns the correct DPI for the monitor the HWND is currently on.
     * Critical for multi-monitor setups where monitors have different DPI.
     * Falls back to getDpiScale() (system DPI) when hwnd is null or on older Windows.
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
        return getDpiScale();
    }

    /**
     * Get the system DPI scale without capping it below Sciter's scale.
     */
    [[nodiscard]] static double getScaleFactor() noexcept {
        return getDpiScale();
    }

    /**
     * Scale a single dimension (width or height).
     */
    [[nodiscard]] static int scale(int baseDimension) noexcept {
        return static_cast<int>(std::lround(baseDimension * getScaleFactor()));
    }

    /**
     * Scale a single dimension using per-window DPI (with optional padding before scale).
     * Falls back to system DPI when hwnd is null.
     */
    [[nodiscard]] static int scaleForWindow(HWND hwnd, int baseDimension, int pad = 0) noexcept {
        return static_cast<int>(std::lround((baseDimension + pad) * getDpiScaleForWindow(hwnd)));
    }

    /**
     * Scale a RECT for Sciter window initialization.
     */
    [[nodiscard]] static RECT scaleRect(int baseWidth, int baseHeight) noexcept {
        double factor = getScaleFactor();
        return RECT{
            0, 0,
            static_cast<LONG>(baseWidth * factor),
            static_cast<LONG>(baseHeight * factor)
        };
    }

    /**
     * Get scaled width and height as separate output values.
     */
    static void getScaledSize(int baseWidth, int baseHeight, int& outWidth, int& outHeight) noexcept {
        double factor = getScaleFactor();
        outWidth = static_cast<int>(baseWidth * factor);
        outHeight = static_cast<int>(baseHeight * factor);
    }
};

}  // namespace NextKey
