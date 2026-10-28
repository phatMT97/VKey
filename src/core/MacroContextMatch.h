// VKey - Macro lookup over the document text preceding the caret
// SPDX-License-Identifier: GPL-3.0-only
//
// Pure, platform-free helper. TSF falls back to this when the tracked raw
// macro buffer no longer mirrors the document — composition interrupted by
// Space, characters removed with Backspace/Delete, shortcut re-typed — but the
// shortcut itself is still sitting in front of the caret.

#pragma once

#include "core/MacroCase.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NextKey::Macro {

struct ContextMatch {
    MacroPlan plan;
    std::size_t shortcutLen{0};   // document characters the expansion replaces
};

/// Longest key in `macroTable`, in characters. Cache this at load time — it
/// bounds the backward scan in MatchInPrecedingText.
[[nodiscard]] inline std::size_t LongestMacroKeyLength(
    const std::unordered_map<std::wstring, std::wstring>& macroTable) noexcept {
    std::size_t longest = 0;
    for (const auto& entry : macroTable) {
        longest = (std::max)(longest, entry.first.size());
    }
    return longest;
}

/// Longest macro shortcut ending at the caret, or nullopt.
///
/// `precedingText` is document text immediately before the caret, oldest char
/// first. `maxShortcutLen` bounds how far back to look; passing the longest key
/// in `macroTable` keeps multi-word and punctuation-containing keys reachable
/// without walking the whole window.
///
/// A candidate only counts when it starts at a word boundary, so typing inside
/// a larger word ("AnMa") cannot fire a shorter macro ("nMa").
[[nodiscard]] inline std::optional<ContextMatch> MatchInPrecedingText(
    std::wstring_view precedingText,
    wchar_t triggerChar,
    const std::unordered_map<std::wstring, std::wstring>& macroTable,
    std::size_t maxShortcutLen,
    bool autoCapsEnabled,
    std::size_t clipboardThreshold,
    const CaseMapper& mapper,
    bool titleCaseEnabled = false) {

    if (precedingText.empty() || macroTable.empty()) return std::nullopt;

    // A text-producing trigger becomes the candidate's last character, so it
    // spends one character of the budget that the document does not hold.
    const std::size_t triggerLen = (triggerChar > L' ') ? 1u : 0u;
    if (maxShortcutLen <= triggerLen) return std::nullopt;

    const std::size_t scan =
        (std::min)(precedingText.size(), maxShortcutLen - triggerLen);

    const std::vector<uint8_t> encodedWidths;
    const std::wstring previousComposition;

    for (std::size_t len = scan; len > 0; --len) {
        const std::size_t start = precedingText.size() - len;
        if (start > 0) {
            const wchar_t prev = precedingText[start - 1];
            if (iswspace(prev) == 0 && iswpunct(prev) == 0) continue;
        }

        std::wstring candidate(precedingText.substr(start));
        if (triggerLen != 0) candidate += triggerChar;

        const PlanInputs inputs{
            .rawMacroBuffer = candidate,
            .previousComposition = previousComposition,
            .previousEncodedWidths = encodedWidths,
            .macroTable = macroTable,
            // The shortcut is already committed text, so the expansion has to
            // replace document characters, not a live composition.
            .macroCrossCommit = true,
            .currentCodeTable = CodeTable::Unicode,
            .autoCapsEnabled = autoCapsEnabled,
            .titleCaseEnabled = titleCaseEnabled,
            // Document text has no evidence of who capitalized it. Do not
            // borrow sentence state from a different tracked candidate; use
            // this candidate's visible case when macro casing is enabled.
            .wasFirstCharAutoCapped = false,
            .triggerChar = triggerChar,
            .clipboardThreshold = clipboardThreshold,
        };

        MacroPlan plan = Plan(inputs, mapper);
        if (!plan.matched) continue;

        // Plan counts back over its own candidate; the document only holds the
        // part before the trigger.
        plan.bsCount = len;
        return ContextMatch{.plan = plan, .shortcutLen = len};
    }
    return std::nullopt;
}

}  // namespace NextKey::Macro
