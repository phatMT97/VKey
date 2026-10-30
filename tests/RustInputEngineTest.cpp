// VKey - RustInputEngine adapter tests
// SPDX-License-Identifier: GPL-3.0-only
//
// Covers the FFI plumbing in the adapter (UTF-16 widening, peek/commit/backspace,
// stub contract) — NOT Vietnamese typing correctness, which the engine repo owns.
//
// One exception: the English-guard escape tests re-run engine cases here because
// the engine's own tests inject a fixture word list, so only this side exercises
// them against the SCOWL set that actually ships.
//
// CMake copies the trusted vendored engine next to VKeyTests.

#ifdef VKEY_USE_RUST_ENGINE

#include <gtest/gtest.h>
#include <vkey_engine.h>  // VKEY_ENGINE_ABI_VERSION — gates the v7-only assertions

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include "core/engine/CommittedTextRestore.h"
#include "core/engine/RustInputEngine.h"

namespace NextKey {
namespace {

TEST(RustInputEngineTrustTest, VendoredLibraryPassesTrustChecks) {
    EXPECT_TRUE(RustInputEngine::LibraryAvailable())
        << ::testing::PrintToString(RustInputEngine::UnavailableReason());
}

class RustInputEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!RustInputEngine::LibraryAvailable()) {
            GTEST_SKIP() << "trusted vkey_engine library did not load";
        }
    }
};

TEST_F(RustInputEngineTest, TelexComposesAndCommits) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    engine.PushChar(L'a');
    engine.PushChar(L'a');
    EXPECT_EQ(engine.Peek(), L"â");  // â
    EXPECT_EQ(engine.Count(), 1u);

    EXPECT_EQ(engine.Commit(), L"â");
    EXPECT_TRUE(engine.Peek().empty());
    EXPECT_EQ(engine.Count(), 0u);
}

// The published engine pin predates this contract. Its source and FFI tests
// run in VKey-rs; exercise the adapter matrix against the local fixed artifact.
#ifdef VKEY_TEST_UNRELEASED_ENGINE
TEST_F(RustInputEngineTest, ContextualHornCompletionAndBackspaceThroughFfi) {
    const struct Case {
        const wchar_t* telex;
        const wchar_t* vni;
        const wchar_t* before;
        int backspaces;
        const wchar_t* after;
    } cases[] = {
        {L"uwo", L"u7o7", L"ươ", 0, L"ươ"},
        {L"uwow", L"u7o7", L"ươ", 0, L"ươ"},
        {L"uow", L"uo7", L"ươ", 0, L"ươ"},
        {L"huow", L"huo7", L"huơ", 0, L"huơ"},
        {L"thuowr", L"thuo73", L"thuở", 0, L"thuở"},
        {L"quow", L"quo7", L"quơ", 0, L"quơ"},
        {L"huowng", L"huo7ng", L"hương", 0, L"hương"},
        {L"thuowng", L"thuo7ng", L"thương", 0, L"thương"},
        {L"duongdwf", L"duong972", L"đường", 2, L"đườ"},
        {L"luoiws", L"luoi71", L"lưới", 1, L"lướ"},
        {L"duocdwj", L"duoc975", L"được", 1, L"đượ"},
        {L"duocdwj", L"duoc975", L"được", 2, L"đư"},
        {L"dadf", L"da92", L"đà", 1, L"đ"},
    };
    for (auto method : {InputMethod::Telex, InputMethod::SimpleTelex,
                        InputMethod::Combined, InputMethod::VNI}) {
        for (bool spellCheck : {false, true}) {
            TypingConfig config;
            config.inputMethod = method;
            config.spellCheckEnabled = spellCheck;
            config.spellSuggestEnabled = false;
            for (const auto& test : cases) {
                const std::wstring raw = method == InputMethod::VNI ? test.vni : test.telex;
                SCOPED_TRACE(::testing::PrintToString(raw));
                RustInputEngine engine(config);
                for (wchar_t key : raw) engine.PushChar(key);
                EXPECT_EQ(engine.Peek(), test.before);
                EXPECT_EQ(engine.PeekRaw(), raw);
                for (int i = 0; i < test.backspaces; ++i) engine.Backspace();
                EXPECT_EQ(engine.Peek(), test.after);
                EXPECT_EQ(engine.Commit(), test.after);
                EXPECT_TRUE(engine.Peek().empty());
            }
        }
    }
}

TEST_F(RustInputEngineTest, InvalidDistantStrokeStaysLiteralWithEitherRestorePolicy) {
    for (auto method : {InputMethod::Telex, InputMethod::SimpleTelex, InputMethod::Combined, InputMethod::VNI}) {
        for (bool suggest : {false, true}) {
            for (bool restore : {false, true}) {
                TypingConfig config;
                config.inputMethod = method;
                config.spellCheckEnabled = true;
                config.spellSuggestEnabled = suggest;
                config.autoRestoreEnabled = restore;
                RustInputEngine engine(config);
                const std::wstring raw = method == InputMethod::VNI ? L"dff9" : L"dffd";
                for (wchar_t ch : raw) engine.PushChar(ch);
                EXPECT_EQ(engine.Peek(), raw);
                EXPECT_EQ(engine.Commit(), raw);
            }
        }
    }
}

