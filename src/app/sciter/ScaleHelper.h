// VKey - DPI & Resolution Scale Helper
// SPDX-License-Identifier: GPL-3.0-only
//
// Utility for scaling dialog sizes based on DPI and screen resolution.
// Ensures UI looks correct across different monitors and DPI settings.

#pragma once
#include <windows.h>
#include <algorithm>

namespace NextKey {

/**
 * ScaleHelper - Utility for scaling dialog sizes based on DPI and resolution.
 *
 * Considers both:
 * 1. DPI scaling (125%, 150%, etc.) - Sciter renders at native DPI
 * 2. Screen resolution (1366x768, etc.) - scales down proportionally
 *
 * Reference: 1920x1080 @ 100% DPI = scale factor 1.0
 */
class ScaleHelper {
public:
    // Reference resolution (development baseline at 100% DPI)
    static constexpr int REF_SCREEN_WIDTH = 1920;
    static constexpr int REF_SCREEN_HEIGHT = 1080;
    static constexpr double MIN_SCALE = 0.7;
    static constexpr double MAX_SCALE = 2.0;
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
     * Get the combined scale factor for dialogs.
     *
     * Examples:
     * - 1920x1080 @ 100%: scale = 1.0
     * - 1920x1080 @ 125%: scale = 1.25
     * - 1920x1080 @ 150%: scale = 1.5
     * - 1366x768 @ 100%:  scale = 0.71
     * - 1366x768 @ 125%:  scale = 0.89
     */
    [[nodiscard]] static double getScaleFactor() noexcept {
        // We only use DPI scaling because Sciter renders CSS pixels (e.g. width: 420px)
        // directly scaled by DPI. If we shrink the native window using a resolution scale 
        // (< 1.0 on small monitors), Sciter's content will overflow and get clipped!
        double dpiScale = getDpiScale();
        return (std::max)(MIN_SCALE, (std::min)(MAX_SCALE, dpiScale));
    }

    /**
     * Scale a single dimension (width or height).
     */
    [[nodiscard]] static int scale(int baseDimension) noexcept {
        return static_cast<int>(baseDimension * getScaleFactor());
    }

    /**
     * Scale a single dimension using per-window DPI (with optional padding before scale).
     * Falls back to system DPI when hwnd is null.
     */
    [[nodiscard]] static int scaleForWindow(HWND hwnd, int baseDimension, int pad = 0) noexcept {
        double dpiScale = getDpiScaleForWindow(hwnd);
        double factor = (std::max)(MIN_SCALE, (std::min)(MAX_SCALE, dpiScale));
        return static_cast<int>((baseDimension + pad) * factor);
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
