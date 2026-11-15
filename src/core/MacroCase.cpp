// VKey - Macro expansion decision logic implementation
// Copyright (c) 2024-2026 PhatMT. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-only

#include "core/MacroCase.h"

#include <cwctype>

#include "core/engine/CodeTableConverter.h"
#include "core/engine/VietnameseTables.h"

namespace NextKey::Macro {

namespace {
[[nodiscard]] std::wstring LowerCopy(std::wstring s, const CaseMapper& mapper) {
    if (!s.empty()) mapper.Lower(s.data(), s.size());
    return s;
}

[[nodiscard]] bool HasUpper(std::wstring_view s, const CaseMapper& mapper) {
    for (wchar_t c : s) {
        wchar_t lower = c;
        mapper.Lower(&lower, 1);
        if (lower != c) return true;
    }
    return false;
}

// Check if the first character of raw input corresponds to the start of composition,
// accounting for quick-start consonants (f->ph, j->gi, w->qu/ư), d->đ, and base vowels.
[[nodiscard]] bool FirstCharMatchesWordStart(wchar_t rawCh, std::wstring_view comp) noexcept {
    if (comp.empty()) return true;
    const wchar_t r = ToLowerVietnamese(rawCh);
    const wchar_t c0 = ToLowerVietnamese(comp[0]);

    // Quick-start consonants: f->ph, j->gi, w->qu
    if (r == L'f') return comp.size() >= 2 && c0 == L'p' && ToLowerVietnamese(comp[1]) == L'h';
    if (r == L'j') return comp.size() >= 2 && c0 == L'g' && ToLowerVietnamese(comp[1]) == L'i';
    if (r == L'w') {
        if (comp.size() >= 2 && c0 == L'q' && ToLowerVietnamese(comp[1]) == L'u') return true;
        // Standalone w -> ư in Telex
        return c0 == L'w' || c0 == L'ư';
    }

    // d -> d or đ
    if (r == L'd') return c0 == L'd' || c0 == L'đ';

    // Vowels: match against base vowel and its modified/toned forms
    if (r == L'a') {
        return c0 == L'a' || c0 == L'à' || c0 == L'á' || c0 == L'ả' || c0 == L'ã' || c0 == L'ạ' ||
               c0 == L'ă' || c0 == L'ằ' || c0 == L'ắ' || c0 == L'ẳ' || c0 == L'ẵ' || c0 == L'ặ' ||
               c0 == L'â' || c0 == L'ầ' || c0 == L'ấ' || c0 == L'ẩ' || c0 == L'ẫ' || c0 == L'ậ';
    }
    if (r == L'e') {
        return c0 == L'e' || c0 == L'è' || c0 == L'é' || c0 == L'ẻ' || c0 == L'ẽ' || c0 == L'ẹ' ||
               c0 == L'ê' || c0 == L'ề' || c0 == L'ế' || c0 == L'ể' || c0 == L'ễ' || c0 == L'ệ';
    }
    if (r == L'i') {
        return c0 == L'i' || c0 == L'ì' || c0 == L'í' || c0 == L'ỉ' || c0 == L'ĩ' || c0 == L'ị';
    }
    if (r == L'o') {
        return c0 == L'o' || c0 == L'ò' || c0 == L'ó' || c0 == L'ỏ' || c0 == L'õ' || c0 == L'ọ' ||
               c0 == L'ô' || c0 == L'ồ' || c0 == L'ố' || c0 == L'ổ' || c0 == L'ỗ' || c0 == L'ộ' ||
               c0 == L'ơ' || c0 == L'ờ' || c0 == L'ớ' || c0 == L'ở' || c0 == L'ỡ' || c0 == L'ợ';
    }
    if (r == L'u') {
        return c0 == L'u' || c0 == L'ù' || c0 == L'ú' || c0 == L'ủ' || c0 == L'ũ' || c0 == L'ụ' ||
               c0 == L'ư' || c0 == L'ừ' || c0 == L'ứ' || c0 == L'ử' || c0 == L'ữ' || c0 == L'ự';
    }
    if (r == L'y') {
        return c0 == L'y' || c0 == L'ỳ' || c0 == L'ý' || c0 == L'ỷ' || c0 == L'ỹ' || c0 == L'ỵ';
    }

    // All other consonants: must match exactly
    return r == c0;
}

// Compute the maximum number of characters that raw input could have expanded by
// through quick consonants (quick-start: f->ph, j->gi, w->qu; quick-end: g->ng, h->nh, k->ch).
[[nodiscard]] size_t MaxAllowedConsonantExpansion(std::wstring_view raw, std::wstring_view comp) noexcept {
    if (raw.empty() || comp.empty()) return 0;

    size_t expansion = 0;

    // 1. Quick-start consonant: f->ph, j->gi, w->qu (+1 char)
    const wchar_t firstRaw = ToLowerVietnamese(raw.front());
    if (comp.size() >= 2) {
        const wchar_t c0 = ToLowerVietnamese(comp[0]);
        const wchar_t c1 = ToLowerVietnamese(comp[1]);
        if ((firstRaw == L'f' && c0 == L'p' && c1 == L'h') ||
            (firstRaw == L'j' && c0 == L'g' && c1 == L'i') ||
            (firstRaw == L'w' && c0 == L'q' && c1 == L'u')) {
            expansion += 1;
        }
    }

    // 2. Quick-end consonant: g->ng, h->nh, k->ch (+1 char)
    // Quick-end requires at least 2 chars in raw (vowel + consonant).
    if (raw.size() >= 2 && comp.size() >= 2) {
        const wchar_t lastRaw = ToLowerVietnamese(raw.back());
        const size_t compLen = comp.size();
        const wchar_t penComp = ToLowerVietnamese(comp[compLen - 2]);
        const wchar_t lastComp = ToLowerVietnamese(comp[compLen - 1]);
        if ((lastRaw == L'g' && penComp == L'n' && lastComp == L'g') ||
            (lastRaw == L'h' && penComp == L'n' && lastComp == L'h') ||
            (lastRaw == L'k' && penComp == L'c' && lastComp == L'h')) {
            expansion += 1;
        }
    }

    return expansion;
}

enum class CapsStyle { None, FirstLetter, Words, All };

[[nodiscard]] CapsStyle DetectCaps(std::wstring_view typed, bool enabled,
                                   bool titleCase, bool sentenceCapped,
                                   const CaseMapper& mapper) {
    if (enabled) {
        std::size_t letters = 0;
        bool allUpper = true;
        bool initialUpper = false;
        for (wchar_t c : typed) {
            wchar_t lower = c, upper = c;
            mapper.Lower(&lower, 1);
            mapper.Upper(&upper, 1);
            if (lower == upper) continue;
            if (letters++ == 0) initialUpper = (c == upper);
            else initialUpper = initialUpper && (c == lower);
            allUpper = allUpper && (c == upper);
        }
        // A single capital is an initial, even in a key such as "A1".
        if (allUpper && letters > 1) return CapsStyle::All;
        if (initialUpper) return (titleCase && !sentenceCapped) ? CapsStyle::Words : CapsStyle::FirstLetter;
    }
    // Sentence auto-caps never implies title case, even with macro casing off.
    return sentenceCapped ? CapsStyle::FirstLetter : CapsStyle::None;
}

void ApplyCaps(std::wstring& text, CapsStyle style, const CaseMapper& mapper) {
    if (style == CapsStyle::None) return;
    bool wordStart = true;
    for (std::size_t i = 0; i < text.size(); ++i) {
        // Preserve the macro newline token for both output dispatch paths.
        if (text[i] == L'\\' && i + 1 < text.size() && text[i + 1] == L'n') {
            ++i;
            wordStart = true;
            continue;
        }
        if (style == CapsStyle::All) {
            mapper.Upper(&text[i], 1);
            continue;
        }
        if (std::iswspace(text[i]) ||
            (std::iswpunct(text[i]) && text[i] != L'\'' && text[i] != L'\u2019')) {
            wordStart = true;
            continue;
        }
        wchar_t lower = text[i], upper = text[i];
        mapper.Lower(&lower, 1);
        mapper.Upper(&upper, 1);
        if (lower == upper) continue;
        if (style == CapsStyle::FirstLetter || wordStart) text[i] = upper;
        if (style == CapsStyle::FirstLetter) return;
        wordStart = false;
    }
}
}  // namespace

MacroPlan Plan(const PlanInputs& in, const CaseMapper& mapper) {
    MacroPlan plan{};

    bool matchedViaComposition = false;
    std::wstring_view typed = in.rawMacroBuffer;
    // Lowercase keys remain flexible regardless of the output-case settings;
    // keys carrying capitals can only win by their exact spelling.
    auto lookup = [&](const std::wstring& candidate) {
        auto match = in.macroTable.find(candidate);
        return match != in.macroTable.end() ? match
            : in.macroTable.find(LowerCopy(candidate, mapper));
    };

    // When previousComposition is active without cross-commit, rawMacroBuffer
    // must account for the whole word on screen. We compute the maximum valid
    // expansion produced by quick consonants (quick-start: f->ph/j->gi/w->qu;
    // quick-end: g->ng/h->nh/k->ch). If rawMacroBuffer is strictly shorter than
    // previousComposition even after accounting for valid expansions, or if its
    // leading character does not align with the start of composition, it is an
    // orphan tail inside a larger word, not at a word boundary.
    std::wstring_view rawView = in.rawMacroBuffer;
    if (!rawView.empty() && in.triggerChar > L' ' && rawView.back() == in.triggerChar) {
        rawView.remove_suffix(1);
    }
    bool isOrphanTail = false;
    if (!in.macroCrossCommit && !in.previousComposition.empty() && !rawView.empty()) {
        const size_t allowedExpansion = MaxAllowedConsonantExpansion(rawView, in.previousComposition);
        const bool tooShort = (rawView.size() + allowedExpansion < in.previousComposition.size());
        const bool mismatchedStart = (rawView.size() < in.previousComposition.size()) &&
                                     !FirstCharMatchesWordStart(rawView.front(), in.previousComposition);
        isOrphanTail = tooShort || mismatchedStart;
    }

    // Priority 1: full buffer (raw exact, fall back to lowered)
    auto it = isOrphanTail ? in.macroTable.end() : lookup(in.rawMacroBuffer);
    if (it != in.macroTable.end() && in.triggerChar > L' ') {
        plan.isPartOfMacro = true;
    }

    // Priority 2: buffer without trigger char (e.g. "btw" from "btw.")
    if (!isOrphanTail && it == in.macroTable.end() && in.triggerChar > L' ' &&
        in.rawMacroBuffer.size() > 1 && in.rawMacroBuffer.back() == in.triggerChar) {
        std::wstring rawWithoutTrigger =
            in.rawMacroBuffer.substr(0, in.rawMacroBuffer.size() - 1);
        it = lookup(rawWithoutTrigger);
        typed.remove_suffix(1);
    }

    std::wstring compWithTrigger;
    // Priority 3/4: use the same key rules for composed text as for raw input.
    if (it == in.macroTable.end() && !in.previousComposition.empty()) {
        if (in.triggerChar > L' ') {
            compWithTrigger = in.previousComposition + in.triggerChar;
            it = lookup(compWithTrigger);
            if (it != in.macroTable.end()) {
                plan.isPartOfMacro = true;
                matchedViaComposition = true;
                typed = compWithTrigger;
            }
        }
        if (it == in.macroTable.end()) {
            it = lookup(in.previousComposition);
            if (it != in.macroTable.end()) matchedViaComposition = true;
        }
        if (!plan.isPartOfMacro) typed = in.previousComposition;
    }
    if (it == in.macroTable.end()) return plan;   // matched: false
    plan.matched = true;
    plan.shortcut = std::wstring(typed);

    // Backspace count = on-screen characters that need erasing.
    if (matchedViaComposition) {
        if (in.currentCodeTable != CodeTable::Unicode) {
            plan.bsCount = 0;
            for (auto w : in.previousEncodedWidths) plan.bsCount += w;
        } else {
            plan.bsCount = in.previousComposition.size();
        }
    } else if (in.macroCrossCommit) {
        plan.bsCount = in.rawMacroBuffer.size();
        if (in.triggerChar > L' ' && plan.bsCount > 0) --plan.bsCount;
    } else if (!in.previousComposition.empty()) {
        if (in.currentCodeTable != CodeTable::Unicode) {
            plan.bsCount = 0;
            for (auto w : in.previousEncodedWidths) plan.bsCount += w;
        } else {
            plan.bsCount = in.previousComposition.size();
        }
    } else {
        plan.bsCount = in.rawMacroBuffer.size();
        if (in.triggerChar > L' ' && plan.bsCount > 0) --plan.bsCount;
    }

    // Explicitly cased keys are verbatim overrides. All other expansions start
    // with the stored text and only gain capitals; existing capitals never gate
    // the transform. Case intent comes from the candidate that actually matched.
    plan.expansion = it->second;
    if (!HasUpper(it->first, mapper)) {
        ApplyCaps(plan.expansion,
                  DetectCaps(typed, in.autoCapsEnabled, in.titleCaseEnabled,
                             in.wasFirstCharAutoCapped, mapper),
                  mapper);
    }

    // Clipboard threshold: Unicode + size > threshold.
    plan.useClipboard = (in.currentCodeTable == CodeTable::Unicode &&
                         plan.expansion.size() > in.clipboardThreshold);

    return plan;
}

std::wstring_view ReplacementDocumentText(
    const MacroPlan& plan, std::wstring_view previousComposition,
    bool macroCrossCommit, wchar_t triggerChar) noexcept {
    if (!macroCrossCommit && !previousComposition.empty()) {
        return previousComposition;
    }
    std::wstring_view text = plan.shortcut;
    if (plan.isPartOfMacro && triggerChar > L' ' &&
        !text.empty() && text.back() == triggerChar) {
        text.remove_suffix(1);
    }
    return text;
}

std::wstring ExpandEscapesForClipboard(std::wstring_view expansion) {
    std::wstring out;
    out.reserve(expansion.size());
    for (std::size_t i = 0; i < expansion.size(); ++i) {
        if (expansion[i] == L'\\' && i + 1 < expansion.size() && expansion[i + 1] == L'n') {
            out += L"\r\n";
            ++i;
        } else {
            out += expansion[i];
        }
    }
    return out;
}

std::vector<Segment> BuildSegments(std::wstring_view expansion, CodeTable codeTable) {
    std::vector<Segment> segments;
    Segment cur{};
    cur.text.reserve(expansion.size());
    auto flush = [&]() {
        if (!cur.text.empty()) { segments.push_back(std::move(cur)); cur = {}; }
    };
    for (std::size_t i = 0; i < expansion.size(); ++i) {
        if (expansion[i] == L'\\' && i + 1 < expansion.size() && expansion[i + 1] == L'n') {
            flush();
            segments.push_back(Segment{.isReturn = true, .text = {}});
            ++i;
            continue;
        }
        if (codeTable != CodeTable::Unicode) {
            auto enc = CodeTableConverter::ConvertChar(expansion[i], codeTable);
            cur.text += enc.units[0];
            if (enc.count == 2) cur.text += enc.units[1];
        } else {
            cur.text += expansion[i];
        }
    }
    flush();
    return segments;
}

bool IsCommitTrigger(uint32_t vkCode) noexcept {
    if (vkCode == 0x20 || vkCode == 0x0D || vkCode == 0x1B) return true;
    if (vkCode == 0x09) return true;

    // Arrow keys
    if (vkCode >= 0x25 && vkCode <= 0x28) return true;
    if (vkCode == 0x24 || vkCode == 0x23 ||
        vkCode == 0x21 || vkCode == 0x22) return true;

    // Number keys (0-9)
    if (vkCode >= 0x30 && vkCode <= 0x39) return true;

    // Numpad keys
    if (vkCode >= 0x60 && vkCode <= 0x6F) return true;

    // OEM keys (punctuation)
    if (vkCode >= 0xBA && vkCode <= 0xC0) return true;
    if (vkCode >= 0xDB && vkCode <= 0xDF) return true;
    if (vkCode == 0xBB || vkCode == 0xBC ||
        vkCode == 0xBD || vkCode == 0xBE) return true;

    // Delete, Insert
    if (vkCode == 0x2E || vkCode == 0x2D) return true;

    return false;
}

bool IsTextProducingTrigger(uint32_t vkCode, wchar_t triggerChar) noexcept {
    return IsCommitTrigger(vkCode)
        && (vkCode == 0x20 || triggerChar > L' ');
}

std::wstring RawBufferForRevive(std::wstring_view raw, std::wstring_view word) {
    return std::wstring(raw.ends_with(word) ? raw : word);
}

bool CanUndoWithBackspaces(std::wstring_view expansion) noexcept {
    for (std::size_t i = 0; i < expansion.size(); ++i) {
        const auto ch = static_cast<uint32_t>(expansion[i]);
        if (ch < 0x20 || ch > 0xFFFF || (ch >= 0xD800 && ch <= 0xDFFF)
            || (ch >= 0x0300 && ch <= 0x036F) || (ch >= 0x1AB0 && ch <= 0x1AFF)
            || (ch >= 0x1DC0 && ch <= 0x1DFF) || (ch >= 0x200C && ch <= 0x200D)
            || (ch >= 0x20D0 && ch <= 0x20FF) || (ch >= 0xFE00 && ch <= 0xFE2F)
            || (ch == L'\\' && i + 1 < expansion.size() && expansion[i + 1] == L'n')) {
            return false;
        }
    }
    return true;
}

bool IsVerbatimCommitTrigger(wchar_t triggerChar) noexcept {
    if (triggerChar <= L' ') return false;
    switch (triggerChar) {
        case L'.': case L',': case L';': case L':':
        case L'!': case L'?': case L')':
            return false;
        default:
            return true;
    }
}

bool ShouldTrigger(uint32_t vkCode,
                   bool triggerSpace,
                   bool triggerEnter,
                   bool triggerTab,
                   bool triggerDir) noexcept {
    // Esc (0x1B) is a modal interrupt / cancel key, never a macro expansion delimiter.
    // Explicitly reject early to prevent falling through to `triggerSpace || triggerEnter`.
    if (vkCode == 0x1B) return false;
    if (!IsCommitTrigger(vkCode)) return false;

    if (vkCode == 0x20) return triggerSpace;
    if (vkCode == 0x0D) return triggerEnter;
    if (vkCode == 0x09) return triggerTab;

    // Direction / Navigation
    if (vkCode >= 0x25 && vkCode <= 0x28) return triggerDir;
    if (vkCode == 0x24 || vkCode == 0x23 ||
        vkCode == 0x21 || vkCode == 0x22) return triggerDir;

    return triggerSpace || triggerEnter;
}

}  // namespace NextKey::Macro