TEST_F(RustInputEngineTest, ScopedStrokeAndLocalRepetitionThroughFfi) {
    const struct Case {
        const wchar_t* telex;
        const wchar_t* vni;
        const wchar_t* expected;
    } cases[] = {
        {L"dddd", L"dd9", L"dđ"},
        {L"ddmd", L"d9md", L"đmd"},
        {L"dmdd", L"dmd9", L"dmđ"},
        {L"dffdd", L"dffd9", L"dffđ"},
        {L"hddldd", L"hd9ld9", L"hđlđ"},
        {L"dieud", L"dieu9", L"đieu"},
        {L"viecj", L"viec5", L"viẹc"},
    };
    for (auto method : {InputMethod::Telex, InputMethod::SimpleTelex,
                        InputMethod::Combined, InputMethod::VNI}) {
        for (bool spellCheck : {false, true}) {
            TypingConfig config;
            config.inputMethod = method;
            config.spellCheckEnabled = spellCheck;
            config.spellSuggestEnabled = false;
            for (const auto& test : cases) {
                RustInputEngine engine(config);
                const std::wstring raw = method == InputMethod::VNI ? test.vni : test.telex;
                SCOPED_TRACE(::testing::PrintToString(raw));
                for (wchar_t key : raw) engine.PushChar(key);
                EXPECT_EQ(engine.Peek(), test.expected);
                EXPECT_EQ(engine.PeekRaw(), raw);
                EXPECT_EQ(engine.Commit(), test.expected);
            }
        }
    }
}
#endif

TEST_F(RustInputEngineTest, ShiftedBracketsPreserveUppercaseAndEscapeThroughFfi) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    engine.PushChar(L'{');
    EXPECT_EQ(engine.Peek(), L"Ơ");
    engine.PushChar(L'{');
    EXPECT_EQ(engine.Peek(), L"{");

    engine.Reset();
    engine.PushChar(L'}');
    EXPECT_EQ(engine.Peek(), L"Ư");
    engine.PushChar(L'}');
    EXPECT_EQ(engine.Peek(), L"}");
}

TEST_F(RustInputEngineTest, BracketCaseIntentDoesNotCorruptPhysicalRawThroughFfi) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    engine.PushKey(L'[', true);
    EXPECT_EQ(engine.Peek(), L"Ơ");
    EXPECT_EQ(engine.PeekRaw(), L"[");
    engine.PushKey(L'[', true);
    EXPECT_EQ(engine.Peek(), L"[");
    EXPECT_EQ(engine.PeekRaw(), L"[[");

    engine.Reset();
    engine.PushKey(L'{', false);
    EXPECT_EQ(engine.Peek(), L"ơ");
    EXPECT_EQ(engine.PeekRaw(), L"{");
    engine.PushKey(L'{', false);
    EXPECT_EQ(engine.Peek(), L"{");
    EXPECT_EQ(engine.PeekRaw(), L"{{");
}

TEST_F(RustInputEngineTest, UserDefinedShiftedBracketsUseBaseBindingsThroughFfi) {
    TypingConfig config;
    config.inputMethod = InputMethod::UserDefined;
    config.customKeyMap[static_cast<uint8_t>(L'[')] = TypingAction::HornInsertO;
    config.customKeyMap[static_cast<uint8_t>(L']')] = TypingAction::HornInsertU;
    RustInputEngine engine(config);

    engine.PushChar(L'{');
    EXPECT_EQ(engine.Peek(), L"Ơ");
    engine.PushChar(L'{');
    EXPECT_EQ(engine.Peek(), L"{");

    engine.Reset();
    engine.PushChar(L'}');
    EXPECT_EQ(engine.Peek(), L"Ư");
    engine.PushChar(L'}');
    EXPECT_EQ(engine.Peek(), L"}");
}

TEST_F(RustInputEngineTest, UserDefinedCapsBracketPreservesBaseLiteralThroughFfi) {
    TypingConfig config;
    config.inputMethod = InputMethod::UserDefined;
    config.customKeyMap[static_cast<uint8_t>(L'[')] = TypingAction::HornInsertO;
    RustInputEngine engine(config);

    engine.PushKey(L'[', true);
    EXPECT_EQ(engine.Peek(), L"Ơ");
    EXPECT_EQ(engine.PeekRaw(), L"[");
    engine.PushKey(L'[', true);
    EXPECT_EQ(engine.Peek(), L"[");
    EXPECT_EQ(engine.PeekRaw(), L"[[");
}

TEST_F(RustInputEngineTest, ToneEscape_UppercaseR_Issue209Comment) {
    // #209 comment (Shzr0): "TeR" → "Tẻ", second R must escape → "TeR".
    // C++ TypingEngine passes this (TelexEngineTest.Escape_ToneHoi_UppercaseR).
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    RustInputEngine engine(config);

    engine.PushChar(L'T');
    engine.PushChar(L'e');
    engine.PushChar(L'R');
    EXPECT_EQ(engine.Peek(), L"Tẻ");
    engine.PushChar(L'R');
    EXPECT_EQ(engine.Peek(), L"TeR");
}

TEST_F(RustInputEngineTest, ToneEscape_MixedCase_rThenShiftR) {
    // #209 (2026-07-26): Te + r → Tẻ, then Shift+R must escape → "TeR".
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    RustInputEngine engine(config);

    engine.PushChar(L'T');
    engine.PushChar(L'e');
    engine.PushChar(L'r');
    EXPECT_EQ(engine.Peek(), L"Tẻ");
    engine.PushChar(L'R');
    EXPECT_EQ(engine.Peek(), L"TeR");
}

