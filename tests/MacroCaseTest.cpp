// VKey - Macro expansion decision unit tests
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "core/MacroCase.h"
#include "core/engine/VietnameseTables.h"

namespace NextKey::Macro {
namespace {

struct VietnameseCaseMapper final : CaseMapper {
    void Upper(wchar_t* buf, std::size_t n) const override {
        for (std::size_t i = 0; i < n; ++i)
            buf[i] = ToUpperVietnamese(buf[i]);
    }
    void Lower(wchar_t* buf, std::size_t n) const override {
        for (std::size_t i = 0; i < n; ++i)
            buf[i] = ToLowerVietnamese(buf[i]);
    }
};

// PlanFixture helper — shared by all Plan test suites (Tasks 4b–4e)
struct PlanFixture {
    VietnameseCaseMapper mapper;
    std::wstring raw;
    std::wstring prevComp;
    std::vector<uint8_t> widths;
    std::unordered_map<std::wstring, std::wstring> table;
    bool crossCommit = false;
    CodeTable codeTable = CodeTable::Unicode;
    bool autoCaps = false;
    bool titleCase = false;
    bool wasFirstCharAutoCapped = false;
    wchar_t trigger = L' ';
    std::size_t threshold = 200;

    [[nodiscard]] MacroPlan Run() const {
        PlanInputs in{
            .rawMacroBuffer        = raw,
            .previousComposition   = prevComp,
            .previousEncodedWidths = widths,
            .macroTable            = table,
            .macroCrossCommit      = crossCommit,
            .currentCodeTable      = codeTable,
            .autoCapsEnabled       = autoCaps,
            .titleCaseEnabled      = titleCase,
            .wasFirstCharAutoCapped = wasFirstCharAutoCapped,
            .triggerChar           = trigger,
            .clipboardThreshold    = threshold,
        };
        return Plan(in, mapper);
    }
};

// -------------------------------------------------------------------------
// Match priorities + stored-key case rule
// -------------------------------------------------------------------------

TEST(PlanMatchPrioritiesTest, FullBufferExact) {
    PlanFixture f;
    f.table[L"BTW"] = L"by the way";
    f.raw = L"BTW";
    f.trigger = L' ';
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"by the way");
    EXPECT_FALSE(p.isPartOfMacro);   // trigger == L' ' is not "> L' '"
}

TEST(PlanMatchPrioritiesTest, FullBufferLowerFallback) {
    PlanFixture f;
    f.table[L"btw"] = L"by the way";
    f.raw = L"BTW";
    f.trigger = L'.';
    auto p = f.Run();
    // No exact match for "BTW.", lowered "btw." also misses.
    // Priority 2: try "BTW" exact (miss), then "btw" lower → hit.
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"by the way");
}

TEST(PlanMatchPrioritiesTest, BufferWithoutTriggerExact) {
    PlanFixture f;
    f.table[L"BTW"] = L"by the way";
    f.raw = L"BTW.";
    f.trigger = L'.';
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_FALSE(p.isPartOfMacro);    // P2 does NOT set isPartOfMacro — trigger '.' passes through to the document
}

TEST(PlanMatchPrioritiesTest, BufferWithoutTriggerLower) {
    PlanFixture f;
    f.table[L"btw"] = L"by the way";
    f.raw = L"btw.";
    f.trigger = L'.';
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_FALSE(p.isPartOfMacro);    // P2 does NOT set isPartOfMacro — trigger '.' passes through to the document
}

TEST(PlanMatchPrioritiesTest, PrevCompositionWithTrigger) {
    PlanFixture f;
    f.table[L"chao."] = L"xin chao";
    f.raw = L".";
    f.prevComp = L"CHAO";
    f.trigger = L'.';
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_TRUE(p.isPartOfMacro);
}

TEST(PlanMatchPrioritiesTest, PrevCompositionWithoutTrigger) {
    PlanFixture f;
    f.table[L"chao"] = L"xin chao";
    f.raw = L"";
    f.prevComp = L"CHAO";
    f.trigger = L' ';
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_FALSE(p.isPartOfMacro);
}

TEST(PlanMatchPrioritiesTest, NoMatchReturnsFalse) {
    PlanFixture f;
    f.table[L"btw"] = L"by the way";
    f.raw = L"xyz";
    f.trigger = L' ';
    auto p = f.Run();
    EXPECT_FALSE(p.matched);
}

TEST(PlanStoredKeyCaseRuleTest, UppercaseKeyRejectsLowercaseTyping) {
    // Uppercase-key contract: only exact case matches.
    PlanFixture f;
    f.table[L"BTW"] = L"BY THE WAY";
    f.raw = L"btw";
    f.trigger = L' ';
    auto p = f.Run();
    EXPECT_FALSE(p.matched);
}

