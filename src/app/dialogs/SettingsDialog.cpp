// VKey - Settings Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "SettingsDialog.h"
#include "../resource.h"
#include "helpers/AppHelpers.h"
#include "system/StartupHelper.h"
#include "system/SubprocessHelper.h"
#include "system/TsfRegistration.h"
#if defined(VKEY_USE_RUST_ENGINE)
#include "system/AdvancedEngineInstaller.h"
#endif
#include "system/UpdateChecker.h"
#include "system/PendingDllApply.h"
#include "system/ToastPopup.h"
#include "core/Version.h"
#include "core/WinStrings.h"
#include "sciter/ScaleHelper.h"
#include "sciter/SciterHelper.h"
#include "system/DarkModeHelper.h"
#include "system/DebugLogWarning.h"
#include "core/config/ConfigManager.h"
#include "core/Strings.h"
#include "core/Debug.h"
#include "core/UIConfig.h"
#include "core/ipc/SharedConstants.h"
#include "sciter-x-dom.hpp"
#include "sciter-x-host-callback.h"
#include <dwmapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <memory>
#include <vector>
#include <algorithm>
#include <thread>

using namespace sciter::dom;  // For ELEMENT_AREAS enum (CONTENT_BOX, etc.)

#pragma comment(lib, "comctl32.lib")

namespace NextKey {

// Timer IDs for async operations
static constexpr UINT_PTR TIMER_RESIZE_WINDOW = 1001;

// Private self-posted message for coalescing rapid V/E mode changes
static constexpr UINT WM_SETTINGS_MODE_SYNC = WM_APP + 1;
// Finish startup outside Sciter's DOCUMENT_READY callback and the constructor.
static constexpr UINT WM_SETTINGS_INITIALIZE = WM_APP + 2;

// DWM constants for dark mode
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Helper: Convert wide string to narrow string (ASCII subset only, for CSS selectors)
static std::string toNarrowString(const std::wstring& wide) {
    std::string result;
    result.reserve(wide.size());
    for (wchar_t wc : wide) {
        result.push_back(static_cast<char>(wc & 0x7F));  // ASCII only
    }
    return result;
}

// Base window dimensions (before DPI scaling)
constexpr int BASE_WIDTH_COLLAPSED = 350;
constexpr int BASE_HEIGHT_COLLAPSED = 460;  // Match OpenKey height

// Static dialog instance for SubclassProc access
static SettingsDialog* s_instance = nullptr;

SettingsDialog::SettingsDialog()
    : sciter::window(SW_MAIN, RECT{-10000, -10000, -10000 + BASE_WIDTH_COLLAPSED, -10000 + BASE_HEIGHT_COLLAPSED}) {

    s_instance = this;

    // Load current settings first
    loadSettings();

    // Open IPC handles once (reused for every toggle save)
    (void)sharedState_.OpenReadWrite();
    (void)configEvent_.Initialize();

    // 0. Set up UI base path for Debug mode (file system loading)
#ifndef SCITER_USE_PACKFOLDER
    wchar_t exeDir[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH) != 0) {
        wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
        if (lastSlash) *lastSlash = L'\0';
        uiBasePath_ = exeDir;
        uiBasePath_ += L"\\ui\\";
    }
#endif

    // Install the native handlers before load(), which can deliver document
    // events synchronously. Keep the window offscreen until DOCUMENT_READY.
    SetWindowSubclass(get_hwnd(), SubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetWindowTextW(get_hwnd(), L"VKey Settings");
    BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(get_hwnd(), 3, &disableTransitions, sizeof(disableTransitions));

    // Transparency must be enabled before loading the document.
    SciterSetOption(get_hwnd(), SCITER_TRANSPARENT_WINDOW, 1);

    // 2. Load HTML using this://app/ scheme (works for both Debug and Release)
    // In Debug: on_load_data() intercepts and loads from file system
    // In Release: on_load_data() loads from packed archive
    if (!load(WSTR("this://app/settings/settings.html"))) {
        NEXTKEY_LOG(L"Failed to load settings.html");
        PostMessageW(get_hwnd(), WM_CLOSE, 0, 0);
    }
}

void SettingsDialog::applyDocumentSettings() {
    sciter::dom::element root = get_root();
    if (!root.is_valid()) return;

    root.set_attribute("lang", GetLanguage() == Language::English ? L"en" : L"vi");
    sciter::dom::element body = root.find_first("body");
    if (body.is_valid()) {
        const bool dark = !forceLightTheme_ && DarkModeHelper::IsWindowsDarkMode();
        body.set_attribute("class", DarkModeHelper::BuildBodyClasses(dark).c_str());
    }
}

void SettingsDialog::initializeWindow() {
    if (uiInitialized_) return;
    HWND hwnd = get_hwnd();
    if (!hwnd) return;
    sciter::dom::element root = get_root();
    if (!root.is_valid() || !root.find_first("#main-container")) {
        NEXTKEY_LOG(L"Settings document ready without its main container");
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return;
    }
    uiInitialized_ = true;

    applyDocumentSettings();
    initializeUI();
    // initializeUI schedules a resize for later config refreshes. Startup sizes
    // the complete document here before bringing the window onscreen.
    KillTimer(hwnd, TIMER_RESIZE_WINDOW);

    const bool dark = !forceLightTheme_ && DarkModeHelper::IsWindowsDarkMode();
    DarkModeHelper::SetWindowDarkMode(hwnd, dark);

    // An offscreen show activates DWM/Sciter rendering before measuring content.
    expand();
    recalcWindowSize();
    SciterHelper::enableWindowBlur(hwnd, BlurMode::Blur);

    // Reveal only the initialized document at its measured size.
    RECT rc;
    GetWindowRect(get_hwnd(), &rc);
    int winWidth = rc.right - rc.left;
    int winHeight = rc.bottom - rc.top;
    POINT pt = NextKey::GetCenteredPos(get_hwnd(), winWidth, winHeight);
    // initializeUI() already applied the saved pin state. Centering must keep
    // that z-order, otherwise reopening leaves a yellow icon on an unpinned HWND.
    SetWindowPos(get_hwnd(), nullptr, pt.x, pt.y, 0, 0,
                 SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOZORDER);
    SetForegroundWindow(get_hwnd());

    // Force taskbar presence while DWM transitions are still disabled (avoids flicker)
    SciterHelper::ForceTaskbarPresence(get_hwnd(), IDI_APP);

    // Re-enable DWM transitions for subsequent show/hide animations
    BOOL disableTransitions = FALSE;
    DwmSetWindowAttribute(get_hwnd(), 3, &disableTransitions, sizeof(disableTransitions));
}

SettingsDialog::~SettingsDialog() {
    s_instance = nullptr;
}

// Custom resource loading handler - loads from file system in Debug, archive in Release
LRESULT SettingsDialog::on_load_data(LPSCN_LOAD_DATA pnmld) {
    // Check if this is a this://app/ URL
    aux::wchars uri = aux::chars_of(pnmld->uri);
    if (!uri.like(WSTR("this://app/*"))) {
        // Not our URL scheme, let Sciter handle it
        return LOAD_OK;
    }

    // Extract the relative path (skip "this://app/")
    const wchar_t* relativePath = pnmld->uri + 11;  // strlen("this://app/") = 11

#ifdef SCITER_USE_PACKFOLDER
    // Release: Load from packed archive
    aux::bytes data = sciter::archive::instance().get(relativePath);
    if (data.length) {
        ::SciterDataReady(pnmld->hwnd, pnmld->uri, data.start, UINT(data.length));
        return LOAD_OK;
    }
    OutputDebugStringW(L"VKey: Failed to load from archive: ");
    OutputDebugStringW(pnmld->uri);
    OutputDebugStringW(L"\n");
    return LOAD_DISCARD;
#else
    // Debug: Load from file system
    if (uiBasePath_.empty()) {
        OutputDebugStringW(L"VKey: UI base path not set\n");
        return LOAD_DISCARD;
    }

    // Build full file path (convert URL forward slashes to Windows backslashes)
    std::wstring filePath = uiBasePath_ + relativePath;
    for (wchar_t& c : filePath) {
        if (c == L'/') c = L'\\';
    }

    // Open and read the file
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        OutputDebugStringW(L"VKey: Failed to open: ");
        OutputDebugStringW(filePath.c_str());
        OutputDebugStringW(L"\n");
        return LOAD_DISCARD;
    }

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        CloseHandle(hFile);
        return LOAD_DISCARD;
    }

    // Read file content
    std::vector<BYTE> buffer(fileSize);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buffer.data(), fileSize, &bytesRead, nullptr) || bytesRead != fileSize) {
        CloseHandle(hFile);
        return LOAD_DISCARD;
    }
    CloseHandle(hFile);

    // Provide data to Sciter
    ::SciterDataReady(pnmld->hwnd, pnmld->uri, buffer.data(), fileSize);
    return LOAD_OK;