TEST_F(RustInputEngineTest, ReviveRawReplay_PreservesToneEscape) {
    // Invariant behind SeedRevivedWord()'s raw-replay branch (#209 Shift+R):
    // replaying the raw keys of a revived word keeps the tone escapable, so the
    // next tone key escapes on the FIRST press. Glyph-seeding via
    // SeedFromText(L"Tẻ") does NOT — this engine swallows that first press when
    // spell-suggest is on (engine-repo gap), which is why the TSF revive path
    // must pass MatchingRawForCommittedWord().
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    RustInputEngine engine(config);

    for (const wchar_t c : std::wstring(L"Ter")) engine.PushChar(c);  // raw replay
    ASSERT_EQ(engine.Peek(), L"Tẻ");
    engine.PushChar(L'R');
    EXPECT_EQ(engine.Peek(), L"TeR");
}

TEST_F(RustInputEngineTest, ToneEscape_LowercaseR_Parity) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    RustInputEngine engine(config);

    engine.PushChar(L't');
    engine.PushChar(L'e');
    engine.PushChar(L'r');
    EXPECT_EQ(engine.Peek(), L"tẻ");
    engine.PushChar(L'r');
    EXPECT_EQ(engine.Peek(), L"ter");
}

TEST_F(RustInputEngineTest, BackspaceShrinksComposition) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    engine.PushChar(L'x');
    engine.PushChar(L'i');
    engine.PushChar(L'n');
    EXPECT_EQ(engine.Peek(), L"xin");

    engine.Backspace();
    EXPECT_EQ(engine.Peek(), L"xi");
    EXPECT_EQ(engine.Count(), 2u);
}

TEST_F(RustInputEngineTest, ResetClearsComposition) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    engine.PushChar(L'a');
    engine.PushChar(L'a');
    ASSERT_FALSE(engine.Peek().empty());

    engine.Reset();
    EXPECT_TRUE(engine.Peek().empty());
    EXPECT_EQ(engine.Count(), 0u);
}

TEST_F(RustInputEngineTest, RawKeystrokesPreserved) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    engine.PushChar(L'a');
    engine.PushChar(L's');  // sắc tone on 'a'
    EXPECT_EQ(engine.Peek(), L"á");
    EXPECT_EQ(engine.PeekRaw(), L"as");
    EXPECT_EQ(engine.PeekRawView(), std::wstring_view(L"as"));
}

TEST_F(RustInputEngineTest, CommittedLiteralCodaReplayUsesEnginePhonology) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    RustInputEngine engine(config);

    for (const wchar_t c : std::wstring_view(L"nghieej")) engine.PushChar(c);
    engine.PushChar(L'n');
    engine.PushChar(L'z');
    EXPECT_EQ(engine.Peek(), L"nghiên");
}

TEST_F(RustInputEngineTest, CommittedLiteralCodaReplayVerdict) {
    // The verdict rides on the ABI v7 export, and the adapter deliberately
    // still loads a pre-v7 engine that lacks it (fail-closed to false). Skip
    // loudly rather than assert: against the vendored v6 artifact the verdict
    // is a no-op by design, and a green EXPECT here would be a lie.
    // if constexpr, not if: a plain constant comparison is C4127 under the
    // global /W4 /WX.
    if constexpr (VKEY_ENGINE_ABI_VERSION < 7u) {
        GTEST_SKIP() << "engine header pins ABI " << VKEY_ENGINE_ABI_VERSION
                     << "; committed-replay verdict needs v7";
    }
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    RustInputEngine engine(config);

    EXPECT_TRUE(engine.ShouldReplayCommittedKey(L"nghiee", L'n'));
    EXPECT_TRUE(engine.ShouldReplayCommittedKey(L"nghieej", L'n'));
    EXPECT_FALSE(engine.ShouldReplayCommittedKey(L"test", L'b'));
    EXPECT_TRUE(engine.Peek().empty());
}

TEST_F(RustInputEngineTest, ToneEscapeReported) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    engine.PushChar(L'a');
    engine.PushChar(L's');
    engine.PushChar(L's');  // repeated tone key escapes -> literal "as"
    EXPECT_EQ(engine.Peek(), L"as");
    EXPECT_TRUE(engine.IsToneEscaped());
}

TEST_F(RustInputEngineTest, RepeatedWModifierEscapeSurvivesEnglishWordTail) {
    TypingConfig config;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    config.autoRestoreEnabled = true;

    for (const InputMethod method : {
             InputMethod::Telex,
             InputMethod::SimpleTelex,
             InputMethod::Combined,
         }) {
        config.inputMethod = method;
        for (const auto& [raw, expected] : {
                 std::pair{std::wstring_view(L"dowwnload"), std::wstring_view(L"download")},
                 std::pair{std::wstring_view(L"powwershell"), std::wstring_view(L"powershell")},
             }) {
            RustInputEngine engine(config);
            for (const wchar_t c : raw) {
                engine.PushChar(c);
            }

            EXPECT_EQ(engine.Peek(), expected);
            EXPECT_TRUE(engine.IsToneEscaped());
            EXPECT_EQ(engine.PeekRaw(), raw);  // ESC restore still needs physical keys.
            EXPECT_EQ(engine.Commit(), expected);
        }
    }
}

