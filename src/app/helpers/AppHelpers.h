// VKey - App-Layer Helper Functions
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/config/ConfigEvent.h"
#include "core/ipc/SharedStateManager.h"
#include "core/CrashLog.h"
#ifdef _WIN32
#include "core/ipc/SharedConstants.h"
#endif
#include <cwctype>
#include <istream>
#include <iterator>
#include <optional>
#include <string>

#ifndef _WIN32
// Stub for non-Windows builds (Linux tests)
inline void InstallCursorCrashHandler() noexcept {}
#else
#include <Windows.h>

/// Crash handler that restores system cursors if app crashes during window picking.
/// SetSystemCursor() changes cursors globally — if we crash mid-pick, the crosshair
/// cursor stays until logoff. This handler restores defaults on any unhandled exception.
inline LONG WINAPI CursorCrashHandler(EXCEPTION_POINTERS* ep) noexcept {
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
    // D1: fatal crash breadcrumb + minidump next to the crash log. Without this a
    // top-level crash left no stack — undiagnosable for a pre-release with no
    // telemetry. Runs as the process is dying; both calls are best-effort.
    ::NextKey::CrashLog(L"UnhandledException", "fatal — minidump written alongside");
    ::NextKey::WriteCrashDump(ep, L"_vkey_crash");
    return EXCEPTION_CONTINUE_SEARCH;  // Let debugger/WER handle it
}

/// Install the cursor crash handler. Call once at startup (main.cpp / main_lite.cpp).
inline void InstallCursorCrashHandler() noexcept {
    SetUnhandledExceptionFilter(CursorCrashHandler);
}
#endif

