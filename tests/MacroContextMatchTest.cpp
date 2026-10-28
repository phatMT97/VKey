// VKey - Preceding-text macro lookup unit tests
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "core/MacroContextMatch.h"

namespace NextKey::Macro {
namespace {

struct AsciiCaseMapper final : CaseMapper {
    void Upper(wchar_t* buf, std::size_t n) const override {
        for (std::size_t i = 0; i < n; ++i)
            if (buf[i] >= L'a' && buf[i] <= L'z') buf[i] = buf[i] - L'a' + L'A';
    }
    void Lower(wchar_t* buf, std::size_t n) const override {
        for (std::size_t i = 0; i < n; ++i)
            if (buf[i] >= L'A' && buf[i] <= L'Z') buf[i] = buf[i] - L'A' + L'a';
    }
};

struct Fixture {
    AsciiCaseMapper mapper;
    std::unordered_map<std::wstring, std::wstring> table{{L"nma", L"nhưng mà"}};
    wchar_t trigger = L' ';
    bool autoCaps = false;
    bool titleCase = false;

    [[nodiscard]] std::optional<ContextMatch> Run(std::wstring_view text) const {
        return MatchInPrecedingText(text, trigger, table,
                                    LongestMacroKeyLength(table), autoCaps,
                                    /*clipboardThreshold=*/200, mapper, titleCase);
    }
};

TEST(MacroContextMatch, MatchesShortcutAtDocumentStart) {
    Fixture f;
    const auto m = f.Run(L"nMa");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->shortcutLen, 3u);
    EXPECT_EQ(m->plan.bsCount, 3u);   // trigger is not in the document
    EXPECT_EQ(m->plan.expansion, L"nhưng mà");
}

TEST(MacroContextMatch, MatchesShortcutAfterSpace) {
    Fixture f;
    const auto m = f.Run(L"xin nMa");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->shortcutLen, 3u);
}

// Word Boundary Guard: a shortcut suffix inside a longer word must not fire.
TEST(MacroContextMatch, RejectsMatchInsideLongerWord) {
    Fixture f;
    EXPECT_FALSE(f.Run(L"AnMa").has_value());
    EXPECT_FALSE(f.Run(L"xin AnMa").has_value());
}

TEST(MacroContextMatch, PunctuationCountsAsBoundary) {
    Fixture f;
    const auto m = f.Run(L"xin,nMa");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->shortcutLen, 3u);
}

// A punctuation trigger completes the shortcut but is not yet in the document,
// so shortcutLen must exclude it.
TEST(MacroContextMatch, TriggerCharNotCountedInShortcutLength) {
    Fixture f;
    f.table = {{L"vn.", L"Việt Nam."}};
    f.trigger = L'.';
    const auto m = f.Run(L"vn");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->shortcutLen, 2u);
    EXPECT_EQ(m->plan.bsCount, 2u);
}

// Punctuation inside a key must stay reachable — the scan is bounded by key
// length, not cut at the nearest punctuation mark.
TEST(MacroContextMatch, MatchesKeyContainingPunctuation) {
    Fixture f;
    f.table = {{L"a/b", L"anh/bạn"}};
    const auto m = f.Run(L"xin a/b");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->shortcutLen, 3u);
}

TEST(MacroContextMatch, MatchesMultiWordKey) {
    Fixture f;
    f.table = {{L"co le", L"có lẽ"}};
    const auto m = f.Run(L"xin co le");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->shortcutLen, 5u);
}

// Longest key wins when two keys both end at the caret on a boundary.
TEST(MacroContextMatch, PrefersLongestCandidate) {
    Fixture f;
    f.table = {{L"a b", L"long"}, {L"b", L"short"}};
    const auto m = f.Run(L"a b");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->plan.expansion, L"long");
    EXPECT_EQ(m->shortcutLen, 3u);
}

TEST(MacroContextMatch, NoMatchReturnsNullopt) {
    Fixture f;
    EXPECT_FALSE(f.Run(L"xin chao").has_value());
    EXPECT_FALSE(f.Run(L"").has_value());
}

TEST(MacroContextMatch, EmptyTableShortCircuits) {
    Fixture f;
    f.table.clear();
    EXPECT_FALSE(f.Run(L"nMa").has_value());
}

// Case comes from the document, so a lowercase candidate must stay lowercase
// even when an earlier word in the window was auto-capitalized.
TEST(MacroContextMatch, AutoCapsFollowsCandidateCaseNotWindowCase) {
    Fixture f;
    f.autoCaps = true;
    const auto lower = f.Run(L"Xin nma");
    ASSERT_TRUE(lower.has_value());
    EXPECT_EQ(lower->plan.expansion, L"nhưng mà");

    const auto upper = f.Run(L"Xin Nma");
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(upper->plan.expansion, L"Nhưng mà");
}

TEST(MacroContextMatch, TitleCaseOptionUsesTheSamePlanner) {
    Fixture f;
    f.autoCaps = true;
    f.titleCase = true;
    const auto match = f.Run(L"xin Nma");
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->plan.expansion, L"Nhưng Mà");
}

TEST(MacroContextMatch, LongestMacroKeyLengthMeasuresLongestKey) {
    const std::unordered_map<std::wstring, std::wstring> table{
        {L"a", L"x"}, {L"abcd", L"y"}, {L"ab", L"z"}};
    EXPECT_EQ(LongestMacroKeyLength(table), 4u);
    EXPECT_EQ(LongestMacroKeyLength({}), 0u);
}

}  // namespace
}  // namespace NextKey::Macro