TEST_F(RustInputEngineTest, AmbiguousToneEscapeResolvesPositionIssue248) {
    TypingConfig config;
    config.inputMethod = InputMethod::SimpleTelex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    config.autoRestoreEnabled = true;

    // Needs the sticky-escape engine (VKey-rs "stabilize ambiguous tone
    // escapes"); the earlier build restored the literal at `possi`.
    RustInputEngine progression(config);
    for (const wchar_t c : std::wstring_view(L"poss")) progression.PushChar(c);
    EXPECT_EQ(progression.Peek(), L"pos");
    progression.PushChar(L'i');
    EXPECT_EQ(progression.Peek(), L"posi");
    progression.PushChar(L't');
    EXPECT_EQ(progression.Peek(), L"posit");
    for (const wchar_t c : std::wstring_view(L"ion")) progression.PushChar(c);
    EXPECT_EQ(progression.Peek(), L"position");

    // The other half of the branch: `posib` stops being an English prefix, so
    // the physical double-s has to come back. Only the shipped SCOWL set can
    // prove that — the engine's own test injects a fixture word list.
    RustInputEngine literalRestore(config);
    for (const wchar_t c : std::wstring_view(L"possi")) literalRestore.PushChar(c);
    EXPECT_EQ(literalRestore.Peek(), L"posi");
    literalRestore.PushChar(L'b');
    EXPECT_EQ(literalRestore.Peek(), L"possib");
    for (const wchar_t c : std::wstring_view(L"le")) literalRestore.PushChar(c);
    EXPECT_EQ(literalRestore.Peek(), L"possible");

    RustInputEngine backspace(config);
    for (const wchar_t c : std::wstring_view(L"possi")) backspace.PushChar(c);
    EXPECT_EQ(backspace.Peek(), L"posi");
    backspace.Backspace();
    EXPECT_EQ(backspace.Peek(), L"pos");
    backspace.PushChar(L'i');
    EXPECT_EQ(backspace.Peek(), L"posi");

    for (const auto& [raw, expected] : {
             std::pair{std::wstring_view(L"position"), std::wstring_view(L"position")},
             std::pair{std::wstring_view(L"possition"), std::wstring_view(L"position")},
             std::pair{std::wstring_view(L"posSition"), std::wstring_view(L"position")},
             std::pair{std::wstring_view(L"poSsition"), std::wstring_view(L"position")},
             std::pair{std::wstring_view(L"poSSition"), std::wstring_view(L"position")},
             std::pair{std::wstring_view(L"PosSition"), std::wstring_view(L"Position")},
             std::pair{std::wstring_view(L"POSSITION"), std::wstring_view(L"POSITION")},
         }) {
        RustInputEngine engine(config);
        for (const wchar_t c : raw) {
            engine.PushChar(c);
        }

        EXPECT_EQ(engine.Peek(), expected);
        EXPECT_EQ(engine.PeekRaw(), raw);
        EXPECT_EQ(engine.Commit(), expected);
    }
}

TEST_F(RustInputEngineTest, ExactAsusEscapeRequiresSpellCheck) {
    TypingConfig guardedConfig;
    guardedConfig.inputMethod = InputMethod::SimpleTelex;
    guardedConfig.spellCheckEnabled = true;
    guardedConfig.spellSuggestEnabled = true;
    guardedConfig.autoRestoreEnabled = true;

    for (const std::wstring_view raw : {L"asus", L"ASUS", L"Asus"}) {
        RustInputEngine engine(guardedConfig);
        for (const wchar_t c : raw) engine.PushChar(c);
        EXPECT_EQ(engine.Peek(), raw);
        EXPECT_EQ(engine.PeekRaw(), raw);
    }

    TypingConfig ordinaryConfig = guardedConfig;
    ordinaryConfig.spellCheckEnabled = false;
    RustInputEngine ordinary(ordinaryConfig);
    for (const wchar_t c : std::wstring_view(L"asus")) ordinary.PushChar(c);
    EXPECT_EQ(ordinary.Peek(), L"asu");
}

TEST_F(RustInputEngineTest, EnglishWordFlag) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    RustInputEngine engine(config);

    ASSERT_TRUE(engine.SeedFromText(L"hỏc"));  // hook tone + stop coda -> not Vietnamese
    EXPECT_TRUE(engine.IsEnglishWord());

    ASSERT_TRUE(engine.SeedFromText(L"việt"));
    EXPECT_FALSE(engine.IsEnglishWord());
}

TEST_F(RustInputEngineTest, SeedFromTextLiteralRestore) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    ASSERT_TRUE(engine.SeedFromText(L"việt"));
    EXPECT_EQ(engine.Peek(), L"việt");
    EXPECT_EQ(engine.PeekRaw(), L"vieetj");
    EXPECT_EQ(engine.Count(), 4u);
    EXPECT_FALSE(engine.IsToneEscaped());  // seeding is not a tone escape
}

