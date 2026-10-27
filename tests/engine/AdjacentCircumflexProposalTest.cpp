// tests/engine/AdjacentCircumflexProposalTest.cpp
//
// W8.1 — verify AdjacentCircumflexProposal routes CircumflexA/E/O dispatch
// through TypingEngine::HandleAdjacentCircumflex without behaviour delta.
//
// Coverage:
//   - Behaviour parity across the adjacent + free-marking branches
//     (Vijeet / Cuara / Susata / Aa / Caa / Vieetj / Vi5e6t-VNI-mirror).
//   - UserDefined remap parity (q → CircumflexA): ActionToVowel resolution
//     stays consistent when routed through the proposal layer.
//   - Metadata pin: relocationKind() == Conditional (drift catch).
//   - Mock-executor delegation: proposal forwards tryApply() arguments
//     verbatim to IModifierSubExecutor.
#include <gtest/gtest.h>

#include <string>

#include "core/config/TypingConfig.h"
#include "core/engine/TypingAction.h"
#include "core/engine/TypingEngine.h"
#include "core/engine/rule/AdjacentCircumflexProposal.h"
#include "core/engine/rule/IModifierSubExecutor.h"
#include "core/engine/rule/ModifierProposal.h"
#include "../TestHelper.h"

namespace NextKey {
namespace {

using Testing::TypeString;
using EngineRule::AdjacentCircumflexProposal;
using EngineRule::IModifierSubExecutor;
using EngineRule::ModifierProposal;
using EngineRule::RelocationKind;

// --- Fixtures -------------------------------------------------------------

class AdjacentCircumflexProposalTest : public ::testing::Test {
protected:
    TypingConfig MakeTelexConfig(bool spell = true) {
        TypingConfig cfg;
        cfg.inputMethod = InputMethod::Telex;
        cfg.spellCheckEnabled = spell;
        cfg.optimizeLevel = 0;
        return cfg;
    }