TEST(PlanStoredKeyCaseRuleTest, LowercaseKeyAcceptsAnyCase) {
    PlanFixture f;
    f.table[L"btw"] = L"by the way";
    for (auto* probe : {L"btw", L"BTW", L"Btw", L"bTw"}) {
        f.raw = probe;
        f.trigger = L' ';
        auto p = f.Run();
        // probe is always ASCII ("btw" variants) — explicit narrow avoids
        // MSVC /WX C4244 from std::string(wchar_t*, wchar_t*).
        const char narrow[4] = {static_cast<char>(probe[0]),
                                static_cast<char>(probe[1]),
                                static_cast<char>(probe[2]),
                                0};
        EXPECT_TRUE(p.matched) << "probe = " << narrow;
    }
}

// -------------------------------------------------------------------------
// Backspace count — covers all five branches of the bs-count tree
// -------------------------------------------------------------------------

TEST(PlanBsCountTest, MatchedViaCompositionUnicode) {
    PlanFixture f;
    f.table[L"chao"] = L"xin chao";
    f.prevComp = L"chao";
    f.raw = L"";
    f.trigger = L' ';
    f.codeTable = CodeTable::Unicode;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.bsCount, 4u);
}

TEST(PlanBsCountTest, MatchedViaCompositionTcvn3Widths) {
    PlanFixture f;
    f.table[L"chao"] = L"xin chao";
    f.prevComp = L"chao";   // Unicode 4 chars
    f.raw = L"";
    f.trigger = L' ';
    f.codeTable = CodeTable::TCVN3;
    f.widths = {1, 1, 2, 1};   // 5 bytes total under encoding
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.bsCount, 5u);
}

TEST(PlanBsCountTest, CrossCommitWithTrigger) {
    PlanFixture f;
    f.table[L"a.i"] = L"artificial intelligence";
    f.raw = L"a.i.";        // includes trigger
    f.crossCommit = true;
    f.trigger = L'.';
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.bsCount, 3u);   // 4 - 1 (trigger)
}

TEST(PlanBsCountTest, CrossCommitWithoutTrigger) {
    PlanFixture f;
    f.table[L"a.i"] = L"artificial intelligence";
    f.raw = L"a.i";
    f.crossCommit = true;
    f.trigger = L' ';
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.bsCount, 3u);   // trigger == ' ', no decrement
}

TEST(PlanBsCountTest, DefaultBranchWithTrigger) {
    PlanFixture f;
    f.table[L"btw"] = L"by the way";
    f.raw = L"btw.";
    f.trigger = L'.';
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.bsCount, 3u);   // 4 - 1 (trigger), default branch
}

TEST(PlanBsCountTest, RawMatchWithNonEmptyPrevCompUnicode) {
    // Raw-buffer match (P1) with prevComp non-empty + non-crossCommit:
    // bsCount comes from previousComposition.size(), not rawMacroBuffer.size().
    PlanFixture f;
    f.table[L"btw"] = L"by the way";
    f.raw = L"btw";
    f.prevComp = L"hoa";       // 3 Unicode chars displayed on screen
    f.crossCommit = false;
    f.trigger = L' ';
    f.codeTable = CodeTable::Unicode;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.bsCount, 3u);   // previousComposition.size()
}

TEST(PlanBsCountTest, RawMatchWithNonEmptyPrevCompTcvn3Widths) {
    // Same branch, non-Unicode encoding: bsCount sums previousEncodedWidths.
    PlanFixture f;
    f.table[L"btw"] = L"by the way";
    f.raw = L"btw";
    f.prevComp = L"hoa";
    f.crossCommit = false;
    f.trigger = L' ';
    f.codeTable = CodeTable::TCVN3;
    f.widths = {1, 2, 1};   // 4 bytes total under encoding
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.bsCount, 4u);
}

// -------------------------------------------------------------------------
// Auto-caps — decision gates and \n-escape preservation
// -------------------------------------------------------------------------

TEST(PlanAutoCapsDecisionTest, AutoCapsDisabledNoTransform) {
    PlanFixture f;
    f.table[L"btw"] = L"by the way";
    f.raw = L"BTW";
    f.trigger = L'.';
    f.autoCaps = false;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"by the way");   // unchanged
}

TEST(PlanAutoCapsDecisionTest, MatchedExactSuppressesAutoCaps) {
    PlanFixture f;
    f.table[L"BTW"] = L"by the way";   // uppercase key (exact-match contract)
    f.raw = L"BTW";
    f.trigger = L'.';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"by the way");   // matchedExact → no transform
}

TEST(PlanAutoCapsDecisionTest, CompositionUsesItsOwnCase) {
    PlanFixture f;
    f.table[L"chao"] = L"xin chao";
    f.prevComp = L"CHAO";
    f.raw = L"";
    f.trigger = L' ';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"XIN CHAO");
}

