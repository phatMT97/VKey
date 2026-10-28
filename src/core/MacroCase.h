// VKey - Macro expansion decision logic
// Copyright (c) 2024-2026 PhatMT. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-only
//
// Linux-portable macro expansion decision unit. Extracted from
// HookEngine::TryExpandMacro for unit testability. Win32 case-mapping
// dependency is injected via the CaseMapper interface.

#pragma once

#include "core/config/TypingConfig.h"   // CodeTable enum

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NextKey::Macro {

/// DI seam for locale-aware uppercase / lowercase. Production wraps
/// CharUpperBuffW / CharLowerBuffW; tests inject portable case mappings.
class CaseMapper {
public:
    virtual ~CaseMapper() = default;
    virtual void Upper(wchar_t* buf, std::size_t n) const = 0;
    virtual void Lower(wchar_t* buf, std::size_t n) const = 0;
};

struct PlanInputs {
    const std::wstring& rawMacroBuffer;
    const std::wstring& previousComposition;
    const std::vector<uint8_t>& previousEncodedWidths;   // Only read when currentCodeTable != Unicode
    const std::unordered_map<std::wstring, std::wstring>& macroTable;
    bool macroCrossCommit;
    CodeTable currentCodeTable;
    bool autoCapsEnabled;                 // Manual shortcut casing, not sentence auto-caps
    bool titleCaseEnabled{false};         // Manual Abc → capitalize each word
    bool wasFirstCharAutoCapped{false};    // First raw character was raised by sentence auto-caps
    wchar_t triggerChar;
    std::size_t clipboardThreshold;     // = HookEngine kMacroClipboardThreshold (200)
};

struct MacroPlan {
    bool matched{false};
    bool isPartOfMacro{false};          // → ExpandedEatTrigger vs ExpandedPassTrigger
    std::size_t bsCount{0};
    std::wstring expansion;             // post auto-caps; \n escapes still literal
    bool useClipboard{false};
    std::wstring shortcut;              // Matched shortcut that was replaced (for undo)
};

/// Decide what (if anything) to expand. Pure function: no I/O, no globals.
[[nodiscard]] MacroPlan Plan(const PlanInputs& in, const CaseMapper& mapper);

/// Convert \n escapes into \r\n for clipboard paste. No other escapes processed.
[[nodiscard]] std::wstring ExpandEscapesForClipboard(std::wstring_view expansion);

/// Single dispatch fragment: either text or a VK_RETURN marker.
struct Segment {
    bool isReturn{false};
    std::wstring text;                  // empty when isReturn == true
};

/// Split expansion at \n boundaries and run per-character ConvertChar
/// for non-Unicode code tables. Returns segments interleaved with returns.
[[nodiscard]] std::vector<Segment> BuildSegments(std::wstring_view expansion,
                                                 CodeTable codeTable);

[[nodiscard]] bool IsCommitTrigger(uint32_t vkCode) noexcept;

/// True when a commit trigger would insert text into the document. TSF uses
/// this to defer printable macro expansion from OnTestKeyDown to OnKeyDown.
[[nodiscard]] bool IsTextProducingTrigger(uint32_t vkCode,
                                          wchar_t triggerChar) noexcept;

/// Backspace counts are reliable only for single-line, precomposed BMP text.
[[nodiscard]] bool CanUndoWithBackspaces(std::wstring_view expansion) noexcept;

/// True when a printable commit trigger must keep the composition as typed
/// (no lexicon correction). Only real sentence punctuation `. , ; : ! ? )`
/// ends a word; anything else glued to it — digits ("egx6" → not "ẽo6"),
/// '@' (emails), '-' '_' (identifiers), '/' (paths), '#' '$' '%' '&' … — is
/// part of a token. Non-printable triggers (Space, Enter, Tab, arrows; char
/// 0) are unaffected. VNI-family mid-word digits never reach a commit.
[[nodiscard]] bool IsVerbatimCommitTrigger(wchar_t triggerChar) noexcept;

[[nodiscard]] bool ShouldTrigger(uint32_t vkCode,
                                 bool triggerSpace,
                                 bool triggerEnter,
                                 bool triggerTab,
                                 bool triggerDir) noexcept;

}  // namespace NextKey::Macro
