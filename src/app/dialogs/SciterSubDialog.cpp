// VKey - Sciter SubDialog Base Class Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "SciterSubDialog.h"
#include "../resource.h"
#include "helpers/AppHelpers.h"
#include "sciter/ScaleHelper.h"
#include "sciter/SciterHelper.h"
#include "system/DarkModeHelper.h"
#include "core/config/ConfigManager.h"
#include "core/Strings.h"
#include "core/Debug.h"
#include "sciter-x-dom.hpp"
#include "sciter-x-host-callback.h"
#include <dwmapi.h>
#include <commctrl.h>
#include <vector>

using namespace sciter::dom;

#pragma comment(lib, "comctl32.lib")

namespace NextKey {

// DWM constants for dark mode
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

SciterSubDialog* SciterSubDialog::s_instance = nullptr;

// Win10 CSS margin on .container must be compensated in HWND size (see Win10Frame).
static int SubDialogWin10Pad() noexcept {
    return DarkModeHelper::IsWindows11OrGreater() ? 0 : Win10Frame::FRAME_SIZE_ADD;
}

SciterSubDialog::SciterSubDialog(const SubDialogConfig& config)
    : sciter::window(SW_MAIN, RECT{-10000, -10000,
        -10000 + ScaleHelper::scaleForWindow(config.parentHwnd, config.baseWidth, SubDialogWin10Pad()),
        -10000 + ScaleHelper::scaleForWindow(config.parentHwnd, config.baseHeight, SubDialogWin10Pad())})
    , config_(config) {

    s_instance = this;

    // Set up UI base path for Debug mode
#ifndef SCITER_USE_PACKFOLDER
    wchar_t exeDir[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH) != 0) {
        wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
        if (lastSlash) *lastSlash = L'\0';
        uiBasePath_ = exeDir;
        uiBasePath_ += L"\\ui\\";
    }
#endif

    // Enable transparency for blur effect
    SciterSetOption(get_hwnd(), SCITER_TRANSPARENT_WINDOW, 1);

    // Load HTML
    if (!load(config_.htmlPath)) {
        NEXTKEY_LOG(L"Failed to load dialog HTML: %s", config_.htmlPath);
        return;
    }

    // Load system config once for theme decisions
    bool forceLightTheme = ConfigManager::LoadSystemConfigOrDefault().forceLightTheme;

    // Set theme class + language (before scripts run)
    {
        sciter::dom::element htmlRoot(get_root());
        // Dark class goes on <body> (CSS targets body.dark), lang goes on <html>
        sciter::dom::element body = htmlRoot.find_first("body");
        if (body.is_valid()) {
            bool dark = !forceLightTheme && DarkModeHelper::IsWindowsDarkMode();
            auto classes = DarkModeHelper::BuildBodyClasses(dark);
            if (!classes.empty()) {
                body.set_attribute("class", classes.c_str());
            }
            TypingConfig typingCfg = ConfigManager::LoadOrDefault();
            body.set_attribute("data-enable-toast", typingCfg.enableToast ? L"true" : L"false");
        }
        if (GetLanguage() == Language::English) {
            htmlRoot.set_attribute("lang", L"en");
            // initSubDialog() defers applyTranslations() via requestAnimationFrame,
            // so it will pick up this lang="en" attribute automatically.
        }
    }

    // Show window
    expand();

    // Set title
    SetWindowTextW(get_hwnd(), config_.windowTitle);

    // Subclass for dragging and close
    SetWindowSubclass(get_hwnd(), SubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    // Theme-aware DWM mode + rounded corners + blur (after subclass)
    HWND hwnd = get_hwnd();
    if (hwnd) {
        bool dark = !forceLightTheme && DarkModeHelper::IsWindowsDarkMode();
        DarkModeHelper::SetWindowDarkMode(hwnd, dark);

        // Refresh body class (may have been set during load() before subclass)
        sciter::dom::element htmlRoot(get_root());
        sciter::dom::element body = htmlRoot.find_first("body");
        if (body.is_valid()) {
            body.set_attribute("class", DarkModeHelper::BuildBodyClasses(dark).c_str());
        }

        int cornerPreference = DwmConstants::DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DwmConstants::DWMWA_WINDOW_CORNER_PREFERENCE,
                              &cornerPreference, sizeof(cornerPreference));

        SciterHelper::enableWindowBlur(hwnd, BlurMode::Blur);
    }