TEST(PlanAutoCapsDecisionTest, ExistingCapitalsDoNotSuppressTransform) {
    PlanFixture f;
    f.table[L"omw"] = L"On My Way";   // expansion not all-lower
    f.raw = L"OMW";
    f.trigger = L'.';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"ON MY WAY");
}

TEST(PlanAutoCapsDecisionTest, AllUpperRawTransformsToAllUpperExpansion) {
    PlanFixture f;
    f.table[L"omw"] = L"on my way";
    f.raw = L"OMW";
    f.trigger = L'.';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"ON MY WAY");
}

TEST(PlanAutoCapsDecisionTest, FirstUpperRawTransformsToFirstUpperExpansion) {
    PlanFixture f;
    f.table[L"omw"] = L"on my way";
    f.raw = L"Omw";
    f.trigger = L'.';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"On my way");
}

TEST(PlanAutoCapsDecisionTest, FirstUpperRawTransformsToFirstLetterVietnamese) {
    PlanFixture f;
    f.table[L"lhq"] = L"liên hiệp quốc";
    f.raw = L"Lhq";
    f.trigger = L' ';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"Liên hiệp quốc");
}

TEST(PlanAutoCapsDecisionTest, DotSeparatedExpansionPreservesDotAndFollowsTypedCase) {
    PlanFixture f;
    f.table[L"tp"] = L"tp.hcm";
    f.trigger = L' ';
    f.autoCaps = true;
    f.titleCase = true;

    f.raw = L"tp";
    EXPECT_EQ(f.Run().expansion, L"tp.hcm");

    f.raw = L"Tp";
    EXPECT_EQ(f.Run().expansion, L"Tp.Hcm");

    f.raw = L"TP";
    EXPECT_EQ(f.Run().expansion, L"TP.HCM");
}

TEST(PlanAutoCapsDecisionTest, ApostrophesStayInsideWords) {
    PlanFixture f;
    f.table[L"ill"] = L"i'll be there";
    f.table[L"dont"] = L"don\u2019t";
    f.trigger = L' ';
    f.autoCaps = true;
    f.titleCase = true;

    f.raw = L"Ill";
    EXPECT_EQ(f.Run().expansion, L"I'll Be There");

    f.raw = L"Dont";
    EXPECT_EQ(f.Run().expansion, L"Don\u2019t");
}

TEST(PlanAutoCapsDecisionTest, FirstCharAutoCappedTransformsToLowercaseMacro) {
    PlanFixture f;
    f.table[L"omw"] = L"on my way";
    f.raw = L"omw";
    f.wasFirstCharAutoCapped = true;
    f.trigger = L' ';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"On my way");
}

// The old guard was `!matchedExact`, which skipped the recap pass for every exact
// match. Now only upper-carrying keys skip it, so an all-lowercase exact match with
// no auto-cap must still come out untouched.
TEST(PlanAutoCapsDecisionTest, LowercaseExactMatchWithoutAutoCapStaysLowercase) {
    PlanFixture f;
    f.table[L"omw"] = L"on my way";
    f.raw = L"omw";
    f.wasFirstCharAutoCapped = false;
    f.trigger = L' ';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"on my way");
}

// Space-containing keys survive a commit (macroCrossCommit_), and the hook has to
// carry wasFirstCharAutoCapped across that commit — see HookEngine step 8.
TEST(PlanAutoCapsDecisionTest, FirstCharAutoCappedAppliesAcrossCrossCommit) {
    PlanFixture f;
    f.table[L"oc om bok"] = L"óoc om bok";
    f.raw = L"oc om bok";
    f.crossCommit = true;
    f.wasFirstCharAutoCapped = true;
    f.trigger = L' ';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"Óoc om bok");
}

TEST(PlanAutoCapsDecisionTest, FirstCharAutoCappedPreservesExactMatch) {
    PlanFixture f;
    f.table[L"oMw"] = L"on My way";
    f.raw = L"oMw";
    f.wasFirstCharAutoCapped = true;
    f.trigger = L' ';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"on My way");
}

TEST(PlanAutoCapsEscapeTest, AllUpperSkipsBackslashN) {
    PlanFixture f;
    f.table[L"sig"] = L"name\\nemail";   // \n escape inside expansion
    f.raw = L"SIG";
    f.trigger = L'.';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    // 'n' in '\n' must remain lowercase; everything else uppercase.
    EXPECT_EQ(p.expansion, L"NAME\\nEMAIL");
}

TEST(PlanAutoCapsEscapeTest, AllUpperPlainNStillUppercases) {
    PlanFixture f;
    f.table[L"hi"] = L"hello name";   // plain 'n' (no leading backslash)
    f.raw = L"HI";
    f.trigger = L'.';
    f.autoCaps = true;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_EQ(p.expansion, L"HELLO NAME");
}

