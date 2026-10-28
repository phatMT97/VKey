// src/app/output/Internal.h
//
// Test-seam infrastructure private to src/app/output/. Function-pointer
// indirection on SendInput / SendMessageW / Sleep so injector impls are
// testable on Windows GTest with mocked APIs (no GUI required).
//
// Production wires the pointers to ::SendInput / ::SendMessageW / ::Sleep.
// Tests swap to capturing lambdas in fixture SetUp() and restore in
// TearDown() — see tests/output/InjectorTestBase.h.
//
// Spec: docs/plans/sprint-2-output-injector.md §2.5
#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace NextKey::Output::Internal {

using SendInputFn          = UINT (WINAPI*)(UINT, LPINPUT, int);
using GetAsyncKeyStateFn   = SHORT (WINAPI*)(int);
using SendMessageWFn       = LRESULT (WINAPI*)(HWND, UINT, WPARAM, LPARAM);
using SendMessageTimeoutWFn = LRESULT (WINAPI*)(HWND, UINT, WPARAM, LPARAM,
                                                UINT, UINT, PDWORD_PTR);
using SleepFn              = void (WINAPI*)(DWORD);
// Sprint 2 D5: synth-counter callback. Fires from TrackedSendInput
// pre-SendInput (positive delta = events about to dispatch) and on
// partial-send (negative delta = compensate for events that didn't
// land). HookEngine wires this to its synthEventsPending_ atomic so
// the per-event decrement in LowLevelKeyboardProc balances correctly.
// Function pointer (not std::function) keeps the layer decoupled — no
// HookEngine dependency leaking into src/app/output/.
using SynthCounterFn       = void (*)(int delta) noexcept;

// Test seams. Production initializes to the real Win32 APIs.
extern SendInputFn           g_sendInput;
extern GetAsyncKeyStateFn    g_getAsyncKeyState;
extern SendMessageWFn        g_sendMessageW;
extern SendMessageTimeoutWFn g_sendMessageTimeoutW;
extern SleepFn               g_sleep;
extern SynthCounterFn        g_synthCounterCallback;  // null = disabled

// Wrapper around g_sendInput with partial-send detection. Returns true
// iff all events delivered; false on partial (renderer drop case —
// detected when SendInput returns fewer events than requested). Fires
// g_synthCounterCallback (when non-null) before SendInput with +count,
// then again with -(count-sent) on partial. `sentCount`, when provided,
// receives the exact SendInput return value for state-recovery decisions.
[[nodiscard]] bool TrackedSendInput(INPUT* events, UINT count,
                                    UINT* sentCount = nullptr) noexcept;

// Marker dwExtraInfo so own synth events skip our own hook (Rule #11.4
// early-return). MUST match HookEngine::VKEY_EXTRA_INFO ("NK"). If
// these diverge, own synth events are not recognized as own → infinite
// re-entry loop. The integration test (chaos corpus) catches this.
constexpr ULONG_PTR kVKeyExtraInfo = 0x4E4BULL;

// Chromium suggest-dismiss bait predicate, shared by Win32 + Split
// injectors. The `suggestKeepChars` user setting (default false) carves
// out pure-BS to behave like EVKey for the issue #108 "face → fac"
// complaint: when ON and text is empty, skip the bait so Edge/Chrome
// inline autocomplete (a SELECTION inside the input) gets dismissed
// without losing a user char. Trade-off when ON: engine state can
// desync after a popup-dismiss-BS in active VN composition ("nex" + BS
// + 'x' → "neẽ"), so default OFF keeps engine sync (bait always fires;
// "face" + BS visually flickers to "fac" but VN typing stays correct).
// Replace operations (text non-empty) ALWAYS keep the bait regardless
// of the setting — a transform's chars must dismiss any stale suggest
// frame before landing.
[[nodiscard]] constexpr bool
ShouldEmitBait(bool needsBaitCharPrefix,
               std::size_t bsCount,
               std::wstring_view text,
               bool suggestKeepChars) noexcept {
    if (!needsBaitCharPrefix || bsCount == 0) return false;
    if (suggestKeepChars && text.empty()) return false;
    return true;
}