    // Apply background opacity from UIConfig (via DOM, not call_function)
    if (config_.applyBackgroundOpacity) {
        auto uiConfig = ConfigManager::LoadUIConfigOrDefault();
        bool isDark = forceLightTheme ? false : DarkModeHelper::IsWindowsDarkMode();
        double opacity = uiConfig.backgroundOpacity / 100.0;
        sciter::dom::element rootEl2(get_root());
        sciter::dom::element mainContainer = rootEl2.find_first("#main-container");
        if (mainContainer.is_valid()) {
            wchar_t bgColor[64];
            if (isDark) {
                swprintf_s(bgColor, L"rgba(18, 20, 28, %.2f)", opacity);
            } else {
                swprintf_s(bgColor, L"rgba(244, 245, 247, %.2f)", opacity);
            }
            mainContainer.set_style_attribute("background-color", bgColor);
        }
    }

    // Temporarily disable DWM transitions (animations) to avoid seeing the window jump
    // DWMWA_TRANSITIONS_FORCEDISABLE = 3
    BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(get_hwnd(), 3, &disableTransitions, sizeof(disableTransitions));

    // Expand (show) the window offscreen first.
    // This is REQUIRED so that DWM Composition and Sciter rendering state become active.
    // Without this, the background stays solid black instead of blurred/transparent.
    expand();

    // Finally, size and move the initialized window onscreen (it was created at -10000, -10000)
    int winWidth = ScaleHelper::scaleForWindow(config_.parentHwnd, config_.baseWidth, SubDialogWin10Pad());
    int winHeight = ScaleHelper::scaleForWindow(config_.parentHwnd, config_.baseHeight, SubDialogWin10Pad());

    int posX, posY;
    HMONITOR monitor;
    if (config_.parentHwnd) {
        RECT parentRect;
        GetWindowRect(config_.parentHwnd, &parentRect);
        posX = parentRect.left + (parentRect.right - parentRect.left - winWidth) / 2;
        posY = parentRect.top + (parentRect.bottom - parentRect.top - winHeight) / 2;
        // Use parent's monitor — correct even if parent sits partly off-screen,
        // which the computed center point alone wouldn't capture.
        monitor = MonitorFromWindow(config_.parentHwnd, MONITOR_DEFAULTTONEAREST);
    } else {
        // No parent — center on primary monitor's work area (honors taskbar).
        POINT pt = NextKey::GetCenteredPos(nullptr, winWidth, winHeight);
        posX = pt.x;
        posY = pt.y;
        POINT center{posX + winWidth / 2, posY + winHeight / 2};
        monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    }

    // Clamp to monitor work area so title bar never falls off-screen (issue #102).
    // MONITOR_DEFAULTTONEAREST is documented to never return NULL, so no fallback.
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    GetMonitorInfoW(monitor, &monitorInfo);
    const RECT& workArea = monitorInfo.rcWork;

    if (posX + winWidth > workArea.right)   posX = workArea.right - winWidth;
    if (posY + winHeight > workArea.bottom) posY = workArea.bottom - winHeight;
    // Top/left clamps come last so that when HWND is taller/wider than the work
    // area, the top-left stays visible and content overflows the bottom/right
    // (which is recoverable via scroll) rather than the title bar (which is not).
    if (posX < workArea.left) posX = workArea.left;
    if (posY < workArea.top)  posY = workArea.top;

    bool isTopmost = config_.topmost;
    if (config_.parentHwnd && IsWindow(config_.parentHwnd)) {
        // Inherit TOPMOST only if parent window is currently TOPMOST (e.g. Settings pinned)
        LONG_PTR exStyle = GetWindowLongPtrW(config_.parentHwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOPMOST) {
            isTopmost = true;
        }
    }

    SetWindowPos(get_hwnd(), isTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 posX, posY, winWidth, winHeight, SWP_SHOWWINDOW);
    SetForegroundWindow(get_hwnd());

    // Force taskbar presence while DWM transitions are still disabled (avoids flicker)
    SciterHelper::ForceTaskbarPresence(get_hwnd(), IDI_APP);

    // Re-enable DWM transitions
    disableTransitions = FALSE;
    DwmSetWindowAttribute(get_hwnd(), 3, &disableTransitions, sizeof(disableTransitions));
}

SciterSubDialog::~SciterSubDialog() {
    s_instance = nullptr;
}

void SciterSubDialog::Show() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);

        if (!IsWindow(get_hwnd())) break;
    }
}

LRESULT SciterSubDialog::on_load_data(LPSCN_LOAD_DATA pnmld) {
    aux::wchars uri = aux::chars_of(pnmld->uri);
    if (!uri.like(WSTR("this://app/*"))) {
        return LOAD_OK;
    }

    const wchar_t* relativePath = pnmld->uri + 11;

#ifdef SCITER_USE_PACKFOLDER
    aux::bytes data = sciter::archive::instance().get(relativePath);
    if (data.length) {
        ::SciterDataReady(pnmld->hwnd, pnmld->uri, data.start, UINT(data.length));
        return LOAD_OK;
    }
    return LOAD_DISCARD;
#else
    if (uiBasePath_.empty()) {
        return LOAD_DISCARD;
    }

    std::wstring filePath = uiBasePath_ + relativePath;
    for (wchar_t& c : filePath) {
        if (c == L'/') c = L'\\';
    }

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return LOAD_DISCARD;
    }

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        CloseHandle(hFile);
        return LOAD_DISCARD;
    }

    std::vector<BYTE> buffer(fileSize);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buffer.data(), fileSize, &bytesRead, nullptr) || bytesRead != fileSize) {
        CloseHandle(hFile);
        return LOAD_DISCARD;
    }
    CloseHandle(hFile);

    ::SciterDataReady(pnmld->hwnd, pnmld->uri, buffer.data(), fileSize);
    return LOAD_OK;