// #265: the issue's original C9/C10 title-case examples remain available through
// the child option; C11/C12 describe the new default for the same input.
TEST(PlanMacroCasingRulesTest, Issue265Cases) {
    struct Case {
        const char* id;
        const wchar_t* key;
        const wchar_t* replacement;
        const wchar_t* typed;
        bool macroCaps;
        bool titleCase;
        bool sentenceCapped;
        const wchar_t* expected;
    };
    const Case cases[] = {
        {"C1", L"nma", L"nhưng mà", L"nma", false, false, false, L"nhưng mà"},
        {"C2", L"nma", L"nhưng mà", L"nma", false, false, true, L"Nhưng mà"},
        {"C3", L"nma", L"Nhưng mà", L"nma", false, false, false, L"Nhưng mà"},
        {"C4", L"nma", L"nhưng Mà", L"nma", false, false, false, L"nhưng Mà"},
        {"C5", L"nMa", L"nhưng mà", L"nMa", false, false, false, L"nhưng mà"},
        {"C6", L"nma", L"nhưng mà", L"nma", true, false, false, L"nhưng mà"},
        {"C7", L"bbh", L"bún bò huế", L"BBH", true, false, false, L"BÚN BÒ HUẾ"},
        {"C8", L"bbh", L"bún bò Huế", L"BBH", true, false, false, L"BÚN BÒ HUẾ"},
        {"C9", L"bbh", L"bún bò huế", L"Bbh", true, true, false, L"Bún Bò Huế"},
        {"C10", L"bbh", L"bún bò Huế", L"Bbh", true, true, false, L"Bún Bò Huế"},
        {"C11", L"bbh", L"bún bò huế", L"Bbh", true, false, false, L"Bún bò huế"},
        {"C12", L"bbh", L"bún bò Huế", L"Bbh", true, false, false, L"Bún bò Huế"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.id);
        PlanFixture f;
        f.table[c.key] = c.replacement;
        f.raw = c.typed;
        f.autoCaps = c.macroCaps;
        f.titleCase = c.titleCase;
        f.wasFirstCharAutoCapped = c.sentenceCapped;
        const auto plan = f.Run();
        ASSERT_TRUE(plan.matched);
        EXPECT_EQ(plan.expansion, c.expected);
    }
}

TEST(PlanMacroCasingRulesTest, SentenceCapsIsIndependentOfBothMacroOptions) {
    PlanFixture f;
    f.table[L"bbh"] = L"bún bò Huế";
    f.raw = L"bbh";
    f.wasFirstCharAutoCapped = true;
    for (bool macroCaps : {false, true}) {
        for (bool titleCase : {false, true}) {
            f.autoCaps = macroCaps;
            f.titleCase = titleCase;
            EXPECT_EQ(f.Run().expansion, L"Bún bò Huế");
        }
    }
}

TEST(PlanMacroCasingRulesTest, ExplicitKeysWinOverLowercaseFallbackAndSentenceCaps) {
    PlanFixture f;
    f.table = {{L"bbh", L"bún bò Huế"}, {L"Bbh", L"custom Initial"},
               {L"BBH", L"custom ALL"}, {L"nMa", L"nhưng Mà"}};
    f.wasFirstCharAutoCapped = true;
    for (bool macroCaps : {false, true}) {
        for (bool titleCase : {false, true}) {
            f.autoCaps = macroCaps;
            f.titleCase = titleCase;
            for (const auto* key : {L"Bbh", L"BBH", L"nMa"}) {
                f.raw = key;
                EXPECT_EQ(f.Run().expansion, f.table.at(key));
            }
            // The screen shows Bbh, but the user typed bbh: do not select Bbh.
            f.raw = L"bbh";
            EXPECT_EQ(f.Run().expansion, L"Bún bò Huế");
        }
    }
}

TEST(PlanMacroCasingRulesTest, MixedCaseDoesNotIntroduceAnImplicitTitleCaseShortcut) {
    PlanFixture f;
    f.table[L"bbh"] = L"bún bò Huế";
    f.autoCaps = true;
    for (bool titleCase : {false, true}) {
        f.titleCase = titleCase;
        for (const auto* typed : {L"BBh", L"bBh", L"BbH"}) {
            f.raw = typed;
            const auto plan = f.Run();
            EXPECT_TRUE(plan.matched);
            EXPECT_EQ(plan.expansion, L"bún bò Huế");
        }
    }
}

TEST(PlanMacroCasingRulesTest, ExistingCapitalsAreOnlyRaisedNeverLowered) {
    PlanFixture f;
    f.table[L"ip"] = L"iPhone và API tại Việt Nam";
    f.raw = L"ip";
    f.autoCaps = true;
    EXPECT_EQ(f.Run().expansion, L"iPhone và API tại Việt Nam");
    f.raw = L"Ip";
    EXPECT_EQ(f.Run().expansion, L"IPhone và API tại Việt Nam");
    f.titleCase = true;
    EXPECT_EQ(f.Run().expansion, L"IPhone Và API Tại Việt Nam");
    f.raw = L"IP";
    EXPECT_EQ(f.Run().expansion, L"IPHONE VÀ API TẠI VIỆT NAM");
}

