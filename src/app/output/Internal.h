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
#include <span>
#include <string_view>

#include "core/SuggestionDismissalDecision.h"

namespace NextKey::Output::Internal {

using SendInputFn          = UINT (WINAPI*)(UINT, LPINPUT, int);
using GetAsyncKeyStateFn   = SHORT (WINAPI*)(int);
using SendMessageWFn       = LRESULT (WINAPI*)(HWND, UINT, WPARAM, LPARAM);
using SendMessageTimeoutWFn = LRESULT (WINAPI*)(HWND, UINT, WPARAM, LPARAM,
                                                UINT, UINT, PDWORD_PTR);
using SleepFn              = void (WINAPI*)(DWORD);
using NativePrefixLayoutFn = HKL (*)() noexcept;
using ToUnicodeExFn        = decltype(&::ToUnicodeEx);
// Sprint 2 D5: synth-counter callback. Fires from TrackedSendInput
// pre-SendInput (positive delta = events about to dispatch) and on
// partial-send (negative delta = compensate for events that didn't
// land). HookEngine wires this to its synthEventsPending_ atomic so
// the per-event decrement in LowLevelKeyboardProc balances correctly.
// Function pointer (not std::function) keeps the layer decoupled — no
// HookEngine dependency leaking into src/app/output/.
using SynthCounterFn       = void (*)(int delta) noexcept;
// Issue 5: callback fired before SendInput so HookEngine can record the exact
// dispatched synthetic events for precise echo-matching (down/up, vk, scan).
using SynthEventsDispatchedFn = void (*)(const INPUT* events, UINT count) noexcept;
// Issue 4: callback fired when SendInput partially fails (sent < count) to rollback
// unsent entries from the echo buffer so they don't linger and match external events.
using SynthEventsRollbackFn   = void (*)(const INPUT* unsentEvents, UINT count) noexcept;

// Test seams. Production initializes to the real Win32 APIs.
extern SendInputFn           g_sendInput;
extern GetAsyncKeyStateFn    g_getAsyncKeyState;
extern SendMessageWFn        g_sendMessageW;
extern SendMessageTimeoutWFn g_sendMessageTimeoutW;
extern SleepFn               g_sleep;
extern SynthCounterFn        g_synthCounterCallback;  // null = disabled
extern SynthEventsDispatchedFn g_synthEventsDispatchedCallback;  // null = disabled
extern SynthEventsRollbackFn   g_synthEventsRollbackCallback;    // null = disabled
extern NativePrefixLayoutFn  g_nativePrefixLayout;
extern ToUnicodeExFn         g_toUnicodeEx;

// GetAsyncKeyState still reports the preceding state inside an LL callback.
// Keep the current DOWN in scope too, without publishing it across threads or
// letting nested own-synthetic callbacks replace the physical context.
extern thread_local std::uint8_t g_inFlightPrefixKeys;

[[nodiscard]] constexpr std::uint8_t NativePrefixKeyMask(WORD vk) noexcept {
    return vk >= 'A' && vk <= 'C'
        ? static_cast<std::uint8_t>(1u << (vk - 'A')) : std::uint8_t{0};
}

[[nodiscard]] inline bool IsInFlightKeyDown(WORD vk) noexcept {
    const auto mask = NativePrefixKeyMask(vk);
    return mask != 0 && (g_inFlightPrefixKeys & mask) != 0;
}

class CurrentKeyDownScope final {
public:
    explicit CurrentKeyDownScope(WORD vk) noexcept
        : mask_(NativePrefixKeyMask(vk)),
          wasDown_(mask_ != 0 && (g_inFlightPrefixKeys & mask_) != 0) {
        if (mask_ != 0) g_inFlightPrefixKeys |= mask_;
    }
    ~CurrentKeyDownScope() noexcept {
        if (mask_ != 0 && !wasDown_) g_inFlightPrefixKeys &= static_cast<std::uint8_t>(~mask_);
    }
    CurrentKeyDownScope(const CurrentKeyDownScope&) = delete;
    CurrentKeyDownScope& operator=(const CurrentKeyDownScope&) = delete;

private:
    std::uint8_t mask_;
    bool wasDown_;
};

[[nodiscard]] SuggestionDismissalPlan PrepareSuggestionDismissal(
    SuggestionDismissalPolicy policy, std::size_t bsCount,
    std::wstring_view text, bool suggestKeepChars) noexcept;

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

[[nodiscard]] constexpr bool IsExtendedKey(WORD vk) noexcept {
    return vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

[[nodiscard]] inline INPUT MakeKeyEvent(WORD vk, bool keyup, WORD scan = 0) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = scan != 0 ? scan
        : static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = keyup ? KEYEVENTF_KEYUP : 0u;
    if (IsExtendedKey(vk)) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    input.ki.dwExtraInfo = kVKeyExtraInfo;
    return input;
}

[[nodiscard]] inline INPUT MakeUnicodeEvent(WCHAR ch, bool keyup) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(ch);
    input.ki.dwFlags = KEYEVENTF_UNICODE | (keyup ? KEYEVENTF_KEYUP : 0u);
    input.ki.dwExtraInfo = kVKeyExtraInfo;
    return input;
}