namespace NextKey {

/// Signal HookEngine that a config value changed.
/// Called from dialog persistence methods after ConfigManager::Save*().
/// Increments configGeneration in SharedState — HookEngine detects on next keystroke.
/// Also signals ConfigEvent for TSF DLL which still uses the Named Event path.
inline void SignalConfigChange(
    bool postHookReload = true,
    std::optional<SpellCheckLevel> committedSpellLevel = std::nullopt) noexcept {
    // Bump configGeneration in SharedState (HookEngine reads this)
    SharedStateManager sm;
    if (sm.OpenReadWrite()) {
        SharedState state = sm.Read();
        if (state.IsValid()) {
            if (committedSpellLevel) {
                state.spellCheck = static_cast<uint8_t>(*committedSpellLevel);
            }
            state.configGeneration++;
            sm.Write(state);
        }
    }
    // Signal ConfigEvent for TSF DLL (still uses Named Event)
    ConfigEvent event;
    if (event.Initialize()) {
        event.Signal();
    }
#ifdef _WIN32
    // Eager hook reload: tell main EXE to QuickSync now so new list applies
    // without waiting for the next keystroke / focus change in the target app.
    if (postHookReload) {
        if (HWND trayWnd = FindWindowW(L"VKeyTrayClass", nullptr)) {
            PostMessageW(trayWnd, WM_VKEY_HOOK_RELOAD, 0, 0);
        }
    }
#endif
}

/// Ask the main process to publish the committed lexicon snapshot to its
/// single-writer mapping, then signal SharedState/TSF. Send synchronously so
/// readers can never observe the generation before the wire contents.
inline bool ApplyCommittedLexicon() noexcept {
#ifdef _WIN32
    const HWND trayWnd = FindWindowW(L"VKeyTrayClass", nullptr);
    if (!trayWnd) return false;
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(
               trayWnd,
               WM_VKEY_LEXICON_COMMITTED,
               0,
               0,
               SMTO_ABORTIFHUNG | SMTO_BLOCK,
               5000,
               &result) != 0 && result != 0;
#else
    return false;
#endif
}

/// Convert wstring to lowercase (ASCII-safe, for app names and macro keys).
inline std::wstring ToLowerAscii(std::wstring str) noexcept {
    for (auto& c : str) c = towlower(c);
    return str;
}

/// Iterate UTF-8 text lines from a stream. Strips trailing CR (Windows
/// CRLF) + UTF-8 BOM on the first line (Notepad-saved files). Skips
/// empty lines and lines starting with ';' (comment). Caller owns the
/// stream + decides what to do with each non-empty content line. Used
/// by dialog Import handlers — keeps the open/close/error-handling per
/// call site since UX varies (Sciter silent on open-fail; Classic shows
/// MessageBox).
template <typename Handler>
inline void ParseConfigLines(std::istream& input, Handler handler) {
    std::string line;
    bool firstLine = true;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (firstLine) {
            firstLine = false;
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        handler(line);
    }
}

#ifdef _WIN32
/// Get the focused child window within a foreground top-level HWND.
/// Uses AttachThreadInput for cross-thread queries — SLOW, avoid per-keystroke.
/// GetGUIThreadInfo was tried as an AttachThreadInput-free alternative and
/// rejected: RichEditEmReplaceSelInjector.cpp's ResolveFocusedHwnd() found it
/// often returns hwndFocus=NULL when called from the LL keyboard hook thread
/// specifically — do not "fix" this function to use it without re-verifying
/// that constraint no longer applies.
/// Returns nullptr if GetFocus() fails or foreground is null.
[[nodiscard]] inline HWND GetFocusedChildHwnd(HWND foreground) noexcept {
    if (!foreground) return nullptr;
    DWORD fgTid = GetWindowThreadProcessId(foreground, nullptr);
    DWORD myTid = GetCurrentThreadId();
    if (fgTid == myTid) return GetFocus();
    HWND focused = nullptr;
    if (AttachThreadInput(myTid, fgTid, TRUE)) {
        focused = GetFocus();
        AttachThreadInput(myTid, fgTid, FALSE);
    }
    return focused;
}

/// Same as GetFocusedChildHwnd, but falls back to the foreground HWND itself
/// when no child is focused. Useful when the caller wants to operate on
/// *something* (e.g. send EM_GETSEL) rather than return failure.
[[nodiscard]] inline HWND GetFocusedChildOrForeground(HWND foreground) noexcept {
    HWND focused = GetFocusedChildHwnd(foreground);
    return focused ? focused : foreground;
}

/// Cheap style-bit check (no message send, no timeout) — an Edit/RichEdit
/// control with ES_PASSWORD. Safe to call synchronously on the hook thread,
/// including inline in LowLevelKeyboardProc's call chain, when `foreground`/
/// `focused` are resolved fresh at the call site (not a cached/latched
/// value) — unlike GetFocusedChildHwnd's own AttachThreadInput cost, this
/// adds no further syscalls beyond the two already spent resolving those
/// HWNDs. Unknown/custom controls default to "not a password field" — a
/// safety NARROWING, not a detector of every possible password widget.
[[nodiscard]] inline bool IsFocusedControlPassword(HWND foreground, HWND focused) noexcept {
    if (foreground == nullptr || GetForegroundWindow() != foreground) return false;
    if (focused == nullptr || GetAncestor(focused, GA_ROOT) != foreground) return false;

    wchar_t className[64] = {};
    if (GetClassNameW(focused, className, static_cast<int>(std::size(className))) <= 0) {
        return false;
    }
    if (_wcsicmp(className, L"Edit") != 0 && _wcsnicmp(className, L"RichEdit", 8) != 0) {
        return false;
    }
    return (GetWindowLongPtrW(focused, GWL_STYLE) & ES_PASSWORD) != 0;
}

/// Multi-monitor-aware top-left for centering a window of given size on the
/// monitor that contains `referenceHwnd`. Falls back to the primary monitor
/// when the reference HWND is null/invalid. Honors taskbar / docked panels
/// by using `rcWork` instead of `rcMonitor`. Replaces the old
/// `GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)` pattern, which always
/// centers on the primary screen and ignores the work area.
/// Surface an already-open window (settings, sub-dialogs) instead of spawning a
/// second one. SetForegroundWindow only lands if this process holds the
/// foreground privilege — a background process must be granted it first via
/// AllowSetForegroundWindow() from whoever is currently foreground.
inline void FocusExistingWindow(HWND hwnd) noexcept {
    if (!hwnd) return;
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
}

/// Handoff foreground activation to owner window before a child process exits.
inline void HandoffFocusToOwner(HWND ownerHwnd) noexcept {
    if (!ownerHwnd || !IsWindow(ownerHwnd)) return;

    DWORD parentPid = 0;
    GetWindowThreadProcessId(ownerHwnd, &parentPid);
    if (parentPid != 0) {
        AllowSetForegroundWindow(parentPid);
    }
    FocusExistingWindow(ownerHwnd);
}

[[nodiscard]] inline POINT GetCenteredPos(HWND referenceHwnd, int width, int height) noexcept {
    HMONITOR mon = MonitorFromWindow(referenceHwnd ? referenceHwnd : GetDesktopWindow(),
                                     MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) {
        // Last-resort: primary screen metrics, ignore work area.
        return { (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - height) / 2 };
    }
    const int sx = mi.rcWork.right - mi.rcWork.left;
    const int sy = mi.rcWork.bottom - mi.rcWork.top;
    return { mi.rcWork.left + (sx - width) / 2,
             mi.rcWork.top + (sy - height) / 2 };
}
#endif

}  // namespace NextKey
