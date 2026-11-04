// tests/output/OutputInjectorFactoryTest.cpp
//
// Unit tests for OutputInjectorFactory::Create dispatch. Covers the
// classification branches (RichEditD2DPT, Electron, Console, per-app
// forced compat split, Win32 default). dynamic_cast asserts the right
// concrete impl is returned; trait getters assert param inheritance.
//
// Why this exists (Gotcha G3 from D2 handoff): a previous attempt
// shipped Create() still hardcoded to Win32SendInputInjector while
// the chaos sweep passed by coincidence. A factory smoke test catches
// that class of bug before chaos.
//
// Spec: docs/plans/sprint-2-output-injector.md §5.2
#include "app/output/OutputInjectorFactory.h"
#include "app/output/Win32SendInputInjector.h"
#include "app/output/RichEditEmReplaceSelInjector.h"
#include "app/output/SplitDispatchInjector.h"

#include <gtest/gtest.h>

namespace NextKey::Output::Test {

TEST(OutputInjectorFactoryTest, DefaultClassificationReturnsWin32) {
    WindowClassification c{};  // all flags false
    auto inj = Create(c);
    ASSERT_NE(inj, nullptr);
    EXPECT_NE(dynamic_cast<Win32SendInputInjector*>(inj.get()), nullptr);
}

TEST(OutputInjectorFactoryTest, ChromiumFlagStillWin32ButCarriesBaitHint) {
    WindowClassification c{};
    c.suggestionDismissal = SuggestionDismissalPolicy::UnicodeBait;
    auto inj = Create(c);
    ASSERT_NE(inj, nullptr);
    EXPECT_NE(dynamic_cast<Win32SendInputInjector*>(inj.get()), nullptr);
    // (Bait-char prefix behavior verified separately in
    // Win32SendInputInjectorTest.BaitCharFiresOnReplaceWithText.)
}

TEST(OutputInjectorFactoryTest, RichEditD2DPTReturnsRichEditImpl) {
    WindowClassification c{};
    c.isRichEditD2DPT = true;
    auto inj = Create(c);
    ASSERT_NE(inj, nullptr);
    EXPECT_NE(dynamic_cast<RichEditEmReplaceSelInjector*>(inj.get()), nullptr);
}

TEST(OutputInjectorFactoryTest, ElectronReturnsSplitDispatch) {
    WindowClassification c{};
    c.isElectron = true;
    auto inj = Create(c);
    ASSERT_NE(inj, nullptr);
    EXPECT_NE(dynamic_cast<SplitDispatchInjector*>(inj.get()), nullptr);
}

TEST(OutputInjectorFactoryTest, ConsoleReturnsSplitDispatch) {
    WindowClassification c{};
    c.isConsole = true;
    auto inj = Create(c);
    ASSERT_NE(inj, nullptr);
    EXPECT_NE(dynamic_cast<SplitDispatchInjector*>(inj.get()), nullptr);
}

TEST(OutputInjectorFactoryTest, RichEditWinsOverElectronAndConsole) {
    // Priority order: RichEdit > Electron > Console > Win32. A pathological
    // classification with all three flags must dispatch to RichEdit.
    WindowClassification c{};
    c.isRichEditD2DPT = true;
    c.isElectron = true;
    c.isConsole = true;
    auto inj = Create(c);
    ASSERT_NE(inj, nullptr);
    EXPECT_NE(dynamic_cast<RichEditEmReplaceSelInjector*>(inj.get()), nullptr);
}

TEST(OutputInjectorFactoryTest, ElectronWinsOverConsole) {
    WindowClassification c{};
    c.isElectron = true;
    c.isConsole = true;
    auto inj = Create(c);
    ASSERT_NE(inj, nullptr);
    EXPECT_NE(dynamic_cast<SplitDispatchInjector*>(inj.get()), nullptr);
    // Could additionally check sleepMs by exposing a getter, but the
    // dispatch type is the contract — sleepMs is internal.
}

// Per-app "send method = compatibility split" (AppOverrideEntry::sendMethod
// 2/3, resolved to an inter-batch sleep in FocusOwner). A forced sleep > 0
// routes the app through SplitDispatchInjector instead of the Win32 batch
// path — mitigates the char-drop race on Firefox-family (sendMethod=2) and
// over cloud/remote desktop where the RDP round-trip stretches the gap
// (sendMethod=3, issue #178). Background:
// docs/plans/firefox-escape-hatch-spike/firefox-voz-sticking-chars-investigation.md.
TEST(OutputInjectorFactoryTest, ForcedSplitReturnsSplitDispatch) {
    WindowClassification c{};
    c.forcedSplitSleepMs = 6;  // sendMethod=2 (Firefox-compat)
    auto inj = Create(c);
    ASSERT_NE(inj, nullptr);
    EXPECT_NE(dynamic_cast<SplitDispatchInjector*>(inj.get()), nullptr);
}

TEST(OutputInjectorFactoryTest, ForcedEmReplaceSelReturnsRichEditImpl) {
    WindowClassification c{};
    c.forceEmReplaceSel = true;
    c.isElectron = true;
    c.useClipboard = true;
    c.forcedSplitSleepMs = 25;
    auto inj = Create(c);
    ASSERT_NE(inj, nullptr);
    auto* richEdit = dynamic_cast<RichEditEmReplaceSelInjector*>(inj.get());
    ASSERT_NE(richEdit, nullptr);
    EXPECT_TRUE(richEdit->IsMessageBasedReplace());
    EXPECT_TRUE(richEdit->IsForced());
}

TEST(OutputInjectorFactoryTest, ForcedSplitZeroDefaultsToWin32) {
    // Default contract: an app with no compat override (forcedSplitSleepMs==0)
    // stays on the Win32 fast path, even if it carries the Chromium bait hint.
    WindowClassification c{};
    c.forcedSplitSleepMs = 0;
    c.suggestionDismissal = SuggestionDismissalPolicy::UnicodeBait;
    auto inj = Create(c);
    ASSERT_NE(inj, nullptr);
    EXPECT_NE(dynamic_cast<Win32SendInputInjector*>(inj.get()), nullptr);
}

TEST(OutputInjectorFactoryTest, ForcedSplitInheritsRendererTraits) {
    // Firefox parity: a browser with UnicodeBait but non-Electron app forced
    // to the compat split must keep the bait-char prefix (Firefox's URL-bar
    // autocomplete behaves like Chromium's) and stay single-process — matching
    // the pre-refactor Firefox path. Traits are derived, not hardcoded.
    WindowClassification c{};
    c.forcedSplitSleepMs = 6;
    c.suggestionDismissal = SuggestionDismissalPolicy::UnicodeBait;
    c.isElectron = false;
    auto inj = Create(c);  // keep alive — .get() on a temporary would dangle
    auto* split = dynamic_cast<SplitDispatchInjector*>(inj.get());
    ASSERT_NE(split, nullptr);
    EXPECT_TRUE(split->NeedsBaitCharPrefix());
    EXPECT_FALSE(split->HasMultiProcessRenderer());
}

TEST(OutputInjectorFactoryTest, ForcedSplitWinsOverElectronButInheritsMultiProc) {
    // Per-app explicit choice outranks auto-detected Electron, but inherits its
    // multi-process trait so the mid-word passthrough block isn't lost when a
    // user forces a bigger (cloud/remote) sleep on an Electron host.
    WindowClassification c{};
    c.forcedSplitSleepMs = 25;  // sendMethod=3 (Cloud/Remote)
    c.isElectron = true;
    c.suggestionDismissal = SuggestionDismissalPolicy::UnicodeBait;
    auto inj = Create(c);  // keep alive — .get() on a temporary would dangle
    auto* split = dynamic_cast<SplitDispatchInjector*>(inj.get());
    ASSERT_NE(split, nullptr);
    EXPECT_TRUE(split->HasMultiProcessRenderer());  // inherited from isElectron
    EXPECT_TRUE(split->NeedsBaitCharPrefix());       // inherited from suggestionDismissal
}

TEST(OutputInjectorFactoryTest, SettleBudgetReflectsImpl) {
    using namespace std::chrono_literals;

    WindowClassification c{};
    EXPECT_EQ(Create(c)->SettleBudget(), 30ms);  // Win32 default

    c = {};
    c.isRichEditD2DPT = true;
    EXPECT_EQ(Create(c)->SettleBudget(), 0ms);  // RichEdit (sent-message, drains synchronously)

    c = {};
    c.isElectron = true;
    EXPECT_EQ(Create(c)->SettleBudget(), 100ms);  // Electron event-loop margin

    c = {};
    c.isConsole = true;
    EXPECT_EQ(Create(c)->SettleBudget(), 100ms);  // Console (split path inherits same budget)
}

}  // namespace NextKey::Output::Test