TEST_F(RustInputEngineTest, CorrectedCommitCanBeSeededBeforeLiteralSuffix) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    config.autoRestoreEnabled = true;
    RustInputEngine engine(config);

    // Keep this as a genuinely corrected commit. The key-conserving correction
    // policy now preserves the `a` in `sauwr` and correctly commits it as
    // "sửa", so that former fixture no longer exercises corrected-text seeding.
    for (const wchar_t c : std::wstring(L"suwrr")) engine.PushChar(c);
    const std::wstring committed = engine.Commit();
    ASSERT_EQ(committed, L"sử");
    ASSERT_TRUE(engine.LastCommitWasCorrected());

    const auto restored = RestoreCommittedText(
        engine, committed, [](IInputEngine& replayEngine) {
            for (const wchar_t c : std::wstring_view(L"suwrr")) replayEngine.PushChar(c);
        });
    ASSERT_EQ(restored, CommittedTextRestoreResult::SeededVisibleText);
    engine.PushChar(L'a');
    EXPECT_EQ(engine.Peek(), L"sửa");
}

TEST_F(RustInputEngineTest, CorrectedCommitCanBeEditedAcrossRepeatedReopen) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    config.autoRestoreEnabled = true;
    RustInputEngine engine(config);

    const auto reopenVisible = [&engine](const std::wstring_view history,
                                         const std::wstring& visible) {
        const auto restored = RestoreCommittedText(
            engine, visible, [history](IInputEngine& replayEngine) {
                for (const wchar_t c : history) replayEngine.PushChar(c);
            });
        EXPECT_NE(restored, CommittedTextRestoreResult::Failed);
        return restored;
    };

    for (const wchar_t c : std::wstring(L"suwrr")) engine.PushChar(c);
    const std::wstring corrected = engine.Commit();
    ASSERT_EQ(corrected, L"sử");

    EXPECT_EQ(reopenVisible(L"suwrr", corrected),
              CommittedTextRestoreResult::SeededVisibleText);
    engine.PushChar(L'a');
    ASSERT_EQ(engine.Commit(), L"sửa");
    EXPECT_FALSE(engine.LastCommitWasCorrected());

    // A text-seeded replay replaces the stale typo history with the visible
    // word. Reopening that edited commit must still let the user revise only
    // its suffix instead of deleting the whole word to escape stale state.
    EXPECT_EQ(reopenVisible(L"sửa", L"sửa"),
              CommittedTextRestoreResult::ReplayedHistory);
    engine.Backspace();
    engine.Backspace();
    for (const wchar_t c : std::wstring(L"uwax")) engine.PushChar(c);
    EXPECT_EQ(engine.Commit(), L"sữa");
    EXPECT_FALSE(engine.LastCommitWasCorrected());
}

TEST_F(RustInputEngineTest, BackspacedAttemptsNormalizeBeforeCommittedBackspace) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    // The first attempt is erased before typing the visible word. Replaying
    // this whole history renders the right text but leaves a stale undo stack:
    // the next Backspace can jump from "sửa" to "su" instead of deleting "a".
    const std::wstring history = L"saw\b\bsuawr";
    const auto restored = RestoreCommittedText(
        engine, L"sửa", [&history](IInputEngine& replayEngine) {
            for (const wchar_t c : history) {
                if (c == L'\b') {
                    replayEngine.Backspace();
                } else {
                    replayEngine.PushChar(c);
                }
            }
        },
        CommittedTextRestorePreference::VisibleText);

    ASSERT_EQ(restored, CommittedTextRestoreResult::SeededVisibleText);
    engine.Backspace();
    EXPECT_EQ(engine.Peek(), L"sử");
}

TEST_F(RustInputEngineTest, BackspacedAttemptsKeepRawReplayForToneEdit) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    const std::wstring history = L"saw\b\bsuawr";
    const auto restored = RestoreCommittedText(
        engine, L"sửa", [&history](IInputEngine& replayEngine) {
            for (const wchar_t c : history) {
                if (c == L'\b') {
                    replayEngine.Backspace();
                } else {
                    replayEngine.PushChar(c);
                }
            }
        });

    ASSERT_EQ(restored, CommittedTextRestoreResult::ReplayedHistory);
    engine.PushChar(L'x');
    EXPECT_EQ(engine.Peek(), L"sữa");
}

TEST_F(RustInputEngineTest, CommittedTextRestoreFailureLeavesEngineReset) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);

    const std::wstring beyondEngineCapacity(65, L'a');
    const auto restored = RestoreCommittedText(
        engine, beyondEngineCapacity, [](IInputEngine& replayEngine) {
            replayEngine.PushChar(L'x');
        });

    EXPECT_EQ(restored, CommittedTextRestoreResult::Failed);
    EXPECT_TRUE(engine.Peek().empty());
    EXPECT_EQ(engine.Count(), 0u);
}

TEST_F(RustInputEngineTest, SeedFromTextRejectsAdapterOverflowWithoutStaleState) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    RustInputEngine engine(config);
    engine.PushChar(L'x');

    const std::wstring beyondAdapterCapacity(257, L'a');
    EXPECT_FALSE(engine.SeedFromText(beyondAdapterCapacity));
    EXPECT_TRUE(engine.Peek().empty());
    EXPECT_EQ(engine.Count(), 0u);
}

TEST_F(RustInputEngineTest, QuickConsonantReported) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.quickConsonant = true;
    RustInputEngine engine(config);

    engine.PushChar(L'c');
    engine.PushChar(L'c');  // cc -> ch
    EXPECT_EQ(engine.Peek(), L"ch");
    EXPECT_TRUE(engine.HasActiveQuickConsonant());
}

TEST_F(RustInputEngineTest, LastCommitWasCorrectedReported) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    RustInputEngine engine(config);

    for (const wchar_t c : std::wstring(L"gnuwowif")) {
        engine.PushChar(c);
    }
    const std::wstring committed = engine.Commit();
    EXPECT_FALSE(committed.empty());
    EXPECT_TRUE(engine.LastCommitWasCorrected());
}