#endif
}

LRESULT CALLBACK SettingsDialog::SubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {

    UNREFERENCED_PARAMETER(uIdSubclass);
    UNREFERENCED_PARAMETER(dwRefData);

    // Prevent Sciter from re-adding WS_EX_TOOLWINDOW (hides window from taskbar).
    // Modifies styleNew in-place, then falls through so other style changes propagate.
    if (msg == WM_STYLECHANGING) {
        (void)SciterHelper::GuardTaskbarStyle(wParam, lParam);
    }

    if (msg == WM_SETTINGS_INITIALIZE) {
        if (s_instance) s_instance->initializeWindow();
        return 0;
    }

    // WM_CLOSE: terminate child subdialogs, flush deferred save, then destroy & exit
    // (MUST use ExitProcess — avoids Sciter assertion failures during normal teardown)
    if (msg == WM_CLOSE) {
        // Terminate any subdialogs we spawned (they run as separate processes)
        TerminateAllSubprocesses();

        if (s_instance && s_instance->configDirty_) {
            KillTimer(hwnd, TIMER_DEFERRED_SAVE);
            s_instance->saveToToml();
        }
        ShowWindow(hwnd, SW_HIDE);
        DestroyWindow(hwnd);
        ExitProcess(0);
        return 0;
    }

    if (msg == WM_DESTROY) {
        RemoveWindowSubclass(hwnd, SubclassProc, 1);
        return 0;
    }

    // Handle V/E mode change notification from main process.
    // Coalesce rapid messages: update the flag immediately but defer the
    // DOM update so N rapid tray clicks produce one toggle repaint, not N.
    if (msg == WM_VKEY_MODE_CHANGED) {
        if (s_instance) {
            s_instance->vietnameseMode_ = (wParam != 0);
            if (!s_instance->modeSyncPending_) {
                s_instance->modeSyncPending_ = true;
                PostMessageW(hwnd, WM_SETTINGS_MODE_SYNC, 0, 0);
            }
        }
        return 0;
    }

    // Deferred toggle update: runs after all queued MODE_CHANGED are consumed.
    if (msg == WM_SETTINGS_MODE_SYNC) {
        if (s_instance) {
            s_instance->modeSyncPending_ = false;
            // CSS: checked=E, unchecked=V — invert for correct display
            s_instance->setToggleState(L"toggle-language", !s_instance->vietnameseMode_);
            // Force repaint — Sciter only repaints on mouse events by default,
            // so the DOM change is invisible until the cursor moves over the window.
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    // Handle config changed from tray menu — reload TOML and refresh UI
    if (msg == WM_VKEY_CONFIG_CHANGED) {
        if (s_instance) {
            s_instance->loadSettings();
            if (s_instance->uiInitialized_) s_instance->initializeUI();
        }
        return 0;
    }

    // The icon sub-dialog writes only SystemConfig. Refresh the cached value
    // without reinitializing the whole main dialog: programmatic control
    // updates can otherwise emit VALUE_CHANGED and mutate unrelated options.
    if (msg == WM_VKEY_ICON_SETTINGS_CHANGED) {
        if (s_instance) {
            s_instance->systemConfig_ = ConfigManager::LoadSystemConfigOrDefault();
        }
        return 0;
    }

    // Handle window restore from minimized: force Sciter to repaint (#258)
    if (s_instance
        && SciterHelper::ConsumeRestoreFromMinimized(msg, wParam, s_instance->wasMinimized_)) {
        SciterHelper::RepaintAfterRestore(hwnd);
    }

    // Handle timer for window resize after CSS transition
    if (msg == WM_TIMER && wParam == TIMER_RESIZE_WINDOW) {
        KillTimer(hwnd, TIMER_RESIZE_WINDOW);
        if (s_instance) {
            s_instance->recalcWindowSize();
        }
        return 0;
    }

    // Handle deferred TOML save timer (30s after last settings change)
    if (msg == WM_TIMER && wParam == TIMER_DEFERRED_SAVE) {
        KillTimer(hwnd, TIMER_DEFERRED_SAVE);
        if (s_instance && s_instance->configDirty_) {
            s_instance->saveToToml();
        }
        return 0;
    }

    // Helper to flush dirty settings before launching a subprocess
    auto flushBeforeSpawn = [&]() {
        if (s_instance && s_instance->configDirty_) {
            KillTimer(hwnd, TIMER_DEFERRED_SAVE);
            s_instance->saveToToml();
        }
    };

    // Deferred: open excluded apps dialog as subprocess
    // (must be a separate process — Sciter SOM assertion fires if a sciter::window
    //  is destroyed while the Sciter runtime is still active in this process)
    if (msg == WM_VKEY_OPEN_EXCLUDED) {
        flushBeforeSpawn();
        SpawnSubprocess(L"VKey - Excluded Apps", L"--excludedapps");
        return 0;
    }

    if (msg == WM_VKEY_OPEN_TSFAPPS) {
        flushBeforeSpawn();
        SpawnSubprocess(L"VKey - TSF Apps", L"--tsfapps");
        return 0;
    }

    if (msg == WM_VKEY_OPEN_MACRO) {
        flushBeforeSpawn();
        SpawnSubprocess(L"VKey - Macro Table", L"--macro");
        return 0;
    }

    if (msg == WM_VKEY_OPEN_APPOVERRIDES) {
        flushBeforeSpawn();
        SpawnSubprocess(L"VKey - App Overrides", L"--appoverrides");
        return 0;
    }

    if (msg == WM_VKEY_OPEN_SPELLEXCL) {
        flushBeforeSpawn();
        SpawnSubprocess(L"VKey - Spell Exclusions", L"--spellexclusions");
        return 0;
    }

    if (msg == WM_VKEY_OPEN_USERDEFINED) {
        flushBeforeSpawn();
        SpawnSubprocess(L"VKey - User Defined Input", L"--userdefined");
        return 0;
    }

    if (msg == WM_VKEY_OPEN_HOTKEYS) {
        flushBeforeSpawn();
        SpawnSubprocess(L"VKey - Phím tắt", L"--hotkeys");
        return 0;
    }

    if (msg == WM_VKEY_OPEN_ICON_SETTINGS) {
        flushBeforeSpawn();
        SpawnSubprocess(L"VKey - Tùy chỉnh icon", L"--icon-settings");
        return 0;
    }

    // Real-time theme switch: Windows broadcasts this when user changes theme
    if (msg == WM_SETTINGCHANGE && lParam) {
        if (wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
            if (s_instance) {
                if (s_instance->forceLightTheme_) return 0;
                bool dark = DarkModeHelper::IsWindowsDarkMode();
                DarkModeHelper::SetWindowDarkMode(hwnd, dark);

                // Toggle body.dark class (get_root() = <html>, need <body>)
                sciter::dom::element htmlRoot(s_instance->get_root());
                sciter::dom::element body = htmlRoot.find_first("body");
                if (body.is_valid()) {
                    body.set_attribute("class", DarkModeHelper::BuildBodyClasses(dark).c_str());
                }

                // Update container background opacity for new theme
                sciter::dom::element container = htmlRoot.find_first("#main-container");
                if (container.is_valid()) {
                    double opacity = s_instance->backgroundOpacity_ / 100.0;
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
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    // Window dragging via title bar area
    // HTLEFT (10) to HTBOTTOMRIGHT (17) are sizing borders
    if (msg == WM_NCHITTEST) {
        LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);

        // ALWAYS check drag zone FIRST to override OS invisible resize borders (e.g. HTTOP)
        // This fixes the "dead zone" at the top edge on Windows 10
        LRESULT dragResult = SciterHelper::handleWindowDrag(hwnd, lParam, 36, 70);
        if (dragResult == HTCAPTION) {
            return HTCAPTION;
        }

        // Prevent window from being resized by the user dragging the borders
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

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void SettingsDialog::SetVietnameseMode(bool vietnamese) {
    vietnameseMode_ = vietnamese;
    // Update toggle UI if window already exists (post-construction call)
    // CSS: checked=E, unchecked=V — invert for correct display
    if (get_hwnd()) {
        setToggleState(L"toggle-language", !vietnamese);
    }
}

void SettingsDialog::Show() {
    // Sciter's loop advances deferred mouse/idle events as well as Win32
    // messages. GetMessageW alone leaves native hover tooltips pending.
    // WM_CLOSE still flushes settings and exits this subprocess explicitly.
    sciter::application::run();
}

bool SettingsDialog::handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) {
    UNREFERENCED_PARAMETER(he);

    if (params.cmd == DOCUMENT_PARSED && params.heTarget == get_root()) {
        // Preserve Sciter's own document handling, then set language/theme for
        // the JavaScript ready handlers (translations and blur tint).
        const bool handled = sciter::window::handle_event(he, params);
        applyDocumentSettings();
        return handled;
    }
    if (params.cmd == DOCUMENT_READY && params.heTarget == get_root()) {
        if (!uiInitialized_) PostMessageW(get_hwnd(), WM_SETTINGS_INITIALIZE, 0, 0);
        return sciter::window::handle_event(he, params);
    }

    // Handle BUTTON_CLICK events
    if (params.cmd == BUTTON_CLICK) {
        sciter::dom::element el(params.heTarget);
        std::wstring id = el.get_attribute("id");

        if (!id.empty()) {
            // Handle close button
            if (id == L"btn-close") {
                onClose();
                return true;
            }
            // Handle pin button
            if (id == L"btn-pin") {
                togglePin();
                return true;
            }
            // Handle other buttons
            handleButtonClick(id);
            return true;
        }
    }

    // Handle VALUE_CHANGED events (includes behavior:check native toggles)
    // behavior:check fires "change" which maps to VALUE_CHANGED
    else if (params.cmd == VALUE_CHANGED) {
        sciter::dom::element el(params.heTarget);
        std::wstring id = el.get_attribute("id");

        if (id.empty()) return sciter::window::handle_event(he, params);

        // Handle expand state change (from val-expand-state hidden input)
        if (id == L"val-expand-state") {
            sciter::value val = el.get_value();
            std::wstring strVal = val.is_string() ? val.get<std::wstring>() : L"0";
            isExpanded_ = (strVal == L"1");
            saveUISettings();
            SetTimer(get_hwnd(), TIMER_RESIZE_WINDOW, 10, NULL);
            return true;
        }

        // Handle background opacity change
        if (id == L"val-bg-opacity") {
            sciter::value val = el.get_value();
            int opacity = 80;
            if (val.is_int()) opacity = val.get<int>();
            else if (val.is_string()) opacity = _wtoi(val.get<std::wstring>().c_str());

            backgroundOpacity_ = static_cast<uint8_t>(std::clamp(opacity, 0, 100));
            saveUISettings();
            return true;
        }

        // Handle dropdown changes
        if (id == L"input-type" || id == L"bang-ma" || id == L"spell-check-level" || id == L"startup-mode" || id == L"temp-off-openkey") {
            sciter::value val = el.get_value();
            int intValue = 0;
            if (val.is_int()) intValue = val.get<int>();
            else if (val.is_string()) intValue = _wtoi(val.get<std::wstring>().c_str());
            handleDropdownChange(id, intValue);
            return true;
        }

        // Handle switch key character input
        if (id == L"switch-key-char") {
            sciter::value val = el.get_value();
            if (val.is_string()) {
                std::wstring raw = val.get<std::wstring>();
                // JS displays space as "Space" — convert back to actual space char
                if (raw == L"Space" || raw == L"space") {
                    switchKeyChar_ = L" ";
                } else {
                    switchKeyChar_ = raw;
                }

                // Update hotkeyConfig_.vk immediately so saveSettings() and syncToSharedState() see it
                if (switchKeyChar_.empty()) {
                    hotkeyConfig_.vk = 0;
                } else if (switchKeyChar_ == L" ") {
                    hotkeyConfig_.vk = 0x20;  // VK_SPACE
                } else {
                    wchar_t c = switchKeyChar_[0];
                    SHORT scan = VkKeyScanW(c);
                    if (scan != -1) {
                        hotkeyConfig_.vk = static_cast<uint32_t>(LOBYTE(scan));
                    } else {
                        hotkeyConfig_.vk = 0;
                    }
                }

                saveSettings();
            }
            return true;
        }

        // Handle toggle changes (val-* hidden inputs fired by JS)
        if (id.find(L"val-") == 0) {
            std::wstring settingId = id.substr(4);  // Remove "val-" prefix
            sciter::value val = el.get_value();
            std::wstring strVal = val.is_string() ? val.get<std::wstring>() : L"0";
            bool boolValue = (strVal == L"1");
            handleToggleChange(settingId, boolValue);
            return true;
        }
    }

    // Call base class for default handling (e.g., DOCUMENT_PARSED)
    return sciter::window::handle_event(he, params);
}

void SettingsDialog::handleToggleChange(const std::wstring& id, bool value) {
    // Re-entrancy guard (see SettingsDialog.h::handlingToggle_). Branches below may open
    // a modal MessageBox / UAC prompt whose nested message loop re-delivers this same
    // VALUE_CHANGED event into handle_event → here, which would cascade duplicate dialogs
    // (debug-log warning, TSF register box). Drop the re-entrant call. The RAII reset
    // covers every early-return path below.
    if (handlingToggle_) return;
    handlingToggle_ = true;
    struct ToggleGuard {
        bool& flag;
        ~ToggleGuard() { flag = false; }
    } toggleGuard{handlingToggle_};

    // Map toggle IDs to settings
    if (id == L"toggle-language") {
        // V/E toggle: send to main process via cross-process message
        // CSS: checked=E (thumb right), unchecked=V (thumb left) — invert
        vietnameseMode_ = !value;
        HWND trayWnd = FindWindowW(L"VKeyTrayClass", nullptr);
        if (trayWnd) {
            PostMessageW(trayWnd, WM_VKEY_SET_MODE, vietnameseMode_ ? 1 : 0, 0);
        }
        return;  // V/E mode is not a persistent config setting
    }
    else if (id == L"beep-sound") {
        config_.beepOnSwitch = value;
    }
    else if (id == L"smart-switch") {
        config_.smartSwitch = value;
    }
    else if (id == L"exclude-apps") {
        config_.excludeApps = value;
    }
    else if (id == L"tsf-apps") {
        config_.tsfApps = value;
        call_function("updateTsfChildren", sciter::value(value));
        // Register/unregister TSF DLL when toggle changes
        // Always attempt full registration (not guarded by IsTsfRegistered) because
        // a previous partial failure could leave CLSID in registry but TIP profile missing.
        if (value) {
            bool ok = RegisterTsf();
            if (!ok) {
                ok = RegisterTsfElevated();
            }
            if (!ok || !IsTsfRegistered()) {
                config_.tsfApps = false;
                setToggleState(L"tsf-apps", false);
                call_function("updateTsfChildren", sciter::value(false));
                MessageBoxW(get_hwnd(),
                    L"Không thể đăng ký TSF.\nVui lòng chạy với quyền Administrator.",
                    L"VKey", MB_OK | MB_ICONWARNING);
                return;  // Don't save broken state
            }
            // #109: registration only makes the TIP *available*. Add it to the
            // user's input list (InstallLayoutOrTip) and select it now so enabling
            // TSF works immediately, without a restart. Idempotent + throttled.
            ActivateVKeyTsfProfile();
            MessageBoxW(get_hwnd(), S(StringId::TSF_REGISTER_SUCCESS),
                L"VKey", MB_OK | MB_ICONINFORMATION);
        } else {
            if (IsTsfRegistered()) {
                UnregisterTsf();
                // Verify actual state — DllUnregisterServer may return S_OK
                // even when it can't delete HKLM keys without admin
                if (IsTsfRegistered()) {
                    UnregisterTsfElevated();
                }
                if (IsTsfRegistered()) {
                    config_.tsfApps = true;
                    setToggleState(L"tsf-apps", true);
                    call_function("updateTsfChildren", sciter::value(true));
                    MessageBoxW(get_hwnd(), S(StringId::TSF_UNREGISTER_FAILED),
                        L"VKey", MB_OK | MB_ICONWARNING);
                    return;  // Don't save broken state
                }
                MessageBoxW(get_hwnd(), S(StringId::TSF_UNREGISTER_SUCCESS),
                    L"VKey", MB_OK | MB_ICONINFORMATION);
            }
        }
        // #253: tsfApps is NOT in EncodeFeatureFlags, so syncToSharedState()
        // cannot carry it — HookEngine only ever learns it from TOML via
        // ReloadFromToml. Leaving the write to the 30s deferred timer keeps TSF
        // inert that whole time (log 2026-09-10: toggle ~15:15:18, reload at
        // 15:15:35 still read tsf_feature=0, it flipped to 1 only at 15:16:18 —
        // Edge was focused in between with no "T" and the hook handling keys).
        // ClassicSettingsDialog.cpp already flushes here for the same reason.
        saveSettings();
        KillTimer(get_hwnd(), TIMER_DEFERRED_SAVE);
        saveToToml();
        if (onSettingsChanged_) onSettingsChanged_();
        return;
    }
    else if (id == L"hide-preedit-underline") {
        config_.hidePreeditUnderline = value;
    }
    else if (id == L"key-ctrl") {
        hotkeyConfig_.ctrl = value;
    }
    else if (id == L"key-alt") {
        hotkeyConfig_.alt = value;
    }
    else if (id == L"key-win") {
        hotkeyConfig_.win = value;
    }
    else if (id == L"key-shift") {
        hotkeyConfig_.shift = value;
    }
    else if (id == L"modern-ortho") {
        config_.modernOrtho = value;
    }
    else if (id == L"auto-caps") {
        config_.autoCaps = value;
    }
    else if (id == L"allow-zwjf") {
        config_.allowZwjf = value;
    }
    else if (id == L"restore-key") {
        config_.autoRestoreEnabled = value;
    }
    else if (id == L"cjk-auto-switch") {
        config_.cjkAutoSwitch = value;
    }
    else if (id == L"debug-log") {
        // Security gate: confirm with user before enabling. Disabling needs
        // no prompt — that always reduces exposure.
        if (value && !config_.debugLogEnabled) {
            bool englishUi = (systemConfig_.language == 1);
            if (!::NextKey::ShowDebugLogWarning(get_hwnd(), englishUi)) {
                setToggleState(L"debug-log", false);  // revert UI
                return;
            }
        }
        config_.debugLogEnabled = value;
    }
    else if (id == L"enable-toast") {
        config_.enableToast = value;
        sciter::dom::element root = get_root();
        sciter::dom::element body = root.find_first("body");
        if (body.is_valid()) {
            body.set_attribute("data-enable-toast", value ? L"true" : L"false");
        }
    }
    else if (id == L"allow-english-bypass") {
        config_.allowEnglishBypass = value;
    }
    else if (id == L"suggest-keep-chars") {
        config_.suggestKeepChars = value;
    }
    else if (id == L"use-macro") {
        config_.macroEnabled = value;
        call_function("updateMacroChildren", sciter::value(value));
    }
    else if (id == L"macro-english") {
        config_.macroInEnglish = value;
    }
    else if (id == L"quick-telex") {
        config_.quickConsonant = value;
    }
    else if (id == L"quick-start") {
        config_.quickStartConsonant = value;
    }
    else if (id == L"quick-end") {
        config_.quickEndConsonant = value;
    }
    else if (id == L"auto-caps-macro") {
        config_.autoCapsMacro = value;
        call_function("updateMacroCapsChildren", sciter::value(value));
    }
    else if (id == L"macro-title-case") {
        config_.macroTitleCase = value;
    }
    else if (id == L"macro-undo-backspace") {
        config_.macroUndoOnBackspace = value;
    }
    // Show-advanced toggle: UI-only setting (expand/collapse panel)
    else if (id == L"show-advanced") {
        isExpanded_ = value;
        saveUISettings();
        SetTimer(get_hwnd(), TIMER_RESIZE_WINDOW, 10, NULL);
        return;
    }
    // System settings — immediate save, no deferred/SharedState
    else if (id == L"run-startup") {
        systemConfig_.runAtStartup = value;
        RegisterRunOnStartup(value, systemConfig_.runAsAdmin);
        saveSystemSettings();
        return;
    }
    else if (id == L"run-admin") {
        systemConfig_.runAsAdmin = value;

        // Clean up startup mechanisms from the current user's context 
        // to ensure the exact user's HKCU or Scheduled Task is targeted.
        if (value) {
            // Enabling admin: remove registry key immediately. The new elevated
            // instance will create the task in EnsureStartupRegistration().
            RemoveRegistryStartup();
        } else {
            // Disabling admin: remove elevated task and fall back to registry.
            // (RemoveScheduledTask requires UAC, which is correct).
            if (!RemoveScheduledTask()) {
                // UAC denied or removal failed — revert toggle to match reality
                systemConfig_.runAsAdmin = true;
                setToggleState(L"run-admin", true);
                return;
            }
            if (systemConfig_.runAtStartup) {
                (void)SetRegistryStartup();
            }
        }

        saveSystemSettings();
        // Restart main process to apply elevation change.
        // Startup registration (Task Scheduler/Registry) is handled by
        // EnsureStartupRegistration() in the new instance — no double UAC.
        HWND trayWnd = FindWindowW(L"VKeyTrayClass", nullptr);
        if (trayWnd) {
            PostMessageW(trayWnd, WM_VKEY_RESTART, 0, 0);
        }
        return;
    }
    else if (id == L"show-on-startup") {
        systemConfig_.showOnStartup = value;
        saveSystemSettings();
        return;
    }
    else if (id == L"desktop-shortcut") {
        systemConfig_.desktopShortcut = value;
        SetDesktopShortcut(value);
        saveSystemSettings();
        return;
    }
    else if (id == L"english-ui") {
        systemConfig_.language = value ? 1 : 0;
        SetLanguage(value ? Language::English : Language::Vietnamese);
        saveSystemSettings();
        // Translated labels can wrap to a different number of lines, changing
        // which tab is tallest — invalidate the #226 cached height and remeasure.
        cachedMaxAdvancedHeight_ = 0;
        if (isExpanded_) {
            SetTimer(get_hwnd(), TIMER_RESIZE_WINDOW, 50, NULL);
        }
        // JS already switched lang attribute + called applyTranslations()
        // Notify main process to update language for tray menu / toasts
        notifySystemConfigChanged();
        return;
    }
    else if (id == L"force-light-theme") {
        systemConfig_.forceLightTheme = value;
        forceLightTheme_ = value;
        saveSystemSettings();

        // Apply immediately: switch to light theme
        bool dark = !value && DarkModeHelper::IsWindowsDarkMode();
        DarkModeHelper::SetWindowDarkMode(get_hwnd(), dark);

        sciter::dom::element htmlRoot(get_root());
        sciter::dom::element body = htmlRoot.find_first("body");
        if (body.is_valid()) {
            body.set_attribute("class", DarkModeHelper::BuildBodyClasses(dark).c_str());
        }

        // Update container background for new theme
        double opacity = backgroundOpacity_ / 100.0;
        sciter::dom::element container = htmlRoot.find_first("#main-container");
        if (container.is_valid()) {
            wchar_t bgColor[64];
            if (dark) {
                swprintf_s(bgColor, L"rgba(18, 20, 28, %.2f)", opacity);
            } else {
                swprintf_s(bgColor, L"rgba(244, 245, 247, %.2f)", opacity);
            }
            container.set_style_attribute("background-color", bgColor);
        }
        return;
    }
    else if (id == L"check-update") {
        systemConfig_.autoCheckUpdate = value;
        saveSystemSettings();
        return;
    }

    saveSettings();
    if (onSettingsChanged_) onSettingsChanged_();
}

void SettingsDialog::handleDropdownChange(const std::wstring& id, int value) {
    if (id == L"input-type") {
        config_.inputMethod = static_cast<InputMethod>(value);
    }
    else if (id == L"bang-ma") {
        config_.codeTable = static_cast<CodeTable>(value);
    }
    else if (id == L"spell-check-level") {
        SpellCheckLevel oldLevel = config_.GetSpellCheckLevel();
        SpellCheckLevel newLevel = static_cast<SpellCheckLevel>(value);
        const bool enteringAdvanced =
            newLevel == SpellCheckLevel::Advanced && oldLevel != SpellCheckLevel::Advanced;

#if defined(VKEY_USE_RUST_ENGINE)
        if (enteringAdvanced) {
            const AdvancedEngineStatus status = AdvancedEngineInstaller::EnsureInstalledWithUi(get_hwnd());
            if (status != AdvancedEngineStatus::Ready) {
                // oldLevel, not Standard: the guard above allows Off here, and a
                // declined download is not a request to switch spell check on.
                // It also keeps the child toggles correct — setDropdownValue()
                // does not fire the change event that re-runs
                // updateSpellCheckChildren().
                config_.SetSpellCheckLevel(oldLevel);
                setDropdownValue(L"spell-check-level", static_cast<int>(oldLevel));
                saveSettings();
                KillTimer(get_hwnd(), TIMER_DEFERRED_SAVE);
                saveToToml();
                if (status == AdvancedEngineStatus::ManualRequested) {
                    AdvancedEngineInstaller::ShowManualInstallWithUi(get_hwnd());
                }
                if (onSettingsChanged_) onSettingsChanged_();
                return;
            }
        }
#endif

        config_.SetSpellCheckLevel(newLevel);

        if (enteringAdvanced ||
            (oldLevel == SpellCheckLevel::Advanced && newLevel != SpellCheckLevel::Advanced)) {
            int result = MessageBoxW(get_hwnd(),
                S(enteringAdvanced ? StringId::SPELL_ADVANCED_LOAD_APP : StringId::SPELL_ADVANCED_CLOSE_APP),
                L"VKey", MB_YESNO | MB_ICONQUESTION);
            if (result == IDYES) {
                saveSettings();
                // Flush synchronously — saveSettings() only arms the 30s deferred
                // timer, and the app is about to close (racing with the tray's own
                // WM_CLOSE across processes). Waiting for the timer or this window's
                // WM_CLOSE handler risks losing the write if the tray process wins.
                KillTimer(get_hwnd(), TIMER_DEFERRED_SAVE);
                saveToToml();

                wchar_t exePath[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                wchar_t cmdLine[MAX_PATH + 64] = {};
                swprintf_s(cmdLine, L"\"%s\" %s", exePath, ADMIN_RESTART_FLAG);
                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi = {};
                if (CreateProcessW(exePath, cmdLine, nullptr, nullptr, FALSE,
                                   CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                }

                HWND trayWnd = FindWindowW(L"VKeyTrayClass", nullptr);
                if (trayWnd) {
                    PostMessageW(trayWnd, WM_CLOSE, 0, 0);
                }
                onClose();
                return;
            }
        }
    }
    else if (id == L"startup-mode") {
        systemConfig_.startupMode = static_cast<uint8_t>(value);
        saveSystemSettings();
        return;  // System setting, not typing config
    }
    saveSettings();
    if (onSettingsChanged_) onSettingsChanged_();
}

void SettingsDialog::handleButtonClick(const std::wstring& id) {
    if (id == L"btn-excluded-apps") {
        // Defer dialog creation — creating a Sciter window inside handle_event causes reentrancy issues
        PostMessage(get_hwnd(), WM_VKEY_OPEN_EXCLUDED, 0, 0);
        return;
    }
    else if (id == L"btn-tsf-apps") {
        PostMessage(get_hwnd(), WM_VKEY_OPEN_TSFAPPS, 0, 0);
        return;
    }
    else if (id == L"btn-macro-table") {
        PostMessage(get_hwnd(), WM_VKEY_OPEN_MACRO, 0, 0);
        return;
    }
    else if (id == L"btn-app-overrides") {
        PostMessage(get_hwnd(), WM_VKEY_OPEN_APPOVERRIDES, 0, 0);
        return;
    }
    else if (id == L"btn-spell-exclusions") {
        PostMessage(get_hwnd(), WM_VKEY_OPEN_SPELLEXCL, 0, 0);
        return;
    }
    else if (id == L"btn-userdefined") {
        PostMessage(get_hwnd(), WM_VKEY_OPEN_USERDEFINED, 0, 0);
        return;
    }
    else if (id == L"btn-hotkeys") {
        PostMessage(get_hwnd(), WM_VKEY_OPEN_HOTKEYS, 0, 0);
        return;
    }
    else if (id == L"btn-icon-settings") {
        PostMessage(get_hwnd(), WM_VKEY_OPEN_ICON_SETTINGS, 0, 0);
        return;
    }
    else if (id == L"btn-check-update") {
        startUpdateCheck();
        return;
    }
    else if (id == L"btn-report-issue") {
        ShellExecuteW(nullptr, L"open",
            L"https://github.com/phatMT97/VKey/issues",
            nullptr, nullptr, SW_SHOW);
        return;
    }
    else if (id == L"btn-open-log-folder") {
        // Resolve to the same folder the Logger writes to. If the user just
        // enabled the toggle, Logger::GetCurrentLogFolder() returns the
        // resolved path (probes install dir, falls back to %APPDATA%).
        std::wstring folder = ::NextKey::Logger::GetCurrentLogFolder();
        if (folder.empty()) {
            wchar_t buf[MAX_PATH] = {0};
            DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
            if (n > 0) {
                std::wstring exePath(buf);
                size_t slash = exePath.find_last_of(L"\\/");
                if (slash != std::wstring::npos) folder = exePath.substr(0, slash);
            }
        }
        if (!folder.empty()) {
            ShellExecuteW(nullptr, L"open", folder.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
}

void SettingsDialog::notifySubdialogPinState() {
    EnumWindows([](HWND child, LPARAM parent) -> BOOL {
        if (GetPropW(child, VKEY_SUBDIALOG_PARENT_PROP) == reinterpret_cast<HANDLE>(parent)) {
            PostMessageW(child, WM_VKEY_PARENT_PIN_CHANGED, 0, parent);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(get_hwnd()));
}

void SettingsDialog::togglePin() {
    HWND hwnd = get_hwnd();
    if (!hwnd) return;

    isPinned_ = !isPinned_;

    // Set window topmost state
    SetWindowPos(hwnd, isPinned_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    notifySubdialogPinState();

    // Update button visual state
    sciter::dom::element root = get_root();
    sciter::dom::element pinBtn = root.find_first("#btn-pin");
    if (pinBtn.is_valid()) {
        if (isPinned_) {
            pinBtn.set_attribute("class", L"btn-pin pinned");
        } else {
            pinBtn.set_attribute("class", L"btn-pin");
        }
    }

    // Save pinned state
    saveUISettings();
}

void SettingsDialog::recalcWindowSize() {
    HWND hwnd = get_hwnd();
    if (!hwnd) return;

    RECT rc;
    GetWindowRect(hwnd, &rc);

    sciter::dom::element rootEl = get_root();

    // Use per-window DPI (not system DPI) — critical for multi-monitor setups
    // where primary and secondary monitors have different DPI scaling.
    // GetDpiForSystem() returns the primary monitor's DPI, which is wrong when
    // the window lives on a different monitor. GetDpiForWindow() returns the
    // correct value for the monitor the HWND is currently on.
    double dpiScale = ScaleHelper::getDpiScaleForWindow(hwnd);

    // Fixed widths (must match CSS)
    int COMPACT_WIDTH = static_cast<int>(BASE_WIDTH_COLLAPSED * dpiScale);
    int ADVANCED_WIDTH = static_cast<int>(400 * dpiScale);

    // Use the #main-container as the source of truth for total height
    sciter::dom::element container = rootEl.find_first("#main-container");

    int newWidth = COMPACT_WIDTH;
    if (isExpanded_) {
        newWidth = COMPACT_WIDTH + ADVANCED_WIDTH;
    }

    int newHeight = static_cast<int>(BASE_HEIGHT_COLLAPSED * dpiScale);

    if (container.is_valid()) {
        // Collapsing: clear any inline height pinned while expanded BEFORE measuring,
        // otherwise this reads the stale pinned value instead of the natural compact
        // height (Codex review caught this — the old code cleared it only *after*
        // already computing newHeight from the still-pinned measurement).
        if (!isExpanded_) {
            container.set_style_attribute("height", L"");
            rootEl.update(false);
        }

        RECT r = container.get_location_ppx(BORDER_BOX);
        int w = r.right - r.left;
        // #226: window height must not change per active tab (was resizing on every
        // tab click, causing a visible "jump"). Fix it to whichever tab needs the
        // most vertical space instead of the currently active one.
        int h = isExpanded_ ? measureMaxAdvancedContentHeight(dpiScale)
                             : (r.bottom - r.top);

        // Safety fallback: if DOM hasn't fully layed out yet or window is minimized,
        // bounds might be 0 or heavily distorted. We fallback to predefined constants
        // (COMPACT_WIDTH/ADVANCED_WIDTH and BASE_HEIGHT_COLLAPSED) unless > 100px.
        if (w > 100) newWidth = w;
        if (h > 100) newHeight = h;

        // #226 follow-up: .container has `height: auto`, so it still shrinks to fit
        // whichever tab is CURRENTLY shown even though the HWND is now fixed to the
        // tallest tab. On every tab shorter than the max this left the card's own
        // background/rounded corners ending above the window's real bottom edge,
        // exposing transparent/blurred desktop underneath ("bottom lệch"). Pin the
        // container's own height to match so the card always fills the window.
        // (ScaleHelper.h documents that Sciter auto-scales CSS px by window DPI, so
        // "px" — not "dip" — matches every other dimension already set this way in
        // this same stylesheet, e.g. .container.expanded { width: 750px }.)
        if (isExpanded_ && h > 100) {
            wchar_t heightStr[16];
            swprintf_s(heightStr, L"%.2fpx", h / dpiScale);
            container.set_style_attribute("height", heightStr);
        }
    }

    if (!DarkModeHelper::IsWindows11OrGreater()) {
        // Win10 .container has margin on all sides for drop shadow; BORDER_BOX
        // excludes margins so the HWND would clip the shadow. Pad by 2×margin.
        const int pad = static_cast<int>(Win10Frame::FRAME_SIZE_ADD * dpiScale);
        newWidth += pad;
        newHeight += pad;
    }

    // Keep window position, just resize
    SetWindowPos(hwnd, NULL, 0, 0, newWidth, newHeight, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    
    // Force Sciter engine to render a new frame. 
    // Transparent windows ignore Windows WM_PAINT/InvalidateRect, so we must trigger a DOM mutation.
    rootEl.set_attribute("force-paint", L"1");
    rootEl.update(false);
    rootEl.remove_attribute("force-paint");
    rootEl.update(false);
}

// #226: measure #main-container's height with each of the 4 tab panels expanded
// in turn, returning the tallest. Result is cached per DPI scale (get_location_ppx
// returns physical pixels, and this dialog is per-monitor-DPI-aware) — recomputed
// only on the first measurement at a given scale, or after saveSettings()-triggered
// cache invalidation (e.g. language switch, see "english-ui" in handleToggleChange).
int SettingsDialog::measureMaxAdvancedContentHeight(double dpiScale) {
    if (cachedMaxAdvancedHeight_ > 0 && cachedAdvancedHeightDpiScale_ == dpiScale) {
        return cachedMaxAdvancedHeight_;
    }

    sciter::dom::element rootEl = get_root();
    sciter::dom::element container = rootEl.find_first("#main-container");
    if (!container.is_valid()) return 0;

    // Clear any height pinned by a previous measurement before remeasuring — the
    // container's own inline height would otherwise clamp BORDER_BOX to that stale
    // value for every tab, so every panel would appear the same (wrong) height and
    // remeasurement could never detect an actual change (Codex review catch).
    container.set_style_attribute("height", L"");
    rootEl.update(false);

    static const char* kPanelSelectors[] = {
        "#tab-panel-1", "#tab-panel-2", "#tab-panel-3", "#tab-panel-4"
    };
    constexpr int kPanelCount = 4;

    sciter::dom::element panels[kPanelCount];
    int currentIndex = -1;
    for (int i = 0; i < kPanelCount; ++i) {
        panels[i] = rootEl.find_first(kPanelSelectors[i]);
        if (panels[i].is_valid() && panels[i].get_state(STATE_EXPANDED)) {
            currentIndex = i;
        }
    }
    const int originalIndex = currentIndex;

    int maxHeight = 0;
    for (int i = 0; i < kPanelCount; ++i) {
        if (!panels[i].is_valid()) continue;

        if (i != currentIndex) {
            if (currentIndex >= 0 && panels[currentIndex].is_valid()) {
                panels[currentIndex].set_state(STATE_COLLAPSED, STATE_EXPANDED, false);
            }
            panels[i].set_state(STATE_EXPANDED, STATE_COLLAPSED, false);
            currentIndex = i;
            rootEl.update(false);  // force synchronous re-layout before measuring
        }

        RECT r = container.get_location_ppx(BORDER_BOX);
        int h = r.bottom - r.top;
        if (h > maxHeight) maxHeight = h;
    }

    // Restore whichever tab was actually active before we started measuring. If
    // none was expanded yet (this can run before JS's ready-time
    // initializeTabPanels() has executed — load() is async), fall back to tab 0,
    // the default active tab per settings.html, instead of stranding the last
    // panel we measured (tab 4) as the visibly-expanded one.
    const int restoreIndex = (originalIndex >= 0) ? originalIndex : 0;
    if (restoreIndex != currentIndex &&
        panels[restoreIndex].is_valid() && panels[currentIndex].is_valid()) {
        panels[currentIndex].set_state(STATE_COLLAPSED, STATE_EXPANDED, false);
        panels[restoreIndex].set_state(STATE_EXPANDED, STATE_COLLAPSED, false);
        rootEl.update(false);
    }

    cachedMaxAdvancedHeight_ = maxHeight;
    cachedAdvancedHeightDpiScale_ = dpiScale;
    return maxHeight;
}

void SettingsDialog::setToggleState(const std::wstring& id, bool checked) {
    sciter::dom::element root = get_root();
    std::string idStr = toNarrowString(id);
    sciter::dom::element toggle = root.find_first(("[id='" + idStr + "']").c_str());

    if (toggle.is_valid()) {
        // Suppress CSS transition for programmatic (non-user) updates so rapid
        // tray-icon clicks snap the toggle instantly without animation lag.
        // Clearing the inline style afterward restores the stylesheet transition
        // for the next user-initiated click.
        toggle.set_style_attribute("transition", L"none");

        // Div-based toggles use CSS class "checked" for styling
        std::wstring cls = toggle.get_attribute("class");
        std::wstring baseClass;

        // Remove existing "checked" class if present
        size_t pos = cls.find(L" checked");
        if (pos != std::wstring::npos) {
            baseClass = cls.substr(0, pos) + cls.substr(pos + 8);
        } else {
            pos = cls.find(L"checked ");
            if (pos != std::wstring::npos) {
                baseClass = cls.substr(0, pos) + cls.substr(pos + 8);
            } else {
                baseClass = cls;
            }
        }

        // Add "checked" class if needed
        if (checked) {
            toggle.set_attribute("class", (baseClass + L" checked").c_str());
        } else {
            toggle.set_attribute("class", baseClass.c_str());
        }

        // Restore transition so user-click animation still works
        toggle.set_style_attribute("transition", L"");
    }

    // Also update the hidden input value
    sciter::dom::element hiddenInput = root.find_first(("[id='val-" + idStr + "']").c_str());
    if (hiddenInput.is_valid()) {
        hiddenInput.set_value(sciter::value(checked ? 1 : 0));
    }
}

void SettingsDialog::setDropdownValue(const std::wstring& id, int value) {
    sciter::dom::element root = get_root();
    sciter::dom::element dropdown = root.find_first(("[id='" + toNarrowString(id) + "']").c_str());

    if (dropdown.is_valid()) {
        dropdown.set_value(sciter::value(value));
    }
}

void SettingsDialog::initializeUI() {
    sciter::dom::element root = get_root();
    if (!root.is_valid()) return;

    // Defer rendering — batch all DOM mutations into one repaint
    root.update(true);

    // Set input method dropdown
    setDropdownValue(L"input-type", static_cast<int>(config_.inputMethod));

    // Set code table dropdown
    setDropdownValue(L"bang-ma", static_cast<int>(config_.codeTable));

    // Set V/E toggle state (synced with main process)
    // CSS: unchecked=V (thumb left), checked=E (thumb right) — invert for correct display
    setToggleState(L"toggle-language", !vietnameseMode_);

    // Set toggle states
    setToggleState(L"beep-sound", config_.beepOnSwitch);
    setToggleState(L"smart-switch", config_.smartSwitch);
    setToggleState(L"exclude-apps", config_.excludeApps);
    // Sync TSF toggle with actual DLL registration state (not just saved config)
    config_.tsfApps = IsTsfRegistered();
    setToggleState(L"tsf-apps", config_.tsfApps);
    setToggleState(L"hide-preedit-underline", config_.hidePreeditUnderline);
    call_function("updateTsfChildren", sciter::value(config_.tsfApps));
    setDropdownValue(L"spell-check-level", static_cast<int>(config_.GetSpellCheckLevel()));
    // Sync spell check child toggles (allow-zwjf, restore-key, exclusions button)
    call_function("updateSpellCheckChildren",
                   sciter::value(config_.GetSpellCheckLevel() != SpellCheckLevel::Off));
    setToggleState(L"modern-ortho", config_.modernOrtho);
    setToggleState(L"auto-caps", config_.autoCaps);
    setToggleState(L"allow-zwjf", config_.allowZwjf);
    setToggleState(L"restore-key", config_.autoRestoreEnabled);
    setToggleState(L"cjk-auto-switch", config_.cjkAutoSwitch);
    setToggleState(L"debug-log", config_.debugLogEnabled);
    setToggleState(L"enable-toast", config_.enableToast);
    {
        sciter::dom::element body = root.find_first("body");
        if (body.is_valid()) {
            body.set_attribute("data-enable-toast", config_.enableToast ? L"true" : L"false");
        }
    }
    setToggleState(L"allow-english-bypass", config_.allowEnglishBypass);
    setToggleState(L"suggest-keep-chars", config_.suggestKeepChars);
    setToggleState(L"use-macro", config_.macroEnabled);
    setToggleState(L"macro-english", config_.macroInEnglish);
    setToggleState(L"quick-telex", config_.quickConsonant);
    setToggleState(L"quick-start", config_.quickStartConsonant);
    setToggleState(L"quick-end", config_.quickEndConsonant);
    setToggleState(L"auto-caps-macro", config_.autoCapsMacro);
    setToggleState(L"macro-undo-backspace", config_.macroUndoOnBackspace);
    call_function("updateMacroChildren", sciter::value(config_.macroEnabled));
    setToggleState(L"macro-title-case", config_.macroTitleCase);
    call_function("updateMacroCapsChildren", sciter::value(config_.autoCapsMacro));
    // esc-restore-raw / temp-off-openkey / temp-off-macro toggles were removed
    // when the unified Hotkey Rebind dialog took over their behavior. Per-intent
    // enable now lives in HotkeyRegistry (hotkey_state TOML section).

    setToggleState(L"key-ctrl", hotkeyConfig_.ctrl);
    setToggleState(L"key-alt", hotkeyConfig_.alt);
    setToggleState(L"key-win", hotkeyConfig_.win);
    setToggleState(L"key-shift", hotkeyConfig_.shift);

    // System settings toggles
    setToggleState(L"run-startup", systemConfig_.runAtStartup);
    setToggleState(L"run-admin", systemConfig_.runAsAdmin);
    setToggleState(L"show-on-startup", systemConfig_.showOnStartup);
    setToggleState(L"desktop-shortcut", systemConfig_.desktopShortcut);
    setToggleState(L"english-ui", systemConfig_.language == 1);

    // Auto-check update toggle
    setToggleState(L"check-update", systemConfig_.autoCheckUpdate);
    setToggleState(L"force-light-theme", systemConfig_.forceLightTheme);

    // Version number display
    {
        sciter::dom::element verSpan = root.find_first("#app-version-number");
        if (verSpan.is_valid()) {
            std::wstring verStr = VKEY_VERSION_WSTR L" (Build: " + GetBuildVersion(__DATE__, __TIME__) + L")";
            verSpan.set_text(verStr.c_str());
        }
        // Also update title bar version
        sciter::dom::element titleText = root.find_first(".title-text");
        if (titleText.is_valid()) {
            titleText.set_text(L"VKey v" VKEY_VERSION_WSTR);
        }
    }

    // Startup mode dropdown
    setDropdownValue(L"startup-mode", static_cast<int>(systemConfig_.startupMode));

    // Set switch key character (display "Space" for space char)
    sciter::dom::element switchKeyInput = root.find_first("#switch-key-char");
    if (switchKeyInput.is_valid()) {
        std::wstring display = (switchKeyChar_ == L" ") ? L"Space" : switchKeyChar_;
        switchKeyInput.set_value(sciter::value(display.c_str()));
    }

    // Set advanced toggle state
    setToggleState(L"show-advanced", isExpanded_);

    // Set expanded state on container
    if (isExpanded_) {
        sciter::dom::element container = root.find_first("#main-container");
        if (container.is_valid()) {
            std::wstring cls = container.get_attribute("class");
            if (cls.find(L"expanded") == std::wstring::npos) {
                container.set_attribute("class", (cls + L" expanded").c_str());
            }
        }
        // Resize window for expanded state
        SetTimer(get_hwnd(), TIMER_RESIZE_WINDOW, 50, NULL);
    } else {
        // Also resize for compact state to avoid extra bottom space
        SetTimer(get_hwnd(), TIMER_RESIZE_WINDOW, 10, NULL);
    }

    // Set background opacity — update slider UI + apply CSS
    {
        // 1. Update hidden input value (so JS slider reads the correct value)
        sciter::dom::element hiddenInput = root.find_first("#val-bg-opacity");
        if (hiddenInput.is_valid()) {
            hiddenInput.set_value(sciter::value(static_cast<int>(backgroundOpacity_)));
        }

        // 2. Update slider thumb, fill, and label directly from C++
        wchar_t pctStr[8];
        swprintf_s(pctStr, L"%d%%", static_cast<int>(backgroundOpacity_));

        sciter::dom::element thumb = root.find_first("#bg-opacity-thumb");
        if (thumb.is_valid()) {
            wchar_t leftStr[8];
            swprintf_s(leftStr, L"%d%%", static_cast<int>(backgroundOpacity_));
            thumb.set_style_attribute("left", leftStr);
        }

        sciter::dom::element fill = root.find_first("#bg-opacity-fill");
        if (fill.is_valid()) {
            wchar_t widthStr[8];
            swprintf_s(widthStr, L"%d%%", static_cast<int>(backgroundOpacity_));
            fill.set_style_attribute("width", widthStr);
        }

        sciter::dom::element valueLabel = root.find_first("#bg-opacity-value");
        if (valueLabel.is_valid()) {
            valueLabel.set_text(pctStr);
        }

        // 3. Apply CSS background color
        bool dark = forceLightTheme_ ? false : DarkModeHelper::IsWindowsDarkMode();
        double opacity = backgroundOpacity_ / 100.0;
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

    // Apply both states: config reloads can also turn pinning off.
    SetWindowPos(get_hwnd(), isPinned_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    notifySubdialogPinState();
    {
        sciter::dom::element pinBtn = root.find_first("#btn-pin");
        if (pinBtn.is_valid()) {
            pinBtn.set_attribute("class", isPinned_ ? L"btn-pin pinned" : L"btn-pin");
        }
    }

    // Admin indicator: reflect the CURRENT process's elevation state, not the
    // config flag. Config and actual state can diverge in several ways:
    //   - UAC denied on startup → config ON, process OFF
    //   - Scheduled task removed by antivirus → config ON, process OFF
    //   - De-elevation (shell dispatch) failed → config OFF, process still ON
    // Showing real state lets the user self-diagnose these cases.
    if (IsRunningAsAdmin()) {
        sciter::dom::element adminBadge = root.find_first("#admin-badge");
        if (adminBadge.is_valid()) {
            adminBadge.set_attribute("class", L"admin-badge active");
        }
    }

    // Restart banner — Settings runs as a subprocess so it cannot observe
    // main-process globals; banner state travels via SharedState flags.
    // Skip the JS round-trip when there is nothing to show.
    if (auto bannerState = GetUpdateBannerState(sharedState_);
        bannerState != UpdateBannerState::Hide) {
        call_function("showUpdateBanner", sciter::value(static_cast<int>(bannerState)));
    }

    // Flush all batched DOM mutations in one repaint
    root.update(false);
}

void SettingsDialog::requestRestartWindows() {
    // Delegate to the shared helper so the Classic dialog and the tray share
    // the same ExitWindowsEx flow (and SE_SHUTDOWN_NAME privilege acquisition).
    RestartWindowsWithPrompt(get_hwnd());
}

void SettingsDialog::onInputMethodChange(int method) {
    config_.inputMethod = static_cast<InputMethod>(method);
    saveSettings();
    if (onSettingsChanged_) onSettingsChanged_();
}

void SettingsDialog::onExpandChange(bool expanded) {
    isExpanded_ = expanded;
    // Set timer to resize window after CSS transition completes
    SetTimer(get_hwnd(), TIMER_RESIZE_WINDOW, 50, NULL);
}

void SettingsDialog::onToggleChange(sciter::string id, bool checked) {
    // Direct SOM call from JS — bypasses Sciter DOM event dispatch entirely
    handleToggleChange(std::wstring(id.c_str()), checked);
}

void SettingsDialog::onClose() {
    HWND hwnd = get_hwnd();
    if (hwnd) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }
}

void SettingsDialog::loadSettings() {
    // Load typing config
    config_ = ConfigManager::LoadOrDefault();

    // Load UI config
    auto uiConfig = ConfigManager::LoadUIConfigOrDefault();
    isExpanded_ = uiConfig.showAdvanced;
    backgroundOpacity_ = uiConfig.backgroundOpacity;
    isPinned_ = uiConfig.pinned;

    // Load hotkey config. Single-char edit box; vk ↔ char round-trip via
    // VkKeyScanW (save) and MapVirtualKeyW (load). Printable keys including
    // OEM punctuation (` ~ ; , . [ ] \ ' / = -) are supported. F-row / arrows
    // / non-printable VKs persisted in vk via TOML survive load but render
    // blank — rebind via the unified Hotkeys dialog if needed.
    hotkeyConfig_ = ConfigManager::LoadHotkeyConfigOrDefault();
    {
        uint32_t vk = hotkeyConfig_.vk;
        if (vk == 0x20) {
            switchKeyChar_ = L" ";
        } else if (vk != 0) {
            // Mask bit 15 to drop the dead-key flag on layouts where the
            // char (vd. ` ~ ^) is a deadkey — without this the cast can
            // produce a control-range wchar that renders as garbage.
            wchar_t c = static_cast<wchar_t>(MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF);
            if (c != 0) {
                switchKeyChar_ = std::wstring(1, towupper(c));
            } else {
                switchKeyChar_ = L"";
            }
        } else {
            switchKeyChar_ = L"";
        }
    }

    // Load system config
    systemConfig_ = ConfigManager::LoadSystemConfigOrDefault();
    forceLightTheme_ = systemConfig_.forceLightTheme;
}

void SettingsDialog::saveSettings() {
    syncToSharedState();      // Immediate — engine sees changes now
    configDirty_ = true;
    // Reset deferred save timer (30s from last change)
    SetTimer(get_hwnd(), TIMER_DEFERRED_SAVE, DEFERRED_SAVE_DELAY_MS, NULL);
}

void SettingsDialog::syncToSharedState() {
    // Update SharedState so DLL picks up changes immediately (cached handles)
    if (sharedState_.IsConnected()) {
        SharedState state = sharedState_.Read();
        if (state.IsValid()) {
            state.inputMethod = static_cast<uint8_t>(config_.inputMethod);
            state.spellCheck = static_cast<uint8_t>(config_.GetSpellCheckLevel());
            state.codeTable = static_cast<uint8_t>(config_.codeTable);
            state.SetFeatureFlags(EncodeFeatureFlags(config_));
            state.SetHotkey(hotkeyConfig_);
            state.configGeneration++;  // HookEngine detects this on next keystroke
            sharedState_.Write(state);
        }
    }

    // Signal ConfigEvent for TSF DLL (still uses Named Event path)
    if (configEvent_.IsValid()) {
        configEvent_.Signal();
    }
}

void SettingsDialog::saveToToml() {
    std::wstring path = ConfigManager::GetConfigPath();
    if (!ConfigManager::SaveToFile(path, config_)) {
        OutputDebugStringW(L"VKey: Failed to save config file\n");
    }

    // Save hotkey config — belt-and-suspenders sync switchKeyChar_ → vk in
    // case the textbox bypassed the live VALUE_CHANGED handler (paste / IME).
    // Same VkKeyScanW rule as the handler; non-mappable chars drop to vk=0.
    {
        if (switchKeyChar_.empty()) {
            hotkeyConfig_.vk = 0;
        } else if (switchKeyChar_ == L" ") {
            hotkeyConfig_.vk = 0x20;  // VK_SPACE
        } else {
            wchar_t c = switchKeyChar_[0];
            SHORT scan = VkKeyScanW(c);
            if (scan != -1) {
                hotkeyConfig_.vk = static_cast<uint32_t>(LOBYTE(scan));
            } else {
                hotkeyConfig_.vk = 0;
            }
        }
    }
    (void)ConfigManager::SaveHotkeyConfig(path, hotkeyConfig_);

    configDirty_ = false;

    // Bump configGeneration again so HookEngine reloads TOML-only fields
    // (convert hotkey, macros, excluded/TSF apps, per-app code table).
    // The first bump (from syncToSharedState) fired before TOML was written;
    // this second bump picks up the fresh values.
    if (sharedState_.IsConnected()) {
        SharedState state = sharedState_.Read();
        if (state.IsValid()) {
            state.configGeneration++;
            sharedState_.Write(state);
        }
    }

    // Signal ConfigEvent for TSF DLL
    if (configEvent_.IsValid()) {
        configEvent_.Signal();
    }
}

void SettingsDialog::saveUISettings() {
    UIConfig uiConfig;
    uiConfig.showAdvanced = isExpanded_;
    uiConfig.backgroundOpacity = backgroundOpacity_;
    uiConfig.pinned = isPinned_;

    std::wstring path = ConfigManager::GetConfigPath();
    if (!ConfigManager::SaveUIConfig(path, uiConfig)) {
        OutputDebugStringW(L"VKey: Failed to save UI config\n");
    }
}

void SettingsDialog::saveSystemSettings() {
    // SaveSystemConfig() writes the WHOLE [system] table, so handing it
    // systemConfig_ reverts every field this dialog has no control for back to
    // its construction-time value: the icon sub-dialog's (iconStyle, custom
    // colors, tsf indicator, floating icon) and the main process's
    // (watchdogEnabled from the tray menu, floatingIconX/Y saved at exit).
    // WM_VKEY_ICON_SETTINGS_CHANGED only covers the first group, and only if
    // that PostMessage lands. Merge onto a fresh snapshot instead — the mirror
    // of what IconSettingsDialog::saveAndNotify() already does. Listing what we
    // own (rather than what we don't) also fails safe: a new [system] field is
    // preserved until someone deliberately adds it here.
    SystemConfig toSave = ConfigManager::LoadSystemConfigOrDefault();
    toSave.runAtStartup    = systemConfig_.runAtStartup;
    toSave.runAsAdmin      = systemConfig_.runAsAdmin;
    toSave.showOnStartup   = systemConfig_.showOnStartup;
    toSave.desktopShortcut = systemConfig_.desktopShortcut;
    toSave.language        = systemConfig_.language;
    toSave.forceLightTheme = systemConfig_.forceLightTheme;
    toSave.autoCheckUpdate = systemConfig_.autoCheckUpdate;
    toSave.startupMode     = systemConfig_.startupMode;

    std::wstring path = ConfigManager::GetConfigPath();
    if (!ConfigManager::SaveSystemConfig(path, toSave)) {
        OutputDebugStringW(L"VKey: Failed to save system config\n");
    }
}

void SettingsDialog::notifySystemConfigChanged(WPARAM wParam) {
    // Notify main process to re-read icon config
    HWND trayWnd = FindWindowW(L"VKeyTrayClass", nullptr);
    if (trayWnd) {
        PostMessageW(trayWnd, WM_VKEY_ICON_CHANGED, wParam, 0);
    }
}

void SettingsDialog::setUpdateButtonEnabled(bool enabled) {
    sciter::dom::element root = get_root();
    sciter::dom::element btn = root.find_first("#btn-check-update");
    if (btn.is_valid()) {
        if (enabled) {
            btn.set_attribute("disabled", nullptr);  // remove attribute
        } else {
            btn.set_attribute("disabled", L"");
        }
    }
}

void SettingsDialog::startUpdateCheck() {
    HWND hwnd = get_hwnd();
    if (!hwnd) return;

    setUpdateButtonEnabled(false);

    // Shared state between background thread and progress dialog
    struct State {
        std::atomic<bool> done{false};
        UpdateInfo info;
    };
    auto state = std::make_shared<State>();

    std::thread([state]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        state->info = UpdateChecker::CheckForUpdate();
        CoUninitialize();
        state->done.store(true, std::memory_order_release);
    }).detach();

    // Blocking progress dialog — closes when done or user cancels
    bool completed = UpdateChecker::ShowProgressDialog(hwnd, S(StringId::UPDATE_CHECKING), state->done);

    setUpdateButtonEnabled(true);

    if (!completed) return;  // User cancelled

    if (state->info.available) {
        if (UpdateChecker::ShowUpdateDialog(hwnd, state->info)) {
            startUpdate(state->info);
        }
    } else if (state->info.checkSucceeded) {
        UpdateChecker::ShowUpToDateMessage(hwnd);
    } else {
        UpdateChecker::ShowCheckFailedMessage(hwnd);
    }
}

void SettingsDialog::startUpdate(const UpdateInfo& info) {
    HWND hwnd = get_hwnd();
    if (!hwnd || info.downloadUrl.empty()) return;

    if (!UpdateChecker::DownloadWithProgress(hwnd, info.downloadUrl, info.packedVersion)) return;

    // Success — signal main process to exit
    HWND trayWnd = FindWindowW(L"VKeyTrayClass", nullptr);
    if (trayWnd) {
        PostMessageW(trayWnd, WM_CLOSE, 0, 0);
    }
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

}  // namespace NextKey
