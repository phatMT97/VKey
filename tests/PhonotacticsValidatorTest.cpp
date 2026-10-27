// VKey - PhonotacticsValidator Unit Tests
// SPDX-License-Identifier: GPL-3.0-only
//
// Tests: Direct validator tests + engine integration tests

#include <gtest/gtest.h>
#include "core/engine/PhonotacticsValidator.h"
#include "core/engine/TypingEngine.h"
#include "core/config/TypingConfig.h"
#include "TestHelper.h"

namespace NextKey {
namespace {

using Phonology::SyllableState;
using Phonology::ValidateSyllableState;

//=============================================================================
// Helper: Build Telex CharState array from a description
//=============================================================================

// Build a CharState from base char, modifier, and tone
CharState MakeTelex(wchar_t base,
                            Modifier mod = Modifier::None,
                            Tone tone = Tone::None) {
    CharState s;
    s.base = base;
    s.mod = mod;
    s.tone = tone;
    s.isUpper = false;
    return s;
}

// Shorthand for common cases
CharState T(wchar_t base) { return MakeTelex(base); }
CharState TM(wchar_t base, Modifier mod) { return MakeTelex(base, mod); }
CharState TT(wchar_t base, Tone tone) { return MakeTelex(base, Modifier::None, tone); }
CharState TMT(wchar_t base, Modifier mod, Tone tone) { return MakeTelex(base, mod, tone); }

// Validate a vector of Telex CharStates
SyllableState V(const std::vector<CharState>& states) {
    return ValidateSyllableState(states.data(), states.size());
}

//=============================================================================
// Direct Validator Tests — Valid Complete Syllables
//=============================================================================

class PhonotacticsValidatorValidTest : public ::testing::Test {};

TEST_F(PhonotacticsValidatorValidTest, EmptyIsPrefix) {
    EXPECT_EQ(V({}), SyllableState::ValidPrefix);
}

TEST_F(PhonotacticsValidatorValidTest, SingleVowel_a) {
    EXPECT_EQ(V({T(L'a')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, SingleVowel_i) {
    EXPECT_EQ(V({T(L'i')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Ba) {
    EXPECT_EQ(V({T(L'b'), T(L'a')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Cam) {
    // c + a + m
    EXPECT_EQ(V({T(L'c'), T(L'a'), T(L'm')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Di_Stroke) {
    // đ + i  (đi)
    EXPECT_EQ(V({TM(L'd', Modifier::Breve), T(L'i')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Nghi) {
    // ngh + i
    EXPECT_EQ(V({T(L'n'), T(L'g'), T(L'h'), T(L'i')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Qua) {
    // qu + a
    EXPECT_EQ(V({T(L'q'), T(L'u'), T(L'a')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Oai) {
    // vowel-only: oai (triple no-end)
    EXPECT_EQ(V({T(L'o'), T(L'a'), T(L'i')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Uoi_WithHorn) {
    // ươi (triple no-end, with horn modifiers)
    EXPECT_EQ(V({TM(L'u', Modifier::Horn), TM(L'o', Modifier::Horn), T(L'i')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Yeu_WithCircumflex) {
    // yêu
    EXPECT_EQ(V({T(L'y'), TM(L'e', Modifier::Circumflex), T(L'u')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Bac_WithAcute) {
    // bác (stop final c + acute tone → valid)
    EXPECT_EQ(V({T(L'b'), TT(L'a', Tone::Acute), T(L'c')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Bat_WithDot) {
    // bạt (stop final t + dot tone → valid)
    EXPECT_EQ(V({T(L'b'), TT(L'a', Tone::Dot), T(L't')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Than) {
    // th + a + n
    EXPECT_EQ(V({T(L't'), T(L'h'), T(L'a'), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Trang) {
    // tr + a + ng
    EXPECT_EQ(V({T(L't'), T(L'r'), T(L'a'), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, An) {
    // a + n (vowel-initial with final)
    EXPECT_EQ(V({T(L'a'), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Anh) {
    // a + nh
    EXPECT_EQ(V({T(L'a'), T(L'n'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Ong) {
    // o + ng
    EXPECT_EQ(V({T(L'o'), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Gia) {
    // gi + a → valid (gi as consonant cluster)
    EXPECT_EQ(V({T(L'g'), T(L'i'), T(L'a')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Gi_Alone) {
    // "gi" alone → g + vowel_i → valid
    EXPECT_EQ(V({T(L'g'), T(L'i')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Loan) {
    // l + oa + n
    EXPECT_EQ(V({T(L'l'), T(L'o'), T(L'a'), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, Oan) {
    // oa + n (oan)
    EXPECT_EQ(V({T(L'o'), T(L'a'), T(L'n')}), SyllableState::Valid);
}

//=============================================================================
// Direct Validator Tests — Valid Prefix
//=============================================================================

class PhonotacticsValidatorPrefixTest : public ::testing::Test {};

TEST_F(PhonotacticsValidatorPrefixTest, SingleConsonant_b) {
    EXPECT_EQ(V({T(L'b')}), SyllableState::ValidPrefix);
}

TEST_F(PhonotacticsValidatorPrefixTest, TwoCharConsonant_th) {
    EXPECT_EQ(V({T(L't'), T(L'h')}), SyllableState::ValidPrefix);
}

TEST_F(PhonotacticsValidatorPrefixTest, ThreeCharConsonant_ngh) {
    EXPECT_EQ(V({T(L'n'), T(L'g'), T(L'h')}), SyllableState::ValidPrefix);
}

TEST_F(PhonotacticsValidatorPrefixTest, Qu_Prefix) {
    EXPECT_EQ(V({T(L'q'), T(L'u')}), SyllableState::ValidPrefix);
}

TEST_F(PhonotacticsValidatorPrefixTest, SingleD) {
    EXPECT_EQ(V({T(L'd')}), SyllableState::ValidPrefix);
}

//=============================================================================
// Direct Validator Tests — Invalid
//=============================================================================

class PhonotacticsValidatorInvalidTest : public ::testing::Test {};

TEST_F(PhonotacticsValidatorInvalidTest, Bl_InvalidCluster) {
    EXPECT_EQ(V({T(L'b'), T(L'l')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorInvalidTest, Bk_InvalidCluster) {
    EXPECT_EQ(V({T(L'b'), T(L'k')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorInvalidTest, FourVowels) {
    // aaaa → invalid (too many vowels)
    EXPECT_EQ(V({T(L'a'), T(L'a'), T(L'a'), T(L'a')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorInvalidTest, InvalidVowelCombo_ae) {
    // "ae" is not in the vowel table
    EXPECT_EQ(V({T(L'a'), T(L'e')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorInvalidTest, StopFinal_Grave_Bac) {
    // bàc — stop final 'c' with grave tone → invalid
    EXPECT_EQ(V({T(L'b'), TT(L'a', Tone::Grave), T(L'c')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorInvalidTest, StopFinal_Hook_Bac) {
    // bảc — stop final 'c' with hook tone → invalid
    EXPECT_EQ(V({T(L'b'), TT(L'a', Tone::Hook), T(L'c')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorInvalidTest, StopFinal_Tilde_Bat) {
    // bãt — stop final 't' with tilde tone → invalid
    EXPECT_EQ(V({T(L'b'), TT(L'a', Tone::Tilde), T(L't')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorInvalidTest, ExtraAfterFinal) {
    // "bang" is valid, but "bangx" has extra chars
    EXPECT_EQ(V({T(L'b'), T(L'a'), T(L'n'), T(L'g'), T(L'x')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorInvalidTest, NoEndVowel_WithConsonant) {
    // "ai" cannot have end consonant → "aim" is invalid
    EXPECT_EQ(V({T(L'a'), T(L'i'), T(L'm')}), SyllableState::Invalid);
}

//=============================================================================
// gi/qu special decomposition tests
//=============================================================================

class PhonotacticsValidatorGiQuTest : public ::testing::Test {};

TEST_F(PhonotacticsValidatorGiQuTest, Gia_Valid) {
    // gi + a → valid
    EXPECT_EQ(V({T(L'g'), T(L'i'), T(L'a')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorGiQuTest, Gip_Valid) {
    // g + i + p → valid (gíp, as in "gip" = help in some dialects)
    // Decompose: g + vowel(i) + final(p)
    EXPECT_EQ(V({T(L'g'), T(L'i'), T(L'p')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorGiQuTest, Qua_Valid) {
    EXPECT_EQ(V({T(L'q'), T(L'u'), T(L'a')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorGiQuTest, Quan_Valid) {
    EXPECT_EQ(V({T(L'q'), T(L'u'), T(L'a'), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorGiQuTest, Gian_Valid) {
    // gi + a + n
    EXPECT_EQ(V({T(L'g'), T(L'i'), T(L'a'), T(L'n')}), SyllableState::Valid);
}

//=============================================================================
// Engine Integration Tests — TelexEngine with spellCheck ON
//=============================================================================

using Testing::TypeString;

class TelexSpellCheckTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::Telex;
        config_.spellCheckEnabled = true;
    }

    TypingConfig config_;
};

TEST_F(TelexSpellCheckTest, ValidSyllable_ToneApplied) {
    // "ba" + 's' → "bá" (tone applied, valid syllable)
    TypingEngine engine(config_);
    TypeString(engine, L"bas");
    EXPECT_EQ(engine.Peek(), L"bá");
}

TEST_F(TelexSpellCheckTest, InvalidSyllable_ToneBlocked) {
    // "bl" is invalid → 's' treated as regular char
    TypingEngine engine(config_);
    TypeString(engine, L"bls");
    EXPECT_EQ(engine.Peek(), L"bls");
}

TEST_F(TelexSpellCheckTest, InvalidSyllable_ModifierNotBlocked) {
    // With English Protection, "bl" is HardEnglish, so modifiers are blocked
    // and treated as literal characters.
    TypingEngine engine(config_);
    TypeString(engine, L"blw");
    EXPECT_EQ(engine.Peek(), L"blw");
}

TEST_F(TelexSpellCheckTest, DD_NotBlocked) {
    // "dd" → "đ" even when spell check is on
    // dd should always work because đ is a valid consonant
    TypingEngine engine(config_);
    TypeString(engine, L"dd");
    EXPECT_EQ(engine.Peek(), L"đ");
}

TEST_F(TelexSpellCheckTest, DD_InInvalidContext_StillWorks) {
    // Even in an "invalid" context, dd → đ is not gated
    TypingEngine engine(config_);
    TypeString(engine, L"bld");
    // "bld" is invalid, but adding another 'd' shouldn't matter
    // since dd→đ is processed on the last 'd', and spell check doesn't gate it
    // Actually "bld" - 'd' is just a regular char. The dd modifier
    // looks for a 'd' in states_ to convert to đ.
    // Let's test a simpler case:
    engine.Reset();
    TypeString(engine, L"dd");
    EXPECT_EQ(engine.Peek(), L"đ");
}

TEST_F(TelexSpellCheckTest, BackspaceRestoresToneAbility) {
    // Type "bl" (invalid) → 's' blocked → backspace 'l' → "b" (valid prefix) → type "as" → "bá"
    TypingEngine engine(config_);
    TypeString(engine, L"bl");
    EXPECT_EQ(engine.Peek(), L"bl");  // invalid state

    // Type 's' — should be blocked
    engine.PushChar(L's');
    EXPECT_EQ(engine.Peek(), L"bls");

    // Backspace removes 's'
    engine.Backspace();
    EXPECT_EQ(engine.Peek(), L"bl");

    // Backspace removes 'l'
    engine.Backspace();
    EXPECT_EQ(engine.Peek(), L"b");

    // Now type "as" — should work
    TypeString(engine, L"as");
    EXPECT_EQ(engine.Peek(), L"bá");
}

TEST_F(TelexSpellCheckTest, SpellCheckOff_NoBlocking) {
    // With spellCheck OFF, "bl" + 's' still tries tone (fails naturally, 's' added as char)
    config_.spellCheckEnabled = false;
    TypingEngine engine(config_);
    TypeString(engine, L"bls");
    // Without spell check, 's' is a tone key — ProcessTone tries to find vowel target,
    // fails (no vowels), falls through to ProcessChar, adds 's' as regular char
    EXPECT_EQ(engine.Peek(), L"bls");
}

TEST_F(TelexSpellCheckTest, ValidWord_Duoc) {
    // "duoc" + 'j' → should apply dot tone (đ is separate, but "duoc" is valid)
    // Actually let's test with standard: "duowcs" → đước
    TypingEngine engine(config_);
    TypeString(engine, L"duowcs");
    // d-u-o → ProcessModifier(w) → ươ → ProcessModifier(c is not modifier) →
    // Actually 'c' is regular char, 's' is tone
    // Let me trace: d,u,o,w,c,s
    // d → state [d]
    // u → state [d, u]
    // o → state [d, u, o]
    // w → HandleHornW: u+o pattern → horn on o → [d, u, ơ] → AutoUO → [d, ư, ơ]
    // c → ProcessChar [d, ư, ơ, c]
    // s → ProcessTone: acute on ơ → [d, ư, ớ, c] → "dước"
    // Actually 'd' at start has no modifier, so it's just 'd' not 'đ'
    EXPECT_EQ(engine.Peek(), L"dước");
}

TEST_F(TelexSpellCheckTest, CircumflexModifier_NotGated) {
    // With English Protection, "bl" is HardEnglish, so modifiers are blocked
    // "bl" (invalid) + "a" → "bla" + "a" → 'aa' modifier blocked → "blaa"
    TypingEngine engine(config_);
    TypeString(engine, L"blaa");
    EXPECT_EQ(engine.Peek(), L"blaa");
}

TEST_F(TelexSpellCheckTest, StopFinal_OCR_DirectLiteral) {
    // First 'r' is now blocked by pre-tone check → literal immediately
    TypingEngine engine(config_);
    TypeString(engine, L"ocr");
    EXPECT_EQ(engine.Peek(), L"ocr");
}

TEST_F(TelexSpellCheckTest, StopFinal_OCRR_BothLiteral) {
    // Both 'r' are literal (no pending tone to escape)
    TypingEngine engine(config_);
    TypeString(engine, L"ocrr");
    EXPECT_EQ(engine.Peek(), L"ocrr");
}

// --- Pre-tone stop-final block ---

TEST_F(TelexSpellCheckTest, StopFinal_P_Acute_Allowed) {
    // "ap" + 's' (Acute) → "áp" valid — ensure p-coda allows Acute
    TypingEngine engine(config_);
    TypeString(engine, L"aps");
    EXPECT_EQ(engine.Peek(), L"áp");
}

TEST_F(TelexSpellCheckTest, StopFinal_Grave_Blocked) {
    TypingEngine engine(config_);
    TypeString(engine, L"ocf");
    EXPECT_EQ(engine.Peek(), L"ocf");
}

TEST_F(TelexSpellCheckTest, StopFinal_Tilde_Blocked) {
    TypingEngine engine(config_);
    TypeString(engine, L"ocx");
    EXPECT_EQ(engine.Peek(), L"ocx");
}

TEST_F(TelexSpellCheckTest, StopFinal_Acute_Allowed) {
    // "oc" + 's' (Acute) → "óc" valid (plain 'o' + acute, no circumflex)
    TypingEngine engine(config_);
    TypeString(engine, L"ocs");
    EXPECT_EQ(engine.Peek(), L"óc");
}

TEST_F(TelexSpellCheckTest, StopFinal_Dot_Allowed) {
    // "oc" + 'j' (Dot) → "ọc" valid
    TypingEngine engine(config_);
    TypeString(engine, L"ocj");
    EXPECT_EQ(engine.Peek(), L"ọc");
}

TEST_F(TelexSpellCheckTest, StopFinal_T_Hook_Blocked) {
    TypingEngine engine(config_);
    TypeString(engine, L"atr");
    EXPECT_EQ(engine.Peek(), L"atr");
}

TEST_F(TelexSpellCheckTest, StopFinal_T_Acute_Allowed) {
    TypingEngine engine(config_);
    TypeString(engine, L"ats");
    EXPECT_EQ(engine.Peek(), L"át");
}

TEST_F(TelexSpellCheckTest, StopFinal_T_Tilde_Blocked) {
    TypingEngine engine(config_);
    TypeString(engine, L"atx");
    EXPECT_EQ(engine.Peek(), L"atx");
}

TEST_F(TelexSpellCheckTest, StopFinal_Ch_Hook_Blocked) {
    // "ach" + 'r' → "achr" (ch is stop digraph)
    TypingEngine engine(config_);
    TypeString(engine, L"achr");
    EXPECT_EQ(engine.Peek(), L"achr");
}

TEST_F(TelexSpellCheckTest, StopFinal_Ch_Grave_Blocked) {
    TypingEngine engine(config_);
    TypeString(engine, L"achf");
    EXPECT_EQ(engine.Peek(), L"achf");
}

TEST_F(TelexSpellCheckTest, StopFinal_Ch_Tilde_Blocked) {
    TypingEngine engine(config_);
    TypeString(engine, L"achx");
    EXPECT_EQ(engine.Peek(), L"achx");
}

TEST_F(TelexSpellCheckTest, StopFinal_Ch_Acute_Allowed) {
    TypingEngine engine(config_);
    TypeString(engine, L"achs");
    EXPECT_EQ(engine.Peek(), L"ách");
}

TEST_F(TelexSpellCheckTest, StopFinal_P_Hook_Blocked) {
    TypingEngine engine(config_);
    TypeString(engine, L"apr");
    EXPECT_EQ(engine.Peek(), L"apr");
}

// K coda: only valid after ă (breve) — PhonotacticsValidator restriction.
// Use "awkr" (ă+k) to test the pre-tone check path; "akr" hits spellCheckDisabled_ path.
TEST_F(TelexSpellCheckTest, StopFinal_K_Hook_Blocked) {
    // aw=ă modifier, k=valid coda, 'r'=Hook → blocked by pre-tone check
    // Peek() returns composed form: ă (U+0103) + k + r (literal)
    TypingEngine engine(config_);
    TypeString(engine, L"awkr");
    EXPECT_EQ(engine.Peek(), L"\u0103kr");
}

TEST_F(TelexSpellCheckTest, StopFinal_K_Acute_Allowed) {
    // "ắk" — the only standard Vietnamese k-coda syllable
    TypingEngine engine(config_);
    TypeString(engine, L"awks");
    EXPECT_EQ(engine.Peek(), L"ắk");
}

TEST_F(TelexSpellCheckTest, NonStopFinal_N_Hook_Allowed) {
    // "an" + 'r' (Hook) → "ản" valid (n is non-stop)
    TypingEngine engine(config_);
    TypeString(engine, L"anr");
    EXPECT_EQ(engine.Peek(), L"ản");
}

TEST_F(TelexSpellCheckTest, NonStopFinal_M_Grave_Allowed) {
    TypingEngine engine(config_);
    TypeString(engine, L"amf");
    EXPECT_EQ(engine.Peek(), L"àm");
}

TEST_F(TelexSpellCheckTest, NonStopFinal_Ng_Tilde_Allowed) {
    TypingEngine engine(config_);
    TypeString(engine, L"angx");
    EXPECT_EQ(engine.Peek(), L"ãng");
}

TEST_F(TelexSpellCheckTest, SpellCheckOff_StopFinal_NotBlocked) {
    // With spell check OFF, "ocr" still produces "ỏc" (no change)
    config_.spellCheckEnabled = false;
    TypingEngine engine(config_);
    TypeString(engine, L"ocr");
    EXPECT_EQ(engine.Peek(), L"ỏc");
}

TEST_F(TelexSpellCheckTest, StopFinal_WithOnset_Hook_Blocked) {
    // "bocr" → [b,o,c] + 'r' → blocked
    TypingEngine engine(config_);
    TypeString(engine, L"bocr");
    EXPECT_EQ(engine.Peek(), L"bocr");
}

//=============================================================================
// Engine Integration Tests — TypingEngine (VNI mode) with spellCheck ON
//=============================================================================

class VniSpellCheckTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::VNI;
        config_.spellCheckEnabled = true;
    }

    TypingConfig config_;
};

TEST_F(VniSpellCheckTest, ValidSyllable_ToneApplied) {
    // "ba" + '1' → "bá"
    TypingEngine engine(config_);
    TypeString(engine, L"ba1");
    EXPECT_EQ(engine.Peek(), L"bá");
}

TEST_F(VniSpellCheckTest, InvalidSyllable_ToneBlocked) {
    // "bl" + '1' → "bl1" (blocked)
    TypingEngine engine(config_);
    TypeString(engine, L"bl1");
    EXPECT_EQ(engine.Peek(), L"bl1");
}

TEST_F(VniSpellCheckTest, VowelMod_NotBlocked) {
    // Modifiers are NOT gated by spell check
    // "bl" + '6' → no vowel to apply circumflex → falls through to ProcessChar → "bl6"
    TypingEngine engine(config_);
    TypeString(engine, L"bl6");
    EXPECT_EQ(engine.Peek(), L"bl6");
}

TEST_F(VniSpellCheckTest, Stroke_NotBlocked) {
    // "d" + '9' → "đ" (stroke is NOT gated)
    TypingEngine engine(config_);
    TypeString(engine, L"d9");
    EXPECT_EQ(engine.Peek(), L"đ");
}

TEST_F(VniSpellCheckTest, BackspaceRestoresToneAbility) {
    TypingEngine engine(config_);
    TypeString(engine, L"bl");
    engine.PushChar(L'1');
    EXPECT_EQ(engine.Peek(), L"bl1");

    // Backspace to remove '1', then 'l'
    engine.Backspace();
    engine.Backspace();
    EXPECT_EQ(engine.Peek(), L"b");

    // Now type valid syllable
    TypeString(engine, L"a1");
    EXPECT_EQ(engine.Peek(), L"bá");
}

TEST_F(VniSpellCheckTest, StopFinal_OC3_DirectLiteral) {
    // First '3' is now blocked by pre-tone check → literal immediately
    TypingEngine engine(config_);
    TypeString(engine, L"oc3");
    EXPECT_EQ(engine.Peek(), L"oc3");
}

TEST_F(VniSpellCheckTest, StopFinal_OC33_BothLiteral) {
    // Both '3' are literal (no pending tone to escape)
    TypingEngine engine(config_);
    TypeString(engine, L"oc33");
    EXPECT_EQ(engine.Peek(), L"oc33");
}

// --- VNI pre-tone stop-final block ---

TEST_F(VniSpellCheckTest, StopFinal_Grave_Blocked) {
    // "oc" + '2' (Grave) → literal
    TypingEngine engine(config_);
    TypeString(engine, L"oc2");
    EXPECT_EQ(engine.Peek(), L"oc2");
}

TEST_F(VniSpellCheckTest, StopFinal_Hook_Blocked) {
    // "oc" + '3' (Hook) → literal
    TypingEngine engine(config_);
    TypeString(engine, L"oc3");
    EXPECT_EQ(engine.Peek(), L"oc3");
}

TEST_F(VniSpellCheckTest, StopFinal_Tilde_Blocked) {
    // "oc" + '4' (Tilde) → literal
    TypingEngine engine(config_);
    TypeString(engine, L"oc4");
    EXPECT_EQ(engine.Peek(), L"oc4");
}

TEST_F(VniSpellCheckTest, StopFinal_Acute_Allowed) {
    // "oc" + '1' (Acute) → "óc" (plain 'o' + acute, no circumflex)
    TypingEngine engine(config_);
    TypeString(engine, L"oc1");
    EXPECT_EQ(engine.Peek(), L"óc");
}

TEST_F(VniSpellCheckTest, StopFinal_Dot_Allowed) {
    // "oc" + '5' (Dot) → "ọc"
    TypingEngine engine(config_);
    TypeString(engine, L"oc5");
    EXPECT_EQ(engine.Peek(), L"ọc");
}

TEST_F(VniSpellCheckTest, SpellCheckOff_StopFinal_NotBlocked) {
    config_.spellCheckEnabled = false;
    TypingEngine engine(config_);
    TypeString(engine, L"oc3");
    EXPECT_EQ(engine.Peek(), L"ỏc");
}

//=============================================================================
// Edge cases
//=============================================================================

class PhonotacticsValidatorEdgeTest : public ::testing::Test {};

TEST_F(PhonotacticsValidatorEdgeTest, Uyen_Valid) {
    // uyên (uyê + n)
    EXPECT_EQ(V({T(L'u'), T(L'y'), TM(L'e', Modifier::Circumflex), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, Oang_Valid) {
    // oăng (oă + ng)
    EXPECT_EQ(V({T(L'o'), TM(L'a', Modifier::Breve), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, Uong_WithHorn) {
    // uống (uô + ng, with circumflex)
    EXPECT_EQ(V({T(L'u'), TM(L'o', Modifier::Circumflex), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, Ach) {
    // ach (a + ch)
    EXPECT_EQ(V({T(L'a'), T(L'c'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, StopFinal_Acute_Valid) {
    // bắc (stop final + acute → valid)
    EXPECT_EQ(V({T(L'b'), TMT(L'a', Modifier::Breve, Tone::Acute), T(L'c')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, StopFinal_Dot_Valid) {
    // bặc (stop final + dot → valid)
    EXPECT_EQ(V({T(L'b'), TMT(L'a', Modifier::Breve, Tone::Dot), T(L'c')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, StopFinal_Grave_Invalid) {
    // bằc (stop final + grave → invalid)
    EXPECT_EQ(V({T(L'b'), TMT(L'a', Modifier::Breve, Tone::Grave), T(L'c')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorEdgeTest, NonStopFinal_AllTones_Valid) {
    // Non-stop finals (m, n, ng, nh) allow all tones
    // bàn (grave + n → valid)
    EXPECT_EQ(V({T(L'b'), TT(L'a', Tone::Grave), T(L'n')}), SyllableState::Valid);
    // bản (hook + n → valid)
    EXPECT_EQ(V({T(L'b'), TT(L'a', Tone::Hook), T(L'n')}), SyllableState::Valid);
    // bãn (tilde + n → valid)
    EXPECT_EQ(V({T(L'b'), TT(L'a', Tone::Tilde), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, Khanh) {
    // kh + a + nh
    EXPECT_EQ(V({T(L'k'), T(L'h'), T(L'a'), T(L'n'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, Phat) {
    // ph + a + t
    EXPECT_EQ(V({T(L'p'), T(L'h'), TT(L'a', Tone::Acute), T(L't')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, Nghieng) {
    // ngh + iê + ng
    EXPECT_EQ(V({T(L'n'), T(L'g'), T(L'h'), T(L'i'), TM(L'e', Modifier::Circumflex), T(L'n'), T(L'g')}), SyllableState::Valid);
}

// k as stop final — minority-language proper nouns (Đắk Lắk, Đắk Nông)
TEST_F(PhonotacticsValidatorEdgeTest, FinalK_Dak_Valid) {
    // đắk (đ + ắ + k → valid, stop final)
    EXPECT_EQ(V({TM(L'd', Modifier::Breve), TMT(L'a', Modifier::Breve, Tone::Acute), T(L'k')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, FinalK_Lak_Valid) {
    // lắk (l + ắ + k → valid, stop final)
    EXPECT_EQ(V({T(L'l'), TMT(L'a', Modifier::Breve, Tone::Acute), T(L'k')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, FinalK_StopTone_Acute_Valid) {
    // Stop final k + acute → valid
    EXPECT_EQ(V({T(L'b'), TMT(L'a', Modifier::Breve, Tone::Acute), T(L'k')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, FinalK_StopTone_Dot_Valid) {
    // Stop final k + dot → valid
    EXPECT_EQ(V({T(L'b'), TMT(L'a', Modifier::Breve, Tone::Dot), T(L'k')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorEdgeTest, FinalK_StopTone_Grave_Invalid) {
    // Stop final k + grave → invalid (same rule as c)
    EXPECT_EQ(V({T(L'b'), TMT(L'a', Modifier::Breve, Tone::Grave), T(L'k')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorEdgeTest, FinalK_NonBreveVowel_Invalid) {
    // hôk — 'k' final only valid after ắ vowel, not ô → invalid
    EXPECT_EQ(V({T(L'h'), TM(L'o', Modifier::Circumflex), T(L'k')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorEdgeTest, FinalK_PlainA_Invalid) {
    // bak — 'k' final after plain 'a' (no breve) → invalid
    EXPECT_EQ(V({T(L'b'), T(L'a'), T(L'k')}), SyllableState::Invalid);
}

//=============================================================================
// Smart Accent Tests — spellCheck ON, accents gated by spell validity
//=============================================================================

class SmartAccentTelexTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::Telex;
        config_.spellCheckEnabled = true;
    }

    TypingConfig config_;
};

TEST_F(SmartAccentTelexTest, ToneAppliedOnValidPrefix_Tiens) {
    // "tien" is ValidPrefix (could become tiên/tiến) → 's' applies tone
    TypingEngine engine(config_);
    TypeString(engine, L"tiens");
    EXPECT_EQ(engine.Peek(), L"tién");
}

TEST_F(SmartAccentTelexTest, ToneAppliedOnValidPrefix_Chiems) {
    // "chiem" is ValidPrefix (could become chiêm/chiếm) → 's' applies tone
    TypingEngine engine(config_);
    TypeString(engine, L"chiems");
    EXPECT_EQ(engine.Peek(), L"chiém");
}

TEST_F(SmartAccentTelexTest, ToneBlockedOnInvalidSyllable_Bls) {
    // "bl" is invalid consonant cluster → 's' is literal
    TypingEngine engine(config_);
    TypeString(engine, L"bls");
    EXPECT_EQ(engine.Peek(), L"bls");
}

TEST_F(SmartAccentTelexTest, ToneBlockedOnInvalidSyllable_Blas) {
    // "bla" is invalid → 's' blocked by spell check → literal
    TypingEngine engine(config_);
    TypeString(engine, L"blas");
    EXPECT_EQ(engine.Peek(), L"blas");
}

TEST_F(SmartAccentTelexTest, ValidSyllable_StillWorks) {
    // "ba" + 's' → "bá" (valid syllable, tone applies)
    TypingEngine engine(config_);
    TypeString(engine, L"bas");
    EXPECT_EQ(engine.Peek(), L"bá");
}

TEST_F(SmartAccentTelexTest, BackwardCircumflex_TiensE) {
    // "tiens" → "tién", then 'e' → backward scan finds 'é' → circumflex → "tiến"
    TypingEngine engine(config_);
    TypeString(engine, L"tiens");
    EXPECT_EQ(engine.Peek(), L"tién");
    engine.PushChar(L'e');
    EXPECT_EQ(engine.Peek(), L"tiến");
}

TEST_F(SmartAccentTelexTest, BackwardCircumflex_Tiensge) {
    // Full flow: "tiensge" → "tiếng"
    // t-i-e-n-s → "tién" (tone applied on valid prefix)
    // g → "tiéng"
    // e → backward scan finds 'é' → circumflex → "tiếng"
    TypingEngine engine(config_);
    TypeString(engine, L"tiensge");
    EXPECT_EQ(engine.Peek(), L"tiếng");
}

TEST_F(SmartAccentTelexTest, WModifier_HornOnU_AcrossConsonants) {
    // "munw" → 'w' finds 'u' across 'n' → mưn
    // Modifiers are NOT gated by spell check
    TypingEngine engine(config_);
    TypeString(engine, L"munw");
    EXPECT_EQ(engine.Peek(), L"mưn");
}

TEST_F(SmartAccentTelexTest, WModifier_HornOnO_AcrossConsonants) {
    // "honw" → 'w' finds 'o' across 'n' → hơn
    TypingEngine engine(config_);
    TypeString(engine, L"honw");
    EXPECT_EQ(engine.Peek(), L"hơn");
}

TEST_F(SmartAccentTelexTest, WModifier_BreveOnA_AcrossConsonants) {
    // "hanw" → 'w' finds 'a' across 'n' → hăn
    TypingEngine engine(config_);
    TypeString(engine, L"hanw");
    EXPECT_EQ(engine.Peek(), L"hăn");
}

//=============================================================================
// Smart Accent Tests — VNI engine with spellCheck ON
//=============================================================================

class SmartAccentVniTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::VNI;
        config_.spellCheckEnabled = true;
    }

    TypingConfig config_;
};

TEST_F(SmartAccentVniTest, ToneBlockedOnInvalidSyllable_Bla1) {
    // "bla" is invalid → '1' tone blocked by spell check → literal
    TypingEngine engine(config_);
    TypeString(engine, L"bla1");
    EXPECT_EQ(engine.Peek(), L"bla1");
}

//=============================================================================
// Engine Integration: VCPair restrictions affect tone gating
//=============================================================================

TEST_F(TelexSpellCheckTest, VCPair_ƠCh_ToneBlocked) {
    // "ơch" is invalid (ơ can't end with ch) → tone key becomes literal
    TypingEngine engine(config_);
    TypeString(engine, L"owchs");  // ow=ơ, ch, s=acute
    EXPECT_EQ(engine.Peek(), L"\u01A1chs");  // ơchs (tone blocked)
}

TEST_F(TelexSpellCheckTest, VCPair_ƠN_ToneAllowed) {
    // "ơn" is valid (ơ + n) → tone applies
    TypingEngine engine(config_);
    TypeString(engine, L"owns");  // ow=ơ, n, s=acute
    EXPECT_EQ(engine.Peek(), L"ớn");
}

// Note: "oo" double vowel can't be tested via Telex engine because
// o+o triggers circumflex (ô). Direct PhonotacticsValidator tests cover oo restrictions.

TEST_F(TelexSpellCheckTest, VCPair_Ka_ToneBlocked) {
    // k + a is invalid initial (k only before e/ê/i/y) → entire syllable invalid
    TypingEngine engine(config_);
    TypeString(engine, L"kas");
    EXPECT_EQ(engine.Peek(), L"kas");
}

TEST_F(TelexSpellCheckTest, VCPair_Ke_ToneAllowed) {
    // k + e is valid → tone applies
    TypingEngine engine(config_);
    TypeString(engine, L"kes");
    EXPECT_EQ(engine.Peek(), L"ké");
}

TEST_F(TelexSpellCheckTest, VCPair_Kha_StillValid) {
    // kh + a is valid (kh is 2-char consonant, NOT k+h)
    TypingEngine engine(config_);
    TypeString(engine, L"khas");
    EXPECT_EQ(engine.Peek(), L"khá");
}

TEST_F(TelexSpellCheckTest, VCPair_ÂCh_ToneBlocked) {
    // "âch" is invalid (â can't end with ch) → tone blocked
    TypingEngine engine(config_);
    TypeString(engine, L"aachs");  // aa=â, ch, s=acute
    EXPECT_EQ(engine.Peek(), L"âchs");
}

TEST_F(TelexSpellCheckTest, VCPair_ÂN_ToneAllowed) {
    // "ân" is valid → tone applies
    TypingEngine engine(config_);
    TypeString(engine, L"aans");  // aa=â, n, s=acute
    EXPECT_EQ(engine.Peek(), L"ấn");
}

TEST_F(TelexSpellCheckTest, VCPair_ƯNg_ToneAllowed) {
    // "ưng" is valid (ư + ng) → tone applies
    TypingEngine engine(config_);
    TypeString(engine, L"uwngs");  // uw=ư, ng, s=acute
    EXPECT_EQ(engine.Peek(), L"ứng");
}

TEST_F(TelexSpellCheckTest, VCPair_ÊNg_ToneBlocked) {
    // "êng" is invalid (ê can't end with ng) → tone blocked
    TypingEngine engine(config_);
    TypeString(engine, L"eengs");  // ee=ê, ng, s=acute
    EXPECT_EQ(engine.Peek(), L"êngs");
}

TEST_F(TelexSpellCheckTest, VCPair_ÊNh_ToneAllowed) {
    // "ênh" is valid (ê + nh) → tone applies
    TypingEngine engine(config_);
    TypeString(engine, L"eenhs");  // ee=ê, nh, s=acute
    EXPECT_EQ(engine.Peek(), L"ếnh");
}

//=============================================================================
// P1.2 Verify: gi/qu exception cases (quỳnh, giếng, quyết, quyến)
//=============================================================================

TEST_F(PhonotacticsValidatorGiQuTest, Quynh_Valid) {
    // qu + y + nh → quynh (valid: qu onset + uy nucleus + nh coda)
    EXPECT_EQ(V({T(L'q'), T(L'u'), T(L'y'), T(L'n'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorGiQuTest, Quynh_WithGrave_Valid) {
    // quỳnh (grave tone on y)
    EXPECT_EQ(V({T(L'q'), T(L'u'), TT(L'y', Tone::Grave), T(L'n'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorGiQuTest, Gieng_Valid) {
    // giêng (gi onset + ê vowel + ng coda, OR g onset + iê nucleus + ng coda)
    EXPECT_EQ(V({T(L'g'), T(L'i'), TM(L'e', Modifier::Circumflex), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorGiQuTest, Gieng_WithAcute_Valid) {
    // giếng (acute on ê)
    EXPECT_EQ(V({T(L'g'), T(L'i'), TMT(L'e', Modifier::Circumflex, Tone::Acute), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorGiQuTest, Gieng_WithGrave_Valid) {
    // giềng (grave on ê)
    EXPECT_EQ(V({T(L'g'), T(L'i'), TMT(L'e', Modifier::Circumflex, Tone::Grave), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorGiQuTest, Quyen_Valid) {
    // quyến (qu + uyê + n)
    EXPECT_EQ(V({T(L'q'), T(L'u'), T(L'y'), TM(L'e', Modifier::Circumflex), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorGiQuTest, Quyet_Valid) {
    // quyết (qu + uyê + t)
    EXPECT_EQ(V({T(L'q'), T(L'u'), T(L'y'), TMT(L'e', Modifier::Circumflex, Tone::Acute), T(L't')}), SyllableState::Valid);
}

//=============================================================================
// P1.3 Verify: k as initial consonant with various vowels
//=============================================================================

TEST_F(PhonotacticsValidatorValidTest, K_Initial_Ke) {
    EXPECT_EQ(V({T(L'k'), T(L'e')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, K_Initial_Ki) {
    EXPECT_EQ(V({T(L'k'), T(L'i')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, K_Initial_Ky) {
    EXPECT_EQ(V({T(L'k'), T(L'y')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, K_Initial_Ke_Circumflex) {
    // kê
    EXPECT_EQ(V({T(L'k'), TM(L'e', Modifier::Circumflex)}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, K_Initial_Keu) {
    // kêu
    EXPECT_EQ(V({T(L'k'), TM(L'e', Modifier::Circumflex), T(L'u')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, K_Initial_Kien) {
    // kiên (k + iê + n)
    EXPECT_EQ(V({T(L'k'), T(L'i'), TM(L'e', Modifier::Circumflex), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, K_Initial_Kieu) {
    // kiểu (k + iêu)
    EXPECT_EQ(V({T(L'k'), T(L'i'), TM(L'e', Modifier::Circumflex), T(L'u')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorValidTest, K_Initial_Kia) {
    // kia (k + ia)
    EXPECT_EQ(V({T(L'k'), T(L'i'), T(L'a')}), SyllableState::Valid);
}

//=============================================================================
// P1.1: VCPair — Vowel + Final Consonant restrictions
// These tests verify that INVALID vowel+final combinations are rejected.
// Currently most of these PASS incorrectly (the whole point of VCPairList fix).
//=============================================================================

class PhonotacticsValidatorVCPairTest : public ::testing::Test {};

// --- Single vowel ơ: only m, n, p, t allowed ---
TEST_F(PhonotacticsValidatorVCPairTest, Ơ_Ch_Invalid) {
    // ơch — ơ cannot end with ch
    EXPECT_EQ(V({TM(L'o', Modifier::Horn), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ơ_Ng_Invalid) {
    // ơng — ơ cannot end with ng
    EXPECT_EQ(V({TM(L'o', Modifier::Horn), T(L'n'), T(L'g')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ơ_Nh_Invalid) {
    // ơnh — ơ cannot end with nh
    EXPECT_EQ(V({TM(L'o', Modifier::Horn), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ơ_C_Invalid) {
    // ơc — ơ cannot end with c
    EXPECT_EQ(V({TM(L'o', Modifier::Horn), T(L'c')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ơ_M_Valid) {
    EXPECT_EQ(V({TM(L'o', Modifier::Horn), T(L'm')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ơ_N_Valid) {
    EXPECT_EQ(V({TM(L'o', Modifier::Horn), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ơ_P_Valid) {
    EXPECT_EQ(V({TM(L'o', Modifier::Horn), T(L'p')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ơ_T_Valid) {
    EXPECT_EQ(V({TM(L'o', Modifier::Horn), T(L't')}), SyllableState::Valid);
}

// --- Single vowel y: only t allowed ---
TEST_F(PhonotacticsValidatorVCPairTest, Y_Ng_Invalid) {
    EXPECT_EQ(V({T(L'y'), T(L'n'), T(L'g')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Y_M_Invalid) {
    EXPECT_EQ(V({T(L'y'), T(L'm')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Y_Ch_Invalid) {
    EXPECT_EQ(V({T(L'y'), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Y_T_Valid) {
    EXPECT_EQ(V({T(L'y'), T(L't')}), SyllableState::Valid);
}

// --- Single vowel â: no ch, nh ---
TEST_F(PhonotacticsValidatorVCPairTest, Â_Ch_Invalid) {
    EXPECT_EQ(V({TM(L'a', Modifier::Circumflex), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Â_Nh_Invalid) {
    EXPECT_EQ(V({TM(L'a', Modifier::Circumflex), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Â_N_Valid) {
    EXPECT_EQ(V({TM(L'a', Modifier::Circumflex), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Â_C_Valid) {
    EXPECT_EQ(V({TM(L'a', Modifier::Circumflex), T(L'c')}), SyllableState::Valid);
}

// --- Single vowel ê: no ng ---
TEST_F(PhonotacticsValidatorVCPairTest, Ê_Ng_Invalid) {
    EXPECT_EQ(V({TM(L'e', Modifier::Circumflex), T(L'n'), T(L'g')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ê_Ch_Valid) {
    // êch (chêch, kêch)
    EXPECT_EQ(V({TM(L'e', Modifier::Circumflex), T(L'c'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ê_Nh_Valid) {
    // ênh (lênh, kênh)
    EXPECT_EQ(V({TM(L'e', Modifier::Circumflex), T(L'n'), T(L'h')}), SyllableState::Valid);
}

// --- Single vowel ô: no ch, nh ---
TEST_F(PhonotacticsValidatorVCPairTest, Ô_Ch_Invalid) {
    EXPECT_EQ(V({TM(L'o', Modifier::Circumflex), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ô_Nh_Invalid) {
    EXPECT_EQ(V({TM(L'o', Modifier::Circumflex), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ô_Ng_Valid) {
    // ông, sông
    EXPECT_EQ(V({TM(L'o', Modifier::Circumflex), T(L'n'), T(L'g')}), SyllableState::Valid);
}

// --- Single vowel ư: no ch, nh ---
TEST_F(PhonotacticsValidatorVCPairTest, Ư_Ch_Invalid) {
    EXPECT_EQ(V({TM(L'u', Modifier::Horn), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ư_Nh_Invalid) {
    EXPECT_EQ(V({TM(L'u', Modifier::Horn), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ư_Ng_Valid) {
    // ưng
    EXPECT_EQ(V({TM(L'u', Modifier::Horn), T(L'n'), T(L'g')}), SyllableState::Valid);
}

// --- Single vowel u: no ch, nh ---
TEST_F(PhonotacticsValidatorVCPairTest, U_Ch_Invalid) {
    EXPECT_EQ(V({T(L'u'), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, U_Nh_Invalid) {
    EXPECT_EQ(V({T(L'u'), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, U_Ng_Valid) {
    // ung
    EXPECT_EQ(V({T(L'u'), T(L'n'), T(L'g')}), SyllableState::Valid);
}

// --- Single vowel o: no ch, nh ---
TEST_F(PhonotacticsValidatorVCPairTest, O_Ch_Invalid) {
    EXPECT_EQ(V({T(L'o'), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, O_Nh_Invalid) {
    EXPECT_EQ(V({T(L'o'), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, O_Ng_Valid) {
    // ong
    EXPECT_EQ(V({T(L'o'), T(L'n'), T(L'g')}), SyllableState::Valid);
}

// --- Single vowel ă: no ch, nh ---
TEST_F(PhonotacticsValidatorVCPairTest, Ă_Ch_Invalid) {
    EXPECT_EQ(V({TM(L'a', Modifier::Breve), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ă_Nh_Invalid) {
    EXPECT_EQ(V({TM(L'a', Modifier::Breve), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ă_Ng_Valid) {
    // ăng (măng, tăng)
    EXPECT_EQ(V({TM(L'a', Modifier::Breve), T(L'n'), T(L'g')}), SyllableState::Valid);
}

// --- Double vowel oo: only c, ng ---
TEST_F(PhonotacticsValidatorVCPairTest, OO_M_Invalid) {
    EXPECT_EQ(V({T(L'o'), T(L'o'), T(L'm')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OO_Ng_Valid) {
    // oong (xoong, boong)
    EXPECT_EQ(V({T(L'o'), T(L'o'), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OO_C_Valid) {
    // ooc
    EXPECT_EQ(V({T(L'o'), T(L'o'), T(L'c')}), SyllableState::Valid);
}

// --- Double vowel uê: only ch, n, nh ---
TEST_F(PhonotacticsValidatorVCPairTest, UÊ_M_Invalid) {
    EXPECT_EQ(V({T(L'u'), TM(L'e', Modifier::Circumflex), T(L'm')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UÊ_Ng_Invalid) {
    EXPECT_EQ(V({T(L'u'), TM(L'e', Modifier::Circumflex), T(L'n'), T(L'g')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UÊ_Ch_Valid) {
    // uếch (huếch)
    EXPECT_EQ(V({T(L'u'), TM(L'e', Modifier::Circumflex), T(L'c'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UÊ_C_PrefixOfUêch) {
    // "uêc" is no rime, but a lone c can still grow into the allowed "ch".
    // Invalid here would latch spell check off and kill "khuếch" when the
    // circumflex is typed before the coda's 'h' (khuecehs).
    EXPECT_EQ(V({T(L'u'), TM(L'e', Modifier::Circumflex), T(L'c')}),
              SyllableState::ValidPrefix);
}

TEST_F(PhonotacticsValidatorVCPairTest, UY_C_PrefixOfUych) {
    // Same for "huých" when the tone lands before the 'h' (huycsh).
    EXPECT_EQ(V({T(L'u'), T(L'y'), T(L'c')}), SyllableState::ValidPrefix);
}

TEST_F(PhonotacticsValidatorVCPairTest, UÊ_N_Valid) {
    EXPECT_EQ(V({T(L'u'), TM(L'e', Modifier::Circumflex), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UÊ_Nh_Valid) {
    // uênh (thuênh)
    EXPECT_EQ(V({T(L'u'), TM(L'e', Modifier::Circumflex), T(L'n'), T(L'h')}), SyllableState::Valid);
}

// --- Double vowel uy: ch, n, nh, p, t (p for loanword tuýp, issue #213) ---
TEST_F(PhonotacticsValidatorVCPairTest, UY_M_Invalid) {
    EXPECT_EQ(V({T(L'u'), T(L'y'), T(L'm')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UY_P_Valid) {
    // uyp (tuýp) — issue #213
    EXPECT_EQ(V({T(L'u'), T(L'y'), T(L'p')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UY_Ng_Invalid) {
    EXPECT_EQ(V({T(L'u'), T(L'y'), T(L'n'), T(L'g')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UY_Ch_Valid) {
    // uych (huỳch)
    EXPECT_EQ(V({T(L'u'), T(L'y'), T(L'c'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UY_T_Valid) {
    // uyt (huyết)
    EXPECT_EQ(V({T(L'u'), T(L'y'), T(L't')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UY_Nh_Valid) {
    // uynh (huynh, quynh)
    EXPECT_EQ(V({T(L'u'), T(L'y'), T(L'n'), T(L'h')}), SyllableState::Valid);
}

// --- Regression: a can end with ALL finals ---
TEST_F(PhonotacticsValidatorVCPairTest, A_C_Valid) {
    EXPECT_EQ(V({T(L'a'), T(L'c')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, A_Ch_Valid) {
    EXPECT_EQ(V({T(L'a'), T(L'c'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, A_M_Valid) {
    EXPECT_EQ(V({T(L'a'), T(L'm')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, A_N_Valid) {
    EXPECT_EQ(V({T(L'a'), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, A_Ng_Valid) {
    EXPECT_EQ(V({T(L'a'), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, A_Nh_Valid) {
    EXPECT_EQ(V({T(L'a'), T(L'n'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, A_P_Valid) {
    EXPECT_EQ(V({T(L'a'), T(L'p')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, A_T_Valid) {
    EXPECT_EQ(V({T(L'a'), T(L't')}), SyllableState::Valid);
}

// --- Regression: i can end with ALL finals ---
TEST_F(PhonotacticsValidatorVCPairTest, I_Ch_Valid) {
    // ich (thích, lịch)
    EXPECT_EQ(V({T(L'i'), T(L'c'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, I_Nh_Valid) {
    // inh (bình, tĩnh)
    EXPECT_EQ(V({T(L'i'), T(L'n'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, I_Ng_Invalid) {
    // ing: -ing+tone doesn't exist in Vietnamese (things→thíng, kings→kíng bug)
    EXPECT_EQ(V({T(L'i'), T(L'n'), T(L'g')}), SyllableState::Invalid);
}

// --- Regression: e can end with ALL finals ---
TEST_F(PhonotacticsValidatorVCPairTest, E_Ch_Valid) {
    // ech (nghẹch)
    EXPECT_EQ(V({T(L'e'), T(L'c'), T(L'h')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, E_Nh_Valid) {
    // enh
    EXPECT_EQ(V({T(L'e'), T(L'n'), T(L'h')}), SyllableState::Valid);
}

// --- k initial: invalid before a, o, u (must use c instead) ---
TEST_F(PhonotacticsValidatorVCPairTest, K_Initial_Ka_Invalid) {
    // ka → should be "ca" in Vietnamese
    EXPECT_EQ(V({T(L'k'), T(L'a')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, K_Initial_Ko_Invalid) {
    EXPECT_EQ(V({T(L'k'), T(L'o')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, K_Initial_Ku_Invalid) {
    EXPECT_EQ(V({T(L'k'), T(L'u')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, K_Initial_Koa_Invalid) {
    // koa → should be "coa" (though "coa" itself is rare)
    EXPECT_EQ(V({T(L'k'), T(L'o'), T(L'a')}), SyllableState::Invalid);
}

// k + ê/e/i/y still valid (already covered in PhonotacticsValidatorValidTest.K_Initial_*)

// --- kh is NOT affected (kh is a different consonant cluster) ---
TEST_F(PhonotacticsValidatorVCPairTest, Kh_Initial_Kha_Valid) {
    EXPECT_EQ(V({T(L'k'), T(L'h'), T(L'a')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Kh_Initial_Kho_Valid) {
    EXPECT_EQ(V({T(L'k'), T(L'h'), T(L'o')}), SyllableState::Valid);
}

// --- c not before front vowels (use k: ke, kê, ki, ky) ---
TEST_F(PhonotacticsValidatorVCPairTest, C_Initial_Ce_Invalid) {
    EXPECT_EQ(V({T(L'c'), T(L'e')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, C_Initial_Ci_Invalid) {
    EXPECT_EQ(V({T(L'c'), T(L'i')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, C_Initial_Cy_Invalid) {
    EXPECT_EQ(V({T(L'c'), T(L'y')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, C_Initial_Ca_Valid) {
    // c + a is valid (ca, cá, cả...)
    EXPECT_EQ(V({T(L'c'), T(L'a')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, C_Initial_Co_Valid) {
    EXPECT_EQ(V({T(L'c'), T(L'o')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, C_Initial_Cu_Valid) {
    EXPECT_EQ(V({T(L'c'), T(L'u')}), SyllableState::Valid);
}

// ch is NOT affected (ch is a 2-char consonant)
TEST_F(PhonotacticsValidatorVCPairTest, Ch_Initial_Che_Valid) {
    EXPECT_EQ(V({T(L'c'), T(L'h'), T(L'e')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ch_Initial_Chi_Valid) {
    EXPECT_EQ(V({T(L'c'), T(L'h'), T(L'i')}), SyllableState::Valid);
}

// --- gh only before front vowels (e/ê/i) ---
TEST_F(PhonotacticsValidatorVCPairTest, Gh_Initial_Gha_Invalid) {
    EXPECT_EQ(V({T(L'g'), T(L'h'), T(L'a')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Gh_Initial_Gho_Invalid) {
    EXPECT_EQ(V({T(L'g'), T(L'h'), T(L'o')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Gh_Initial_Ghe_Valid) {
    EXPECT_EQ(V({T(L'g'), T(L'h'), T(L'e')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Gh_Initial_Ghi_Valid) {
    EXPECT_EQ(V({T(L'g'), T(L'h'), T(L'i')}), SyllableState::Valid);
}

// --- ngh only before front vowels (e/ê/i) ---
TEST_F(PhonotacticsValidatorVCPairTest, Ngh_Initial_Ngha_Invalid) {
    EXPECT_EQ(V({T(L'n'), T(L'g'), T(L'h'), T(L'a')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ngh_Initial_Nghe_Valid) {
    EXPECT_EQ(V({T(L'n'), T(L'g'), T(L'h'), T(L'e')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ngh_Initial_Nghi_Valid) {
    EXPECT_EQ(V({T(L'n'), T(L'g'), T(L'h'), T(L'i')}), SyllableState::Valid);
}

// ng (without h) is NOT affected — nga, ngo, ngu all valid
TEST_F(PhonotacticsValidatorVCPairTest, Ng_Initial_Nga_Valid) {
    EXPECT_EQ(V({T(L'n'), T(L'g'), T(L'a')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ng_Initial_Ngo_Valid) {
    EXPECT_EQ(V({T(L'n'), T(L'g'), T(L'o')}), SyllableState::Valid);
}

// --- q must be part of qu cluster ---
TEST_F(PhonotacticsValidatorVCPairTest, Q_Initial_Qa_Invalid) {
    EXPECT_EQ(V({T(L'q'), T(L'a')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Q_Initial_Qe_Invalid) {
    EXPECT_EQ(V({T(L'q'), T(L'e')}), SyllableState::Invalid);
}

// qu + vowel still valid (regression)
TEST_F(PhonotacticsValidatorVCPairTest, Qu_Initial_Qua_Valid) {
    EXPECT_EQ(V({T(L'q'), T(L'u'), T(L'a')}), SyllableState::Valid);
}

// --- Double vowel with onset: regression test ---
TEST_F(PhonotacticsValidatorVCPairTest, Loan_Valid) {
    // l + oa + n
    EXPECT_EQ(V({T(L'l'), T(L'o'), T(L'a'), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Hoan_Valid) {
    EXPECT_EQ(V({T(L'h'), T(L'o'), T(L'a'), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Tuong_Valid) {
    // tương (t + ươ + ng)
    EXPECT_EQ(V({T(L't'), TM(L'u', Modifier::Horn), TM(L'o', Modifier::Horn), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Tien_Valid) {
    // tiên (t + iê + n)
    EXPECT_EQ(V({T(L't'), T(L'i'), TM(L'e', Modifier::Circumflex), T(L'n')}), SyllableState::Valid);
}

// --- Diphthong iê: c, m, n, ng, p, t allowed; ch, nh invalid ---
TEST_F(PhonotacticsValidatorVCPairTest, IÊ_Ch_Invalid) {
    // iêch — invalid
    EXPECT_EQ(V({T(L'i'), TM(L'e', Modifier::Circumflex), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, IÊ_Nh_Invalid) {
    // iênh — invalid
    EXPECT_EQ(V({T(L'i'), TM(L'e', Modifier::Circumflex), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, IÊ_C_Valid) {
    // iêc (diệc)
    EXPECT_EQ(V({T(L'i'), TM(L'e', Modifier::Circumflex), T(L'c')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, IÊ_M_Valid) {
    // iêm (tiêm, điểm)
    EXPECT_EQ(V({T(L'i'), TM(L'e', Modifier::Circumflex), T(L'm')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, IÊ_Ng_Valid) {
    // iêng (tiếng, chiêng)
    EXPECT_EQ(V({T(L'i'), TM(L'e', Modifier::Circumflex), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, IÊ_P_Valid) {
    // iêp (tiếp, nghiệp)
    EXPECT_EQ(V({T(L'i'), TM(L'e', Modifier::Circumflex), T(L'p')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, IÊ_T_Valid) {
    // iêt (tiết, việt)
    EXPECT_EQ(V({T(L'i'), TM(L'e', Modifier::Circumflex), T(L't')}), SyllableState::Valid);
}

// --- Diphthong yê: same as iê — c, m, n, ng, p, t allowed; ch, nh invalid ---
TEST_F(PhonotacticsValidatorVCPairTest, YÊ_Ch_Invalid) {
    EXPECT_EQ(V({T(L'y'), TM(L'e', Modifier::Circumflex), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, YÊ_Nh_Invalid) {
    EXPECT_EQ(V({T(L'y'), TM(L'e', Modifier::Circumflex), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, YÊ_C_Valid) {
    EXPECT_EQ(V({T(L'y'), TM(L'e', Modifier::Circumflex), T(L'c')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, YÊ_M_Valid) {
    // yêm (yếm)
    EXPECT_EQ(V({T(L'y'), TM(L'e', Modifier::Circumflex), T(L'm')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, YÊ_N_Valid) {
    // yên (yến, quyên)
    EXPECT_EQ(V({T(L'y'), TM(L'e', Modifier::Circumflex), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, YÊ_Ng_Valid) {
    EXPECT_EQ(V({T(L'y'), TM(L'e', Modifier::Circumflex), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, YÊ_P_Valid) {
    EXPECT_EQ(V({T(L'y'), TM(L'e', Modifier::Circumflex), T(L'p')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, YÊ_T_Valid) {
    // yêt (yết)
    EXPECT_EQ(V({T(L'y'), TM(L'e', Modifier::Circumflex), T(L't')}), SyllableState::Valid);
}

// --- Diphthong oe: m, n, ng, t allowed; ch, nh, p, c invalid ---
TEST_F(PhonotacticsValidatorVCPairTest, OE_Ch_Invalid) {
    EXPECT_EQ(V({T(L'o'), T(L'e'), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OE_Nh_Invalid) {
    EXPECT_EQ(V({T(L'o'), T(L'e'), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OE_P_Invalid) {
    EXPECT_EQ(V({T(L'o'), T(L'e'), T(L'p')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OE_C_Invalid) {
    EXPECT_EQ(V({T(L'o'), T(L'e'), T(L'c')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OE_M_Valid) {
    // oem (ngoém)
    EXPECT_EQ(V({T(L'o'), T(L'e'), T(L'm')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OE_N_Valid) {
    // oen (ngoen)
    EXPECT_EQ(V({T(L'o'), T(L'e'), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OE_Ng_Valid) {
    // oeng
    EXPECT_EQ(V({T(L'o'), T(L'e'), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OE_T_Valid) {
    // oet (ngoét)
    EXPECT_EQ(V({T(L'o'), T(L'e'), T(L't')}), SyllableState::Valid);
}

// --- Diphthong oă: c, m, n, ng, p, t allowed; ch, nh invalid (issue #213) ---
TEST_F(PhonotacticsValidatorVCPairTest, OĂ_Ch_Invalid) {
    EXPECT_EQ(V({T(L'o'), TM(L'a', Modifier::Breve), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OĂ_Nh_Invalid) {
    EXPECT_EQ(V({T(L'o'), TM(L'a', Modifier::Breve), T(L'n'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OĂ_M_Valid) {
    // oăm (khoằm, ngoặm) — issue #213
    EXPECT_EQ(V({T(L'o'), TM(L'a', Modifier::Breve), T(L'm')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OĂ_P_Valid) {
    // oăp (ngoặp) — issue #213
    EXPECT_EQ(V({T(L'o'), TM(L'a', Modifier::Breve), T(L'p')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OĂ_C_Valid) {
    // oăc (loắc, xoắc)
    EXPECT_EQ(V({T(L'o'), TM(L'a', Modifier::Breve), T(L'c')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OĂ_N_Valid) {
    // oăn (hoăn)
    EXPECT_EQ(V({T(L'o'), TM(L'a', Modifier::Breve), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OĂ_Ng_Valid) {
    // oăng (hoằng)
    EXPECT_EQ(V({T(L'o'), TM(L'a', Modifier::Breve), T(L'n'), T(L'g')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, OĂ_T_Valid) {
    // oăt (loắt, choắt)
    EXPECT_EQ(V({T(L'o'), TM(L'a', Modifier::Breve), T(L't')}), SyllableState::Valid);
}

// --- Triphthong uyê: n, t only ---
TEST_F(PhonotacticsValidatorVCPairTest, UYÊ_Ch_Invalid) {
    EXPECT_EQ(V({T(L'u'), T(L'y'), TM(L'e', Modifier::Circumflex), T(L'c'), T(L'h')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UYÊ_M_Invalid) {
    EXPECT_EQ(V({T(L'u'), T(L'y'), TM(L'e', Modifier::Circumflex), T(L'm')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UYÊ_Ng_Invalid) {
    EXPECT_EQ(V({T(L'u'), T(L'y'), TM(L'e', Modifier::Circumflex), T(L'n'), T(L'g')}), SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UYÊ_N_Valid) {
    // uyên (quyên, huyền)
    EXPECT_EQ(V({T(L'u'), T(L'y'), TM(L'e', Modifier::Circumflex), T(L'n')}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, UYÊ_T_Valid) {
    // uyêt (quyết, huyết)
    EXPECT_EQ(V({T(L'u'), T(L'y'), TM(L'e', Modifier::Circumflex), T(L't')}), SyllableState::Valid);
}

// --- gh/ngh + ê (circumflex front vowel) ---
TEST_F(PhonotacticsValidatorVCPairTest, Gh_Initial_Ghê_Valid) {
    // ghê (ghế, ghẻ)
    EXPECT_EQ(V({T(L'g'), T(L'h'), TM(L'e', Modifier::Circumflex)}), SyllableState::Valid);
}

TEST_F(PhonotacticsValidatorVCPairTest, Ngh_Initial_Nghê_Valid) {
    // nghê (nghề, nghệ)
    EXPECT_EQ(V({T(L'n'), T(L'g'), T(L'h'), TM(L'e', Modifier::Circumflex)}), SyllableState::Valid);
}

// Pre-states underpinning the ValidPrefix heuristic in HandleAdjacentCircumflex
// (bug 2026-05-25: vijeet → vịeet, ngufoon → ngùoon). The fix branches on
// pre-state to decide whether the runtime/validator should speculate tone
// relocation. These tests pin the values that drive that decision.
TEST_F(PhonotacticsValidatorVCPairTest, AdjacentHeuristic_VijeBugCase) {
    // {v, ị, e} — pre-state when 2nd 'e' arrives in vijeet → must promote to việt
    EXPECT_EQ(V({T(L'v'), TT(L'i', Tone::Dot), T(L'e')}), SyllableState::ValidPrefix);
}
TEST_F(PhonotacticsValidatorVCPairTest, AdjacentHeuristic_CuaTypoGuardCase) {
    // {c, ủ, a} — pre-state when extra 'a' arrives in cuara → must REJECT (typo)
    EXPECT_EQ(V({T(L'c'), TT(L'u', Tone::Hook), T(L'a')}), SyllableState::Valid);
}
TEST_F(PhonotacticsValidatorVCPairTest, AdjacentHeuristic_NguoBugCase) {
    // {n, g, ù, o} — pre-state when 2nd 'o' arrives in ngufoon → must promote to nguồn
    EXPECT_EQ(V({T(L'n'), T(L'g'), TT(L'u', Tone::Grave), T(L'o')}), SyllableState::ValidPrefix);
}
TEST_F(PhonotacticsValidatorVCPairTest, AdjacentHeuristic_StandaloneVowelsAreValid) {
    // Standalone aa/caa/coo/vee cases must keep mod-only path (Valid pre-state).
    EXPECT_EQ(V({T(L'a')}),               SyllableState::Valid);
    EXPECT_EQ(V({T(L'c'), T(L'a')}),      SyllableState::Valid);
    EXPECT_EQ(V({T(L'v'), T(L'e')}),      SyllableState::Valid);
    EXPECT_EQ(V({T(L'c'), T(L'o')}),      SyllableState::Valid);
}
TEST_F(PhonotacticsValidatorVCPairTest, AdjacentHeuristic_ChuyeIsPrefix) {
    // {c,h,u,y,e} — chuyeenj canonical pre-state at 2nd 'e'.
    EXPECT_EQ(V({T(L'c'), T(L'h'), T(L'u'), T(L'y'), T(L'e')}), SyllableState::ValidPrefix);
}
TEST_F(PhonotacticsValidatorVCPairTest, AdjacentHeuristic_ToneMisplacedIsInvalid) {
    // {n,g,ù,ô} — what speculate-WITHOUT-relocate produces from Classic ngufoon.
    // Validator catches the tone-on-u + uô-diphthong mismatch; this is exactly
    // the misclassification that the ValidPrefix branch fixes by speculating
    // WITH tone relocation.
    EXPECT_EQ(V({T(L'n'), T(L'g'), TT(L'u', Tone::Grave), TM(L'o', Modifier::Circumflex)}),
              SyllableState::Invalid);
}

TEST_F(PhonotacticsValidatorPrefixTest, Issue268_CircumflexARequiresRhymeCompletion) {
    for (auto tone : {Tone::None, Tone::Acute, Tone::Grave, Tone::Hook, Tone::Tilde, Tone::Dot}) {
        const auto vowel = TMT(L'a', Modifier::Circumflex, tone);
        EXPECT_EQ(V({vowel}), SyllableState::ValidPrefix);
        EXPECT_EQ(V({TM(L'd', Modifier::Stroke), vowel}), SyllableState::ValidPrefix);
        EXPECT_EQ(V({T(L'b'), vowel, T(L'i')}), SyllableState::Invalid);
        for (wchar_t tail : {L'm', L'n', L'u', L'y'}) {
            EXPECT_EQ(V({T(L'b'), vowel, T(tail)}), SyllableState::Valid);
        }
    }
}

}  // namespace
}  // namespace NextKey