TEST_F(RustInputEngineTest, LastCommitWasCorrectedFalseWhenSpellSuggestDisabled) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = false;
    RustInputEngine engine(config);

    for (const wchar_t c : std::wstring(L"gnuwowif")) {
        engine.PushChar(c);
    }
    (void)engine.Commit();
    EXPECT_FALSE(engine.LastCommitWasCorrected());
}

// Regression: vkey_engine_set_spell_exclusions_utf16 is a process-global 2-arg
// (buf, len) FFI call with no engine handle. The adapter previously called it
// through a bogus 3-arg (engine, buf, len) function pointer, which shifts every
// argument register under the Win64 ABI -- Rust read the engine handle as `buf`
// and the real buffer's address as `len`, then read far out of bounds. That
// crashed instantly whenever config.spellExclusions was non-empty.
TEST_F(RustInputEngineTest, SpellExclusionsDoNotCrashConstruction) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    config.spellExclusions = {L"đcđt", L"hđ"};
    RustInputEngine engine(config);

    engine.PushChar(L'a');
    engine.PushChar(L'a');
    EXPECT_EQ(engine.Peek(), L"â");
}

// Proves the ABI v4 custom-keymap wiring end-to-end: a physical key remapped
// to a non-Telex/VNI action must actually apply that action through the Rust
// engine, not silently fall back to Telex (VKey-rs ADR-0007).
TEST_F(RustInputEngineTest, UserDefinedCustomKeymapAppliesRemappedAction) {
    TypingConfig config;
    config.inputMethod = InputMethod::UserDefined;
    config.customKeyMap[static_cast<uint8_t>(L'p')] = TypingAction::StrokeD;  // 'p' -> đ
    config.customKeyMap[static_cast<uint8_t>(L'q')] = TypingAction::ToneAcute;  // 'q' -> sắc
    RustInputEngine engine(config);

    for (const wchar_t c : std::wstring(L"dpaq")) {  // d, stroke(p), a, acute(q)
        engine.PushChar(c);
    }

    EXPECT_EQ(engine.Peek(), L"đá");
}

TEST_F(RustInputEngineTest, UserDefinedUnboundKeyStaysLiteral) {
    TypingConfig config;
    config.inputMethod = InputMethod::UserDefined;
    // No bindings installed: every key must type as itself.
    RustInputEngine engine(config);

    for (const wchar_t c : std::wstring(L"das")) {
        engine.PushChar(c);
    }

    EXPECT_EQ(engine.Peek(), L"das");
}

// Tier 2 (byte codes 18-34): Unikey-compatibility actions. Proves the newly
// vendored engine actually decodes these, not just the tier-1 Telex/VNI remap.
TEST_F(RustInputEngineTest, UserDefinedTier2DirectInsertAndHornOrInsertU) {
    TypingConfig config;
    config.inputMethod = InputMethod::UserDefined;
    config.customKeyMap[static_cast<uint8_t>(L'p')] = TypingAction::InsertDStroke;    // 'p' -> đ
    config.customKeyMap[static_cast<uint8_t>(L'y')] = TypingAction::HornOrInsertU;    // 'y' -> ư (or horn on u/o)
    RustInputEngine engine(config);

    engine.PushChar(L'p');
    EXPECT_EQ(engine.Peek(), L"đ");
    engine.PushChar(L'p');  // repeat escapes back to literal 'p'
    EXPECT_EQ(engine.Peek(), L"p");

    engine.Reset();
    engine.PushChar(L'y');  // no target -> standalone insert fallback
    EXPECT_EQ(engine.Peek(), L"ư");
}

// #221 parity: vowel-less abbreviation chain (PLHĐ) must compose despite the
// pl- hard-English onset; a vowel in the buffer keeps the block (pladd literal).
// Doubled-modifier escape consumes the third `a`; the verbatim literal
// fallback must not replay it. Also guards against a stale vendored engine
// binary — this passed in VKey-rs source while the shipped .so/.dll lagged.
// C++ twin: TelexEngineTest.Escape_Circumflex_A.
TEST_F(RustInputEngineTest, CircumflexEscapeStaysOutOfLiteralFallback) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    RustInputEngine engine(config);

    for (wchar_t c : std::wstring(L"aaaccj")) engine.PushChar(c);
    EXPECT_EQ(engine.Peek(), L"aaccj");

    engine.Reset();
    for (wchar_t c : std::wstring(L"aaardvark")) engine.PushChar(c);
    EXPECT_EQ(engine.Peek(), L"aardvark");
}

// C++ twin: TelexEngineTest.StrokeD_AbbrevChain_*.
TEST_F(RustInputEngineTest, StrokeD_AbbrevChain_PLHD_Parity) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;
    RustInputEngine engine(config);

    for (wchar_t c : std::wstring(L"plhdd")) engine.PushChar(c);
    EXPECT_EQ(engine.Peek(), L"plhđ");

    engine.Reset();
    for (wchar_t c : std::wstring(L"pladd")) engine.PushChar(c);
    EXPECT_EQ(engine.Peek(), L"pladd");
}