#endif
}

LRESULT CALLBACK SciterSubDialog::SubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {

    UNREFERENCED_PARAMETER(uIdSubclass);
    UNREFERENCED_PARAMETER(dwRefData);

    // Prevent Sciter from re-adding WS_EX_TOOLWINDOW (hides window from taskbar).
    // Modifies styleNew in-place, then falls through so other style changes propagate.
    if (msg == WM_STYLECHANGING) {
        (void)SciterHelper::GuardTaskbarStyle(wParam, lParam);
    }

    // Handle window restore from minimized: force Sciter to repaint (#258)
    if (s_instance
        && SciterHelper::ConsumeRestoreFromMinimized(msg, wParam, s_instance->wasMinimized_)) {
        SciterHelper::RepaintAfterRestore(hwnd);
    }

    if (msg == WM_CLOSE) {
        if (s_instance) {
            s_instance->onBeforeClose();
            HandoffFocusToOwner(s_instance->config_.parentHwnd);
        }
        ShowWindow(hwnd, SW_HIDE);
        RemoveWindowSubclass(hwnd, SubclassProc, 1);
        ExitProcess(0);
    }

    if (msg == WM_DESTROY) {
        RemoveWindowSubclass(hwnd, SubclassProc, 1);
        return 0;
    }

    // Real-time theme switch (like OpenKey — pure DOM manipulation, no call_function)
    if (msg == WM_SETTINGCHANGE && lParam) {
        if (wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
            if (s_instance) {
                auto sysConfig = ConfigManager::LoadSystemConfigOrDefault();
                if (sysConfig.forceLightTheme) return 0;
                bool dark = DarkModeHelper::IsWindowsDarkMode();
                DarkModeHelper::SetWindowDarkMode(hwnd, dark);

                // Toggle body.dark class (get_root() = <html>, need <body>)
                sciter::dom::element htmlRoot(s_instance->get_root());
                sciter::dom::element body = htmlRoot.find_first("body");
                if (body.is_valid()) {
                    body.set_attribute("class", DarkModeHelper::BuildBodyClasses(dark).c_str());
                }

                // Update container background opacity for new theme
                if (s_instance->config_.applyBackgroundOpacity) {
                    auto uiConfig = ConfigManager::LoadUIConfigOrDefault();
                    double opacity = uiConfig.backgroundOpacity / 100.0;
                    sciter::dom::element root = s_instance->get_root();
                    sciter::dom::element container = root.find_first("#main-container");
                    if (container.is_valid()) {
                        wchar_t bgColor[64];
                        if (dark) {
                            swprintf_s(bgColor, L"rgba(18, 20, 28, %.2f)", opacity);
                        } else {
                            swprintf_s(bgColor, L"rgba(244, 245, 247, %.2f)", opacity);
                        }
                        container.set_style_attribute("background-color", bgColor);
                    }
                }
            }
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    // Window dragging via title bar
    // HTLEFT (10) to HTBOTTOMRIGHT (17) are sizing borders
    if (msg == WM_NCHITTEST) {
        LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);

        // ALWAYS check drag zone FIRST to override OS invisible resize borders (e.g. HTTOP)
        if (s_instance) {
            LRESULT dragResult = SciterHelper::handleWindowDrag(hwnd, lParam,
                s_instance->config_.titleBarHeight, s_instance->config_.buttonsWidth);
            if (dragResult == HTCAPTION) {
                return HTCAPTION;
            }
        }

        // Prevent window from being resized by user
        if (result >= HTLEFT && result <= HTBOTTOMRIGHT) {
            return HTBORDER;
        }

        return result;
    }

    // Suppress the default Windows non-client border redraw when the window loses focus
    // Returning TRUE without passing to DefSubclassProc stops Windows from drawing the white inactive border
    if (msg == WM_NCACTIVATE) {
        return TRUE;
    }

    // Let subclass handle custom messages
    if (s_instance) {
        LRESULT customResult = s_instance->onCustomMessage(hwnd, msg, wParam, lParam);
        if (customResult != -1) {
            return customResult;
        }
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

}  // namespace NextKey
