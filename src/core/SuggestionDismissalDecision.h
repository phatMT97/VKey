// VKey - portable autocomplete suggestion dismissal policy and plan
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace NextKey {

enum class SuggestionDismissalPolicy {
    None,
    UnicodeBait,
    NativePrintablePrefix,
};

inline constexpr std::uint16_t kNativePrintablePrefixVk = 0x41;  // 'A'
inline constexpr wchar_t kUnicodeBaitChar = 0x202F;

[[nodiscard]] constexpr SuggestionDismissalPolicy DecideSuggestionDismissalPolicy(
        bool isBrowser, bool isOutlook, bool isExcel, bool forceNoBait,
        std::int8_t sendMethodOverride) noexcept {
    if (forceNoBait || sendMethodOverride == 6) return SuggestionDismissalPolicy::None;
    // Excel's dropdown ignores Unicode bait. Legacy method 5 must not change
    // this policy or opt Excel into game re-injection in the focus layer.
    if (isExcel) return SuggestionDismissalPolicy::NativePrintablePrefix;
    if (isOutlook || isBrowser) return SuggestionDismissalPolicy::UnicodeBait;
    return SuggestionDismissalPolicy::None;
}

struct SuggestionDismissalPlan {
    bool emitPrefix{false};
    std::uint16_t prefixVk{0};  // nonzero only for a native printable prefix
    std::uint16_t prefixScan{0};  // resolved by the platform for the target layout
    wchar_t prefixChar{0};     // nonzero only for a Unicode prefix
    std::size_t effectiveBsCount{0};
};

[[nodiscard]] constexpr SuggestionDismissalPlan PlanSuggestionDismissal(
        SuggestionDismissalPolicy policy, std::size_t bsCount,
        std::wstring_view text, bool suggestKeepChars) noexcept {
    SuggestionDismissalPlan plan{};
    plan.effectiveBsCount = bsCount;
    if (bsCount == 0 || policy == SuggestionDismissalPolicy::None) return plan;
    if (bsCount == (std::numeric_limits<std::size_t>::max)()) return plan;
    if (policy == SuggestionDismissalPolicy::UnicodeBait) {
        // Pure Backspace (text.empty()): ALWAYS skip Unicode bait (fixes #280 & #108).
        // Transform (!text.empty()): skip bait when user enables suggestKeepChars
        // to protect Auto-Restore and tone escape in <input type="email">/OTP fields.
        if (text.empty() || suggestKeepChars) {
            return plan;
        }
        plan.prefixChar = kUnicodeBaitChar;
    } else {
        plan.prefixVk = kNativePrintablePrefixVk;
    }
    plan.emitPrefix = true;
    plan.effectiveBsCount = bsCount + 1;
    return plan;
}

}  // namespace NextKey