    TypingConfig MakeUserDefinedConfig(bool spell = true) {
        TypingConfig cfg;
        cfg.inputMethod = InputMethod::UserDefined;
        cfg.spellCheckEnabled = spell;
        cfg.optimizeLevel = 0;
        return cfg;
    }
};

// --- 1. Adjacent branch behaviour ----------------------------------------

// Tone-mid smart accent (c6369dd): `vijeet` types as v-i-j(tone)-e-e(circumflex).
// Pre-state ValidPrefix → speculate WITH relocate → accept promotion to việt.
TEST_F(AdjacentCircumflexProposalTest, Vijeet_Adjacent_PromotesViaValidPrefix) {
    TypingEngine engine(MakeTelexConfig());
    TypeString(engine, L"vijeet");
    EXPECT_EQ(engine.Peek(), L"việt");
}

// Typo guard (ad09f15 / c6369dd Valid pre-state branch): user already typed
// the complete syllable `của` (Telex `cuar` = `c,u,a` + tone-hook on `u`).
// Adjacent `a` is most likely a typo. Mod-only path rejects → literal `a`.
TEST_F(AdjacentCircumflexProposalTest, Cuara_Adjacent_RejectsTypoOnValidSyllable) {
    TypingEngine engine(MakeTelexConfig());
    TypeString(engine, L"cuara");
    EXPECT_EQ(engine.Peek(), L"củaa");  // của + literal a
}

// --- 2. Free-marking branch behaviour ------------------------------------

// `susata` types as s-u-s(tone)-a-t-a(circumflex). The final `a` crosses
// the coda `t` back to the prior `a` in `suat`. Spell-check disabled at the
// invalid mid-state `súat`; free-marking branch must speculate WITH
// relocate (ad09f15) so the promotion to `suất` is accepted.
TEST_F(AdjacentCircumflexProposalTest, Susata_FreeMarking_PromotesViaCrossVowel) {
    TypingEngine engine(MakeTelexConfig());
    TypeString(engine, L"susata");
    EXPECT_EQ(engine.Peek(), L"suất");
}

// `chiêu` requires explicit circumflex on `e` (Telex `chieeuj`). The raw
// form `chieuj` (no circumflex) routes tone via FindToneTarget on the
// uncomposed `ieu` triphthong → tone-dot on `e` → `chiẹu`. Pins that the
// free-marking branch isn't accidentally invoked without an `e`-trigger.
TEST_F(AdjacentCircumflexProposalTest, Chieuj_NoCircumflex_TonePlacesOnE) {
    TypingEngine engine(MakeTelexConfig());
    TypeString(engine, L"chieuj");
    EXPECT_EQ(engine.Peek(), L"chiẹu");
}

// `chieeuj` invokes the proposal's free-marking branch: second `e` is the
// CircumflexE trigger that promotes `chieu` to `chiêu`; tone then relocates.
TEST_F(AdjacentCircumflexProposalTest, Chieeuj_FreeMarking_TonePropagates) {
    TypingEngine engine(MakeTelexConfig());
    TypeString(engine, L"chieeuj");
    EXPECT_EQ(engine.Peek(), L"chiệu");
}

// --- 3. Standalone vowel / pre-state matrix ------------------------------

// Single `aa` (no consonant prefix). Pre-state Valid `{a}` → accept to â.
// Pins that Valid pre-state is NOT a blanket reject (counter to the
// `acceptanceOnValid()` framing dropped in v2 of the plan).
TEST_F(AdjacentCircumflexProposalTest, Aa_Standalone_AcceptsOnValid) {
    TypingEngine engine(MakeTelexConfig());
    TypeString(engine, L"aa");
    EXPECT_EQ(engine.Peek(), L"â");
}

// Consonant-prefixed adjacent (control for the typo guard case).
// `{c,a}` is Valid → applying circumflex yields `câ` (ValidPrefix to câu,
// cân, …) → accept.
TEST_F(AdjacentCircumflexProposalTest, Caa_PrefixedAdjacent_Accepts) {
    TypingEngine engine(MakeTelexConfig());
    TypeString(engine, L"caa");
    EXPECT_EQ(engine.Peek(), L"câ");
}

// Canonical "tone last" path: `vieetj` ≡ `vịêt`. Pre-state ValidPrefix
// at the `ee` step → free-marking circumflex on the inner `e`.
TEST_F(AdjacentCircumflexProposalTest, Vieetj_CanonicalToneLast) {
    TypingEngine engine(MakeTelexConfig());
    TypeString(engine, L"vieetj");
    EXPECT_EQ(engine.Peek(), L"việt");
}

// --- 4. UserDefined remap parity -----------------------------------------

// UserDefined remap `q → CircumflexA`. HandleAdjacentCircumflex resolves
// targetBase via ActionToVowel(action) = 'a' (Problem 6 fix). Proposal
// layer must preserve this — q as keyChar with action=CircumflexA still
// applies circumflex to `a`.
TEST_F(AdjacentCircumflexProposalTest, UserDefined_QMapsToCircumflexA_FollowsAction) {
    TypingConfig cfg = MakeUserDefinedConfig();
    cfg.customKeyMap[static_cast<size_t>(L'q')] = TypingAction::CircumflexA;
    TypingEngine engine(cfg);
    TypeString(engine, L"caq");
    EXPECT_EQ(engine.Peek(), L"câ");
}

// Double-press escape under UserDefined remap. Last `â` adjacent branch
// L914 fires (last.base == ActionToVowel('a') == 'a') → escape â→a + literal q.
TEST_F(AdjacentCircumflexProposalTest, UserDefined_QMapsToCircumflexA_DoublePressEscapes) {
    TypingConfig cfg = MakeUserDefinedConfig();
    cfg.customKeyMap[static_cast<size_t>(L'q')] = TypingAction::CircumflexA;
    TypingEngine engine(cfg);
    TypeString(engine, L"caqq");
    EXPECT_EQ(engine.Peek(), L"caq");
}

// --- 5. Metadata pin (W8.1 plan v3 §3.1) ---------------------------------

// AdjacentCircumflexProposal declares RelocationKind::Conditional because
// adjacent branch is conditional on pre-state (ValidPrefix → relocate;
// Valid → mod-only) and free-marking branch always relocates — body spans
// both. Test pins this so future class-level edits surface in diff review.
TEST(AdjacentCircumflexProposalMetadata, ReportsConditionalRelocation) {
    struct StubExec final : IModifierSubExecutor {
        bool HandleAdjacentCircumflex(TypingAction, wchar_t) override { return false; }
        bool HandleHornW(TypingAction, wchar_t) override { return false; }
        bool HandleStrokeD(TypingAction, wchar_t) override { return false; }
        bool HandleHornInsert(TypingAction, wchar_t) override { return false; }
        bool HandleVniCircumflex(TypingAction, wchar_t) override { return false; }
        bool HandleVniHorn(TypingAction, wchar_t) override { return false; }
        bool HandleVniBreve(TypingAction, wchar_t) override { return false; }
    } stub;
    AdjacentCircumflexProposal proposal(stub);
    EXPECT_EQ(proposal.relocationKind(), RelocationKind::Conditional);
}

// --- 6. Mock-executor delegation -----------------------------------------

// Proposal MUST forward action + keyChar verbatim, and propagate the
// executor's boolean return. Pins the wrap-don't-lift contract: proposal
// has no logic of its own beyond delegation.
TEST(AdjacentCircumflexProposalDelegation, ForwardsArgumentsAndReturnVerbatim) {
    struct RecordingExec final : IModifierSubExecutor {
        TypingAction lastAction = TypingAction::None;
        wchar_t lastChar = 0;
        bool returnValue = false;
        int calls = 0;
        bool HandleAdjacentCircumflex(TypingAction action, wchar_t c) override {
            ++calls;
            lastAction = action;
            lastChar = c;
            return returnValue;
        }
        bool HandleHornW(TypingAction, wchar_t) override { return false; }
        bool HandleStrokeD(TypingAction, wchar_t) override { return false; }
        bool HandleHornInsert(TypingAction, wchar_t) override { return false; }
        bool HandleVniCircumflex(TypingAction, wchar_t) override { return false; }
        bool HandleVniHorn(TypingAction, wchar_t) override { return false; }
        bool HandleVniBreve(TypingAction, wchar_t) override { return false; }
    } rec;

    AdjacentCircumflexProposal proposal(rec);

    rec.returnValue = true;
    EXPECT_TRUE(proposal.tryApply(TypingAction::CircumflexA, L'a'));
    EXPECT_EQ(rec.calls, 1);
    EXPECT_EQ(rec.lastAction, TypingAction::CircumflexA);
    EXPECT_EQ(rec.lastChar, L'a');

    rec.returnValue = false;
    EXPECT_FALSE(proposal.tryApply(TypingAction::CircumflexE, L'e'));
    EXPECT_EQ(rec.calls, 2);
    EXPECT_EQ(rec.lastAction, TypingAction::CircumflexE);
    EXPECT_EQ(rec.lastChar, L'e');

    rec.returnValue = true;
    EXPECT_TRUE(proposal.tryApply(TypingAction::CircumflexO, L'o'));
    EXPECT_EQ(rec.calls, 3);
    EXPECT_EQ(rec.lastAction, TypingAction::CircumflexO);
    EXPECT_EQ(rec.lastChar, L'o');
}

// --- 7. Golden replay (byte-identical pre/post refactor) -----------------

// Replay 15+ inputs that exercise the adjacent + free-marking branches.
// Outputs are the values established by the W8.0/c6369dd test suite —
// if W8.1's dispatch reroute changes ANY of these, the refactor is not
// behaviour-neutral and should be reverted/diagnosed.
TEST_F(AdjacentCircumflexProposalTest, GoldenReplay_ByteIdenticalAcrossInputs) {
    struct Case {
        const wchar_t* input;
        const wchar_t* expected;
    };
    const Case cases[] = {
        // Adjacent branch
        {L"aa",       L"â"},
        {L"caa",      L"câ"},
        {L"vijeet",   L"việt"},
        {L"ngufoon",  L"nguồn"},
        {L"chuyeenj", L"chuyện"},
        {L"cuara",    L"củaa"},   // typo guard: của + literal a

        // Free-marking branch
        {L"susata",   L"suất"},
        {L"chieeuj",  L"chiệu"},
        {L"vieetj",   L"việt"},

        // Standalone / multi-syllable
        {L"oo",       L"ô"},
        {L"ee",       L"ê"},

        // Escape chains
        {L"caaa",     L"caa"},
        {L"oooo",     L"ooo"},   // 3rd o escapes ô→o+o; 4th o blocked by escape_
    };
    for (const auto& c : cases) {
        TypingEngine engine(MakeTelexConfig());
        TypeString(engine, c.input);
        EXPECT_EQ(engine.Peek(), c.expected)
            << "Golden mismatch for input '" << std::string(c.input, c.input + wcslen(c.input)) << "'";
    }
}

TEST_F(AdjacentCircumflexProposalTest, Issue270_CoupledQualitySwitchPreservesBothVowels) {
    struct Step { wchar_t key; const wchar_t* expected; };
    const Step steps[] = {{L'w', L"ươ"}, {L'o', L"uô"}, {L'w', L"ươ"},
                          {L'o', L"uô"}, {L'w', L"ươ"}};
    for (auto method : {InputMethod::Telex, InputMethod::SimpleTelex, InputMethod::Combined}) {
        for (bool spell : {false, true}) {
            for (bool modern : {false, true}) {
                for (const wchar_t* onset : {L"", L"b", L"d", L"D"}) {
                    auto cfg = MakeTelexConfig();
                    cfg.inputMethod = method;
                    cfg.spellCheckEnabled = spell;
                    cfg.modernOrtho = modern;
                    TypingEngine engine(cfg);
                    std::wstring raw = std::wstring(onset) + L"uo";
                    TypeString(engine, raw);
                    for (const auto& step : steps) {
                        engine.PushChar(step.key);
                        raw += step.key;
                        EXPECT_EQ(engine.Peek(), std::wstring(onset) + step.expected);
                        EXPECT_EQ(engine.Count(), std::wstring(onset).size() + 2);
                        EXPECT_EQ(engine.PeekRaw(), raw);
                    }
                }
            }
        }
    }
}

TEST_F(AdjacentCircumflexProposalTest, Issue270_QualitySwitchKeepsToneAndUppercaseIntent) {
    for (bool spell : {false, true}) {
        auto cfg = MakeTelexConfig();
        cfg.spellCheckEnabled = spell;
        TypingEngine engine(cfg);
        TypeString(engine, L"DUOWF");
        EXPECT_EQ(engine.Peek(), L"DƯỜ");
        engine.PushChar(L'O');
        EXPECT_EQ(engine.Peek(), L"DUỒ");
        engine.PushChar(L'W');
        EXPECT_EQ(engine.Peek(), L"DƯỜ");
    }
}

TEST_F(AdjacentCircumflexProposalTest, Issue270_InverseHornOrderAlsoAllowsCircumflexReplacement) {
    for (auto method : {InputMethod::Telex, InputMethod::VNI}) {
        for (bool spell : {false, true}) {
            auto cfg = MakeTelexConfig();
            cfg.inputMethod = method;
            cfg.spellCheckEnabled = spell;
            TypingEngine engine(cfg);
            TypeString(engine, method == InputMethod::VNI ? L"du7o6" : L"duwoo");
            EXPECT_EQ(engine.Peek(), L"duô");
        }
    }
}

TEST_F(AdjacentCircumflexProposalTest, Issue270_ExistingOpenHornAndBackspaceContractsRemainAvailable) {
    for (auto method : {InputMethod::Telex, InputMethod::SimpleTelex, InputMethod::Combined, InputMethod::VNI}) {
        for (bool spell : {false, true}) {
            auto cfg = MakeTelexConfig();
            cfg.inputMethod = method;
            cfg.spellCheckEnabled = spell;
            TypingEngine engine(cfg);
            TypeString(engine, method == InputMethod::VNI ? L"huo76767" : L"huowowow");
            EXPECT_EQ(engine.Peek(), L"huơ");
            engine.Reset();
            TypeString(engine, method == InputMethod::VNI ? L"huo7ng6" : L"huowngo");
            EXPECT_EQ(engine.Peek(), L"huông");
            engine.Backspace();
            EXPECT_EQ(engine.Peek(), L"huôn");
            engine.PushChar(L'g');
            EXPECT_EQ(engine.Peek(), L"huông");
        }
    }
}

TEST_F(AdjacentCircumflexProposalTest, Issue270_CustomCircumflexAndHornActionsShareQualitySwitch) {
    auto cfg = MakeUserDefinedConfig();
    cfg.customKeyMap[static_cast<size_t>(L'q')] = TypingAction::CircumflexO;
    cfg.customKeyMap[static_cast<size_t>(L'k')] = TypingAction::HornW;
    TypingEngine engine(cfg);
    TypeString(engine, L"duok");
    EXPECT_EQ(engine.Peek(), L"dươ");
    engine.PushChar(L'q');
    EXPECT_EQ(engine.Peek(), L"duô");
    engine.PushChar(L'k');
    EXPECT_EQ(engine.Peek(), L"dươ");
}

}  // namespace
}  // namespace NextKey