// Held modifier detection for modifier-neutral synthetic injection.
// Backspace (VK_BACK) sent while Shift/Ctrl/Alt/Win is held combines with
// the modifier into destructive shortcuts:
//   - Shift+BS: destructive selection/edit command in Excel Web (issue #225)
//   - Ctrl+BS: word/line delete in Sublime, Notepad++, Word, Chrome (issue #267)
//   - Alt+BS: Undo command in Windows text controls
//   - Win+BS: OS shortcut
// Furthermore, non-Shift modifiers (Ctrl/Alt/Win) held while Unicode text is
// injected can cause applications (such as Microsoft Word) to discard the text
// or treat it as control sequences.
//
// Shift is excluded when bsCount == 0 because typing uppercase with Shift held
// is normal typing behavior and KEYEVENTF_UNICODE already case-encodes the glyph.
struct HeldModifiers {
    std::array<WORD, 8> vks{};
    std::size_t count = 0;
};

[[nodiscard]] inline INPUT MakeModifierEvent(WORD vk, bool keyup) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = keyup ? KEYEVENTF_KEYUP : 0u;
    if (vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    input.ki.dwExtraInfo = kVKeyExtraInfo;
    return input;
}

inline void RestoreModifiersAfterPartialSend(const HeldModifiers& held, UINT sent,
                                              std::size_t releaseStart,
                                              std::size_t restoreStart) noexcept {
    std::array<INPUT, 8> restore{};
    UINT count = 0;
    for (std::size_t k = 0; k < held.count; ++k) {
        if (sent > releaseStart + k && sent <= restoreStart + k) {
            restore[count++] = MakeModifierEvent(held.vks[k], false);
        }
    }
    if (count > 0) (void)TrackedSendInput(restore.data(), count);
}

[[nodiscard]] inline HeldModifiers QueryHeldModifiers(std::size_t bsCount,
                                                      std::wstring_view text) noexcept {
    HeldModifiers result{};
    if (bsCount == 0 && text.empty()) return result;

    const bool hasLShift = (g_getAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
    const bool hasRShift = (g_getAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
    const bool hasLCtrl  = (g_getAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
    const bool hasRCtrl  = (g_getAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
    const bool hasLAlt   = (g_getAsyncKeyState(VK_LMENU) & 0x8000) != 0;
    const bool hasRAlt   = (g_getAsyncKeyState(VK_RMENU) & 0x8000) != 0;
    const bool hasLWin   = (g_getAsyncKeyState(VK_LWIN) & 0x8000) != 0;
    const bool hasRWin   = (g_getAsyncKeyState(VK_RWIN) & 0x8000) != 0;

    if (bsCount > 0) {
        if (hasLShift) result.vks[result.count++] = VK_LSHIFT;
        if (hasRShift) result.vks[result.count++] = VK_RSHIFT;
        if (!hasLShift && !hasRShift && (g_getAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
            result.vks[result.count++] = VK_LSHIFT;
        }
    }

    if (bsCount > 0 || !text.empty()) {
        if (hasLCtrl) result.vks[result.count++] = VK_LCONTROL;
        if (hasRCtrl) result.vks[result.count++] = VK_RCONTROL;
        if (!hasLCtrl && !hasRCtrl && (g_getAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
            result.vks[result.count++] = VK_LCONTROL;
        }

        if (hasLAlt) result.vks[result.count++] = VK_LMENU;
        if (hasRAlt) result.vks[result.count++] = VK_RMENU;
        if (!hasLAlt && !hasRAlt && (g_getAsyncKeyState(VK_MENU) & 0x8000) != 0) {
            result.vks[result.count++] = VK_LMENU;
        }

        if (hasLWin) result.vks[result.count++] = VK_LWIN;
        if (hasRWin) result.vks[result.count++] = VK_RWIN;
    }

    return result;
}

}  // namespace NextKey::Output::Internal
