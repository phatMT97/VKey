// src/app/output/RichEditEmReplaceSelInjector.cpp
//
// D2 implementation. Port of HookEngine::TryEditMessagePaste — the
// sent-message channel that Sprint 1 D12 proved is required for Win11
// New Notepad RichEditD2DPT under chaos pressure. Posted-message
// channel (raw SendInput VK_BACK + Unicode chars) interleaves with
// already-queued messages and gets pre-empted by the next sent
// EM_REPLACESEL — chaos 5.3 verdict.
//
// Hang protection: SendMessageTimeoutW with SMTO_ABORTIFHUNG and a
// 50 ms timeout — if the target window is hung, we bail rather than
// block the hook callback indefinitely (LowLevelHooksTimeout default
// 300 ms; we stay well under).
//
// SendKey delegates to the Win32 SendInput physical channel because
// re-inject semantics (HookEngine::InjectKey use case) require the
// event to land AFTER any pending posted synth — sent messages would
// land BEFORE.
//
// Spec: docs/plans/sprint-2-output-injector.md §2.3
#include "RichEditEmReplaceSelInjector.h"

#include <richedit.h>

#include <string>

#include "Internal.h"
#include "Win32SendInputInjector.h"

#include "core/Logger.h"

namespace NextKey::Output {

namespace {

constexpr UINT kEditMsgFlags     = SMTO_ABORTIFHUNG | SMTO_NORMAL;
constexpr UINT kEditMsgTimeoutMs = 50;

// Class-compat check ported from HookEngine.cpp:1901. Accepts plain
// Edit, RichEdit*, ThunderRT6 (VB6), and modern WinUI 3 InputSiteWindowClass
// (Notepad) variants. Sprint 1 D12 fix targets RichEditD2DPT specifically
// but the broader compat list is preserved so EM_REPLACESEL works on any
// Edit-compatible host.
bool IsEditCompatibleClass(const wchar_t* cls) noexcept {
    if (!cls || !*cls) return false;
    if (_wcsnicmp(cls, L"ThunderRT6TextBox", 17) == 0) return true;
    if (_wcsnicmp(cls, L"ThunderRT6RichText", 18) == 0) return true;
    if (_wcsicmp(cls, L"Edit") == 0) return true;
    if (_wcsnicmp(cls, L"RichEdit", 8) == 0) return true;
    return false;
}

// Resolve focused HWND inside the foreground process. Mirrors
// GetFocusedChildHwnd (AppHelpers.h) — uses AttachThreadInput + GetFocus
// instead of GetGUIThreadInfo because the latter often returns
// hwndFocus=NULL from the LL keyboard hook thread, causing Replace() to
// fall back to the foreground window whose class (e.g. "Notepad") fails
// IsEditCompatibleClass. AttachThreadInput is ~µs per call; acceptable
// for the ~5% of host classes that use this injector.
HWND ResolveFocusedHwnd() noexcept {
    HWND foregroundWindow = ::GetForegroundWindow();
    if (!foregroundWindow) return nullptr;
    DWORD foregroundThreadId = ::GetWindowThreadProcessId(foregroundWindow, nullptr);
    DWORD currentThreadId = ::GetCurrentThreadId();
    if (foregroundThreadId == currentThreadId) return ::GetFocus();
    HWND focusedWindow = nullptr;
    if (::AttachThreadInput(currentThreadId, foregroundThreadId, TRUE)) {
        focusedWindow = ::GetFocus();
        ::AttachThreadInput(currentThreadId, foregroundThreadId, FALSE);
    }
    return focusedWindow ? focusedWindow : foregroundWindow;
}

}  // namespace

bool RichEditEmReplaceSelInjector::Replace(std::size_t backspaceCount,
                                           std::wstring_view text,
                                           unsigned short reinjectVk) noexcept {
    // reinjectVk is reachable despite HandleAlphaKey's editMsgPath gate:
    // that gate reads OutputDispatcher::IsSyncReplaceChannel(), which for a
    // non-forced instance also requires the *currently focused child class*
    // to be edit-compatible. When it isn't, editMsgPath is false, reinjectVk
    // gets set, and the generic branch dispatches through this injector.
    if (backspaceCount == 0 && text.empty() && reinjectVk == 0) return true;

    HWND targetWindow = ResolveFocusedHwnd();
    if (!targetWindow) {
        Win32SendInputInjector fallbackInjector(SuggestionDismissalPolicy::None);
        return fallbackInjector.Replace(backspaceCount, text, reinjectVk);
    }

    wchar_t className[64] = {};
    if (::GetClassNameW(targetWindow, className, _countof(className)) == 0) {
        Win32SendInputInjector fallbackInjector(SuggestionDismissalPolicy::None);
        return fallbackInjector.Replace(backspaceCount, text, reinjectVk);
    }

    if (!forced_ && !IsEditCompatibleClass(className)) {
        if (::NextKey::Logger::IsEnabled()) {
            ::NextKey::Logger::Log(L"[Hook] RichEditEmReplaceSelInjector: class '%ls' not compatible, falling back to SendInput BS=%zu",
                                   className, backspaceCount);
        }
        Win32SendInputInjector fallbackInjector(SuggestionDismissalPolicy::None);
        return fallbackInjector.Replace(backspaceCount, text, reinjectVk);
    }

    // EM_REPLACESEL edits the document directly — there is no key stream to
    // re-inject into, so the raw char HandleAlphaKey already added to
    // previousComposition_ never reaches this control. Drop the backspace
    // that was counted for it, or we delete one real character too many.
    // Same compensation OutputDispatcher applies on its clipboard branch.
    if (reinjectVk != 0 && backspaceCount > 0) --backspaceCount;

    DWORD_PTR timeoutResult = 0;
    DWORD     newSelectionStart = 0, selectionEnd = 0;

    // EM_GETSEL — query caret. Safe to call before redraw suppression.
    if (backspaceCount > 0) {
        DWORD selectionStart = 0;
        if (!Internal::g_sendMessageTimeoutW(targetWindow, EM_GETSEL,
                reinterpret_cast<WPARAM>(&selectionStart),
                reinterpret_cast<LPARAM>(&selectionEnd),
                kEditMsgFlags, kEditMsgTimeoutMs, &timeoutResult)) {
            return false;  // timed out / no result
        }
        if (static_cast<DWORD>(backspaceCount) > selectionEnd) {
            // BS would cross before the start of buffer — bail.
            return false;
        }
        newSelectionStart = selectionEnd - static_cast<DWORD>(backspaceCount);
    }

    // Suppress redraw between EM_SETSEL (highlights selection) and
    // EM_REPLACESEL — otherwise the user sees a brief blue selection flash.
    // erase=FALSE because text controls paint their own background; TRUE
    // would cause a background-color flash before text redraws.
    bool redrawSuppressed = false;
    if (backspaceCount > 0) {
        redrawSuppressed = Internal::g_sendMessageTimeoutW(targetWindow, WM_SETREDRAW,
            FALSE, 0, kEditMsgFlags, kEditMsgTimeoutMs, &timeoutResult) != 0;

        if (!Internal::g_sendMessageTimeoutW(targetWindow, EM_SETSEL,
                static_cast<WPARAM>(newSelectionStart), static_cast<LPARAM>(selectionEnd),
                kEditMsgFlags, kEditMsgTimeoutMs, &timeoutResult)) {
            if (redrawSuppressed) {
                Internal::g_sendMessageTimeoutW(targetWindow, WM_SETREDRAW, TRUE, 0,
                    kEditMsgFlags, kEditMsgTimeoutMs, &timeoutResult);
                ::InvalidateRect(targetWindow, nullptr, FALSE);
            }
            return false;
        }
    }

    // wParam=TRUE → goes on the undo stack so Ctrl+Z still works.
    // text needs to be null-terminated for EM_REPLACESEL — the std::wstring
    // copy is on the cold-ish RichEdit path (~5% of cases) so the alloc is
    // acceptable. Stack-array would need a max-size cap; std::wstring is
    // simpler and small-string-optimized for typical Vietnamese words.
    std::wstring zterm(text);
    BOOL replaceOk = Internal::g_sendMessageTimeoutW(targetWindow, EM_REPLACESEL,
        static_cast<WPARAM>(TRUE), reinterpret_cast<LPARAM>(zterm.c_str()),
        kEditMsgFlags, kEditMsgTimeoutMs, &timeoutResult) != 0;

    if (redrawSuppressed) {
        Internal::g_sendMessageTimeoutW(targetWindow, WM_SETREDRAW, TRUE, 0,
            kEditMsgFlags, kEditMsgTimeoutMs, &timeoutResult);
        ::InvalidateRect(targetWindow, nullptr, FALSE);
    }

    return replaceOk != FALSE;
}

void RichEditEmReplaceSelInjector::SendKey(unsigned short vkCode) noexcept {
    // Re-inject semantics: physical SendInput so the event lands AFTER
    // any pending posted synth in the OS input queue. Sent-message
    // delivery would land BEFORE (different queue), breaking InjectKey's
    // contract at HookEngine line ~905.
    Win32SendInputInjector(SuggestionDismissalPolicy::None).SendKey(vkCode);
}

}  // namespace NextKey::Output
