// VKey - Pure decision: should a low-level keyboard event be classified as an
// external injected key (e.g. PhraseExpress, AutoHotkey, macro tools)?
// SPDX-License-Identifier: GPL-3.0-only
//
// Extracted from HookEngine::LowLevelKeyboardProc so the logic can be unit-tested
// on Linux (HookEngine.cpp itself is Windows-only). Mirrors the
// LeakedKeyDuringSendDecision.h / CjkSwitchDecision.h pattern.
//
// ── The problem (Issue #274 vs Issue #275) ──
// Issue #274: Third-party snippet expanders like PhraseExpress expand snippets (e.g.,
// "bttb" -> "bánh tráng Trảng Bàng") by sending Backspaces to delete the trigger
// abbreviation directly in the target application, then pasting/typing the expansion.
// If VKey intercepts and eats those external Backspaces with its own async diff /
// Chromium bait sequence (U+202F + 2 BS), VKey races with the external paste operation,
// cutting off the expansion text ("bttbbánh tráng Tr").
//
// Issue #275: Automation and typing simulation tools like AutoHotkey (AHK), Auto Keyboard,
// Macro Recorder, and UI test harnesses send character keys via SendInput (which Windows
// stamps with LLKHF_INJECTED). If VKey bypasses ALL injected keys, these tools cannot
// type Vietnamese at all (outputting raw ASCII like "dungf ahk gox thuwr...").
//
// ── The solution ──
// 1. External injected deletion keys (VK_BACK, VK_DELETE):
//    Bypass VKey's engine so the external tool's deletion reaches the host app directly.
//    On key-down, reset any active composition so stale buffer does not corrupt subsequent text.
// 2. External injected typing keys (alpha, digits, space, punctuation, etc.):
//    Route normally through VKey's typing engine so Vietnamese diacritics are applied
//    correctly for AHK and automation tools (identical to Unikey).

#pragma once

#include <cstddef>
#include <cstdint>

namespace NextKey {

inline constexpr uint32_t kInjectedFlagDefault = 0x00000010u;         // LLKHF_INJECTED
inline constexpr uint32_t kInjectedFlagLowerIntegrity = 0x00000002u;  // LLKHF_LOWER_IL_INJECTED
inline constexpr uint32_t kInjectedMask = kInjectedFlagDefault | kInjectedFlagLowerIntegrity;

inline constexpr uint32_t kVkBack = 0x08u;    // VK_BACK
inline constexpr uint32_t kVkDelete = 0x2Eu;  // VK_DELETE

[[nodiscard]] constexpr bool IsExternalInjectedKey(
    uint32_t flags,
    uintptr_t dwExtraInfo,
    uintptr_t vkeyExtraInfo) noexcept {
    if ((flags & kInjectedMask) == 0) {
        return false;
    }
    return dwExtraInfo != vkeyExtraInfo;
}

[[nodiscard]] constexpr bool ShouldBypassAndResetOnInjected(
    bool isExternalInjected,
    uint32_t vkCode) noexcept {
    if (!isExternalInjected) {
        return false;
    }
    return vkCode == kVkBack || vkCode == kVkDelete;
}

[[nodiscard]] constexpr bool ShouldResetCompositionOnInjected(
    bool isExternalInjected,
    uint32_t vkCode,
    bool isDown,
    size_t engineCount,
    bool hasPreviousComposition) noexcept {
    if (!ShouldBypassAndResetOnInjected(isExternalInjected, vkCode) || !isDown) {
        return false;
    }
    return engineCount > 0 || hasPreviousComposition;
}

// Overload for backwards compatibility with tests that don't pass vkCode
[[nodiscard]] constexpr bool ShouldResetCompositionOnInjected(
    bool isExternalInjected,
    bool isDown,
    size_t engineCount,
    bool hasPreviousComposition) noexcept {
    return ShouldResetCompositionOnInjected(
        isExternalInjected, kVkBack, isDown, engineCount, hasPreviousComposition);
}

}  // namespace NextKey
