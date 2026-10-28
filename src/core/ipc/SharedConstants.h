// VKey - Shared Constants between EXE and TSF DLL
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#ifdef _WIN32

#include <Windows.h>

namespace NextKey {

// Cross-process constants shared between EXE and TSF DLL.
// Icon management moved to DLL's ITfLangBarItemButton — no cross-process messaging needed.

// Custom window messages for cross-process V/E mode sync (main ↔ settings subprocess)
constexpr UINT WM_VKEY_SET_MODE = WM_USER + 100;      // Settings → Main: set mode (wParam: 1=Vietnamese, 0=English)
constexpr UINT WM_VKEY_MODE_CHANGED = WM_USER + 101;  // Main → Settings: mode changed (wParam: 1=Vietnamese, 0=English)
constexpr UINT WM_VKEY_OPEN_EXCLUDED = WM_USER + 102;  // Deferred: open excluded apps dialog
constexpr UINT WM_VKEY_OPEN_MACRO = WM_USER + 103;    // Deferred: open macro table dialog
constexpr UINT WM_VKEY_ICON_CHANGED = WM_USER + 104;  // Settings → Main: icon style/color changed, re-read config
constexpr UINT WM_VKEY_CONFIG_CHANGED = WM_USER + 105;  // Main → Settings: config changed from tray menu, re-read TOML
constexpr UINT WM_VKEY_UPDATE_RESULT = WM_USER + 106;    // RESERVED (was: Settings update check result, now unused — synchronous flow)
constexpr UINT WM_VKEY_UPDATE_AVAILABLE = WM_USER + 107; // Main: auto-check found update (lParam: UpdateInfo*)
constexpr UINT WM_VKEY_TRAY_MODE_SYNC = WM_USER + 108;  // Deferred: sync tray icon V/E mode (wParam: 1=Vietnamese, 0=English)
constexpr UINT WM_VKEY_OPEN_TSFAPPS = WM_USER + 109;    // Deferred: open TSF apps dialog
constexpr UINT WM_VKEY_SHOW_SETTINGS = WM_USER + 110;   // Main: show settings dialog (from second instance)
constexpr UINT WM_VKEY_OPEN_APPOVERRIDES = WM_USER + 111; // Deferred: open app overrides dialog
constexpr UINT WM_VKEY_OPEN_SPELLEXCL = WM_USER + 112;    // Deferred: open spell exclusions dialog
constexpr UINT WM_VKEY_RESTART = WM_USER + 113;           // Settings → Main: restart app (admin mode changed)
constexpr UINT WM_VKEY_HOOK_RELOAD = WM_USER + 114;       // Subprocess → Main: eager hook reload after SignalConfigChange
constexpr UINT WM_VKEY_OPEN_USERDEFINED = WM_USER + 115;  // Deferred: open user-defined keymap dialog
constexpr UINT WM_VKEY_OPEN_HOTKEYS = WM_USER + 116;      // Deferred: open unified hotkey rebind dialog
constexpr UINT WM_VKEY_ACTIVATE_TSF = WM_USER + 117;      // Main: activate VKey TSF profile programmatically
constexpr UINT WM_VKEY_TRAY_TSF_SYNC = WM_USER + 118;     // Deferred: sync tray icon TSF indicator (wParam: 1=focused app is TSF, 0=not)
constexpr UINT WM_VKEY_OPEN_ICON_SETTINGS = WM_USER + 119; // Deferred: open icon customization dialog
constexpr UINT WM_VKEY_ICON_SETTINGS_CHANGED = WM_USER + 120; // Icon dialog → Settings: refresh cached SystemConfig only
constexpr UINT WM_VKEY_TRAY_APP_SYNC = WM_USER + 121;     // Deferred: consume latest tray app-context snapshot
constexpr UINT WM_VKEY_LEXICON_COMMITTED = WM_USER + 122; // Lexicon dialog → Main: publish wire, then signal generation
constexpr UINT WM_VKEY_PARENT_PIN_CHANGED = WM_USER + 123; // Settings → subdialog: re-read parent pin (lParam: Settings HWND)

// Identifies subdialogs without Win32 ownership across process input queues.
inline constexpr wchar_t VKEY_SUBDIALOG_PARENT_PROP[] = L"VKey.Subdialog.SettingsParent";

}  // namespace NextKey

#endif  // _WIN32
