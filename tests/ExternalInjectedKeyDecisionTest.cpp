// Tests for IsExternalInjectedKey and DecideInjectedDeletionAction — pure
// decision for external macro tool conflict resolution (issue #274).
// HookEngine.cpp is Windows-only, so we test the extracted decision in isolation
// (same pattern as LeakedKeyDuringSendDecisionTest).
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "core/ExternalInjectedKeyDecision.h"

using NextKey::DecideInjectedDeletionAction;
using NextKey::InjectedDeletionAction;
using NextKey::IsExternalInjectedKey;
using NextKey::ShouldBypassAndResetOnInjected;
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
TEST(ExternalInjectedKeyDecision, InjectedDeletionAction_KeyUp_PassThroughOnly) {
    // Key-up must never alter state regardless of key, composition, or sending state
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vieet",
            /*newComposition=*/L"viee",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/false),
        InjectedDeletionAction::PassThroughOnly);

    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vieet",
            /*newComposition=*/L"viee",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkDelete,
            /*isDown=*/false),
        InjectedDeletionAction::PassThroughOnly);
}

TEST(ExternalInjectedKeyDecision, InjectedDeletionAction_IsSending_PassThroughOnly) {
    // Re-entrancy guard: when injector is sending, touch NO state
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vieet",
            /*newComposition=*/L"viee",
            /*isUnicodeTable=*/true,
            /*isSending=*/true,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::PassThroughOnly);

    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vieet",
            /*newComposition=*/L"viee",
            /*isUnicodeTable=*/true,
            /*isSending=*/true,
            /*vk=*/kVkDelete,
            /*isDown=*/true),
        InjectedDeletionAction::PassThroughOnly);
}

TEST(ExternalInjectedKeyDecision, InjectedDeletionAction_SyncEngine_OnPureOneCharDeletion) {
    // Pure one-character deletion in Unicode table keeps engine in sync with screen
    // e.g. TeamViewer / OSK: "vieet" -> Backspace deletes 't' -> "viee"
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vieet",
            /*newComposition=*/L"viee",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::SyncEngine);

    // Vietnamese diacritic: "viêt" -> BS deletes 't' -> "viê"
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"viêt",
            /*newComposition=*/L"viê",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::SyncEngine);

    // Single character word: "a" -> BS deletes 'a' -> ""
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"a",
            /*newComposition=*/L"",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::SyncEngine);

    // "toán" -> BS deletes 'n' -> "toá"
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"toán",
            /*newComposition=*/L"toá",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::SyncEngine);
}

TEST(ExternalInjectedKeyDecision, InjectedDeletionAction_Reset_OnReRenderOrToneMove) {
    // Tone change or shift: e.g. "toán" -> "toan" is not a pure tail deletion
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"toán",
            /*newComposition=*/L"toan",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::Reset);

    // Tone change: "việ" -> "viê" (same length, tone removed)
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"việ",
            /*newComposition=*/L"viê",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::Reset);

    // Multi-char deletion
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vieet",
            /*newComposition=*/L"vie",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::Reset);

    // Different prefix
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vieet",
            /*newComposition=*/L"xiee",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::Reset);

    // Character appended rather than deleted
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vie",
            /*newComposition=*/L"viee",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::Reset);
}

TEST(ExternalInjectedKeyDecision, InjectedDeletionAction_Reset_OnEmptyCompositionOrDeleteOrNonUnicode) {
    // Empty composition MUST reset unconditionally to disarm undo state
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"",
            /*newComposition=*/L"",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::Reset);

    // VK_DELETE always resets unconditionally
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vieet",
            /*newComposition=*/L"viee",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkDelete,
            /*isDown=*/true),
        InjectedDeletionAction::Reset);

    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"",
            /*newComposition=*/L"",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkDelete,
            /*isDown=*/true),
        InjectedDeletionAction::Reset);

    // Non-Unicode code table (multi-unit encoding) always resets unconditionally
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vieet",
            /*newComposition=*/L"viee",
            /*isUnicodeTable=*/false,
            /*isSending=*/false,
            /*vk=*/kVkBack,
            /*isDown=*/true),
        InjectedDeletionAction::Reset);
}

TEST(ExternalInjectedKeyDecision, InjectedDeletionAction_PassThroughOnly_OnNonDeletionKey) {
    // Non-deletion keys are not handled by deletion actions
    EXPECT_EQ(
        DecideInjectedDeletionAction(
            /*prevComposition=*/L"vieet",
            /*newComposition=*/L"viee",
            /*isUnicodeTable=*/true,
            /*isSending=*/false,
            /*vk=*/kVkKeyA,
            /*isDown=*/true),
        InjectedDeletionAction::PassThroughOnly);
}
