// VKey - portable suggestion dismissal regression tests (#279, #252)
// SPDX-License-Identifier: GPL-3.0-only
#include "core/SuggestionDismissalDecision.h"

#include <gtest/gtest.h>

namespace NextKey {
namespace {

using Policy = SuggestionDismissalPolicy;

TEST(SuggestionDismissalDecisionTest, PolicyCoversHostAndOverridePrecedence) {
    // Exhaust all host combinations, both opt-out sources, legacy method 5,
    // and unrelated/unknown overrides. Excel wins over Browser and Outlook;
    // explicit no-bait wins over every host.
    for (bool browser : {false, true}) {
        for (bool outlook : {false, true}) {
            for (bool excel : {false, true}) {
                for (bool forceNoBait : {false, true}) {
                    for (std::int8_t method : {-1, 0, 1, 2, 3, 4, 5, 6, 7}) {
                        SCOPED_TRACE(::testing::Message()
                            << "browser=" << browser << " outlook=" << outlook
                            << " excel=" << excel << " noBait=" << forceNoBait
                            << " method=" << static_cast<int>(method));
                        const auto expected = (forceNoBait || method == 6) ? Policy::None
                            : excel ? Policy::NativePrintablePrefix
                            : (outlook || browser) ? Policy::UnicodeBait : Policy::None;
                        EXPECT_EQ(DecideSuggestionDismissalPolicy(
                            browser, outlook, excel, forceNoBait, method), expected);
                    }
                }
            }
        }
    }
}

TEST(SuggestionDismissalDecisionTest, LegacyGameMethodKeepsExcelNativePolicy) {
    EXPECT_EQ(DecideSuggestionDismissalPolicy(false, false, true, false, 5),
              Policy::NativePrintablePrefix);
    EXPECT_EQ(DecideSuggestionDismissalPolicy(true, true, true, false, 5),
              Policy::NativePrintablePrefix);
}

TEST(SuggestionDismissalDecisionTest, ZeroBackspacesNeverEmitPrefixOrDelete) {
    for (auto policy : {Policy::None, Policy::UnicodeBait, Policy::NativePrintablePrefix}) {
        for (auto text : {std::wstring_view{}, std::wstring_view{L"đ"}}) {
            for (bool keep : {false, true}) {
                const auto plan = PlanSuggestionDismissal(policy, 0, text, keep);
                EXPECT_FALSE(plan.emitPrefix);
                EXPECT_EQ(plan.effectiveBsCount, 0u);
                EXPECT_EQ(plan.prefixVk, 0);
                EXPECT_EQ(plan.prefixChar, 0);
            }
        }
    }
}

TEST(SuggestionDismissalDecisionTest, NonePreservesRequestedBackspaces) {
    for (std::size_t count : {1u, 3u, 32u}) {
        for (auto text : {std::wstring_view{}, std::wstring_view{L"tiếng"}}) {
            for (bool keep : {false, true}) {
                const auto plan = PlanSuggestionDismissal(Policy::None, count, text, keep);
                EXPECT_FALSE(plan.emitPrefix);
                EXPECT_EQ(plan.effectiveBsCount, count);
                EXPECT_EQ(plan.prefixVk, 0);
                EXPECT_EQ(plan.prefixChar, 0);
            }
        }
    }
}

TEST(SuggestionDismissalDecisionTest, UnicodeAddsOneDeleteUnlessKeepingCharsOnPureBackspace) {
    for (std::size_t count : {1u, 3u, 32u}) {
        for (auto text : {std::wstring_view{}, std::wstring_view{L"tiếng"}}) {
            for (bool keep : {false, true}) {
                const auto plan = PlanSuggestionDismissal(Policy::UnicodeBait, count, text, keep);
                const bool emit = !(keep && text.empty());
                EXPECT_EQ(plan.emitPrefix, emit);
                EXPECT_EQ(plan.effectiveBsCount, count + (emit ? 1u : 0u));
                EXPECT_EQ(plan.prefixVk, 0);
                EXPECT_EQ(plan.prefixChar, emit ? kUnicodeBaitChar : 0);
            }
        }
    }
}

TEST(SuggestionDismissalDecisionTest, NativeAlwaysAddsOneDeleteRegardlessOfKeepChars) {
    for (std::size_t count : {1u, 3u, 32u}) {
        for (auto text : {std::wstring_view{}, std::wstring_view{L"tiếng"}}) {
            for (bool keep : {false, true}) {
                const auto plan = PlanSuggestionDismissal(
                    Policy::NativePrintablePrefix, count, text, keep);
                EXPECT_TRUE(plan.emitPrefix);
                EXPECT_EQ(plan.effectiveBsCount, count + 1);
                EXPECT_EQ(plan.prefixVk, 0x41);
                EXPECT_EQ(plan.prefixVk, kNativePrintablePrefixVk);
                EXPECT_EQ(plan.prefixChar, 0);
            }
        }
    }
}

TEST(SuggestionDismissalDecisionTest, MaximumCountCannotWrapTheCompensatingDelete) {
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    for (auto policy : {Policy::UnicodeBait, Policy::NativePrintablePrefix}) {
        const auto plan = PlanSuggestionDismissal(policy, maximum, L"đ", false);
        EXPECT_FALSE(plan.emitPrefix);
        EXPECT_EQ(plan.effectiveBsCount, maximum);
        EXPECT_EQ(plan.prefixVk, 0);
        EXPECT_EQ(plan.prefixChar, 0);
    }
}

// The Core contract is usable at compile time without Windows types.
static_assert(kUnicodeBaitChar == 0x202F);
static_assert(PlanSuggestionDismissal(Policy::NativePrintablePrefix, 1, L"đ", true)
                  .effectiveBsCount == 2);

}  // namespace
}  // namespace NextKey