TEST(PlanMacroCasingRulesTest, FirstLetterSkipsNonLettersAndDoesNotRecapLaterSentences) {
    PlanFixture f;
    f.table[L"sig"] = L"\\n  \"123 đặng\". xin chào\\niPhone";
    f.raw = L"Sig";
    f.autoCaps = true;
    EXPECT_EQ(f.Run().expansion, L"\\n  \"123 Đặng\". xin chào\\niPhone");
    f.titleCase = true;
    EXPECT_EQ(f.Run().expansion, L"\\n  \"123 Đặng\". Xin Chào\\nIPhone");
    f.raw = L"SIG";
    EXPECT_EQ(f.Run().expansion, L"\\n  \"123 ĐẶNG\". XIN CHÀO\\nIPHONE");
}

TEST(PlanMacroCasingRulesTest, AllCapsMapsVietnameseVowelsAndStrokeD) {
    PlanFixture f;
    f.table[L"vn"] = L"đ ă â ê ô ơ ư à ả ã á ạ ằ ẳ ẵ ắ ặ ầ ẩ ẫ ấ ậ ề ể ễ ế ệ "
                     L"ì ỉ ĩ í ị ò ỏ õ ó ọ ồ ổỗ ố ộ ờ ở ỡ ớ ợ ù ủ ũ ú ụ ừử ữ ứ ự ỳ ỷ ỹ ý ỵ";
    f.raw = L"VN";
    f.autoCaps = true;
    EXPECT_EQ(f.Run().expansion,
              L"Đ Ă Â Ê Ô Ơ Ư À Ả Ã Á Ạ Ằ Ẳ Ẵ Ắ Ặ Ầ Ẩ Ẫ Ấ Ậ Ề Ể Ễ Ế Ệ "
              L"Ì Ỉ Ĩ Í Ị Ò Ỏ Õ Ó Ọ Ồ ỔỖ Ố Ộ Ờ Ở Ỡ Ớ Ợ Ù Ủ Ũ Ú Ụ ỪỬ Ữ Ứ Ự Ỳ Ỷ Ỹ Ý Ỵ");
}

TEST(PlanMacroCasingRulesTest, SingleCasedLetterIsAnInitialRegardlessOfKeyLength) {
    PlanFixture f;
    f.table = {{L"a", L"xin chào"}, {L"a1", L"xin chào"}, {L"123", L"xin chào"}};
    f.autoCaps = true;
    for (const auto* typed : {L"A", L"A1"}) {
        f.raw = typed;
        EXPECT_EQ(f.Run().expansion, L"Xin chào");
    }
    f.raw = L"123";
    EXPECT_EQ(f.Run().expansion, L"xin chào");
}

TEST(PlanMacroCasingRulesTest, CompositionFallbackPreservesExactKeysAndItsOwnIntent) {
    PlanFixture f;
    f.table = {{L"đn", L"đà Nẵng"}, {L"Đn", L"custom casing"}};
    f.raw = L"unmatched";
    f.autoCaps = true;
    f.prevComp = L"Đn";
    EXPECT_EQ(f.Run().expansion, L"custom casing");
    f.prevComp = L"ĐN";
    EXPECT_EQ(f.Run().expansion, L"ĐÀ NẴNG");
    f.table.erase(L"Đn");
    f.prevComp = L"Đn";
    EXPECT_EQ(f.Run().expansion, L"Đà Nẵng");
}

TEST(PlanMacroCasingRulesTest, EmptyAndUncasedReplacementsRemainValid) {
    PlanFixture f;
    f.autoCaps = true;
    f.raw = L"BBH";
    for (const auto* text : {L"", L"123!\\n", L"\U0001F600"}) {
        f.table[L"bbh"] = text;
        const auto plan = f.Run();
        EXPECT_TRUE(plan.matched);
        EXPECT_EQ(plan.expansion, text);
    }
}

// -------------------------------------------------------------------------
// useClipboard threshold
// -------------------------------------------------------------------------

TEST(PlanUseClipboardTest, UnicodeAboveThresholdSetsClipboardTrue) {
    PlanFixture f;
    std::wstring big(250, L'x');   // > threshold (200)
    f.table[L"big"] = big;
    f.raw = L"big";
    f.trigger = L' ';
    f.codeTable = CodeTable::Unicode;
    f.threshold = 200;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_TRUE(p.useClipboard);
}

