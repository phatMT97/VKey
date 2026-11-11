// VKey - delayed focus output regression tests
// SPDX-License-Identifier: GPL-3.0-only

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "InjectorTestBase.h"

#include "app/system/FocusOwner.h"
#include "app/system/HookCommandMailbox.h"
#include "app/system/OutputDispatcher.h"
#include "core/FocusApplyDecision.h"
#include "core/engine/TypingEngine.h"

#include <string>

namespace NextKey::Output::Test {
namespace {

class FocusOutputTest : public InjectorTestBase {
protected:
    FocusOwner focus_;
    OutputDispatcher dispatcher_{focus_};

    static FocusClassification Prepare(bool browser, bool game) {
        FocusClassification cls{};
        cls.hwndOpaque = 0x1234;
        cls.pid = 10;
        cls.requestSerial = 7;
        cls.inputEpochAtRequest = 12;
        cls.localSuggestionDismissal = browser ? SuggestionDismissalPolicy::UnicodeBait
                                             : SuggestionDismissalPolicy::None;
        cls.localGameReinject = game;
        OutputDispatcher::PrepareFocusOutput(cls);
        return cls;
    }

    static FocusApplyDisposition DelayedDecision() {
        return DecideFocusApply({
            .snapshotRequestSerial = 7,
            .latestRequestSerial = 7,
            .snapshotInputEpoch = 12,
            .currentInputEpoch = 13,
            .hasComposition = true,
        });
    }