#if VKEY_ENGINE_ABI_VERSION >= 8u
class RustUserDictionaryFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!RustInputEngine::LibraryAvailable()) {
            GTEST_SKIP() << "trusted ABI-v8 vkey_engine library did not load";
        }
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path()
              / (L"vkey-user-dictionary-test-" + std::to_wstring(nonce));
        std::filesystem::create_directories(root_);
        configPath_ = root_ / L"config.toml";
        dictionaryPath_ = root_ / L"user_dictionary.txt";
    }

    void TearDown() override {
        std::error_code ignored;
        if (!root_.empty()) std::filesystem::remove_all(root_, ignored);
    }

    void WriteDictionary(std::string_view bytes) {
        std::ofstream output(dictionaryPath_, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.good());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good());
    }

    static TypingConfig AdvancedConfig() {
        TypingConfig config;
        config.inputMethod = InputMethod::Telex;
        config.spellCheckEnabled = true;
        config.spellSuggestEnabled = true;
        config.autoRestoreEnabled = true;
        return config;
    }

    static std::wstring CommitRaw(
        const std::shared_ptr<const RustUserDictionarySnapshot>& snapshot,
        bool* corrected = nullptr) {
        RustInputEngine engine(AdvancedConfig());
        EXPECT_TRUE(engine.SetUserDictionary(snapshot));
        for (const wchar_t c : std::wstring_view(L"gnuwowif")) engine.PushChar(c);
        std::wstring committed = engine.Commit();
        if (corrected) *corrected = engine.LastCommitWasCorrected();
        return committed;
    }

    std::filesystem::path root_;
    std::filesystem::path configPath_;
    std::filesystem::path dictionaryPath_;
};

TEST_F(RustUserDictionaryFileTest, MissingFileLoadsEmptySnapshotWithoutCreatingFile) {
    ASSERT_FALSE(std::filesystem::exists(dictionaryPath_));

    const auto loaded = RustInputEngine::LoadUserDictionary(configPath_.wstring());

    ASSERT_TRUE(loaded.Succeeded());
    EXPECT_FALSE(loaded.created);
    EXPECT_FALSE(std::filesystem::exists(dictionaryPath_));
    ASSERT_NE(loaded.snapshot, nullptr);

    bool corrected = true;
    // An empty dictionary provides no protection against correction of unknown/broken words
    EXPECT_NE(CommitRaw(loaded.snapshot, &corrected), L"gnuwowif");
    EXPECT_TRUE(corrected);
}

TEST_F(RustUserDictionaryFileTest, ExplicitCreateCreatesTemplateAndIsIdempotent) {
    ASSERT_FALSE(std::filesystem::exists(dictionaryPath_));

    bool created = false;
    EXPECT_TRUE(RustInputEngine::CreateUserDictionaryTemplate(configPath_.wstring(), &created));
    EXPECT_TRUE(created);
    EXPECT_TRUE(std::filesystem::exists(dictionaryPath_));

    std::ifstream input(dictionaryPath_, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("https://github.com/phatMT97/VKey/issues"), std::string::npos);

    // Second call is idempotent: does not overwrite or fail
    created = true;
    EXPECT_TRUE(RustInputEngine::CreateUserDictionaryTemplate(configPath_.wstring(), &created));
    EXPECT_FALSE(created);
    EXPECT_TRUE(std::filesystem::exists(dictionaryPath_));
}

TEST_F(RustUserDictionaryFileTest, EmptyFileIsValidAndClearsProtection) {
    WriteDictionary("gnuwowif\n");
    const auto protectedWords = RustInputEngine::LoadUserDictionary(configPath_.wstring());
    ASSERT_TRUE(protectedWords.Succeeded());
    bool corrected = true;
    EXPECT_EQ(CommitRaw(protectedWords.snapshot, &corrected), L"gnuwowif");
    EXPECT_FALSE(corrected);

    WriteDictionary("");
    const auto empty = RustInputEngine::LoadUserDictionary(configPath_.wstring());
    ASSERT_TRUE(empty.Succeeded());
    EXPECT_FALSE(empty.created);
    EXPECT_NE(CommitRaw(empty.snapshot, &corrected), L"gnuwowif");
    EXPECT_TRUE(corrected);
}

TEST_F(RustUserDictionaryFileTest, InvalidEditLetsHostRetainLastValidSnapshot) {
    WriteDictionary("gnuwowif\n");
    const auto valid = RustInputEngine::LoadUserDictionary(configPath_.wstring());
    ASSERT_TRUE(valid.Succeeded());

    WriteDictionary(std::string_view("\xF0\x28\x8C\x28", 4));
    const auto invalid = RustInputEngine::LoadUserDictionary(configPath_.wstring());
    EXPECT_FALSE(invalid.Succeeded());
    EXPECT_EQ(invalid.status, RustUserDictionaryLoadStatus::InvalidUtf8);

    // This is the host fail-stale contract: no replacement was published, so
    // the previous immutable snapshot remains attachable and effective.
    bool corrected = true;
    EXPECT_EQ(CommitRaw(valid.snapshot, &corrected), L"gnuwowif");
    EXPECT_FALSE(corrected);
}

TEST_F(RustUserDictionaryFileTest, DecodedUtf16LimitIsReportedAsPayloadTooLarge) {
    WriteDictionary(std::string(VKEY_USER_DICTIONARY_MAX_UTF16_UNITS + 1, 'a'));

    const auto oversized = RustInputEngine::LoadUserDictionary(configPath_.wstring());

    EXPECT_FALSE(oversized.Succeeded());
    EXPECT_EQ(oversized.status, RustUserDictionaryLoadStatus::EngineRejected);
    EXPECT_EQ(oversized.engineStatus, VKEY_USER_DICTIONARY_PAYLOAD_TOO_LARGE);
}