TEST(PlanUseClipboardTest, UnicodeAtOrBelowThresholdKeepsSendInput) {
    PlanFixture f;
    std::wstring small(200, L'x');   // == threshold; uses ">" not ">="
    f.table[L"small"] = small;
    f.raw = L"small";
    f.trigger = L' ';
    f.codeTable = CodeTable::Unicode;
    f.threshold = 200;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_FALSE(p.useClipboard);
}

TEST(PlanUseClipboardTest, NonUnicodeAlwaysSendInputRegardlessOfSize) {
    PlanFixture f;
    std::wstring big(500, L'x');
    f.table[L"big"] = big;
    f.raw = L"big";
    f.trigger = L' ';
    f.codeTable = CodeTable::TCVN3;
    f.threshold = 200;
    auto p = f.Run();
    EXPECT_TRUE(p.matched);
    EXPECT_FALSE(p.useClipboard);   // non-Unicode → always SendInput
}

// -------------------------------------------------------------------------
// Clipboard escape expansion + segment builder
// -------------------------------------------------------------------------

TEST(ClipboardEscapesTest, EmptyInput) {
    EXPECT_EQ(ExpandEscapesForClipboard(L""), L"");
}

TEST(ClipboardEscapesTest, NoEscapes) {
    EXPECT_EQ(ExpandEscapesForClipboard(L"hello world"), L"hello world");
}

TEST(ClipboardEscapesTest, SingleNewlineEscape) {
    EXPECT_EQ(ExpandEscapesForClipboard(L"line1\\nline2"), L"line1\r\nline2");
}

TEST(ClipboardEscapesTest, MultipleNewlinesEscape) {
    EXPECT_EQ(ExpandEscapesForClipboard(L"a\\nb\\nc"), L"a\r\nb\r\nc");
}

TEST(ClipboardEscapesTest, LiteralBackslashFollowedByNonN) {
    // \\t is NOT an escape — only \n is recognized. \t passes through verbatim.
    EXPECT_EQ(ExpandEscapesForClipboard(L"a\\tb"), L"a\\tb");
}

TEST(BuildSegmentsTest, EmptyExpansion) {
    auto segs = BuildSegments(L"", CodeTable::Unicode);
    EXPECT_TRUE(segs.empty());
}

TEST(BuildSegmentsTest, PureTextNoNewline) {
    auto segs = BuildSegments(L"hello", CodeTable::Unicode);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_FALSE(segs[0].isReturn);
    EXPECT_EQ(segs[0].text, L"hello");
}

TEST(BuildSegmentsTest, TextNewlineText) {
    auto segs = BuildSegments(L"line1\\nline2", CodeTable::Unicode);
    ASSERT_EQ(segs.size(), 3u);
    EXPECT_FALSE(segs[0].isReturn); EXPECT_EQ(segs[0].text, L"line1");
    EXPECT_TRUE(segs[1].isReturn);  EXPECT_EQ(segs[1].text, L"");
    EXPECT_FALSE(segs[2].isReturn); EXPECT_EQ(segs[2].text, L"line2");
}

TEST(BuildSegmentsTest, LeadingNewline) {
    auto segs = BuildSegments(L"\\nhello", CodeTable::Unicode);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_TRUE(segs[0].isReturn);
    EXPECT_FALSE(segs[1].isReturn); EXPECT_EQ(segs[1].text, L"hello");
}

TEST(BuildSegmentsTest, NonUnicodeSingleByteEncoding) {
    // 'à' (U+00E0) under TCVN3 maps to 0xB5 (single byte).
    // Verify ConvertChar is invoked and result has the encoded byte, not 'à'.
    auto segs = BuildSegments(L"à", CodeTable::TCVN3);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_FALSE(segs[0].isReturn);
    ASSERT_EQ(segs[0].text.size(), 1u);
    EXPECT_NE(segs[0].text[0], L'à');               // must have been converted
    EXPECT_EQ(static_cast<uint16_t>(segs[0].text[0]), 0x00B5);
}

TEST(BuildSegmentsTest, NonUnicodeMultiUnitEncoding) {
    // 'á' (U+00E1) under VNIWindows maps to encoded value 0xF961 → 2 units:
    // units[0] = LOBYTE = 0x61 ('a'), units[1] = HIBYTE = 0xF9 (tone marker).
    // Both units must be appended to the segment text in order.
    auto segs = BuildSegments(L"á", CodeTable::VNIWindows);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_FALSE(segs[0].isReturn);
    ASSERT_EQ(segs[0].text.size(), 2u);
    EXPECT_EQ(static_cast<uint16_t>(segs[0].text[0]), 0x0061);
    EXPECT_EQ(static_cast<uint16_t>(segs[0].text[1]), 0x00F9);
}