    // Model only inline selection: the first edit removes the suggested
    // suffix; a Backspace against that selection leaves the typed prefix.
    static std::wstring ReplayInlineSuggestion(std::wstring prefix,
                                               bool hasSuggestedSuffix = true) {
        for (const auto& input : capturedInputs) {
            if (input.ki.dwFlags & KEYEVENTF_KEYUP) continue;
            if (input.ki.dwFlags & KEYEVENTF_UNICODE) {
                hasSuggestedSuffix = false;
                prefix.push_back(static_cast<wchar_t>(input.ki.wScan));
            } else if (input.ki.wVk == VK_BACK) {
                if (hasSuggestedSuffix) hasSuggestedSuffix = false;
                else if (!prefix.empty()) prefix.pop_back();
            }
        }
        return prefix;
    }
};

TEST_F(FocusOutputTest, DelayedBrowserUsesAutocompleteBaitBeforeWordBoundary) {
    const auto game = Prepare(false, true);
    ASSERT_TRUE(dispatcher_.ApplyFocusOutput(
        game, FocusApplyDisposition::ApplyNow, false));
    ASSERT_TRUE(dispatcher_.WantsGameReinject());

    TypingConfig cfg{};
    cfg.inputMethod = InputMethod::SimpleTelex;
    TypingEngine engine(cfg);
    engine.PushChar(L'd');
    ASSERT_EQ(engine.Peek(), L"d");

    const auto browser = Prepare(true, false);
    const auto disposition = DelayedDecision();
    ASSERT_EQ(disposition, FocusApplyDisposition::DeferUntilBoundary);
    EXPECT_TRUE(dispatcher_.ApplyFocusOutput(browser, disposition, false));
    EXPECT_FALSE(dispatcher_.WantsGameReinject());
    ASSERT_EQ(engine.Peek(), L"d");

    engine.PushChar(L'd');
    ASSERT_EQ(engine.Peek(), L"đ");
    ASSERT_TRUE(dispatcher_.GetInjector()->Replace(1, engine.Peek()));
    EXPECT_EQ(ReplayInlineSuggestion(L"d"), L"đ");
}

TEST_F(FocusOutputTest, StaleGameResultCannotRemoveBrowserProtection) {
    const auto browser = Prepare(true, false);
    ASSERT_TRUE(dispatcher_.ApplyFocusOutput(
        browser, FocusApplyDisposition::ApplyNow, false));
    const auto originalInjector = dispatcher_.GetInjector();
    const auto game = Prepare(false, true);
    const auto stale = DecideFocusApply({
        .snapshotRequestSerial = 6,
        .latestRequestSerial = 7,
        .hasComposition = true,
    });

    EXPECT_FALSE(dispatcher_.ApplyFocusOutput(game, stale, true));
    EXPECT_EQ(dispatcher_.GetInjector(), originalInjector);
    EXPECT_FALSE(dispatcher_.WantsGameReinject());
    EXPECT_FALSE(originalInjector->GetSuggestKeepChars());
    ASSERT_TRUE(originalInjector->Replace(1, L"đ"));
    EXPECT_EQ(ReplayInlineSuggestion(L"d"), L"đ");
}

TEST_F(FocusOutputTest, DelayedGameRestoresItsCompleteOutputPolicy) {
    auto previous = Prepare(true, false);
    previous.localSkipEmpty = true;
    previous.localClipboard = true;
    ASSERT_TRUE(dispatcher_.ApplyFocusOutput(
        previous, FocusApplyDisposition::ApplyNow, false));

    const auto game = Prepare(false, true);
    EXPECT_TRUE(dispatcher_.ApplyFocusOutput(game, DelayedDecision(), false));
    EXPECT_TRUE(dispatcher_.WantsGameReinject());
    EXPECT_FALSE(dispatcher_.SkipEmptyChar());
    EXPECT_FALSE(dispatcher_.ShouldUseClipboard(CodeTable::Unicode));
    EXPECT_FALSE(dispatcher_.GetInjector()->NeedsBaitCharPrefix());
}

TEST_F(FocusOutputTest, DelayedOutputKeepsLiveSettingsAndSyntheticWordState) {
    const auto browser = Prepare(true, false);
    dispatcher_.SetHadSynthInWord(true);
    EXPECT_TRUE(dispatcher_.ApplyFocusOutput(browser, DelayedDecision(), true));
    EXPECT_TRUE(dispatcher_.HadSynthInWord());
    EXPECT_TRUE(dispatcher_.GetInjector()->GetSuggestKeepChars());

    // The strict-field opt-out deliberately omits suggestion dismissal.
    // Verify replacement in a field without an inline suggested suffix.
    ASSERT_TRUE(dispatcher_.GetInjector()->Replace(1, L"đ"));
    ASSERT_EQ(capturedInputs.size(), 4u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[2].ki.wScan, L'đ');
    EXPECT_EQ(ReplayInlineSuggestion(L"d", false), L"đ");
}

TEST_F(FocusOutputTest, UnpreparedResultLeavesCurrentOutputUntouched) {
    const auto originalInjector = dispatcher_.GetInjector();
    const FocusClassification unprepared{};
    EXPECT_FALSE(dispatcher_.ApplyFocusOutput(
        unprepared, FocusApplyDisposition::ApplyNow, true));
    EXPECT_EQ(dispatcher_.GetInjector(), originalInjector);
}

TEST_F(FocusOutputTest, NativePolicySurvivesDispatcherAndFactoryForBothChannels) {
    for (int splitSleep : {0, 6}) {
        capturedInputs.clear();
        FocusClassification cls{};
        cls.localSuggestionDismissal = SuggestionDismissalPolicy::NativePrintablePrefix;
        cls.localForcedSplitSleepMs = splitSleep;
        OutputDispatcher::PrepareFocusOutput(cls);
        ASSERT_TRUE(dispatcher_.ApplyFocusOutput(
            cls, FocusApplyDisposition::ApplyNow, true));
        EXPECT_FALSE(dispatcher_.WantsGameReinject());
        ASSERT_TRUE(dispatcher_.GetInjector()->Replace(1, L"đ"));
        ASSERT_EQ(capturedInputs.size(), 8u);
        EXPECT_EQ(capturedInputs[0].ki.wVk, kNativePrintablePrefixVk);
        EXPECT_EQ(capturedInputs[0].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
        EXPECT_EQ(capturedInputs[2].ki.wVk, VK_BACK);
        EXPECT_EQ(capturedInputs[4].ki.wVk, VK_BACK);
        EXPECT_EQ(capturedInputs[6].ki.wScan, L'đ');
    }
}

}  // namespace
}  // namespace NextKey::Output::Test
