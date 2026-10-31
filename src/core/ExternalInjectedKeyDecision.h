// VKey - Pure decision: should a low-level keyboard event be classified as an
// external injected key (e.g. PhraseExpress, AutoHotkey, macro tools)?
// SPDX-License-Identifier: GPL-3.0-only
//
// Extracted from HookEngine::LowLevelKeyboardProc so the logic can be unit-tested
// on Linux (HookEngine.cpp itself is Windows-only). Mirrors the
// LeakedKeyDuringSendDecision.h / CjkSwitchDecision.h pattern.
//
// ── The problem (Issue #274, PhraseExpress / macro tool conflict) ──
// When third-party macro tools like PhraseExpress expand snippets (e.g., "bttb" ->
// "bánh tráng Trảng Bàng"), they simulate Backspaces to delete the trigger abbreviation
// and then paste/type the expansion.
//
// Because SendInput marks generated events with LLKHF_INJECTED (0x10) or
// LLKHF_LOWER_IL_INJECTED (0x02) in KBDLLHOOKSTRUCT::flags, external tools send
// injected events with dwExtraInfo == 0 (or their own IDs).
//
// VKey generates its own synthetic events stamped with VKEY_EXTRA_INFO ("NK" / 0x4E4B).
// If HookEngine only checks dwExtraInfo == VKEY_EXTRA_INFO and fails to check
// LLKHF_INJECTED, external backspaces and characters enter ProcessKeyDown as if they
// were real physical user keystrokes. This causes VKey to eat backspaces, fire
// Chromium bait sequences (U+202F + 2 BS), and race with external paste operations,
// cutting off the expansion text ("bttbbánh tráng Tr").
//
// ── The fix ──
// 1. If an event has LLKHF_INJECTED or LLKHF_LOWER_IL_INJECTED set, AND dwExtraInfo is
//    NOT VKEY_EXTRA_INFO, it is an external injected key.
// 2. On key-down of an external injected event, if VKey has an active or previous
//    composition, VKey resets composition immediately so stale state does not corrupt
//    subsequent typing.
// 3. All external injected events pass through untouched via CallNextHookEx without
//    entering VKey's engine or hotkey matcher.

#pragma once

#include <cstddef>
#include <cstdint>

namespace NextKey {

inline constexpr uint32_t kInjectedFlagDefault = 0x00000010u;         // LLKHF_INJECTED
inline constexpr uint32_t kInjectedFlagLowerIntegrity = 0x00000002u;  // LLKHF_LOWER_IL_INJECTED
inline constexpr uint32_t kInjectedMask = kInjectedFlagDefault | kInjectedFlagLowerIntegrity;

[[nodiscard]] constexpr bool IsExternalInjectedKey(
    uint32_t flags,
    uintptr_t dwExtraInfo,
    uintptr_t vkeyExtraInfo) noexcept {
    if ((flags & kInjectedMask) == 0) {
        return false;
    }
    return dwExtraInfo != vkeyExtraInfo;
}

[[nodiscard]] constexpr bool ShouldResetCompositionOnInjected(
    bool isExternalInjected,
    bool isDown,
    size_t engineCount,
    bool hasPreviousComposition) noexcept {
    if (!isExternalInjected || !isDown) {
        return false;
    }
    return engineCount > 0 || hasPreviousComposition;
}

}  // namespace NextKey