TEST(MacroTriggerDecisionTest, IsCommitTriggerIdentifiesWordBoundaries) {
    EXPECT_TRUE(IsCommitTrigger(0x20));  // VK_SPACE
    EXPECT_TRUE(IsCommitTrigger(0x0D));  // VK_RETURN
    EXPECT_TRUE(IsCommitTrigger(0x09));  // VK_TAB
    EXPECT_TRUE(IsCommitTrigger(0x25));  // VK_LEFT
    EXPECT_TRUE(IsCommitTrigger(0xBE));  // VK_OEM_PERIOD
    EXPECT_TRUE(IsCommitTrigger(0xBC));  // VK_OEM_COMMA
    EXPECT_TRUE(IsCommitTrigger(0x2E));  // VK_DELETE

    EXPECT_FALSE(IsCommitTrigger(0x41)); // 'A'
    EXPECT_FALSE(IsCommitTrigger(0x5A)); // 'Z'
    EXPECT_FALSE(IsCommitTrigger(0x70)); // VK_F1
}

TEST(MacroTriggerDecisionTest, TextProducingTriggerSeparatesPrintableFromActionKeys) {
    EXPECT_TRUE(IsTextProducingTrigger(0x20, L' '));  // VK_SPACE
    EXPECT_TRUE(IsTextProducingTrigger(0x31, L'1'));  // number row
    EXPECT_TRUE(IsTextProducingTrigger(0xBE, L'.'));  // VK_OEM_PERIOD

    EXPECT_FALSE(IsTextProducingTrigger(0x0D, 0));    // VK_RETURN
    EXPECT_FALSE(IsTextProducingTrigger(0x09, 0));    // VK_TAB
    EXPECT_FALSE(IsTextProducingTrigger(0x09, L'\t'));  // VK_TAB translated by ToUnicode
    EXPECT_FALSE(IsTextProducingTrigger(0x25, 0));    // VK_LEFT
    EXPECT_FALSE(IsTextProducingTrigger(0x2E, 0));    // VK_DELETE
    EXPECT_FALSE(IsTextProducingTrigger(0x41, L'a')); // not a commit trigger
}

TEST(MacroTriggerDecisionTest, OnlySentencePunctuationAllowsCommitCorrection) {
    // Real punctuation ends a word → lexicon correction may apply.
    for (wchar_t c : {L'.', L',', L';', L':', L'!', L'?', L')'}) {
        EXPECT_FALSE(IsVerbatimCommitTrigger(c)) << static_cast<char>(c);
    }
    // Non-printable triggers (Space/Enter/Tab/arrows) are not this rule's call.
    EXPECT_FALSE(IsVerbatimCommitTrigger(L' '));
    EXPECT_FALSE(IsVerbatimCommitTrigger(L'\t'));
    EXPECT_FALSE(IsVerbatimCommitTrigger(0));
    // Token-glued chars keep the word as typed: "egx6" must not become "ẽo6",
    // emails, identifiers, paths, hashtags, ...
    for (wchar_t c : {L'0', L'6', L'9', L'@', L'-', L'_', L'/', L'\\', L'#', L'$',
                      L'%', L'^', L'&', L'*', L'(', L'+', L'=', L'[', L']', L'{',
                      L'}', L'|', L'<', L'>', L'`', L'~', L'\'', L'"'}) {
        EXPECT_TRUE(IsVerbatimCommitTrigger(c)) << static_cast<char>(c);
    }
}

TEST(MacroTriggerDecisionTest, ShouldTriggerRespectsTabOnly) {
    bool triggerSpace = false;
    bool triggerEnter = false;
    bool triggerTab   = true;
    bool triggerDir   = false;

    EXPECT_TRUE(ShouldTrigger(0x09, triggerSpace, triggerEnter, triggerTab, triggerDir));   // VK_TAB

    EXPECT_FALSE(ShouldTrigger(0x20, triggerSpace, triggerEnter, triggerTab, triggerDir));  // VK_SPACE
    EXPECT_FALSE(ShouldTrigger(0x0D, triggerSpace, triggerEnter, triggerTab, triggerDir));  // VK_RETURN
    EXPECT_FALSE(ShouldTrigger(0x25, triggerSpace, triggerEnter, triggerTab, triggerDir));  // VK_LEFT
    EXPECT_FALSE(ShouldTrigger(0xBE, triggerSpace, triggerEnter, triggerTab, triggerDir));  // VK_OEM_PERIOD
}

