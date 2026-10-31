// FocusApplyDecisionTest.cpp
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <atomic>
#include <utility>

#include "core/FocusApplyDecision.h"

namespace NextKey {
namespace {

TEST(FocusApplyDecisionTest, LatestSnapshotWithoutOvertakingInputAppliesNow) {
    const FocusApplyInputs in{
        .snapshotRequestSerial = 7,
        .latestRequestSerial = 7,
        .snapshotInputEpoch = 12,
        .currentInputEpoch = 12,
        .hasComposition = true,
    };

    EXPECT_EQ(DecideFocusApply(in), FocusApplyDisposition::ApplyNow);
}

TEST(FocusApplyDecisionTest, LatestSnapshotOvertakenByInputDefersLiveComposition) {
    const FocusApplyInputs in{
        .snapshotRequestSerial = 7,
        .latestRequestSerial = 7,
        .snapshotInputEpoch = 12,
        .currentInputEpoch = 16,
        .hasComposition = true,
    };

    EXPECT_EQ(DecideFocusApply(in),
              FocusApplyDisposition::DeferUntilBoundary);
}

TEST(FocusApplyDecisionTest, OvertakenSnapshotAppliesOnceCompositionIsEmpty) {
    const FocusApplyInputs in{
        .snapshotRequestSerial = 7,
        .latestRequestSerial = 7,
        .snapshotInputEpoch = 12,
        .currentInputEpoch = 16,
        .hasComposition = false,
    };

    EXPECT_EQ(DecideFocusApply(in), FocusApplyDisposition::ApplyNow);
}

TEST(FocusApplyDecisionTest, OlderRequestAlwaysDropsEvenAtSafeBoundary) {
    for (const bool hasComposition : {false, true}) {
        const FocusApplyInputs in{
            .snapshotRequestSerial = 7,
            .latestRequestSerial = 8,
            .snapshotInputEpoch = 12,
            .currentInputEpoch = 12,
            .hasComposition = hasComposition,
        };

        EXPECT_EQ(DecideFocusApply(in), FocusApplyDisposition::DropStale);
    }
}

TEST(FocusApplyDecisionTest, SerialIdentityDoesNotDependOnArrivalOrder) {
    const FocusApplyInputs newerResultArrivedFirst{
        .snapshotRequestSerial = 42,
        .latestRequestSerial = 42,
        .snapshotInputEpoch = 100,
        .currentInputEpoch = 100,
        .hasComposition = false,
    };
    const FocusApplyInputs olderResultArrivedLast{
        .snapshotRequestSerial = 41,
        .latestRequestSerial = 42,
        .snapshotInputEpoch = 100,
        .currentInputEpoch = 100,
        .hasComposition = false,
    };

    EXPECT_EQ(DecideFocusApply(newerResultArrivedFirst),
              FocusApplyDisposition::ApplyNow);
    EXPECT_EQ(DecideFocusApply(olderResultArrivedLast),
              FocusApplyDisposition::DropStale);
}

TEST(FocusApplyDecisionTest, ExhaustiveOrderingMatrixMatchesPolicy) {
    for (const bool isLatest : {false, true}) {
        for (const bool inputWasOvertaken : {false, true}) {
            for (const bool hasComposition : {false, true}) {
                const FocusApplyInputs in{
                    .snapshotRequestSerial = isLatest ? 9u : 8u,
                    .latestRequestSerial = 9u,
                    .snapshotInputEpoch = 21u,
                    .currentInputEpoch = inputWasOvertaken ? 22u : 21u,
                    .hasComposition = hasComposition,
                };

                const auto expected = !isLatest
                    ? FocusApplyDisposition::DropStale
                    : (inputWasOvertaken && hasComposition
                        ? FocusApplyDisposition::DeferUntilBoundary
                        : FocusApplyDisposition::ApplyNow);
                EXPECT_EQ(DecideFocusApply(in), expected)
                    << "isLatest=" << isLatest
                    << " inputWasOvertaken=" << inputWasOvertaken
                    << " hasComposition=" << hasComposition;
            }
        }
    }
}

TEST(TsfFocusActivationDecisionTest, FirstTsfFocusActivatesOnce) {
    TsfFocusActivationState state{};

    EXPECT_TRUE(ShouldActivateTsfProfileForFocus(state, true, 0x1234, 10));
    EXPECT_FALSE(ShouldActivateTsfProfileForFocus(state, true, 0x1234, 10));
    EXPECT_FALSE(ShouldActivateTsfProfileForFocus(state, true, 0x1234, 10));
}

TEST(TsfFocusActivationDecisionTest, DifferentTsfWindowReactivates) {
    TsfFocusActivationState state{};

    ASSERT_TRUE(ShouldActivateTsfProfileForFocus(state, true, 0x1234, 10));
    EXPECT_TRUE(ShouldActivateTsfProfileForFocus(state, true, 0x5678, 10));
    EXPECT_FALSE(ShouldActivateTsfProfileForFocus(state, true, 0x5678, 10));
}

TEST(TsfFocusActivationDecisionTest, LeavingTsfRearmsSameWindow) {
    TsfFocusActivationState state{};

    ASSERT_TRUE(ShouldActivateTsfProfileForFocus(state, true, 0x1234, 10));
    EXPECT_FALSE(ShouldActivateTsfProfileForFocus(state, false, 0x9999, 11));
    EXPECT_TRUE(ShouldActivateTsfProfileForFocus(state, true, 0x1234, 10));
}

TEST(TsfFocusActivationDecisionTest, RecycledHandleInNewProcessReactivates) {
    TsfFocusActivationState state{};

    ASSERT_TRUE(ShouldActivateTsfProfileForFocus(state, true, 0x1234, 10));
    EXPECT_TRUE(ShouldActivateTsfProfileForFocus(state, true, 0x1234, 11));
    EXPECT_FALSE(ShouldActivateTsfProfileForFocus(state, true, 0x1234, 11));
}

TEST(FocusResetCompositionDecisionTest, ResetsWhenTopLevelWindowChanges) {
    // Window changed from 0x1000 to 0x2000
    EXPECT_TRUE(ShouldResetCompositionOnFocusChange(0x2000, 0x2000, 0x1000, 0x1000));
}

TEST(FocusResetCompositionDecisionTest, ResetsWhenFocusedChildControlChanges) {
    // Top-level stays 0x1000, but child control changes from 0xAAA to 0xBBB
    EXPECT_TRUE(ShouldResetCompositionOnFocusChange(0x1000, 0xBBB, 0x1000, 0xAAA));
}

TEST(FocusResetCompositionDecisionTest, ResetsWhenSwitchingBetweenChildAndRootWindow) {
    // Top-level 0x1000, child 0xAAA -> root (effective 0x1000)
    EXPECT_TRUE(ShouldResetCompositionOnFocusChange(0x1000, 0x1000, 0x1000, 0xAAA));
    // Root (effective 0x1000) -> child 0xAAA
    EXPECT_TRUE(ShouldResetCompositionOnFocusChange(0x1000, 0xAAA, 0x1000, 0x1000));
}

TEST(FocusResetCompositionDecisionTest, DoesNotResetWhenTargetIsUnchanged) {
    // Exact same top-level and effective target (e.g. FastStone background timer)
    EXPECT_FALSE(ShouldResetCompositionOnFocusChange(0x1000, 0x1000, 0x1000, 0x1000));
    EXPECT_FALSE(ShouldResetCompositionOnFocusChange(0x1000, 0xAAA, 0x1000, 0xAAA));
}

TEST(SpuriousBackgroundFocusEventTest, IdentifiesSpuriousBackgroundTrigger) {
    // FastStone timer trigger 0x9999 fires, foreground is 0x1000, target 0x1000
    // Previously dispatched: hwnd 0x1000, target 0x1000
    EXPECT_TRUE(IsSpuriousBackgroundFocusEvent(
        0x9999, /*classifiedHwnd=*/0x1000, /*currentTarget=*/0x1000,
        /*lastDispatchedHwnd=*/0x1000, /*lastDispatchedTarget=*/0x1000));

    // Stale trigger with child target unchanged
    EXPECT_TRUE(IsSpuriousBackgroundFocusEvent(
        0x9999, /*classifiedHwnd=*/0x1000, /*currentTarget=*/0xAAA,
        /*lastDispatchedHwnd=*/0x1000, /*lastDispatchedTarget=*/0xAAA));
}

TEST(SpuriousBackgroundFocusEventTest, DoesNotDropWhenTargetActuallyChanged) {
    // Trigger is different, but child control actually changed (0xAAA -> 0xBBB)
    EXPECT_FALSE(IsSpuriousBackgroundFocusEvent(
        0x9999, /*classifiedHwnd=*/0x1000, /*currentTarget=*/0xBBB,
        /*lastDispatchedHwnd=*/0x1000, /*lastDispatchedTarget=*/0xAAA));

    // Trigger is different, and top-level actually changed (0x1000 -> 0x2000)
    EXPECT_FALSE(IsSpuriousBackgroundFocusEvent(
        0x9999, /*classifiedHwnd=*/0x2000, /*currentTarget=*/0x2000,
        /*lastDispatchedHwnd=*/0x1000, /*lastDispatchedTarget=*/0x1000));
}

TEST(SpuriousBackgroundFocusEventTest, DoesNotDropDirectFocusOrNullTrigger) {
    // Direct focus (trigger == classifiedHwnd)
    EXPECT_FALSE(IsSpuriousBackgroundFocusEvent(
        0x1000, 0x1000, 0x1000, 0x1000, 0x1000));

    // Null trigger (e.g. MINIMIZEEND / poll)
    EXPECT_FALSE(IsSpuriousBackgroundFocusEvent(
        0, 0x1000, 0x1000, 0x1000, 0x1000));
}

static_assert(noexcept(DecideFocusApply(FocusApplyInputs{})));
static_assert(noexcept(ShouldActivateTsfProfileForFocus(
    std::declval<TsfFocusActivationState&>(), false, 0, 0)));
static_assert(noexcept(ShouldResetCompositionOnFocusChange(0, 0, 0, 0)));
static_assert(noexcept(IsSpuriousBackgroundFocusEvent(0, 0, 0, 0, 0)));
// The hook hot path cost of the epoch is exactly this property. A wall-clock
// benchmark here measured ~1 ns against a 50 ns bar — it could never fail for a
// real regression, only for a loaded CI box, so the static_assert is the check.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "focus input epoch must stay lock-free on the hook hot path");

}  // namespace
}  // namespace NextKey