TEST_F(RustUserDictionaryFileTest, AttachWaitsForAnEmptyComposition) {
    WriteDictionary("alo\n");
    const auto loaded = RustInputEngine::LoadUserDictionary(configPath_.wstring());
    ASSERT_TRUE(loaded.Succeeded());
    RustInputEngine engine(AdvancedConfig());
    engine.PushChar(L'a');

    EXPECT_FALSE(engine.SetUserDictionary(loaded.snapshot));
    EXPECT_EQ(engine.Peek(), L"a");
}

TEST_F(RustInputEngineTest, SerializedCreationAndSpellExclusionCanonicalization) {
    TypingConfig config;
    config.spellCheckEnabled = true;
    config.spellExclusions = {L" ZÔ ", L"rose", L"zo\u0302", L"HĐ"};

    RustInputEngine engine(config);
    EXPECT_EQ(engine.Count(), 0u);

    TypingConfig emptyConfig;
    emptyConfig.spellCheckEnabled = true;
    RustInputEngine engine2(emptyConfig);
    EXPECT_EQ(engine2.Count(), 0u);
}

TEST_F(RustInputEngineTest, CreateUserDictionaryFromUtf16DirectCompileAndAttach) {
    std::u16string dictText = u"alo\nban\n";
    auto snapshot = RustInputEngine::CreateUserDictionaryFromUtf16(
        reinterpret_cast<const uint16_t*>(dictText.data()), dictText.size());
    ASSERT_NE(snapshot, nullptr);

    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    RustInputEngine engine(config);

    EXPECT_TRUE(engine.SetUserDictionary(snapshot));
    // Clear protection with null snapshot
    EXPECT_TRUE(engine.SetUserDictionary(nullptr));
}

TEST_F(RustInputEngineTest, SetSpellExclusionsFromUtf16PlumbingAndDeduplication) {
    std::u16string exclText = u"msword\nchrome\n";
    bool changed = false;
    EXPECT_TRUE(RustInputEngine::SetSpellExclusionsFromUtf16(
        reinterpret_cast<const uint16_t*>(exclText.data()), exclText.size(), &changed));
    EXPECT_TRUE(changed);

    // Call again with same text: no change
    changed = true;
    EXPECT_TRUE(RustInputEngine::SetSpellExclusionsFromUtf16(
        reinterpret_cast<const uint16_t*>(exclText.data()), exclText.size(), &changed));
    EXPECT_FALSE(changed);

    // Clear
    EXPECT_TRUE(RustInputEngine::SetSpellExclusionsFromUtf16(nullptr, 0, &changed));
    EXPECT_TRUE(changed);
}

TEST_F(RustInputEngineTest, TestSoafUserDictionary) {
    std::u16string dictText = u"# VKey User Dictionary\n# One word per line\nkh\u01b0m\nso\u00e0\n";
    auto snapshot = RustInputEngine::CreateUserDictionaryFromUtf16(
        reinterpret_cast<const uint16_t*>(dictText.data()), dictText.size());
    ASSERT_NE(snapshot, nullptr);

    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = true;
    config.spellSuggestEnabled = true;

    // Test without user dictionary: words get auto-corrected
    {
        RustInputEngine engine(config);
        for (wchar_t c : std::wstring_view(L"soaf")) engine.PushChar(c);
        EXPECT_EQ(engine.Peek(), L"so\u00e0");
        EXPECT_EQ(engine.Commit(), L"s\u00e0o");
        const bool correctedWithoutDict = engine.LastCommitWasCorrected();
        EXPECT_TRUE(correctedWithoutDict);
    }
    {
        RustInputEngine engine(config);
        for (wchar_t c : std::wstring_view(L"khuwm")) engine.PushChar(c);
        EXPECT_EQ(engine.Peek(), L"kh\u01b0m");
        EXPECT_EQ(engine.Commit(), L"khum");
        const bool correctedWithoutDict = engine.LastCommitWasCorrected();
        EXPECT_TRUE(correctedWithoutDict);
    }

    // Test WITH user dictionary: words are protected and preserved!
    {
        RustInputEngine engine(config);
        EXPECT_TRUE(engine.SetUserDictionary(snapshot));
        for (wchar_t c : std::wstring_view(L"soaf")) engine.PushChar(c);
        EXPECT_EQ(engine.Peek(), L"so\u00e0");
        EXPECT_EQ(engine.Commit(), L"so\u00e0");
        const bool correctedWithDict = engine.LastCommitWasCorrected();
        EXPECT_FALSE(correctedWithDict);
    }
    {
        RustInputEngine engine(config);
        EXPECT_TRUE(engine.SetUserDictionary(snapshot));
        for (wchar_t c : std::wstring_view(L"khuwm")) engine.PushChar(c);
        EXPECT_EQ(engine.Peek(), L"kh\u01b0m");
        EXPECT_EQ(engine.Commit(), L"kh\u01b0m");
        const bool correctedWithDict = engine.LastCommitWasCorrected();
        EXPECT_FALSE(correctedWithDict);
    }
}

#endif

}  // namespace
}  // namespace NextKey

#endif  // VKEY_USE_RUST_ENGINE