// Each caller preflights the complete batch before appending any event.
inline void AppendKeyPair(INPUT* buffer, std::size_t& cursor, WORD vk,
                          WORD scan = 0) noexcept {
    const auto down = MakeKeyEvent(vk, false, scan);
    buffer[cursor++] = down;
    auto up = down;
    up.ki.dwFlags |= KEYEVENTF_KEYUP;
    buffer[cursor++] = up;
}

inline void AppendUnicodePair(INPUT* buffer, std::size_t& cursor, WCHAR ch) noexcept {
    const auto down = MakeUnicodeEvent(ch, false);
    buffer[cursor++] = down;
    auto up = down;
    up.ki.dwFlags |= KEYEVENTF_KEYUP;
    buffer[cursor++] = up;
}

inline void AppendSuggestionPrefix(INPUT* buffer, std::size_t& cursor,
                                    const SuggestionDismissalPlan& plan) noexcept {
    if (!plan.emitPrefix) return;
    if (plan.prefixVk != 0) {
        AppendKeyPair(buffer, cursor, plan.prefixVk, plan.prefixScan);
    } else {
        AppendUnicodePair(buffer, cursor, plan.prefixChar);
    }
}

inline void RecoverAfterPartialSend(std::span<const INPUT> batch, UINT sent,
                                     const HeldModifiers& held,
                                     std::size_t releaseStart,
                                     std::size_t restoreStart) noexcept {
    std::array<INPUT, 9> restore{};
    UINT count = 0;
    if (sent > 0 && sent < batch.size()) {
        const auto& down = batch[sent - 1].ki;
        const auto& up = batch[sent].ki;
        // Only complete an owned pair. A game re-inject has no paired UP;
        // its eventual physical release must retain ownership of that key.
        if ((down.dwFlags & KEYEVENTF_KEYUP) == 0 &&
            up.dwFlags == (down.dwFlags | KEYEVENTF_KEYUP) &&
            down.wVk == up.wVk && down.wScan == up.wScan) {
            restore[count++] = batch[sent];
        }
    }
    for (std::size_t k = 0; k < held.count; ++k) {
        if (sent > releaseStart + k && sent <= restoreStart + k) {
            restore[count++] = MakeKeyEvent(held.vks[k], false);
        }
    }
    if (count > 0) (void)TrackedSendInput(restore.data(), count);
}

[[nodiscard]] inline HeldModifiers QueryHeldModifiers(std::size_t bsCount,
                                                      std::wstring_view text,
                                                      bool hasReinject = false) noexcept {
    HeldModifiers result{};
    const bool hasTextOrReinject = !text.empty() || hasReinject;
    if (bsCount == 0 && !hasTextOrReinject) return result;

    if (bsCount > 0) {
        if ((g_getAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
            const bool hasLShift = (g_getAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
            const bool hasRShift = (g_getAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
            if (hasLShift) result.vks[result.count++] = VK_LSHIFT;
            if (hasRShift) result.vks[result.count++] = VK_RSHIFT;
            if (!hasLShift && !hasRShift) {
                result.vks[result.count++] = VK_LSHIFT;
            }
        }
    }

    if (bsCount > 0 || hasTextOrReinject) {
        if ((g_getAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
            const bool hasLCtrl = (g_getAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
            const bool hasRCtrl = (g_getAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
            if (hasLCtrl) result.vks[result.count++] = VK_LCONTROL;
            if (hasRCtrl) result.vks[result.count++] = VK_RCONTROL;
            if (!hasLCtrl && !hasRCtrl) {
                result.vks[result.count++] = VK_LCONTROL;
            }
        }

        if ((g_getAsyncKeyState(VK_MENU) & 0x8000) != 0) {
            const bool hasLAlt = (g_getAsyncKeyState(VK_LMENU) & 0x8000) != 0;
            const bool hasRAlt = (g_getAsyncKeyState(VK_RMENU) & 0x8000) != 0;
            if (hasLAlt) result.vks[result.count++] = VK_LMENU;
            if (hasRAlt) result.vks[result.count++] = VK_RMENU;
            if (!hasLAlt && !hasRAlt) {
                result.vks[result.count++] = VK_LMENU;
            }
        }

        if ((g_getAsyncKeyState(VK_LWIN) & 0x8000) != 0) {
            result.vks[result.count++] = VK_LWIN;
        }
        if ((g_getAsyncKeyState(VK_RWIN) & 0x8000) != 0) {
            result.vks[result.count++] = VK_RWIN;
        }
    }

    return result;
}

}  // namespace NextKey::Output::Internal
