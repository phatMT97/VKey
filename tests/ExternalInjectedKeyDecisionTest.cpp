// Tests for IsExternalInjectedKey and ShouldResetCompositionOnInjected — pure
// decision for external macro tool conflict resolution (issue #274).
// HookEngine.cpp is Windows-only, so we test the extracted decision in isolation
// (same pattern as LeakedKeyDuringSendDecisionTest).
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "core/ExternalInjectedKeyDecision.h"

using NextKey::IsExternalInjectedKey;
using NextKey::ShouldBypassAndResetOnInjected;
using NextKey::ShouldResetCompositionOnInjected;
using NextKey::kInjectedFlagDefault;
using NextKey::kInjectedFlagLowerIntegrity;
using NextKey::kInjectedMask;
using NextKey::kVkBack;
using NextKey::kVkDelete;

namespace {

constexpr uintptr_t kVKeyExtraInfo = 0x4E4B;  // "NK" magic
constexpr uint32_t kVkKeyA = 0x41u;           // 'A'
constexpr uint32_t kVkSpace = 0x20u;          // Space

}  // namespace

// ── Physical key (not injected) ───────────────────────────────────────
TEST(ExternalInjectedKeyDecision, PhysicalKey_NotClassifiedAsInjected) {
    constexpr uint32_t kPhysicalFlags = 0x00000000u;
    EXPECT_FALSE(IsExternalInjectedKey(kPhysicalFlags, 0, kVKeyExtraInfo));
    EXPECT_FALSE(IsExternalInjectedKey(kPhysicalFlags, 0x1234, kVKeyExtraInfo));
}

// ── VKey synthetic key ────────────────────────────────────────────────
TEST(ExternalInjectedKeyDecision, VKeySyntheticKey_NotClassifiedAsExternal) {
    // LLKHF_INJECTED is set, but dwExtraInfo carries VKEY_EXTRA_INFO
    constexpr uint32_t kInjectedFlags = kInjectedFlagDefault;
    EXPECT_FALSE(IsExternalInjectedKey(kInjectedFlags, kVKeyExtraInfo, kVKeyExtraInfo));

    // Both flags set with VKEY_EXTRA_INFO
    constexpr uint32_t kBothFlags = kInjectedMask;
    EXPECT_FALSE(IsExternalInjectedKey(kBothFlags, kVKeyExtraInfo, kVKeyExtraInfo));
}

// ── External injected keys (PhraseExpress, AutoHotkey, etc.) ──────────
TEST(ExternalInjectedKeyDecision, ExternalInjectedKey_ClassifiedCorrectly) {
    // Normal SendInput injection (flags=0x10, dwExtraInfo=0)
    EXPECT_TRUE(IsExternalInjectedKey(kInjectedFlagDefault, 0, kVKeyExtraInfo));

    // External tool with its own custom dwExtraInfo (e.g. 0x9999)
    EXPECT_TRUE(IsExternalInjectedKey(kInjectedFlagDefault, 0x9999, kVKeyExtraInfo));

    // Lower integrity level injection (UIA / UIPI, flags=0x02)
    EXPECT_TRUE(IsExternalInjectedKey(kInjectedFlagLowerIntegrity, 0, kVKeyExtraInfo));

    // KeyUp event (flags=0x90 = LLKHF_UP | LLKHF_INJECTED)
    constexpr uint32_t kKeyUpInjected = 0x00000080u | kInjectedFlagDefault;
    EXPECT_TRUE(IsExternalInjectedKey(kKeyUpInjected, 0, kVKeyExtraInfo));
}