TEST(MacroTriggerDecisionTest, ShouldTriggerRespectsSpaceAndEnter) {
    bool triggerSpace = true;
    bool triggerEnter = true;
    bool triggerTab   = false;
    bool triggerDir   = false;

    EXPECT_TRUE(ShouldTrigger(0x20, triggerSpace, triggerEnter, triggerTab, triggerDir));   // VK_SPACE
    EXPECT_TRUE(ShouldTrigger(0x0D, triggerSpace, triggerEnter, triggerTab, triggerDir));   // VK_RETURN

    EXPECT_FALSE(ShouldTrigger(0x09, triggerSpace, triggerEnter, triggerTab, triggerDir));  // VK_TAB
    EXPECT_FALSE(ShouldTrigger(0x25, triggerSpace, triggerEnter, triggerTab, triggerDir));  // VK_LEFT

    EXPECT_TRUE(ShouldTrigger(0xBE, triggerSpace, triggerEnter, triggerTab, triggerDir));   // VK_OEM_PERIOD
    EXPECT_TRUE(ShouldTrigger(0x30, triggerSpace, triggerEnter, triggerTab, triggerDir));   // '0'
}

TEST(MacroTriggerDecisionTest, ShouldTriggerRejectsEscapeUnderAllTriggerConfigs) {
    constexpr uint32_t kVkEscape = 0x1B;
    // TC-03: Esc must never trigger macro expansion, regardless of active triggers.
    EXPECT_FALSE(ShouldTrigger(kVkEscape, /*space=*/true,  /*enter=*/true,  /*tab=*/true,  /*dir=*/true));
    EXPECT_FALSE(ShouldTrigger(kVkEscape, /*space=*/false, /*enter=*/false, /*tab=*/false, /*dir=*/false));
    EXPECT_FALSE(ShouldTrigger(kVkEscape, /*space=*/true,  /*enter=*/false, /*tab=*/false, /*dir=*/false));
    EXPECT_FALSE(ShouldTrigger(kVkEscape, /*space=*/false, /*enter=*/true,  /*tab=*/false, /*dir=*/false));
    EXPECT_FALSE(ShouldTrigger(kVkEscape, /*space=*/false, /*enter=*/false, /*tab=*/true,  /*dir=*/false));
    EXPECT_FALSE(ShouldTrigger(kVkEscape, /*space=*/false, /*enter=*/false, /*tab=*/false, /*dir=*/true));
}

TEST(PlanShortcutCaptureTest, CapturesExactTypedShortcutForUndo) {
    PlanFixture f;
    f.table[L"bbh"] = L"bún bò Huế";

    // 1. Exact raw match
    f.raw = L"bbh";
    f.trigger = L' ';
    auto plan1 = f.Run();
    EXPECT_TRUE(plan1.matched);
    EXPECT_EQ(plan1.shortcut, L"bbh");

    // 2. First letter capitalized
    f.raw = L"Bbh";
    auto plan2 = f.Run();
    EXPECT_TRUE(plan2.matched);
    EXPECT_EQ(plan2.shortcut, L"Bbh");

    // 3. All uppercase raw input
    f.raw = L"BBH";
    auto plan3 = f.Run();
    EXPECT_TRUE(plan3.matched);
    EXPECT_EQ(plan3.shortcut, L"BBH");

    // 4. Raw buffer without trigger char (e.g. "btw" matched from "btw." with '.')
    f.table[L"btw"] = L"by the way";
    f.raw = L"btw.";
    f.trigger = L'.';
    auto plan4 = f.Run();
    EXPECT_TRUE(plan4.matched);
    EXPECT_EQ(plan4.shortcut, L"btw");

    // 5. Matched via previousComposition
    f.raw = L"";
    f.prevComp = L"bbh";
    f.trigger = L' ';
    auto plan5 = f.Run();
    EXPECT_TRUE(plan5.matched);
    EXPECT_EQ(plan5.shortcut, L"bbh");
}

TEST(MacroUndoTest, BackspaceUndoRejectsAmbiguousCharacterCounts) {
    EXPECT_TRUE(CanUndoWithBackspaces(L"bún bò Huế"));
    EXPECT_FALSE(CanUndoWithBackspaces(L"a\\nb"));
    EXPECT_FALSE(CanUndoWithBackspaces(L"a\r\nb"));
    EXPECT_FALSE(CanUndoWithBackspaces(L"a\tb"));
    EXPECT_FALSE(CanUndoWithBackspaces(L"a\u0301"));
    EXPECT_FALSE(CanUndoWithBackspaces(L"\U0001F600"));
}

TEST(PlanShortcutCaptureTest, CompositionMatchIncludesConsumedPunctuation) {
    PlanFixture f;
    f.table[L"vn."] = L"Việt Nam";
    f.raw = L"unmatched.";
    f.prevComp = L"VN";
    f.trigger = L'.';
    f.autoCaps = true;
    const auto plan = f.Run();
    ASSERT_TRUE(plan.matched);
    EXPECT_TRUE(plan.isPartOfMacro);
    EXPECT_EQ(plan.shortcut, L"VN.");
    EXPECT_EQ(plan.bsCount, 2u);
    EXPECT_EQ(plan.expansion, L"VIỆT NAM");
}

}  // namespace
}  // namespace NextKey::Macro