// ── Bypass Decision (Issue #274 vs #275) ──────────────────────────────
TEST(ExternalInjectedKeyDecision, Bypass_OnlyDeletionKeysFromExternalTools) {
    // External injected Backspace -> MUST bypass (PhraseExpress / AHK hotstrings, #274)
    EXPECT_TRUE(ShouldBypassAndResetOnInjected(/*isExternalInjected=*/true, kVkBack));

    // External injected Delete -> MUST bypass
    EXPECT_TRUE(ShouldBypassAndResetOnInjected(/*isExternalInjected=*/true, kVkDelete));

    // External injected printable typing keys -> MUST NOT bypass (#275, AutoHotkey / auto typing)
    EXPECT_FALSE(ShouldBypassAndResetOnInjected(/*isExternalInjected=*/true, kVkKeyA));
    EXPECT_FALSE(ShouldBypassAndResetOnInjected(/*isExternalInjected=*/true, kVkSpace));
    EXPECT_FALSE(ShouldBypassAndResetOnInjected(/*isExternalInjected=*/true, 'D'));
    EXPECT_FALSE(ShouldBypassAndResetOnInjected(/*isExternalInjected=*/true, 'F'));

    // Physical keys -> NEVER bypass
    EXPECT_FALSE(ShouldBypassAndResetOnInjected(/*isExternalInjected=*/false, kVkBack));
    EXPECT_FALSE(ShouldBypassAndResetOnInjected(/*isExternalInjected=*/false, kVkDelete));
    EXPECT_FALSE(ShouldBypassAndResetOnInjected(/*isExternalInjected=*/false, kVkKeyA));
}

// ── Composition Reset Decision ────────────────────────────────────────
TEST(ExternalInjectedKeyDecision, ResetComposition_OnlyOnKeyDownWithActiveState) {
    // Key-down with active engine composition on Backspace -> MUST reset
    EXPECT_TRUE(ShouldResetCompositionOnInjected(
        /*isExternalInjected=*/true,
        /*vkCode=*/kVkBack,
        /*isDown=*/true,
        /*engineCount=*/4,
        /*hasPreviousComposition=*/false));

    // Key-down with active engine composition on Delete -> MUST reset
    EXPECT_TRUE(ShouldResetCompositionOnInjected(
        /*isExternalInjected=*/true,
        /*vkCode=*/kVkDelete,
        /*isDown=*/true,
        /*engineCount=*/4,
        /*hasPreviousComposition=*/false));

    // Key-down with previous composition pending on Backspace -> MUST reset
    EXPECT_TRUE(ShouldResetCompositionOnInjected(
        /*isExternalInjected=*/true,
        /*vkCode=*/kVkBack,
        /*isDown=*/true,
        /*engineCount=*/0,
        /*hasPreviousComposition=*/true));

    // Key-down on printable typing key (e.g. 'A') -> NEVER reset on bypass check (#275)
    EXPECT_FALSE(ShouldResetCompositionOnInjected(
        /*isExternalInjected=*/true,
        /*vkCode=*/kVkKeyA,
        /*isDown=*/true,
        /*engineCount=*/4,
        /*hasPreviousComposition=*/false));

    // Key-up event -> NEVER reset (avoid duplicate reset on Down+Up pair)
    EXPECT_FALSE(ShouldResetCompositionOnInjected(
        /*isExternalInjected=*/true,
        /*vkCode=*/kVkBack,
        /*isDown=*/false,
        /*engineCount=*/4,
        /*hasPreviousComposition=*/true));

    // Key-down but engine and previous composition are already empty -> no reset needed
    EXPECT_FALSE(ShouldResetCompositionOnInjected(
        /*isExternalInjected=*/true,
        /*vkCode=*/kVkBack,
        /*isDown=*/true,
        /*engineCount=*/0,
        /*hasPreviousComposition=*/false));

    // Not an external injected key -> ShouldResetCompositionOnInjected returns false
    EXPECT_FALSE(ShouldResetCompositionOnInjected(
        /*isExternalInjected=*/false,
        /*vkCode=*/kVkBack,
        /*isDown=*/true,
        /*engineCount=*/4,
        /*hasPreviousComposition=*/true));
}
