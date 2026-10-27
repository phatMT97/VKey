// VKey - TelexEngine Unit Tests
// SPDX-License-Identifier: GPL-3.0-only
// Story 1.2: Comprehensive Telex transformation tests (50+ tests)

#include <gtest/gtest.h>
#include "core/engine/TypingEngine.h"
#include "core/config/TypingConfig.h"
#include "TestHelper.h"

namespace NextKey {
namespace {

using Testing::TypeString;

class TelexEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::Telex;
        config_.spellCheckEnabled = false;
        config_.optimizeLevel = 0;
        config_.modernOrtho = false; // Tests here expect classic behavior by default
        engine_ = std::make_unique<TypingEngine>(config_);
    }

    TypingConfig config_;
    std::unique_ptr<TypingEngine> engine_;
};

// ============================================================================
// CIRCUMFLEX TESTS (aa→â, ee→ê, oo→ô)
// ============================================================================

TEST_F(TelexEngineTest, Circumflex_AA_LowerCase) {
    TypeString(*engine_, L"aa");
    EXPECT_EQ(engine_->Peek(), L"â");
}

TEST_F(TelexEngineTest, Circumflex_AA_UpperCase) {
    TypeString(*engine_, L"AA");
    EXPECT_EQ(engine_->Peek(), L"Â");
}

TEST_F(TelexEngineTest, Circumflex_EE_LowerCase) {
    TypeString(*engine_, L"ee");
    EXPECT_EQ(engine_->Peek(), L"ê");
}

TEST_F(TelexEngineTest, Circumflex_EE_UpperCase) {
    TypeString(*engine_, L"EE");
    EXPECT_EQ(engine_->Peek(), L"Ê");
}

TEST_F(TelexEngineTest, Circumflex_OO_LowerCase) {
    TypeString(*engine_, L"oo");
    EXPECT_EQ(engine_->Peek(), L"ô");
}

TEST_F(TelexEngineTest, Circumflex_OO_UpperCase) {
    TypeString(*engine_, L"OO");
    EXPECT_EQ(engine_->Peek(), L"Ô");
}

TEST_F(TelexEngineTest, Circumflex_WithPrefix) {
    TypeString(*engine_, L"coo");
    EXPECT_EQ(engine_->Peek(), L"cô");
}

TEST_F(TelexEngineTest, Circumflex_AcrossVowel_A) {
    TypeString(*engine_, L"ddaua");
    EXPECT_EQ(engine_->Peek(), L"đâu");
}

TEST_F(TelexEngineTest, Circumflex_AcrossVowel_O) {
    TypeString(*engine_, L"uoio");
    EXPECT_EQ(engine_->Peek(), L"uôi");
}

TEST_F(TelexEngineTest, Circumflex_AcrossVowel_E) {
    TypeString(*engine_, L"ieue");
    EXPECT_EQ(engine_->Peek(), L"iêu");
}


// ============================================================================
// BREVE TESTS (aw→ă)
// ============================================================================

TEST_F(TelexEngineTest, Breve_AW_LowerCase) {
    TypeString(*engine_, L"aw");
    EXPECT_EQ(engine_->Peek(), L"ă");
}

TEST_F(TelexEngineTest, Breve_AW_UpperCase) {
    TypeString(*engine_, L"AW");
    EXPECT_EQ(engine_->Peek(), L"Ă");
}

TEST_F(TelexEngineTest, Breve_WithPrefix) {
    TypeString(*engine_, L"taw");
    EXPECT_EQ(engine_->Peek(), L"tă");
}

// ============================================================================
// HORN TESTS (ow→ơ, uw→ư)
// ============================================================================

TEST_F(TelexEngineTest, Horn_OW_LowerCase) {
    TypeString(*engine_, L"ow");
    EXPECT_EQ(engine_->Peek(), L"ơ");
}

TEST_F(TelexEngineTest, Horn_OW_UpperCase) {
    TypeString(*engine_, L"OW");
    EXPECT_EQ(engine_->Peek(), L"Ơ");
}

TEST_F(TelexEngineTest, Horn_UW_LowerCase) {
    TypeString(*engine_, L"uw");
    EXPECT_EQ(engine_->Peek(), L"ư");
}

TEST_F(TelexEngineTest, Horn_UW_UpperCase) {
    TypeString(*engine_, L"UW");
    EXPECT_EQ(engine_->Peek(), L"Ư");
}

TEST_F(TelexEngineTest, Horn_UO_DirectTransform) {
    // uow → ươ (direct horn on both u and o)
    TypeString(*engine_, L"uow");
    EXPECT_EQ(engine_->Peek(), L"ươ");
}

TEST_F(TelexEngineTest, Horn_UO_AutoTransform) {
    // uown → ươn (already ươ from P2, AutoUO not needed)
    TypeString(*engine_, L"uown");
    EXPECT_EQ(engine_->Peek(), L"ươn");
}

TEST_F(TelexEngineTest, Horn_UO_AutoTransform_WAfterConsonant) {
    // huonw → hươn (w horns both u and o, n already follows)
    TypeString(*engine_, L"huonw");
    EXPECT_EQ(engine_->Peek(), L"hươn");
}

TEST_F(TelexEngineTest, Horn_UA_UndoCircumflex) {
    // giuaaw → giưa (w on u, undo circumflex on a)
    // When 'w' applies horn to 'u' and 'â' follows, undo the circumflex
    TypeString(*engine_, L"giuaaw");
    EXPECT_EQ(engine_->Peek(), L"giưa");
}

TEST_F(TelexEngineTest, Horn_UO_Escape_2State) {
    // uoww → uo (non h/th/kh: 2-state cycle, second w escapes)
    TypeString(*engine_, L"uoww");
    EXPECT_EQ(engine_->Peek(), L"uow");
}

TEST_F(TelexEngineTest, Horn_UO_CircumflexThenW) {
    // luoow → l + u + ô (from oo) + w → lươ (w replaces circumflex with horn)
    TypeString(*engine_, L"luoow");
    EXPECT_EQ(engine_->Peek(), L"lươ");
}

TEST_F(TelexEngineTest, Horn_UO_ToneRelocate) {
    // trưrơw: tone typed before diphthong complete → relocates to ơ
    // t-r-u-w-r-o-w → trư + hook → trử + o + w → trưở (not trửơ)
    TypeString(*engine_, L"truwrow");
    EXPECT_EQ(engine_->Peek(), L"trưở");
}

TEST_F(TelexEngineTest, Horn_O_CircumflexToHorn) {
    // loow → l + ô (from oo) + w → lơ (w replaces circumflex with horn)
    TypeString(*engine_, L"loow");
    EXPECT_EQ(engine_->Peek(), L"lơ");
}

// --- UO Horn Cycle: h/th/kh edge case prefix (3-state) ---

TEST_F(TelexEngineTest, Horn_UO_HPrefix_FirstW) {
    // huow → huơ (first w: default uơ for h prefix)
    TypeString(*engine_, L"huow");
    EXPECT_EQ(engine_->Peek(), L"huơ");
}

TEST_F(TelexEngineTest, Horn_UO_HPrefix_SecondW) {
    // huoww → hươ (second w: uơ → ươ for h prefix)
    TypeString(*engine_, L"huoww");
    EXPECT_EQ(engine_->Peek(), L"hươ");
}

TEST_F(TelexEngineTest, Horn_UO_HPrefix_ThirdW) {
    // huowww → huo + w literal (third w: escape)
    TypeString(*engine_, L"huowww");
    EXPECT_EQ(engine_->Peek(), L"huow");
}

TEST_F(TelexEngineTest, Horn_UO_THPrefix_FirstW) {
    // thuow → thuơ (first w: default uơ for th prefix)
    TypeString(*engine_, L"thuow");
    EXPECT_EQ(engine_->Peek(), L"thuơ");
}

TEST_F(TelexEngineTest, Horn_UO_THPrefix_SecondW) {
    // thuoww → thươ (second w: uơ → ươ for th prefix)
    TypeString(*engine_, L"thuoww");
    EXPECT_EQ(engine_->Peek(), L"thươ");
}

TEST_F(TelexEngineTest, Horn_UO_THPrefix_ThirdW) {
    // thuowww → thuo + w literal (third w: escape)
    TypeString(*engine_, L"thuowww");
    EXPECT_EQ(engine_->Peek(), L"thuow");
}

TEST_F(TelexEngineTest, Horn_UO_KHPrefix_SecondW) {
    // khuoww → khươ (second w: uơ → ươ for kh prefix)
    TypeString(*engine_, L"khuoww");
    EXPECT_EQ(engine_->Peek(), L"khươ");
}

TEST_F(TelexEngineTest, Horn_UO_THPrefix_Thuor) {
    // thuowr → thuở (first w gives uơ, then r adds tone hỏi)
    TypeString(*engine_, L"thuowr");
    EXPECT_EQ(engine_->Peek(), L"thuở");
}

// --- UO Horn Cycle: non-edge prefix (2-state) ---

TEST_F(TelexEngineTest, Horn_UO_DPrefix_FirstW) {
    // duow → dươ (first w: default ươ)
    TypeString(*engine_, L"duow");
    EXPECT_EQ(engine_->Peek(), L"dươ");
}

TEST_F(TelexEngineTest, Horn_UO_DPrefix_SecondW) {
    // duoww → duo + w literal (second w: escape, skip uơ)
    TypeString(*engine_, L"duoww");
    EXPECT_EQ(engine_->Peek(), L"duow");
}

TEST_F(TelexEngineTest, Horn_UO_NPrefix_WithFinal) {
    // nuowng → nương (w horns both, then ng final)
    TypeString(*engine_, L"nuowng");
    EXPECT_EQ(engine_->Peek(), L"nương");
}

// --- AutoUO respects h/th/kh edge case ---

TEST_F(TelexEngineTest, Horn_UO_HPrefix_AutoUO_Completes) {
    // huown → hươn (first w gives huơ, n triggers AutoUO to complete ươ)
    TypeString(*engine_, L"huown");
    EXPECT_EQ(engine_->Peek(), L"hươn");
}

// --- D12.5: 3.3 chaos truongwf → trường (full uong + late w + tone) ---

TEST_F(TelexEngineTest, D12_5_Truongwf_FullCodaThenWThenTone) {
    // truongwf → trường
    // Steps: t-r-u-o-n-g (= "truong"), then w should retro-horn uo→ươ
    // making "trương", then f tone on second vowel (coda present) → "trường".
    TypeString(*engine_, L"truongwf");
    EXPECT_EQ(engine_->Peek(), L"trường");
}

TEST_F(TelexEngineTest, D12_5_Truongw_NoTone) {
    // Intermediate state: truongw → trương (w retro-horns uo after full ng coda)
    TypeString(*engine_, L"truongw");
    EXPECT_EQ(engine_->Peek(), L"trương");
}

// --- Sprint 2 D0: 5.3 Chrome bug — engine-layer isolation test ---
//
// Repro for chaos 5.3 "vieejt nam BS×4 s" → expected "viết" but Chrome
// produces "việts". Per Sprint 2 D0 hook-log analysis, the failure is
// in HookEngine's commit-undo "Primed → cancel on synthPending" state
// machine: under chaos 1ms inter-key, 's' arrives before the BS#4 synth
// events drain → Primed canceled → engine never gets "việt" replayed →
// 's' processed fresh → screen shows "việts".
//
// This test ISOLATES the engine layer: pushes the equivalent post-replay
// sequence ("viejt" then "s") directly. No hook, no commit, no synthetic
// events.
//
//   PASS → engine is clean. Bug is purely in HookEngine commit-undo
//          replay. Sprint 2 fix lives in HookEngine, not TypingEngine.
//   FAIL → engine itself can't do the tone replacement. Fix needed in
//          TypingEngine tone-replacement logic.
//
// Reference: docs/baselines/perf-baseline-d12-chrome-cross-app.md
// HookEngine state-machine site: HookEngine.cpp "commit-undo: cancel Primed"

TEST_F(TelexEngineTest, S2D0_ChromeBug53_VieejtPlusS_ToneReplace) {
    // Per HookEngine::ReplayCommittedChars (HookEngine.cpp:1621): replay pushes
    // the ORIGINAL user keystrokes from CommitEntry::history into the engine —
    // not the composed Unicode chars. So a committed 'việt' from input 'vieejt'
    // replays as PushChar('v'),('i'),('e'),('e'),('j'),('t').
    //
    // This test feeds the post-replay-equivalent sequence: 'vieejt' then 's'.

    TypeString(*engine_, L"vieejt");
    EXPECT_EQ(engine_->Peek(), L"việt") << "Baseline 'vieejt' → 'việt' must hold";

    // Bug repro: 's' after 'việt' should replace j-tone (nặng) with
    // s-tone (sắc), preserving the ê circumflex → 'viết'.
    engine_->PushChar(L's');
    EXPECT_EQ(engine_->Peek(), L"viết")
        << "After 'vieejt'+'s', engine should replace nặng with sắc → 'viết'.\n"
        << "PASS means bug is purely in HookEngine commit-undo state machine.\n"
        << "FAIL means TypingEngine tone-replacement logic is also broken.";
}

// Sanity check: the simpler path (forward typing without replay) for the same
// final word — verifies the engine CAN produce 'viết' when 's' is the only
// tone modifier on the syllable. This locks in what 'right' looks like.
TEST_F(TelexEngineTest, S2D0_ChromeBug53_Sanity_VieetsForward) {
    TypeString(*engine_, L"vieets");
    EXPECT_EQ(engine_->Peek(), L"viết");
}

// ============================================================================
// STROKE TESTS (dd→đ)
// ============================================================================

TEST_F(TelexEngineTest, Stroke_DD_LowerCase) {
    TypeString(*engine_, L"dd");
    EXPECT_EQ(engine_->Peek(), L"đ");
}

TEST_F(TelexEngineTest, Stroke_DD_UpperCase) {
    TypeString(*engine_, L"DD");
    EXPECT_EQ(engine_->Peek(), L"Đ");
}

TEST_F(TelexEngineTest, Stroke_WithSuffix) {
    TypeString(*engine_, L"ddi");
    EXPECT_EQ(engine_->Peek(), L"đi");
}

TEST_F(TelexEngineTest, Stroke_NotContinue) {
    TypeString(*engine_, L"did");
    EXPECT_EQ(engine_->Peek(), L"đi");
}
// ============================================================================
// TONE MARKS - Basic Vowels (s=acute, f=grave, r=hook, x=tilde, j=dot)
// ============================================================================

// Acute (s)
TEST_F(TelexEngineTest, Tone_Acute_A) {
    TypeString(*engine_, L"as");
    EXPECT_EQ(engine_->Peek(), L"á");
}

TEST_F(TelexEngineTest, Tone_Acute_E) {
    TypeString(*engine_, L"es");
    EXPECT_EQ(engine_->Peek(), L"é");
}

TEST_F(TelexEngineTest, Tone_Acute_I) {
    TypeString(*engine_, L"is");
    EXPECT_EQ(engine_->Peek(), L"í");
}

TEST_F(TelexEngineTest, Tone_Acute_O) {
    TypeString(*engine_, L"os");
    EXPECT_EQ(engine_->Peek(), L"ó");
}

TEST_F(TelexEngineTest, Tone_Acute_U) {
    TypeString(*engine_, L"us");
    EXPECT_EQ(engine_->Peek(), L"ú");
}

TEST_F(TelexEngineTest, Tone_Acute_Y) {
    TypeString(*engine_, L"ys");
    EXPECT_EQ(engine_->Peek(), L"ý");
}

// Grave (f)
TEST_F(TelexEngineTest, Tone_Grave_A) {
    TypeString(*engine_, L"af");
    EXPECT_EQ(engine_->Peek(), L"à");
}

TEST_F(TelexEngineTest, Tone_Grave_E) {
    TypeString(*engine_, L"ef");
    EXPECT_EQ(engine_->Peek(), L"è");
}

// Hook (r)
TEST_F(TelexEngineTest, Tone_Hook_A) {
    TypeString(*engine_, L"ar");
    EXPECT_EQ(engine_->Peek(), L"ả");
}

TEST_F(TelexEngineTest, Tone_Hook_O) {
    TypeString(*engine_, L"or");
    EXPECT_EQ(engine_->Peek(), L"ỏ");
}

// Tilde (x)
TEST_F(TelexEngineTest, Tone_Tilde_A) {
    TypeString(*engine_, L"ax");
    EXPECT_EQ(engine_->Peek(), L"ã");
}

TEST_F(TelexEngineTest, Tone_Tilde_U) {
    TypeString(*engine_, L"ux");
    EXPECT_EQ(engine_->Peek(), L"ũ");
}

// Dot below (j)
TEST_F(TelexEngineTest, Tone_Dot_A) {
    TypeString(*engine_, L"aj");
    EXPECT_EQ(engine_->Peek(), L"ạ");
}

TEST_F(TelexEngineTest, Tone_Dot_E) {
    TypeString(*engine_, L"ej");
    EXPECT_EQ(engine_->Peek(), L"ẹ");
}

// ============================================================================
// TONE MARKS - Modified Vowels (circumflex, breve, horn)
// ============================================================================

TEST_F(TelexEngineTest, Tone_Circumflex_Acute) {
    TypeString(*engine_, L"aas");
    EXPECT_EQ(engine_->Peek(), L"ấ");
}

TEST_F(TelexEngineTest, Tone_Circumflex_Grave) {
    TypeString(*engine_, L"aaf");
    EXPECT_EQ(engine_->Peek(), L"ầ");
}

TEST_F(TelexEngineTest, Tone_Circumflex_E_Acute) {
    TypeString(*engine_, L"ees");
    EXPECT_EQ(engine_->Peek(), L"ế");
}

TEST_F(TelexEngineTest, Tone_Breve_Acute) {
    TypeString(*engine_, L"aws");
    EXPECT_EQ(engine_->Peek(), L"ắ");
}

TEST_F(TelexEngineTest, Tone_Breve_Grave) {
    TypeString(*engine_, L"awf");
    EXPECT_EQ(engine_->Peek(), L"ằ");
}

TEST_F(TelexEngineTest, Tone_Horn_O_Acute) {
    TypeString(*engine_, L"ows");
    EXPECT_EQ(engine_->Peek(), L"ớ");
}

TEST_F(TelexEngineTest, Tone_Horn_U_Acute) {
    TypeString(*engine_, L"uws");
    EXPECT_EQ(engine_->Peek(), L"ứ");
}

TEST_F(TelexEngineTest, Tone_Horn_U_Grave) {
    TypeString(*engine_, L"uwf");
    EXPECT_EQ(engine_->Peek(), L"ừ");
}

// ============================================================================
// TONE PLACEMENT - Vietnamese vowel pair rules
// ============================================================================

TEST_F(TelexEngineTest, TonePlacement_AO_ToneOnA) {
    TypeString(*engine_, L"aos");
    EXPECT_EQ(engine_->Peek(), L"áo");
}

TEST_F(TelexEngineTest, TonePlacement_EO_ToneOnE) {
    TypeString(*engine_, L"eof");
    EXPECT_EQ(engine_->Peek(), L"èo");
}

TEST_F(TelexEngineTest, TonePlacement_AU_ToneOnA) {
    TypeString(*engine_, L"aur");
    EXPECT_EQ(engine_->Peek(), L"ảu");
}

TEST_F(TelexEngineTest, TonePlacement_AI_ToneOnA) {
    TypeString(*engine_, L"aix");
    EXPECT_EQ(engine_->Peek(), L"ãi");
}

TEST_F(TelexEngineTest, TonePlacement_OI_ToneOnO) {
    TypeString(*engine_, L"oif");
    EXPECT_EQ(engine_->Peek(), L"òi");
}

TEST_F(TelexEngineTest, TonePlacement_UI_ToneOnU) {
    TypeString(*engine_, L"uij");
    EXPECT_EQ(engine_->Peek(), L"ụi");
}

TEST_F(TelexEngineTest, TonePlacement_Chao_Grave) {
    TypeString(*engine_, L"chaof");
    EXPECT_EQ(engine_->Peek(), L"chào");
}

// ============================================================================
// FULL VIETNAMESE WORDS
// ============================================================================

TEST_F(TelexEngineTest, Word_Viet) {
    TypeString(*engine_, L"vieetj");
    EXPECT_EQ(engine_->Peek(), L"việt");
}

TEST_F(TelexEngineTest, Word_Chao) {
    TypeString(*engine_, L"chaof");
    EXPECT_EQ(engine_->Peek(), L"chào");
}

TEST_F(TelexEngineTest, Word_Cam_On) {
    // cảm = c + ả + m, typed as carm (r = hook tone)
    TypeString(*engine_, L"carm");
    EXPECT_EQ(engine_->Peek(), L"cảm");
}

TEST_F(TelexEngineTest, Word_Xin) {
    TypeString(*engine_, L"xin");
    EXPECT_EQ(engine_->Peek(), L"xin");
}

TEST_F(TelexEngineTest, Word_Di) {
    TypeString(*engine_, L"ddi");
    EXPECT_EQ(engine_->Peek(), L"đi");
}

TEST_F(TelexEngineTest, Word_Duong) {
    TypeString(*engine_, L"dduowngf");
    EXPECT_EQ(engine_->Peek(), L"đường");
}

TEST_F(TelexEngineTest, Word_Nguoi) {
    // người = ng + ư + ờ + i
    // Typical typing: nguwowif = ng + uw(→ư) + ow(→ơ) + f(grave) + i
    TypeString(*engine_, L"nguowif");
    EXPECT_EQ(engine_->Peek(), L"người");
}

TEST_F(TelexEngineTest, Word_Huo) {
    // huow → huơ (first w: default uơ for h prefix)
    TypeString(*engine_, L"huow");
    EXPECT_EQ(engine_->Peek(), L"huơ");
}

TEST_F(TelexEngineTest, Word_Huo_WithSecondW) {
    // huoww → hươ (second w cycles uơ → ươ for h prefix)
    TypeString(*engine_, L"huoww");
    EXPECT_EQ(engine_->Peek(), L"hươ");
}

TEST_F(TelexEngineTest, Word_Cuoiwo_UndoHorn) {
    // cươi + o → cuôi: typing 'o' undoes the horn (ươ→uô)
    TypeString(*engine_, L"cuoiwo");
    EXPECT_EQ(engine_->Peek(), L"cuôi");
}

TEST_F(TelexEngineTest, Word_Yunr_ToneOnU) {
    // "yunr": yu diphthong with coda 'n' → tone on 'u' (SECOND), not 'y'
    TypeString(*engine_, L"yunr");
    EXPECT_EQ(engine_->Peek(), L"yủn");
}

TEST_F(TelexEngineTest, Word_Kkhuyur_Produces_Khuỷu) {
    // kk→kh, uyu triphthong, r→hook tone
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"kkuyur");
    EXPECT_EQ(engine_->Peek(), L"khuỷu");
}

TEST_F(TelexEngineTest, Triphthong_UYU_ExtraU_NotExpandedToUO) {
    // khuyu + u: triphthong uyu complete, extra 'u' should NOT trigger uu→ươ
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"kkuyuu");
    EXPECT_EQ(engine_->Peek(), L"khuyuu");
}

TEST_F(TelexEngineTest, Word_Ngoeoof_Produces_Ngoèo) {
    // oeo triphthong: 4th 'o' should NOT apply circumflex to 3rd 'o'
    // ngoeo + o → ngoeoo (literal), then f → ngoèo
    TypeString(*engine_, L"ngoeoof");
    EXPECT_EQ(engine_->Peek(), L"ngoèo");
}

TEST_F(TelexEngineTest, Triphthong_OEO_ExtraO_Consumed) {
    // ngoeo + o: triphthong oeo complete, extra 'o' consumed (no-op)
    TypeString(*engine_, L"ngoeo");
    EXPECT_EQ(engine_->Peek(), L"ngoeo");
    EXPECT_EQ(engine_->Count(), 5);
    engine_->PushChar(L'o');
    EXPECT_EQ(engine_->Count(), 5);  // No new state added
    EXPECT_EQ(engine_->Peek(), L"ngoeo");
}

TEST_F(TelexEngineTest, Triphthong_OAO_ExtraO_Consumed) {
    // ngoao + o: triphthong oao complete, extra 'o' consumed (no-op)
    TypeString(*engine_, L"ngoao");
    EXPECT_EQ(engine_->Peek(), L"ngoao");
    EXPECT_EQ(engine_->Count(), 5);
    engine_->PushChar(L'o');
    EXPECT_EQ(engine_->Count(), 5);  // No new state added
    EXPECT_EQ(engine_->Peek(), L"ngoao");
}

TEST_F(TelexEngineTest, Word_Quo) {
    TypeString(*engine_, L"quow");
    EXPECT_EQ(engine_->Peek(), L"quơ");
}

TEST_F(TelexEngineTest, Word_Hoc) {
    // học = h + ọ + c, typed as hocj (j = dot below)
    TypeString(*engine_, L"hocj");
    EXPECT_EQ(engine_->Peek(), L"học");
}

TEST_F(TelexEngineTest, Word_Duoc_NotContinue) {
    // được = đ + ư + ợ + c, tone on ơ (NOT ư)
    // Typed as: dd + uow + c + j
    TypeString(*engine_, L"duowjdc");
    EXPECT_EQ(engine_->Peek(), L"được");
}

TEST_F(TelexEngineTest, Word_Tieng) {
    TypeString(*engine_, L"tieengs");
    EXPECT_EQ(engine_->Peek(), L"tiếng");
}

TEST_F(TelexEngineTest, Word_Nam) {
    TypeString(*engine_, L"nawm");
    EXPECT_EQ(engine_->Peek(), L"năm");
}

TEST_F(TelexEngineTest, Word_Duoc) {
    // được = đ + ư + ợ + c, tone on ơ (NOT ư)
    // Typed as: dd + uow + c + j
    TypeString(*engine_, L"dduowcj");
    EXPECT_EQ(engine_->Peek(), L"được");
}

// ============================================================================
// TONE RELOCATION AFTER CIRCUMFLEX TESTS
// ============================================================================

TEST_F(TelexEngineTest, ToneRelocate_ChuanRA_ToneMovesToCircumflex) {
    // "chuanra": tone 'r' first (on 'u'), then 'a' adds circumflex → tone moves to 'â'
    TypeString(*engine_, L"chuanra");
    EXPECT_EQ(engine_->Peek(), L"chuẩn");
}

TEST_F(TelexEngineTest, ToneRelocate_TuanFA_ToneMovesToCircumflex) {
    // "tuanfa": tone 'f' first (on 'u'), then 'a' adds circumflex → tone moves to 'â'
    TypeString(*engine_, L"tuanfa");
    EXPECT_EQ(engine_->Peek(), L"tuần");
}

TEST_F(TelexEngineTest, ToneRelocate_LuatJA_ToneMovesToCircumflex) {
    // "luatja": tone 'j' first (on 'u'), then 'a' adds circumflex → tone moves to 'â'
    TypeString(*engine_, L"luatja");
    EXPECT_EQ(engine_->Peek(), L"luật");
}

// ============================================================================
// BACKSPACE TESTS
// ============================================================================

TEST_F(TelexEngineTest, Backspace_RemovesLastCharacter) {
    TypeString(*engine_, L"vie");
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"vi");
}

TEST_F(TelexEngineTest, Backspace_AfterCircumflex) {
    TypeString(*engine_, L"aa");  // → â (single state with circumflex)
    engine_->Backspace();         // Removes entire â
    EXPECT_EQ(engine_->Count(), 0u);
}

TEST_F(TelexEngineTest, Backspace_OnEmptyBuffer) {
    engine_->Backspace();  // Should not crash
    EXPECT_EQ(engine_->Count(), 0u);
}

TEST_F(TelexEngineTest, Backspace_AfterTone) {
    TypeString(*engine_, L"as");  // → á (single state with acute tone)
    engine_->Backspace();         // Removes entire á
    EXPECT_EQ(engine_->Count(), 0u);
}

TEST_F(TelexEngineTest, Backspace_AfterModifierAndTone) {
    TypeString(*engine_, L"aas"); // → ấ (â with acute)
    engine_->Backspace();         // Removes entire ấ
    EXPECT_EQ(engine_->Count(), 0u);
}

TEST_F(TelexEngineTest, Backspace_MultipleConsecutive) {
    TypeString(*engine_, L"abcde");
    engine_->Backspace();
    engine_->Backspace();
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"ab");
}

// ============================================================================
// COMMIT AND RESET TESTS
// ============================================================================

TEST_F(TelexEngineTest, Commit_ReturnsCorrectTextAndClears) {
    TypeString(*engine_, L"vieetj");
    std::wstring committed = engine_->Commit();
    
    EXPECT_EQ(committed, L"việt");
    EXPECT_EQ(engine_->Count(), 0u);
    EXPECT_EQ(engine_->Peek(), L"");
}

TEST_F(TelexEngineTest, Reset_ClearsAllState) {
    TypeString(*engine_, L"hello");
    engine_->Reset();
    
    EXPECT_EQ(engine_->Count(), 0u);
    EXPECT_EQ(engine_->Peek(), L"");
}

TEST_F(TelexEngineTest, AfterCommit_CanTypeNewWord) {
    TypeString(*engine_, L"xin");
    (void)engine_->Commit();  // Discard result, testing post-commit behavior
    TypeString(*engine_, L"chaof");
    
    EXPECT_EQ(engine_->Peek(), L"chào");
}

// ============================================================================
// ENGINE INTERFACE TESTS
// ============================================================================

TEST_F(TelexEngineTest, Count_ReturnsCorrectValue) {
    EXPECT_EQ(engine_->Count(), 0u);
    engine_->PushChar(L'a');
    EXPECT_EQ(engine_->Count(), 1u);
    engine_->PushChar(L'b');
    EXPECT_EQ(engine_->Count(), 2u);
}

TEST_F(TelexEngineTest, NoGlobalState_MultipleInstances) {
    TypingEngine engine2(config_);
    
    // Use strings without Vietnamese patterns
    TypeString(*engine_, L"abc");
    TypeString(engine2, L"xyz");
    
    EXPECT_EQ(engine_->Peek(), L"abc");
    EXPECT_EQ(engine2.Peek(), L"xyz");
}

// ============================================================================
// ESCAPE TESTS (typing same key twice clears transformation + adds key)
// ============================================================================

TEST_F(TelexEngineTest, Escape_ToneAcute) {
    // "tess" → te(acute on e from s) + s(escape, clears tone, adds s) = "tes"
    TypeString(*engine_, L"tess");
    EXPECT_EQ(engine_->Peek(), L"tes");
}

TEST_F(TelexEngineTest, Escape_ToneAcute_FullWord) {
    // "tesst" → "test" (escape 's' clears tone and adds 's', then 't')
    TypeString(*engine_, L"tesst");
    EXPECT_EQ(engine_->Peek(), L"test");
}

TEST(TelexEscapeSpellOnTest, ToneHoi_UppercaseR_SpellCheckOn) {
    // #209 comment repro attempt with app-default config (spell check ON)
    TypingConfig cfg;
    cfg.inputMethod = InputMethod::Telex;
    cfg.spellCheckEnabled = true;
    TypingEngine engine(cfg);
    Testing::TypeString(engine, L"TeR");
    EXPECT_EQ(engine.Peek(), L"Tẻ");
    Testing::TypeString(engine, L"R");
    EXPECT_EQ(engine.Peek(), L"TeR");
    Testing::TypeString(engine, L"iRi");
    EXPECT_EQ(engine.Peek(), L"TeRiRi");
}

TEST(TelexEscapeSpellOnTest, ToneHoi_MixedCase_rThenShiftR) {
    // #209 (2026-07-26): Te + r → Tẻ, then Shift+R must escape → "TeR".
    TypingConfig cfg;
    cfg.inputMethod = InputMethod::Telex;
    cfg.spellCheckEnabled = true;
    TypingEngine engine(cfg);
    Testing::TypeString(engine, L"Ter");
    EXPECT_EQ(engine.Peek(), L"Tẻ");
    Testing::TypeString(engine, L"R");
    EXPECT_EQ(engine.Peek(), L"TeR");
}

TEST(TelexEscapeSpellOnTest, ToneEscape_RestoresKeyAtConsumedPosition) {
    // #209: the escape key belongs in the slot the consumed tone key occupied, not
    // after everything typed since — "t-e-r-i-r" is "teri", never "teir".
    TypingConfig cfg;
    cfg.inputMethod = InputMethod::Telex;
    cfg.spellCheckEnabled = true;
    TypingEngine engine(cfg);
    Testing::TypeString(engine, L"ter");
    EXPECT_EQ(engine.Peek(), L"tẻ");
    Testing::TypeString(engine, L"i");
    Testing::TypeString(engine, L"r");
    EXPECT_EQ(engine.Peek(), L"teri");
    Testing::TypeString(engine, L"r");  // third press: plain literal
    EXPECT_EQ(engine.Peek(), L"terir");
}

TEST(TelexEscapeSpellOnTest, ToneEscape_ConsumedPositionKeepsShiftedCase) {
    // Same reposition, uppercase escape key: "TeRiR" → "TeRi" (was "TeiR").
    TypingConfig cfg;
    cfg.inputMethod = InputMethod::Telex;
    cfg.spellCheckEnabled = true;
    TypingEngine engine(cfg);
    Testing::TypeString(engine, L"TeRiR");
    EXPECT_EQ(engine.Peek(), L"TeRi");
}

TEST_F(TelexEngineTest, Escape_ToneHoi_UppercaseR) {
    // Issue #209 comment (Shzr0): "TeR" → "Tẻ", second R must escape → "TeR"
    TypeString(*engine_, L"TeRR");
    EXPECT_EQ(engine_->Peek(), L"TeR");
}

TEST_F(TelexEngineTest, Escape_ToneHoi_UppercaseR_FullWord) {
    // "TeRRiRi" → "TeRiRi" (first R pair escapes; latch keeps later R literal)
    TypeString(*engine_, L"TeRRiRi");
    EXPECT_EQ(engine_->Peek(), L"TeRiRi");
}

TEST_F(TelexEngineTest, Escape_Circumflex) {
    // "eee" → ê(from ee) + e(escape) = "ee"
    TypeString(*engine_, L"eee");
    EXPECT_EQ(engine_->Peek(), L"ee");
}

TEST_F(TelexEngineTest, Escape_Circumflex_A) {
    // "aaa" → â(from aa) + a(escape) = "aa"
    TypeString(*engine_, L"aaa");
    EXPECT_EQ(engine_->Peek(), L"aa");
}

TEST_F(TelexEngineTest, Escape_Stroke_DD) {
    // "ddd" → đ(from dd) + d(escape) = "dd"
    TypeString(*engine_, L"ddd");
    EXPECT_EQ(engine_->Peek(), L"dd");
}

TEST_F(TelexEngineTest, Escape_Breve_AW) {
    // "aww" → ă(from aw) + w(escape) = "aw"
    TypeString(*engine_, L"aww");
    EXPECT_EQ(engine_->Peek(), L"aw");
}

TEST_F(TelexEngineTest, Escape_Horn_OW) {
    // "oww" → ơ(from ow) + w(escape) = "ow"
    TypeString(*engine_, L"oww");
    EXPECT_EQ(engine_->Peek(), L"ow");
}

TEST_F(TelexEngineTest, Word_Giua_WithTone)  {
    // giữa = g + i + ữ + a (ữ = u with horn and tilde)
    TypeString(*engine_, L"giuawx");
    EXPECT_EQ(engine_->Peek(), L"giữa");
} 


// ============================================================================
// EDGE CASES
// ============================================================================

TEST_F(TelexEngineTest, EdgeCase_ConsecutiveTones) {
    // Different tone key replaces existing: á (s=acute) → à (f=grave replaces)
    TypeString(*engine_, L"asf");
    EXPECT_EQ(engine_->Peek(), L"à");
    EXPECT_EQ(engine_->Count(), 1u);
}

TEST_F(TelexEngineTest, EdgeCase_PlainConsonants) {
    TypeString(*engine_, L"bcdgh");
    EXPECT_EQ(engine_->Peek(), L"bcdgh");
}

TEST_F(TelexEngineTest, EdgeCase_WWithoutVowel) {
    // Full Telex: standalone 'w' → ư
    TypeString(*engine_, L"w");
    EXPECT_EQ(engine_->Peek(), L"ư");
}

TEST_F(TelexEngineTest, RemoveTone_WithTone) {
    TypeString(*engine_, L"tesst");
    EXPECT_EQ(engine_->Peek(), L"test");
}

TEST_F(TelexEngineTest, RemoveCircumflex_WithCircumflex) {
    TypeString(*engine_, L"eee");
    EXPECT_EQ(engine_->Peek(), L"ee");
}

TEST_F(TelexEngineTest, Word_Cua) {
    TypeString(*engine_, L"cuar");
    EXPECT_EQ(engine_->Peek(), L"của");
}

TEST_F(TelexEngineTest, Word_Hoac) {
    TypeString(*engine_, L"hoacwj");
    EXPECT_EQ(engine_->Peek(), L"hoặc");
}

// GI consonant cluster: 'i' is part of consonant, tone goes on real vowel
TEST_F(TelexEngineTest, Word_Giac_ToneOnA) {
    TypeString(*engine_, L"giacs");  // giác — tone on 'a', not 'i'
    EXPECT_EQ(engine_->Peek(), L"giác");
}

TEST_F(TelexEngineTest, Word_Gian_ToneOnA) {
    TypeString(*engine_, L"gianf");  // giàn — tone on 'a'
    EXPECT_EQ(engine_->Peek(), L"giàn");
}

// Mixed case: uppercase G + lowercase iar — CapsLock mid-word scenario.
// Engine must treat 'gi' as onset cluster regardless of case.
TEST_F(TelexEngineTest, Word_Giar_MixedCase_ToneOnA) {
    TypeString(*engine_, L"Giar");  // Giả — uppercase G, tone on 'a' not 'i'
    EXPECT_EQ(engine_->Peek(), L"Gi\x1EA3");  // Giả
}

TEST_F(TelexEngineTest, Word_Giar_AllUpper_ToneOnA) {
    TypeString(*engine_, L"GIAR");  // GIẢ — all uppercase, tone still on 'A'
    EXPECT_EQ(engine_->Peek(), L"GI\x1EA2");  // GIẢ
}

// Simulate CapsLock mid-word bug: user types G, then CapsLock commits the word
// (hook-layer step 9), then types "iar" as a new word.
// Without the HookEngine CapsLock bypass fix, this produces "G" + "ỉa" = "Gỉa"
// instead of the correct "Giả".
TEST_F(TelexEngineTest, CapsLockBug_GCommit_Iar_ToneOnI) {
    // Step 1: Type 'G' → engine has [g]
    engine_->PushChar(L'G');
    EXPECT_EQ(engine_->Peek(), L"G");

    // Step 2: CapsLock key → hook commits and resets engine (simulated here)
    (void)engine_->Commit();  // Returns "G", engine resets

    // Step 3: User types "iar" as a new word — no 'g' prefix!
    TypeString(*engine_, L"iar");

    // BUG: 'ia' diphthong rule puts tone on 'i' (first vowel) → "ỉa"
    // User sees "G" + "ỉa" = "Gỉa" instead of "Giả"
    EXPECT_EQ(engine_->Peek(), L"\x1EC9" L"a");  // ỉa — this IS the bug output
}

TEST_F(TelexEngineTest, Word_Gieo_ToneOnE) {
    TypeString(*engine_, L"gieos");  // giéo — tone on 'e'
    EXPECT_EQ(engine_->Peek(), L"giéo");
}

// UA diphthong: tone on first vowel (u)
TEST_F(TelexEngineTest, Word_Ua_ToneOnU) {
    TypeString(*engine_, L"uar");  // ủa — tone on 'u', not 'a'
    EXPECT_EQ(engine_->Peek(), L"ủa");
}

TEST_F(TelexEngineTest, Word_Mua_ToneOnU) {
    TypeString(*engine_, L"muaf");  // mùa — tone on 'u'
    EXPECT_EQ(engine_->Peek(), L"mùa");
}

// QU consonant cluster: 'u' is part of consonant, tone goes on real vowel
TEST_F(TelexEngineTest, Word_Quan_ToneOnA) {
    TypeString(*engine_, L"quans");  // quán — tone on 'a', not 'u'
    EXPECT_EQ(engine_->Peek(), L"quán");
}

TEST_F(TelexEngineTest, Word_Que_ToneOnE) {
    TypeString(*engine_, L"queer");  // quê with hỏi → quể
    EXPECT_EQ(engine_->Peek(), L"quể");
}

// ============================================================================
// TRIPHTHONG TESTS (From test-specification.md)
// ============================================================================

TEST_F(TelexEngineTest, Triphthong_IEU_Acute) {
    TypeString(*engine_, L"ieeus");  // iếu
    EXPECT_EQ(engine_->Peek(), L"iếu");
}

TEST_F(TelexEngineTest, Triphthong_IEU_Grave) {
    TypeString(*engine_, L"ieeuf");  // iều
    EXPECT_EQ(engine_->Peek(), L"iều");
}

TEST_F(TelexEngineTest, Triphthong_YEU_Acute) {
    TypeString(*engine_, L"yeeus");  // yếu
    EXPECT_EQ(engine_->Peek(), L"yếu");
}

TEST_F(TelexEngineTest, Triphthong_UOI_Acute) {
    TypeString(*engine_, L"uoois");  // uối (muối)
    EXPECT_EQ(engine_->Peek(), L"uối");
}

TEST_F(TelexEngineTest, Triphthong_UOI_Hook) {
    TypeString(*engine_, L"uooir");  // uổi (tuổi)
    EXPECT_EQ(engine_->Peek(), L"uổi");
}

TEST_F(TelexEngineTest, Triphthong_UOI_Horn) {
    TypeString(*engine_, L"uowis");  // ưới (tưới)
    EXPECT_EQ(engine_->Peek(), L"ưới");
}

TEST_F(TelexEngineTest, Triphthong_UOI_Horn_Grave) {
    TypeString(*engine_, L"uowif");  // ười (mười)
    EXPECT_EQ(engine_->Peek(), L"ười");
}

TEST_F(TelexEngineTest, Triphthong_OAI_Grave) {
    TypeString(*engine_, L"oaif");  // oài (hoài)
    EXPECT_EQ(engine_->Peek(), L"oài");
}

TEST_F(TelexEngineTest, Triphthong_OAY_Acute) {
    TypeString(*engine_, L"oays");  // oáy (xoáy)
    EXPECT_EQ(engine_->Peek(), L"oáy");
}

TEST_F(TelexEngineTest, Triphthong_UYE_Dot) {
    TypeString(*engine_, L"uyeetj");  // uyệt (tuyệt)
    EXPECT_EQ(engine_->Peek(), L"uyệt");
}

// ============================================================================
// RISING DIPHTHONG TESTS (Tone on SECOND vowel)
// ============================================================================

TEST_F(TelexEngineTest, RisingDiphthong_OA_Grave) {
    TypeString(*engine_, L"oaf");  // òa (classic: tone on first)
    EXPECT_EQ(engine_->Peek(), L"òa");
}

TEST_F(TelexEngineTest, RisingDiphthong_OA_Acute) {
    TypeString(*engine_, L"oas");  // óa (classic: tone on first)
    EXPECT_EQ(engine_->Peek(), L"óa");
}

TEST_F(TelexEngineTest, RisingDiphthong_OE_Tilde) {
    TypeString(*engine_, L"oex");  // õe (classic: tone on first)
    EXPECT_EQ(engine_->Peek(), L"õe");
}

// ============================================================================
// CODA-AWARE RISING DIPHTHONG TESTS (oa, oe with coda)
// ============================================================================

TEST_F(TelexEngineTest, CodaAware_OAN_Grave_NormalOrder) {
    TypeString(*engine_, L"hoanf");  // hoàn (coda 'n' -> tone ALWAYS on second vowel 'a')
    EXPECT_EQ(engine_->Peek(), L"hoàn");
}

TEST_F(TelexEngineTest, CodaAware_OAN_Grave_EarlyTone) {
    TypeString(*engine_, L"hofan");  // h-o-f -> hò, a -> hòa, n -> hoàn (tone relocates to 'a')
    EXPECT_EQ(engine_->Peek(), L"hoàn");
}

TEST_F(TelexEngineTest, CodaAware_OAC_Acute_NormalOrder) {
    TypeString(*engine_, L"hoacs");  // hoác
    EXPECT_EQ(engine_->Peek(), L"hoác");
}

TEST_F(TelexEngineTest, CodaAware_OAC_Acute_EarlyTone) {
    TypeString(*engine_, L"hosac");  // h-o-s -> hó, a -> hóa, c -> hoác
    EXPECT_EQ(engine_->Peek(), L"hoác");
}

TEST_F(TelexEngineTest, CodaAware_OET_Dot_NormalOrder) {
    TypeString(*engine_, L"xoetj");  // xoẹt
    EXPECT_EQ(engine_->Peek(), L"xoẹt");
}

TEST_F(TelexEngineTest, CodaAware_OET_Dot_EarlyTone) {
    TypeString(*engine_, L"xojet");  // x-o-j -> xọ, e -> xọe, t -> xoẹt
    EXPECT_EQ(engine_->Peek(), L"xoẹt");
}

TEST_F(TelexEngineTest, CodaAware_NoCoda_StaysOnFirst) {
    TypeString(*engine_, L"hoaf");  // hòa (no coda -> stays on FIRST per classic rule)
    EXPECT_EQ(engine_->Peek(), L"hòa");
}

TEST_F(TelexEngineTest, CodaAware_NoCoda_EarlyTone_StaysOnFirst) {
    TypeString(*engine_, L"hofa");  // h-o-f -> hò, a -> hòa (no relocation needed)
    EXPECT_EQ(engine_->Peek(), L"hòa");
}

// ============================================================================
// MODERN ORTHOGRAPHY TESTS (modernOrtho = true)
// ============================================================================

TEST_F(TelexEngineTest, ModernOrtho_NoCoda_MovesToSecond) {
    config_.modernOrtho = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    
    TypeString(*engine_, L"hoaf");  // hoà (modern rule -> tone on 'a')
    EXPECT_EQ(engine_->Peek(), L"hoà");
}

TEST_F(TelexEngineTest, ModernOrtho_NoCoda_EarlyTone_RelocatesToSecond) {
    config_.modernOrtho = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    
    TypeString(*engine_, L"hofa");  // h-o-f -> hò, a -> hoà (relocates because modern rule puts tone on SECOND)
    EXPECT_EQ(engine_->Peek(), L"hoà");
}

TEST_F(TelexEngineTest, ModernOrtho_WithCoda_StaysOnSecond) {
    config_.modernOrtho = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    
    TypeString(*engine_, L"hoanf");  // hoàn
    EXPECT_EQ(engine_->Peek(), L"hoàn");
}

TEST_F(TelexEngineTest, RisingDiphthong_OE_Grave) {
    TypeString(*engine_, L"oef");  // òe (classic: tone on first)
    EXPECT_EQ(engine_->Peek(), L"òe");
}

TEST_F(TelexEngineTest, RisingDiphthong_UY_Grave) {
    TypeString(*engine_, L"uyf");  // ùy (classic: tone on first)
    EXPECT_EQ(engine_->Peek(), L"ùy");
}

TEST_F(TelexEngineTest, RisingDiphthong_Hoa) {
    TypeString(*engine_, L"hoaf");  // hòa (classic: tone on first)
    EXPECT_EQ(engine_->Peek(), L"hòa");
}

TEST_F(TelexEngineTest, RisingDiphthong_Quy) {
    TypeString(*engine_, L"quyf");  // quỳ
    EXPECT_EQ(engine_->Peek(), L"quỳ");
}

// ============================================================================
// W-MODIFIER PRIORITY TESTS (7 levels from spec)
// ============================================================================

TEST_F(TelexEngineTest, WPriority_1_UA_Horn) {
    // Priority 1: ua → ưa (horn on u, not breve on a)
    TypeString(*engine_, L"muaw");
    EXPECT_EQ(engine_->Peek(), L"mưa");
}

TEST_F(TelexEngineTest, WPriority_1_UA_Horn_2) {
    TypeString(*engine_, L"cuaw");
    EXPECT_EQ(engine_->Peek(), L"cưa");
}

TEST_F(TelexEngineTest, WPriority_1_UA_Horn_3) {
    TypeString(*engine_, L"thuaw");
    EXPECT_EQ(engine_->Peek(), L"thưa");
}

TEST_F(TelexEngineTest, WPriority_2_OA_Breve) {
    // Priority 2: oa → oă (breve on a)
    TypeString(*engine_, L"hoaw");
    EXPECT_EQ(engine_->Peek(), L"hoă");
}

TEST_F(TelexEngineTest, WPriority_2_OA_Breve_2) {
    TypeString(*engine_, L"toaw");
    EXPECT_EQ(engine_->Peek(), L"toă");
}

TEST_F(TelexEngineTest, WPriority_3_SingleU) {
    // Priority 3: standalone u → ư
    TypeString(*engine_, L"tuw");
    EXPECT_EQ(engine_->Peek(), L"tư");
}

TEST_F(TelexEngineTest, WPriority_4_SingleO) {
    // Priority 4: standalone o → ơ (not in oa pattern)
    TypeString(*engine_, L"tow");
    EXPECT_EQ(engine_->Peek(), L"tơ");
}

TEST_F(TelexEngineTest, WPriority_5_SingleA) {
    // Priority 5: standalone a → ă
    TypeString(*engine_, L"taw");
    EXPECT_EQ(engine_->Peek(), L"tă");
}

TEST_F(TelexEngineTest, WPriority_6_Escape_U) {
    // Priority 6: escape - clear existing horn
    TypeString(*engine_, L"uww");
    EXPECT_EQ(engine_->Peek(), L"uw");
}

TEST_F(TelexEngineTest, WPriority_6_Escape_O) {
    TypeString(*engine_, L"oww");
    EXPECT_EQ(engine_->Peek(), L"ow");
}

TEST_F(TelexEngineTest, WPriority_6_Escape_A) {
    TypeString(*engine_, L"aww");
    EXPECT_EQ(engine_->Peek(), L"aw");
}

TEST_F(TelexEngineTest, WW_Standalone_Escape) {
    // ww → w (P8 synthetic ư removed entirely)
    TypeString(*engine_, L"ww");
    EXPECT_EQ(engine_->Peek(), L"w");
}

TEST_F(TelexEngineTest, WW_Standalone_Escape_UppercasePreserved) {
    // W + w → Ư (P8) then escape. Result should be 'W' (preserve first keystroke's
    // uppercase intent), not 'w'. Without the fix, second keystroke's lowercase
    // wins and user loses the capitalization.
    TypeString(*engine_, L"Ww");
    EXPECT_EQ(engine_->Peek(), L"W");
}

TEST_F(TelexEngineTest, WW_Standalone_Escape_LowerThenUpper_StillLower) {
    // Symmetric case: lowercase w first, uppercase W second. First keystroke wins.
    TypeString(*engine_, L"wW");
    EXPECT_EQ(engine_->Peek(), L"w");
}

TEST_F(TelexEngineTest, BWW_Standalone_Escape) {
    // bww → bw (P8 synthetic ư after consonant removed)
    TypeString(*engine_, L"bww");
    EXPECT_EQ(engine_->Peek(), L"bw");
}

TEST_F(TelexEngineTest, UWW_RealU_Escape) {
    // uww → uw (real 'u' keeps base, only clears horn)
    TypeString(*engine_, L"uww");
    EXPECT_EQ(engine_->Peek(), L"uw");
}

TEST_F(TelexEngineTest, Window_SyntheticNotErased) {
    // "window" → synthetic ư from first 'w' should NOT be erased by second 'w'
    // (only immediate ww escapes the synthetic ư)
    TypeString(*engine_, L"window");
    // First 'w' → ư, then 'i','n','d','o' → ưindo
    // Second 'w' → regular P4 escape (not synthetic erase) → uindow + w
    EXPECT_EQ(engine_->Peek(), L"uindow");
}

TEST_F(TelexEngineTest, P8_LiteralWAfterVowel_Rework) {
    // P8 must NOT fire when vowels already exist in buffer.
    // "rework": r-e-w-o-r-k → 'w' after 'e' is literal, not ư.
    TypeString(*engine_, L"rework");
    EXPECT_EQ(engine_->Peek(), L"rework");
}

TEST_F(TelexEngineTest, P8_LiteralWAfterVowel_rew) {
    // 'w' after vowel 'e' is literal (P8 blocked)
    TypeString(*engine_, L"rew");
    EXPECT_EQ(engine_->Peek(), L"rew");
}

TEST_F(TelexEngineTest, P8_StillFiresAfterConsonantOnly) {
    // P8 still fires when only consonants precede 'w'
    TypeString(*engine_, L"trwa");  // tr + ư + a = trưa
    EXPECT_EQ(engine_->Peek(), L"tr\x01B0" L"a");  // trưa
}

TEST_F(TelexEngineTest, P8_QW_EscapeAfterQ) {
    // q + w → qư (P8 synthetic), then second w → escape → qw
    // The synthetic ư after 'q' must NOT be treated as QU cluster
    TypeString(*engine_, L"qww");
    EXPECT_EQ(engine_->Peek(), L"qw");
}

TEST_F(TelexEngineTest, Tone_IA_Diphthong_Classic) {
    // ia diphthong: tone on first vowel (classic)
    TypeString(*engine_, L"nghiax");
    EXPECT_EQ(engine_->Peek(), L"nghĩa");
}

TEST_F(TelexEngineTest, Tone_IA_Diphthong_Mia) {
    TypeString(*engine_, L"mias");
    EXPECT_EQ(engine_->Peek(), L"mía");
}

TEST_F(TelexEngineTest, Tone_IA_Diphthong_Kia) {
    TypeString(*engine_, L"kiaf");
    EXPECT_EQ(engine_->Peek(), L"kìa");
}

// ============================================================================
// TONE PLACEMENT PRIORITY TESTS
// ============================================================================

TEST_F(TelexEngineTest, TonePriority_1_Horn_Last) {
    // Priority 1: Last horn vowel (ơ, ư)
    // uow → ươ (both horned), f → grave on ơ → ườ
    TypeString(*engine_, L"uowf");  // tone on ơ (last horn)
    EXPECT_EQ(engine_->Peek(), L"ườ");
}

TEST_F(TelexEngineTest, TonePriority_1_UO_Cluster) {
    TypeString(*engine_, L"dduowcj");  // được
    EXPECT_EQ(engine_->Peek(), L"được");
}

TEST_F(TelexEngineTest, TonePriority_1_Nguoi) {
    TypeString(*engine_, L"nguowif");  // người
    EXPECT_EQ(engine_->Peek(), L"người");
}

TEST_F(TelexEngineTest, TonePriority_1_Muoi) {
    TypeString(*engine_, L"muowif");  // mười
    EXPECT_EQ(engine_->Peek(), L"mười");
}

TEST_F(TelexEngineTest, TonePriority_2_Circumflex) {
    // Priority 2: Modified vowel (â, ê, ô, ă)
    TypeString(*engine_, L"caaps");  // cấp
    EXPECT_EQ(engine_->Peek(), L"cấp");
}

TEST_F(TelexEngineTest, TonePriority_2_Breve) {
    TypeString(*engine_, L"awcs");  // ắc
    EXPECT_EQ(engine_->Peek(), L"ắc");
}

TEST_F(TelexEngineTest, TonePriority_3_Falling_AO) {
    // Priority 3: Diphthong rules
    TypeString(*engine_, L"caos");  // cáo
    EXPECT_EQ(engine_->Peek(), L"cáo");
}

TEST_F(TelexEngineTest, TonePriority_3_Rising_OA) {
    TypeString(*engine_, L"hoas");  // hóa (classic: tone on first)
    EXPECT_EQ(engine_->Peek(), L"hóa");
}

TEST_F(TelexEngineTest, TonePriority_4_Default) {
    // Priority 4: Default (rightmost)
    TypeString(*engine_, L"mas");  // má
    EXPECT_EQ(engine_->Peek(), L"má");
}

// ============================================================================
// TONE RELOCATION TESTS
// ============================================================================

TEST_F(TelexEngineTest, ToneReloc_Cua_W) {
    // của + w → cửa (tone relocates from u to ư)
    TypeString(*engine_, L"cuarw");
    EXPECT_EQ(engine_->Peek(), L"cửa");
}

TEST_F(TelexEngineTest, ToneReloc_Lua) {
    // lửa
    TypeString(*engine_, L"luwar");
    EXPECT_EQ(engine_->Peek(), L"lửa");
}

// ============================================================================
// ADDITIONAL ESCAPE TESTS
// ============================================================================

TEST_F(TelexEngineTest, Escape_UWAW) {
    // ư(uw) → ưa(uwa) → uaw (w escapes horn)
    TypeString(*engine_, L"uwaw");
    EXPECT_EQ(engine_->Peek(), L"uaw");
}

// ============================================================================
// REAL WORD TESTS (From spec)
// ============================================================================

TEST_F(TelexEngineTest, RealWord_Nuoc) {
    TypeString(*engine_, L"nuowcs");  // nước
    EXPECT_EQ(engine_->Peek(), L"nước");
}

TEST_F(TelexEngineTest, RealWord_Yeu) {
    TypeString(*engine_, L"yeeu");  // yêu
    EXPECT_EQ(engine_->Peek(), L"yêu");
}

TEST_F(TelexEngineTest, RealWord_Yeu_Tone) {
    TypeString(*engine_, L"yeeus");  // yếu
    EXPECT_EQ(engine_->Peek(), L"yếu");
}

TEST_F(TelexEngineTest, RealWord_Tieu) {
    TypeString(*engine_, L"tieeur");  // tiểu
    EXPECT_EQ(engine_->Peek(), L"tiểu");
}

TEST_F(TelexEngineTest, RealWord_Tuyet) {
    TypeString(*engine_, L"tuyeetj");  // tuyệt
    EXPECT_EQ(engine_->Peek(), L"tuyệt");
}

TEST_F(TelexEngineTest, RealWord_Nguyen) {
    TypeString(*engine_, L"nguyeen");  // nguyên
    EXPECT_EQ(engine_->Peek(), L"nguyên");
}

TEST_F(TelexEngineTest, RealWord_Khuya) {
    TypeString(*engine_, L"khuya");  // khuya
    EXPECT_EQ(engine_->Peek(), L"khuya");
}

// When the user typos an extra vowel after placing a tone on a diphthong,
// the tone must stay on the original diphthong (first two vowels of the cluster),
// not slide forward onto the typo vowel. Examples: gạo + i → gạoi (not gaọi),
// thúi + a → thúia, của + i → củai, chảo + i → chảoi, vào + a → vàoa.
TEST_F(TelexEngineTest, ExtraVowel_ToneStaysOnFirstDiphthong_GaoPlusI) {
    TypeString(*engine_, L"gaoji");  // gạo + i
    EXPECT_EQ(engine_->Peek(), L"gạoi");
}

TEST_F(TelexEngineTest, ExtraVowel_ToneStaysOnFirstDiphthong_ThuiPlusA) {
    TypeString(*engine_, L"thuisa");  // thúi + a
    EXPECT_EQ(engine_->Peek(), L"thúia");
}

TEST_F(TelexEngineTest, ExtraVowel_ToneStaysOnFirstDiphthong_CuaPlusI) {
    TypeString(*engine_, L"cuari");  // của + i
    EXPECT_EQ(engine_->Peek(), L"củai");
}

TEST_F(TelexEngineTest, ExtraVowel_ToneStaysOnFirstDiphthong_ChaoPlusI) {
    TypeString(*engine_, L"chaori");  // chảo + i
    EXPECT_EQ(engine_->Peek(), L"chảoi");
}

// Typo followed by backspace must end up at the original word.
TEST_F(TelexEngineTest, ExtraVowel_BackspaceRestoresOriginal) {
    TypeString(*engine_, L"gaoji");  // gạoi
    engine_->Backspace();            // remove 'i'
    EXPECT_EQ(engine_->Peek(), L"gạo");
}

// Vietnamese vowels within a syllable are always contiguous. Once a consonant
// closes the syllable, a following vowel starts a NEW syllable and the tone
// must not migrate across the consonant boundary.
TEST_F(TelexEngineTest, NoToneRelocationAcrossConsonant_TrinhPlusE) {
    TypeString(*engine_, L"trinhfe");  // trình + literal 'e'
    EXPECT_EQ(engine_->Peek(), L"trìnhe");
}

TEST_F(TelexEngineTest, NoToneRelocationAcrossConsonant_KinPlusA) {
    TypeString(*engine_, L"kifna");  // kìn + literal 'a'
    EXPECT_EQ(engine_->Peek(), L"kìna");
}

TEST_F(TelexEngineTest, NoToneRelocationAcrossConsonant_HoanPlusE) {
    TypeString(*engine_, L"hofane");  // hòan + literal 'e' (tone stays on a, not e)
    EXPECT_EQ(engine_->Peek(), L"hoàne");
}

TEST_F(TelexEngineTest, RealWord_Hoai) {
    TypeString(*engine_, L"hoaif");  // hoài
    EXPECT_EQ(engine_->Peek(), L"hoài");
}

TEST_F(TelexEngineTest, RealWord_Xoay) {
    TypeString(*engine_, L"xoays");  // xoáy
    EXPECT_EQ(engine_->Peek(), L"xoáy");
}

TEST_F(TelexEngineTest, RealWord_Tuoi) {
    TypeString(*engine_, L"tuooir");  // tuổi
    EXPECT_EQ(engine_->Peek(), L"tuổi");
}

TEST_F(TelexEngineTest, RealWord_Muoi) {
    TypeString(*engine_, L"muoois");  // muối
    EXPECT_EQ(engine_->Peek(), L"muối");
}

TEST_F(TelexEngineTest, RealWord_Toan) {
    TypeString(*engine_, L"toawnf");  // toằn (oă + grave)
    EXPECT_EQ(engine_->Peek(), L"toằn");
}

TEST_F(TelexEngineTest, RealWord_Hoan) {
    TypeString(*engine_, L"hoawnf");  // hoằn
    EXPECT_EQ(engine_->Peek(), L"hoằn");
}

// Issue #213: rare rhymes oăm/oăp/uyp dropped tone (oă/uy missing m/p codas).
TEST_F(TelexEngineTest, RealWord_Khoam) {
    TypeString(*engine_, L"khoawmf");  // khoằm (oă + m + grave)
    EXPECT_EQ(engine_->Peek(), L"khoằm");
}

TEST_F(TelexEngineTest, RealWord_Ngoap) {
    TypeString(*engine_, L"ngoawpj");  // ngoặp (oă + p + dot)
    EXPECT_EQ(engine_->Peek(), L"ngoặp");
}

TEST_F(TelexEngineTest, RealWord_Tuyp) {
    TypeString(*engine_, L"tuyps");  // tuýp (uy + p + acute, loanword)
    EXPECT_EQ(engine_->Peek(), L"tuýp");
}

TEST_F(TelexEngineTest, RealWord_An) {
    TypeString(*engine_, L"awn");  // ăn
    EXPECT_EQ(engine_->Peek(), L"ăn");
}

TEST_F(TelexEngineTest, RealWord_Lang) {
    TypeString(*engine_, L"lawngj");  // lặng
    EXPECT_EQ(engine_->Peek(), L"lặng");
}

TEST_F(TelexEngineTest, RealWord_Bat) {
    TypeString(*engine_, L"bawts");  // bắt
    EXPECT_EQ(engine_->Peek(), L"bắt");
}

TEST_F(TelexEngineTest, RealWord_Dac) {
    TypeString(*engine_, L"ddawcj");  // đặc
    EXPECT_EQ(engine_->Peek(), L"đặc");
}

TEST_F(TelexEngineTest, RealWord_Thang) {
    TypeString(*engine_, L"thawngs");  // thắng
    EXPECT_EQ(engine_->Peek(), L"thắng");
}

TEST_F(TelexEngineTest, RealWord_Mua) {
    TypeString(*engine_, L"muaw");  // mưa
    EXPECT_EQ(engine_->Peek(), L"mưa");
}

TEST_F(TelexEngineTest, RealWord_Thua) {
    TypeString(*engine_, L"thuaw");  // thưa
    EXPECT_EQ(engine_->Peek(), L"thưa");
}

TEST_F(TelexEngineTest, RealWord_Giua_Full) {
    TypeString(*engine_, L"giuwax");  // giữa
    EXPECT_EQ(engine_->Peek(), L"giữa");
}

TEST_F(TelexEngineTest, RealWord_Lua) {
    TypeString(*engine_, L"luwar");  // lửa
    EXPECT_EQ(engine_->Peek(), L"lửa");
}

TEST_F(TelexEngineTest, RealWord_Su) {
    TypeString(*engine_, L"suwr");  // sử
    EXPECT_EQ(engine_->Peek(), L"sử");
}

TEST_F(TelexEngineTest, RealWord_Pho) {
    TypeString(*engine_, L"phowr");  // phở
    EXPECT_EQ(engine_->Peek(), L"phở");
}

TEST_F(TelexEngineTest, RealWord_Tho) {
    TypeString(*engine_, L"thow");  // thơ
    EXPECT_EQ(engine_->Peek(), L"thơ");
}

TEST_F(TelexEngineTest, RealWord_Son) {
    TypeString(*engine_, L"sown");  // sơn
    EXPECT_EQ(engine_->Peek(), L"sơn");
}

TEST_F(TelexEngineTest, RealWord_Hoi) {
    TypeString(*engine_, L"howi");  // hơi
    EXPECT_EQ(engine_->Peek(), L"hơi");
}

// ============================================================================
// FALLING DIPHTHONG TESTS
// ============================================================================

TEST_F(TelexEngineTest, FallingDiphthong_AI_Acute) {
    TypeString(*engine_, L"ais");
    EXPECT_EQ(engine_->Peek(), L"ái");
}

TEST_F(TelexEngineTest, FallingDiphthong_AO_Grave) {
    TypeString(*engine_, L"aof");
    EXPECT_EQ(engine_->Peek(), L"ào");
}

TEST_F(TelexEngineTest, FallingDiphthong_AU_Hook) {
    TypeString(*engine_, L"aur");
    EXPECT_EQ(engine_->Peek(), L"ảu");
}

TEST_F(TelexEngineTest, FallingDiphthong_AY_Grave) {
    TypeString(*engine_, L"ayf");
    EXPECT_EQ(engine_->Peek(), L"ày");
}

TEST_F(TelexEngineTest, FallingDiphthong_AU_Circum) {
    TypeString(*engine_, L"aaus");  // ấu
    EXPECT_EQ(engine_->Peek(), L"ấu");
}

TEST_F(TelexEngineTest, FallingDiphthong_AY_Circum) {
    TypeString(*engine_, L"aays");  // ấy
    EXPECT_EQ(engine_->Peek(), L"ấy");
}

TEST_F(TelexEngineTest, FallingDiphthong_EO_Hook) {
    TypeString(*engine_, L"eor");
    EXPECT_EQ(engine_->Peek(), L"ẻo");
}

TEST_F(TelexEngineTest, FallingDiphthong_EU_Circum) {
    TypeString(*engine_, L"eeus");  // ếu
    EXPECT_EQ(engine_->Peek(), L"ếu");
}

TEST_F(TelexEngineTest, FallingDiphthong_IU_Dot) {
    TypeString(*engine_, L"iuj");  // ịu (dịu)
    EXPECT_EQ(engine_->Peek(), L"ịu");
}

TEST_F(TelexEngineTest, FallingDiphthong_OI_Circum) {
    TypeString(*engine_, L"oois");  // ối (tối)
    EXPECT_EQ(engine_->Peek(), L"ối");
}

TEST_F(TelexEngineTest, FallingDiphthong_OI_Horn) {
    TypeString(*engine_, L"owis");  // ới (trời)
    EXPECT_EQ(engine_->Peek(), L"ới");
}

TEST_F(TelexEngineTest, FallingDiphthong_UI_Horn) {
    TypeString(*engine_, L"uwix");  // ữi (gửi)
    EXPECT_EQ(engine_->Peek(), L"ữi");
}

// ============================================================================
// EDGE CASE TESTS
// ============================================================================

TEST_F(TelexEngineTest, EdgeCase_ToneReplace) {
    // Different tone replaces: á → à
    TypeString(*engine_, L"asf");
    EXPECT_EQ(engine_->Peek(), L"à");
}

TEST_F(TelexEngineTest, EdgeCase_ToneReplace_2) {
    // á → ả
    TypeString(*engine_, L"asr");
    EXPECT_EQ(engine_->Peek(), L"ả");
}

// T5 (docs/TODO.md): tone replacement on already-toned syllable WITH coda.
// Baseline: forward typing 'casc' should produce 'các' — no replacement involved.
TEST_F(TelexEngineTest, T5_ToneAfterCoda_BaselineForward) {
    TypeString(*engine_, L"casc");
    EXPECT_EQ(engine_->Peek(), L"các");
}

// T5 (docs/TODO.md): user mistypes 'f' (huyền) on 'ca', adds coda 'c', then
// corrects with 's' (sắc). 's' must REPLACE huyền with sắc on the toned vowel.
// Spell-check OFF here — isolates whether the engine alone handles this.
//   PASS → engine OK; bug lives in PhonotacticsValidator path (CombinedEngineSpellTest).
//   FAIL → engine tone-replacement logic broken irrespective of validator.
TEST_F(TelexEngineTest, T5_ToneReplaceAfterCoda_NoSpellCheck) {
    TypeString(*engine_, L"cafcs");
    EXPECT_EQ(engine_->Peek(), L"các")
        << "After 'cafcs', engine should replace huyền with sắc on 'à' → 'các'.\n"
        << "PASS means engine alone is fine; bug is in validator (CombinedEngineSpellTest).\n"
        << "FAIL means TypingEngine tone-replacement logic itself is broken.";
}

TEST_F(TelexEngineTest, EdgeCase_GI_Plus_U) {
    TypeString(*engine_, L"giuw");
    EXPECT_EQ(engine_->Peek(), L"giư");
}

TEST_F(TelexEngineTest, GIW_SynthesizesHornU) {
    // g-i-w → giư (gi is consonant cluster, w synthesizes ư as nucleus)
    TypeString(*engine_, L"giw");
    EXPECT_EQ(engine_->Peek(), L"giư");
}

TEST_F(TelexEngineTest, GIW_ToneOnSynthesizedU) {
    // g-i-w-x → giữ
    TypeString(*engine_, L"giwx");
    EXPECT_EQ(engine_->Peek(), L"giữ");
}

TEST_F(TelexEngineTest, GIW_EscapeWithDoubleW) {
    // g-i-w-w → giw (second w escapes)
    TypeString(*engine_, L"giww");
    EXPECT_EQ(engine_->Peek(), L"giw");
}

TEST_F(TelexEngineTest, GIW_FullWord_Giua) {
    // g-i-w-a → giưa (gi cluster + ưa nucleus)
    TypeString(*engine_, L"giwa");
    EXPECT_EQ(engine_->Peek(), L"giưa");
}

TEST_F(TelexEngineTest, GIW_FullWord_Giuax) {
    // g-i-w-a-x → giữa
    TypeString(*engine_, L"giwax");
    EXPECT_EQ(engine_->Peek(), L"giữa");
}

TEST_F(TelexEngineTest, NonGI_Cluster_IW_Literal) {
    // k-i-w should NOT synthesize ư (ki is not a consonant cluster)
    TypeString(*engine_, L"kiw");
    EXPECT_EQ(engine_->Peek(), L"kiw");
}

TEST_F(TelexEngineTest, EdgeCase_QU_Plus_E) {
    TypeString(*engine_, L"quew");
    EXPECT_EQ(engine_->Peek(), L"quew");
}

TEST_F(TelexEngineTest, EdgeCase_AllVowels) {
    TypeString(*engine_, L"aeiou");
    EXPECT_EQ(engine_->Peek(), L"aeiou");
}

// ============================================================================
// COMPLETE TONE TESTS - All vowels × All tones
// ============================================================================

// Acute (s) - remaining vowels
TEST_F(TelexEngineTest, Tone_Acute_Circumflex_A) {
    TypeString(*engine_, L"aas");
    EXPECT_EQ(engine_->Peek(), L"ấ");
}

TEST_F(TelexEngineTest, Tone_Acute_Breve_A) {
    TypeString(*engine_, L"aws");
    EXPECT_EQ(engine_->Peek(), L"ắ");
}

TEST_F(TelexEngineTest, Tone_Acute_Circumflex_E) {
    TypeString(*engine_, L"ees");
    EXPECT_EQ(engine_->Peek(), L"ế");
}

TEST_F(TelexEngineTest, Tone_Acute_Circumflex_O) {
    TypeString(*engine_, L"oos");
    EXPECT_EQ(engine_->Peek(), L"ố");
}

TEST_F(TelexEngineTest, Tone_Acute_Horn_O) {
    TypeString(*engine_, L"ows");
    EXPECT_EQ(engine_->Peek(), L"ớ");
}

TEST_F(TelexEngineTest, Tone_Acute_Horn_U) {
    TypeString(*engine_, L"uws");
    EXPECT_EQ(engine_->Peek(), L"ứ");
}

// Grave (f) - all vowels
TEST_F(TelexEngineTest, Tone_Grave_Circumflex_A) {
    TypeString(*engine_, L"aaf");
    EXPECT_EQ(engine_->Peek(), L"ầ");
}

TEST_F(TelexEngineTest, Tone_Grave_Breve_A) {
    TypeString(*engine_, L"awf");
    EXPECT_EQ(engine_->Peek(), L"ằ");
}

TEST_F(TelexEngineTest, Tone_Grave_Circumflex_E) {
    TypeString(*engine_, L"eef");
    EXPECT_EQ(engine_->Peek(), L"ề");
}

TEST_F(TelexEngineTest, Tone_Grave_I) {
    TypeString(*engine_, L"if");
    EXPECT_EQ(engine_->Peek(), L"ì");
}

TEST_F(TelexEngineTest, Tone_Grave_Circumflex_O) {
    TypeString(*engine_, L"oof");
    EXPECT_EQ(engine_->Peek(), L"ồ");
}

TEST_F(TelexEngineTest, Tone_Grave_Horn_O) {
    TypeString(*engine_, L"owf");
    EXPECT_EQ(engine_->Peek(), L"ờ");
}

TEST_F(TelexEngineTest, Tone_Grave_U) {
    TypeString(*engine_, L"uf");
    EXPECT_EQ(engine_->Peek(), L"ù");
}

TEST_F(TelexEngineTest, Tone_Grave_Horn_U) {
    TypeString(*engine_, L"uwf");
    EXPECT_EQ(engine_->Peek(), L"ừ");
}

TEST_F(TelexEngineTest, Tone_Grave_Y) {
    TypeString(*engine_, L"yf");
    EXPECT_EQ(engine_->Peek(), L"ỳ");
}

// Hook (r) - all vowels
TEST_F(TelexEngineTest, Tone_Hook_Circumflex_A) {
    TypeString(*engine_, L"aar");
    EXPECT_EQ(engine_->Peek(), L"ẩ");
}

TEST_F(TelexEngineTest, Tone_Hook_Breve_A) {
    TypeString(*engine_, L"awr");
    EXPECT_EQ(engine_->Peek(), L"ẳ");
}

TEST_F(TelexEngineTest, Tone_Hook_E) {
    TypeString(*engine_, L"er");
    EXPECT_EQ(engine_->Peek(), L"ẻ");
}

TEST_F(TelexEngineTest, Tone_Hook_Circumflex_E) {
    TypeString(*engine_, L"eer");
    EXPECT_EQ(engine_->Peek(), L"ể");
}

TEST_F(TelexEngineTest, Tone_Hook_I) {
    TypeString(*engine_, L"ir");
    EXPECT_EQ(engine_->Peek(), L"ỉ");
}

TEST_F(TelexEngineTest, Tone_Hook_Circumflex_O) {
    TypeString(*engine_, L"oor");
    EXPECT_EQ(engine_->Peek(), L"ổ");
}

TEST_F(TelexEngineTest, Tone_Hook_Horn_O) {
    TypeString(*engine_, L"owr");
    EXPECT_EQ(engine_->Peek(), L"ở");
}

TEST_F(TelexEngineTest, Tone_Hook_U) {
    TypeString(*engine_, L"ur");
    EXPECT_EQ(engine_->Peek(), L"ủ");
}

TEST_F(TelexEngineTest, Tone_Hook_Horn_U) {
    TypeString(*engine_, L"uwr");
    EXPECT_EQ(engine_->Peek(), L"ử");
}

TEST_F(TelexEngineTest, Tone_Hook_Y) {
    TypeString(*engine_, L"yr");
    EXPECT_EQ(engine_->Peek(), L"ỷ");
}

// Tilde (x) - all vowels
TEST_F(TelexEngineTest, Tone_Tilde_Circumflex_A) {
    TypeString(*engine_, L"aax");
    EXPECT_EQ(engine_->Peek(), L"ẫ");
}

TEST_F(TelexEngineTest, Tone_Tilde_Breve_A) {
    TypeString(*engine_, L"awx");
    EXPECT_EQ(engine_->Peek(), L"ẵ");
}

TEST_F(TelexEngineTest, Tone_Tilde_E) {
    TypeString(*engine_, L"ex");
    EXPECT_EQ(engine_->Peek(), L"ẽ");
}

TEST_F(TelexEngineTest, Tone_Tilde_Circumflex_E) {
    TypeString(*engine_, L"eex");
    EXPECT_EQ(engine_->Peek(), L"ễ");
}

TEST_F(TelexEngineTest, Tone_Tilde_I) {
    TypeString(*engine_, L"ix");
    EXPECT_EQ(engine_->Peek(), L"ĩ");
}

TEST_F(TelexEngineTest, Tone_Tilde_O) {
    TypeString(*engine_, L"ox");
    EXPECT_EQ(engine_->Peek(), L"õ");
}

TEST_F(TelexEngineTest, Tone_Tilde_Circumflex_O) {
    TypeString(*engine_, L"oox");
    EXPECT_EQ(engine_->Peek(), L"ỗ");
}

TEST_F(TelexEngineTest, Tone_Tilde_Horn_O) {
    TypeString(*engine_, L"owx");
    EXPECT_EQ(engine_->Peek(), L"ỡ");
}

TEST_F(TelexEngineTest, Tone_Tilde_Horn_U) {
    TypeString(*engine_, L"uwx");
    EXPECT_EQ(engine_->Peek(), L"ữ");
}

TEST_F(TelexEngineTest, Tone_Tilde_Y) {
    TypeString(*engine_, L"yx");
    EXPECT_EQ(engine_->Peek(), L"ỹ");
}

// Dot below (j) - all vowels
TEST_F(TelexEngineTest, Tone_Dot_Circumflex_A) {
    TypeString(*engine_, L"aaj");
    EXPECT_EQ(engine_->Peek(), L"ậ");
}

TEST_F(TelexEngineTest, Tone_Dot_Breve_A) {
    TypeString(*engine_, L"awj");
    EXPECT_EQ(engine_->Peek(), L"ặ");
}

TEST_F(TelexEngineTest, Tone_Dot_Circumflex_E) {
    TypeString(*engine_, L"eej");
    EXPECT_EQ(engine_->Peek(), L"ệ");
}

TEST_F(TelexEngineTest, Tone_Dot_I) {
    TypeString(*engine_, L"ij");
    EXPECT_EQ(engine_->Peek(), L"ị");
}

TEST_F(TelexEngineTest, Tone_Dot_Circumflex_O) {
    TypeString(*engine_, L"ooj");
    EXPECT_EQ(engine_->Peek(), L"ộ");
}

TEST_F(TelexEngineTest, Tone_Dot_Horn_O) {
    TypeString(*engine_, L"owj");
    EXPECT_EQ(engine_->Peek(), L"ợ");
}

TEST_F(TelexEngineTest, Tone_Dot_U) {
    TypeString(*engine_, L"uj");
    EXPECT_EQ(engine_->Peek(), L"ụ");
}

TEST_F(TelexEngineTest, Tone_Dot_Horn_U) {
    TypeString(*engine_, L"uwj");
    EXPECT_EQ(engine_->Peek(), L"ự");
}

TEST_F(TelexEngineTest, Tone_Dot_Y) {
    TypeString(*engine_, L"yj");
    EXPECT_EQ(engine_->Peek(), L"ỵ");
}

// ============================================================================
// ADDITIONAL ESCAPE TESTS
// ============================================================================

TEST_F(TelexEngineTest, Escape_Grave) {
    TypeString(*engine_, L"aff");
    EXPECT_EQ(engine_->Peek(), L"af");
}

TEST_F(TelexEngineTest, Escape_Hook) {
    TypeString(*engine_, L"arr");
    EXPECT_EQ(engine_->Peek(), L"ar");
}

TEST_F(TelexEngineTest, Escape_Tilde) {
    TypeString(*engine_, L"axx");
    EXPECT_EQ(engine_->Peek(), L"ax");
}

TEST_F(TelexEngineTest, Escape_Dot) {
    TypeString(*engine_, L"ajj");
    EXPECT_EQ(engine_->Peek(), L"aj");
}

TEST_F(TelexEngineTest, Escape_Circumflex_O) {
    TypeString(*engine_, L"ooo");
    EXPECT_EQ(engine_->Peek(), L"oo");
}

TEST_F(TelexEngineTest, Escape_Stroke_Quad) {
    // dddd: dd→đ, d→escape(đ→d+d), d→literal (user rejected đ)
    TypeString(*engine_, L"dddd");
    EXPECT_EQ(engine_->Peek(), L"ddd");
}

// ============================================================================
// MIXED CASE TESTS
// ============================================================================

TEST_F(TelexEngineTest, MixedCase_Circumflex_Aa) {
    TypeString(*engine_, L"Aa");
    EXPECT_EQ(engine_->Peek(), L"Â");
}

TEST_F(TelexEngineTest, MixedCase_Circumflex_aA) {
    TypeString(*engine_, L"aA");
    EXPECT_EQ(engine_->Peek(), L"â");
}

TEST_F(TelexEngineTest, MixedCase_Breve_Aw) {
    TypeString(*engine_, L"Aw");
    EXPECT_EQ(engine_->Peek(), L"Ă");
}

TEST_F(TelexEngineTest, MixedCase_Horn_Ow) {
    TypeString(*engine_, L"Ow");
    EXPECT_EQ(engine_->Peek(), L"Ơ");
}

TEST_F(TelexEngineTest, MixedCase_Horn_Uw) {
    TypeString(*engine_, L"Uw");
    EXPECT_EQ(engine_->Peek(), L"Ư");
}

TEST_F(TelexEngineTest, MixedCase_Stroke_Dd) {
    TypeString(*engine_, L"Dd");
    EXPECT_EQ(engine_->Peek(), L"Đ");
}

// ============================================================================
// ADDITIONAL FALLING DIPHTHONG TESTS
// ============================================================================

TEST_F(TelexEngineTest, FallingDiphthong_AI_Hook) {
    TypeString(*engine_, L"air");
    EXPECT_EQ(engine_->Peek(), L"ải");
}

TEST_F(TelexEngineTest, FallingDiphthong_AO_Acute) {
    TypeString(*engine_, L"aos");
    EXPECT_EQ(engine_->Peek(), L"áo");
}

TEST_F(TelexEngineTest, FallingDiphthong_AU_Acute) {
    TypeString(*engine_, L"aus");
    EXPECT_EQ(engine_->Peek(), L"áu");
}

TEST_F(TelexEngineTest, FallingDiphthong_AY_Acute) {
    TypeString(*engine_, L"ays");
    EXPECT_EQ(engine_->Peek(), L"áy");
}

TEST_F(TelexEngineTest, FallingDiphthong_EO_Acute) {
    TypeString(*engine_, L"eos");
    EXPECT_EQ(engine_->Peek(), L"éo");
}

TEST_F(TelexEngineTest, FallingDiphthong_OI_Dot) {
    TypeString(*engine_, L"oij");
    EXPECT_EQ(engine_->Peek(), L"ọi");
}

TEST_F(TelexEngineTest, FallingDiphthong_UI_Acute) {
    TypeString(*engine_, L"uis");
    EXPECT_EQ(engine_->Peek(), L"úi");
}

TEST_F(TelexEngineTest, FallingDiphthong_UI_Dot) {
    TypeString(*engine_, L"uij");
    EXPECT_EQ(engine_->Peek(), L"ụi");
}

// ============================================================================
// ADDITIONAL RISING DIPHTHONG TESTS
// ============================================================================

TEST_F(TelexEngineTest, RisingDiphthong_OA_Hook) {
    TypeString(*engine_, L"oar");
    EXPECT_EQ(engine_->Peek(), L"ỏa");
}

TEST_F(TelexEngineTest, RisingDiphthong_UY_Acute) {
    TypeString(*engine_, L"uys");
    EXPECT_EQ(engine_->Peek(), L"úy");  // classic: tone on first
}

TEST_F(TelexEngineTest, RisingDiphthong_UY_Dot) {
    TypeString(*engine_, L"uyj");
    EXPECT_EQ(engine_->Peek(), L"ụy");  // classic: tone on first
}

// ============================================================================
// ADDITIONAL REAL WORD TESTS
// ============================================================================

TEST_F(TelexEngineTest, RealWord_Quoc) {
    TypeString(*engine_, L"quoocs");  // quốc
    EXPECT_EQ(engine_->Peek(), L"quốc");
}

TEST_F(TelexEngineTest, RealWord_Gia) {
    TypeString(*engine_, L"gia");  // gia
    EXPECT_EQ(engine_->Peek(), L"gia");
}

TEST_F(TelexEngineTest, RealWord_Nghia) {
    TypeString(*engine_, L"nghiax");  // nghĩa
    EXPECT_EQ(engine_->Peek(), L"nghĩa");
}

TEST_F(TelexEngineTest, RealWord_Dinh) {
    TypeString(*engine_, L"ddinh");  // định → wait đình?
    EXPECT_EQ(engine_->Peek(), L"đinh");
}

TEST_F(TelexEngineTest, RealWord_Dinh_PostfixD) {
    TypeString(*engine_, L"dinhd");  // d-i-n-h-d → đinh (dd fires postfix)
    EXPECT_EQ(engine_->Peek(), L"đinh");
}

TEST_F(TelexEngineTest, RealWord_Tra) {
    TypeString(*engine_, L"traf");  // trà
    EXPECT_EQ(engine_->Peek(), L"trà");
}

TEST_F(TelexEngineTest, RealWord_Cafe) {
    TypeString(*engine_, L"cafs");  // cà phê → cáf?
    EXPECT_EQ(engine_->Peek(), L"cá");  // just testing tone on a
}

TEST_F(TelexEngineTest, RealWord_Banh) {
    TypeString(*engine_, L"banhf");  // bành
    EXPECT_EQ(engine_->Peek(), L"bành");
}

TEST_F(TelexEngineTest, RealWord_Mi) {
    TypeString(*engine_, L"mif");  // mì
    EXPECT_EQ(engine_->Peek(), L"mì");
}

TEST_F(TelexEngineTest, RealWord_Pho_Full) {
    TypeString(*engine_, L"phor");  // phỏ
    EXPECT_EQ(engine_->Peek(), L"phỏ");
}

TEST_F(TelexEngineTest, RealWord_Bo) {
    TypeString(*engine_, L"bof");  // bò
    EXPECT_EQ(engine_->Peek(), L"bò");
}

TEST_F(TelexEngineTest, RealWord_Ga) {
    TypeString(*engine_, L"gaf");  // gà
    EXPECT_EQ(engine_->Peek(), L"gà");
}

TEST_F(TelexEngineTest, RealWord_Heo) {
    TypeString(*engine_, L"heor");  // hẻo
    EXPECT_EQ(engine_->Peek(), L"hẻo");
}

TEST_F(TelexEngineTest, RealWord_Ca) {
    TypeString(*engine_, L"cas");  // cá
    EXPECT_EQ(engine_->Peek(), L"cá");
}

TEST_F(TelexEngineTest, RealWord_Trung) {
    TypeString(*engine_, L"trungj");  // trụng
    EXPECT_EQ(engine_->Peek(), L"trụng");
}

TEST_F(TelexEngineTest, RealWord_Xoi) {
    TypeString(*engine_, L"xooi");  // xôi
    EXPECT_EQ(engine_->Peek(), L"xôi");
}

TEST_F(TelexEngineTest, RealWord_Com) {
    TypeString(*engine_, L"coom");  // cơm
    EXPECT_EQ(engine_->Peek(), L"côm");
}

TEST_F(TelexEngineTest, RealWord_Com_Horn) {
    TypeString(*engine_, L"cowm");  // cơm
    EXPECT_EQ(engine_->Peek(), L"cơm");
}

TEST_F(TelexEngineTest, RealWord_Nuong) {
    TypeString(*engine_, L"nuowng");  // nương
    EXPECT_EQ(engine_->Peek(), L"nương");
}

TEST_F(TelexEngineTest, RealWord_Ran) {
    TypeString(*engine_, L"rawn");  // răn
    EXPECT_EQ(engine_->Peek(), L"răn");
}

TEST_F(TelexEngineTest, RealWord_Chien) {
    TypeString(*engine_, L"chieens");  // chiến
    EXPECT_EQ(engine_->Peek(), L"chiến");
}

TEST_F(TelexEngineTest, RealWord_Hap) {
    TypeString(*engine_, L"haaps");  // hấp
    EXPECT_EQ(engine_->Peek(), L"hấp");
}

TEST_F(TelexEngineTest, Freestyles) {
    // Nonsense mash of repeated 'e' after "lè" — pins the circumflex
    // apply/escape cycling behavior rather than a real word. Tone now
    // follows the circumflexed vowel (P1/P2 always attracts, see
    // RelocateToneToTarget's unconditional call) instead of staying
    // anchored to whichever vowel happened to be toned first.
    TypeString(*engine_, L"lefeeee");  // lefeeee
    EXPECT_EQ(engine_->Peek(), L"leèe");
}

TEST_F(TelexEngineTest, TestUych) {
    TypeString(*engine_, L"huychj");  // lefeeee
    EXPECT_EQ(engine_->Peek(), L"huỵch");
}
// ============================================================================
// Z KEY — CLEAR TONE TESTS
// ============================================================================

TEST_F(TelexEngineTest, Z_ClearsTone_Acute) {
    TypeString(*engine_, L"asz");  // á → z clears tone → a
    EXPECT_EQ(engine_->Peek(), L"a");
}

TEST_F(TelexEngineTest, Z_ClearsTone_Grave) {
    TypeString(*engine_, L"afz");  // à → z → a
    EXPECT_EQ(engine_->Peek(), L"a");
}

TEST_F(TelexEngineTest, Z_ClearsTone_InWord) {
    TypeString(*engine_, L"basngz");  // bá + ng → z clears tone → "bang"
    EXPECT_EQ(engine_->Peek(), L"bang");
}

TEST_F(TelexEngineTest, Z_NoTone_AsRegularChar) {
    // No tone to clear → z is added as regular character
    TypeString(*engine_, L"az");
    EXPECT_EQ(engine_->Peek(), L"az");
}

TEST_F(TelexEngineTest, Z_Standalone) {
    TypeString(*engine_, L"z");
    EXPECT_EQ(engine_->Peek(), L"z");
}

// ============================================================================
// BRACKET KEYS [ ] — ơ ư TESTS
// ============================================================================

TEST_F(TelexEngineTest, Bracket_Open_Inserts_Oi) {
    TypeString(*engine_, L"[");
    EXPECT_EQ(engine_->Peek(), L"ơ");
}

TEST_F(TelexEngineTest, Bracket_Close_Inserts_Ui) {
    TypeString(*engine_, L"]");
    EXPECT_EQ(engine_->Peek(), L"ư");
}

TEST_F(TelexEngineTest, ShiftedBracketsInsertUppercaseHornVowels) {
    TypeString(*engine_, L"{");
    EXPECT_EQ(engine_->Peek(), L"Ơ");

    engine_->Reset();
    TypeString(*engine_, L"}");
    EXPECT_EQ(engine_->Peek(), L"Ư");
}

TEST_F(TelexEngineTest, ShiftedBracketDoubleKeyEscapesToLiteralBrace) {
    TypeString(*engine_, L"{{");
    EXPECT_EQ(engine_->Peek(), L"{");

    engine_->Reset();
    TypeString(*engine_, L"}}");
    EXPECT_EQ(engine_->Peek(), L"}");
}

TEST_F(TelexEngineTest, CapsLockBracketKeepsPhysicalRawIdentity) {
    engine_->PushKey(L'[', true);
    EXPECT_EQ(engine_->Peek(), L"Ơ");
    EXPECT_EQ(engine_->PeekRaw(), L"[");

    engine_->PushKey(L'[', true);
    EXPECT_EQ(engine_->Peek(), L"[");
    EXPECT_EQ(engine_->PeekRaw(), L"[[");
}

TEST_F(TelexEngineTest, ShiftCapsBracketCanRequestLowercaseWithoutLosingBrace) {
    engine_->PushKey(L'{', false);
    EXPECT_EQ(engine_->Peek(), L"ơ");
    EXPECT_EQ(engine_->PeekRaw(), L"{");

    engine_->PushKey(L'{', false);
    EXPECT_EQ(engine_->Peek(), L"{");
    EXPECT_EQ(engine_->PeekRaw(), L"{{");
}

TEST_F(TelexEngineTest, MixedPhysicalBracketCharactersDoNotEscapeEachOther) {
    engine_->PushKey(L'[', true);
    engine_->PushKey(L'{', true);

    EXPECT_EQ(engine_->Peek(), L"ƠƠ");
    EXPECT_EQ(engine_->PeekRaw(), L"[{");
}

TEST_F(TelexEngineTest, Bracket_InWord) {
    TypeString(*engine_, L"th[");
    EXPECT_EQ(engine_->Peek(), L"thơ");
}

TEST_F(TelexEngineTest, Bracket_Close_InWord) {
    TypeString(*engine_, L"th]");
    EXPECT_EQ(engine_->Peek(), L"thư");
}

TEST_F(TelexEngineTest, Bracket_Open_Escape) {
    // [[ → literal '['
    TypeString(*engine_, L"[[");
    EXPECT_EQ(engine_->Peek(), L"[");
}

TEST_F(TelexEngineTest, Bracket_Close_Escape) {
    // ]] → literal ']'
    TypeString(*engine_, L"]]");
    EXPECT_EQ(engine_->Peek(), L"]");
}

TEST_F(TelexEngineTest, Bracket_Open_Escape_InWord) {
    // th[[ → th[
    TypeString(*engine_, L"th[[");
    EXPECT_EQ(engine_->Peek(), L"th[");
}

TEST_F(TelexEngineTest, Bracket_Close_Escape_InWord) {
    // th]] → th]
    TypeString(*engine_, L"th]]");
    EXPECT_EQ(engine_->Peek(), L"th]");
}

TEST_F(TelexEngineTest, Bracket_NonConsecutive_NoEscape) {
    // n[s[ → nớơ (not escape — brackets separated by 's')
    TypeString(*engine_, L"n[s[");
    // First [ inserts ơ after n → nơ, s applies sắc → nớ, second [ inserts ơ → nớơ
    EXPECT_EQ(engine_->Peek(), L"n\u1EDBơ");
}

// Bracket escape after a toned vowel + invalid horn target.
// Bug 2026-05-21: spell-check ON, typing `tar]]` produced `taử]` because
// the first `]` created invalid "aư" (spellCheckDisabled_=true), the
// second `]` then bypassed ProcessModifier (escape unrecognised), fell
// through to literal-char path, and RelocateToneToTarget hijacked the
// hỏi tone from `a` onto the orphan ư. Fix: WouldModifierRecoverOrEscape
// now detects the `[[`/`]]` doubled-trigger pattern.
TEST_F(TelexEngineTest, BracketEscape_AfterTonedVowel_SpellOff) {
    TypeString(*engine_, L"tar]]");
    EXPECT_EQ(engine_->Peek(), L"tả]");
}

TEST_F(TelexEngineTest, BracketEscape_AfterTonedVowel_SpellOn) {
    config_.spellCheckEnabled = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"tar]]");
    EXPECT_EQ(engine_->Peek(), L"tả]");
}

TEST_F(TelexEngineTest, BracketEscape_AfterTonedVowel_OpenBracket_SpellOn) {
    // Mirror case for `[`: `tar[[` → tone stays on `a`, second `[` escapes.
    config_.spellCheckEnabled = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"tar[[");
    EXPECT_EQ(engine_->Peek(), L"tả[");
}

TEST_F(TelexEngineTest, W_Standalone_ProducesUHorn) {
    TypeString(*engine_, L"w");
    EXPECT_EQ(engine_->Peek(), L"ư");
}

TEST_F(TelexEngineTest, W_AfterConsonant_ProducesUHorn) {
    // "tw" → t + ư
    TypeString(*engine_, L"tw");
    EXPECT_EQ(engine_->Peek(), L"tư");
}

TEST_F(TelexEngineTest, W_UU_HornsFirstU) {
    // "luuw" → lưu (not luư): horn on first 'u', second 'u' stays as glide
    TypeString(*engine_, L"luuw");
    EXPECT_EQ(engine_->Peek(), L"l\u01B0u");  // lưu
}

TEST_F(TelexEngineTest, W_UU_NoConsonant) {
    // "uuw" → ưu (horn on first u)
    TypeString(*engine_, L"uuw");
    EXPECT_EQ(engine_->Peek(), L"\u01B0u");  // ưu
}

TEST_F(TelexEngineTest, W_UU_WithTone) {
    // "luuwj" → lựu (pomegranate): horn on first u, nặng tone
    TypeString(*engine_, L"luuwj");
    EXPECT_EQ(engine_->Peek(), L"l\u1EF1u");  // lựu
}

TEST_F(TelexEngineTest, W_UO_ReverseOrder_Duoc) {
    // u-w-o sequence: horn u first, then o → same result as u-o-w
    // "dduwocj" → được (not đựoc)
    TypeString(*engine_, L"dduwocj");
    EXPECT_EQ(engine_->Peek(), L"\u0111\u01B0\u1EE3c");  // được
}

TEST_F(TelexEngineTest, W_UO_ReverseOrder_Nuoc) {
    // "nuwocs" → nước
    TypeString(*engine_, L"nuwocs");
    EXPECT_EQ(engine_->Peek(), L"n\u01B0\u1EDBc");  // nước
}

TEST_F(TelexEngineTest, W_UO_ReverseOrder_NoTone) {
    // "dduwoc" → đươc (no tone)
    TypeString(*engine_, L"dduwoc");
    EXPECT_EQ(engine_->Peek(), L"\u0111\u01B0\u01A1c");  // đươc
}

TEST_F(TelexEngineTest, W_UO_ReverseOrder_ToneBeforeCoda) {
    // u-w-o-j-c: tone typed before coda → should still produce được (not đựơc)
    // j (nặng) arrives when only ư is a horn vowel → tone lands on ư → ự
    // then c arrives → Pattern 2 fires + tone relocates ự→ợ → được
    TypeString(*engine_, L"dduwojc");
    EXPECT_EQ(engine_->Peek(), L"\u0111\u01B0\u1EE3c");  // được
}

// ============================================================================
// BACKSPACE — DELETE WHOLE CHARACTER TESTS
// ============================================================================

TEST_F(TelexEngineTest, Backspace_DeletesWholeChar) {
    TypeString(*engine_, L"bas");  // bá
    EXPECT_EQ(engine_->Peek(), L"bá");
    engine_->Backspace();  // removes á entirely
    EXPECT_EQ(engine_->Peek(), L"b");
    EXPECT_EQ(engine_->Count(), 1u);
}

TEST_F(TelexEngineTest, Backspace_DeletesModifiedChar) {
    TypeString(*engine_, L"baa");  // bâ
    EXPECT_EQ(engine_->Peek(), L"bâ");
    engine_->Backspace();  // removes â entirely
    EXPECT_EQ(engine_->Peek(), L"b");
}

TEST_F(TelexEngineTest, Backspace_DeletesBracketChar) {
    TypeString(*engine_, L"th[");  // thơ
    engine_->Backspace();  // removes ơ
    EXPECT_EQ(engine_->Peek(), L"th");
}

// ============================================================================
// ENGLISH PROTECTION TESTS (spell check ENABLED)
// Tests that impossible Vietnamese patterns suppress diacritics.
// ============================================================================

class EnglishProtectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.spellCheckEnabled = true;
        config_.optimizeLevel = 0;
        config_.modernOrtho = false;
        engine_ = std::make_unique<TypingEngine>(config_);
    }
    TypingConfig config_;
    std::unique_ptr<TypingEngine> engine_;
};

TEST_F(EnglishProtectionTest, Ruou_AllUOTypedBeforeHornW) {
    // "uou" (3 raw vowels, no modifiers yet) must validate as ValidPrefix —
    // TryModifiedVowels used to only try flipping ONE vowel slot, missing
    // that u+o move together under horn (→ươu). That falsely marked the
    // syllable Invalid, which blocked 'w' entirely (WouldModifierRecoverOrEscape
    // only allows escaping an EXISTING horn, not applying a new one).
    TypeString(*engine_, L"ruouwj");
    EXPECT_EQ(engine_->Peek(), L"rượu");
}

TEST_F(EnglishProtectionTest, Ruou_HornWBeforeSecondU) {
    TypeString(*engine_, L"ruowuj");
    EXPECT_EQ(engine_->Peek(), L"rượu");
}

TEST_F(EnglishProtectionTest, Ruou_ToneBeforeSecondU) {
    TypeString(*engine_, L"ruowju");
    EXPECT_EQ(engine_->Peek(), L"rượu");
}

TEST_F(EnglishProtectionTest, HardReject_DR_Cluster) {
    // "drive" starts with "dr" → impossible in Vietnamese
    // Note: "dropdown" starts with "dd" which is a Vietnamese modifier (đ);
    // using "drive" instead to test pure start cluster detection
    TypeString(*engine_, L"drive");
    EXPECT_EQ(engine_->Peek(), L"drive");
}

TEST_F(EnglishProtectionTest, HardReject_WH_Cluster_SpellOn) {
    // Spell-check-ON parallel of the no-spell-check `where` test — the wh
    // revert path is gated on inputMethod==Telex, not on spellCheck, so it
    // must hold under both fixtures.
    TypeString(*engine_, L"where");
    EXPECT_EQ(engine_->Peek(), L"where");
}

TEST_F(EnglishProtectionTest, HardReject_CL_Cluster) {
    // "clear" starts with "cl" → impossible in Vietnamese  
    TypeString(*engine_, L"clear");
    EXPECT_EQ(engine_->Peek(), L"clear");
}

TEST_F(EnglishProtectionTest, HardReject_SP_Cluster) {
    // "sport" starts with "sp" → impossible in Vietnamese
    TypeString(*engine_, L"sport");
    EXPECT_EQ(engine_->Peek(), L"sport");
}

TEST_F(EnglishProtectionTest, HardReject_BR_Cluster) {
    // "brown" starts with "br" → impossible in Vietnamese.
    // The 'w' should be treated as literal, not as an 'ow'rightarrow'ơ' modifier.
    TypeString(*engine_, L"brown");
    EXPECT_EQ(engine_->Peek(), L"brown");
}

TEST_F(EnglishProtectionTest, ValidVietnamese_Thuong) {
    // "thương" is valid Vietnamese → should still compose
    TypeString(*engine_, L"thuowng");
    EXPECT_EQ(engine_->Peek(), L"thương");
}

TEST_F(EnglishProtectionTest, ValidVietnamese_Yeu) {
    // "yêu" is valid Vietnamese y-sequence
    TypeString(*engine_, L"yeeu");
    EXPECT_EQ(engine_->Peek(), L"yêu");
}

TEST_F(EnglishProtectionTest, ValidVietnamese_Dau) {
    // Regular Vietnamese word should still work
    TypeString(*engine_, L"ddaua");
    EXPECT_EQ(engine_->Peek(), L"đâu");
}

TEST_F(EnglishProtectionTest, Backspace_ResetsProtection) {
    // Type "dr" → HardEnglish, backspace twice → Unknown, then "a" is normal
    TypeString(*engine_, L"dr");
    EXPECT_EQ(engine_->Peek(), L"dr");
    engine_->Backspace();
    engine_->Backspace();
    TypeString(*engine_, L"das");
    // "da" + "s" = "dá" (Vietnamese, bias reset)
    EXPECT_EQ(engine_->Peek(), L"dá");
}

TEST_F(EnglishProtectionTest, Backspace_KeepsHardEnglish_WhenOffendingClusterStillBuried) {
    // #209 (Shzr0, 2026-07-05): "android" latches HardEnglish at "and" (invalid
    // "nd" coda) and stays literal through the whole word. Backspacing only the
    // TRAILING 'd' leaves "androi" — "nd" is still present, just no longer
    // trailing. RecalcEnglishBias must not un-latch HardEnglish here: a
    // one-shot recheck of "androi" alone misses "nd" (buffer now ends in a
    // vowel), which used to let a retyped 'd' misfire dd→đ ("anđroi").
    TypeString(*engine_, L"android");
    EXPECT_EQ(engine_->Peek(), L"android");
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"androi");
    TypeString(*engine_, L"d");
    EXPECT_EQ(engine_->Peek(), L"android");
}

TEST_F(EnglishProtectionTest, SoftReject_Effect) {
    // "effect": 'e'+'f' → spell check OK at this point (awaiting more vowels ValidPrefix).
    // 2nd 'f' = same tone escape → clears tone + adds literal 'f'.
    // Spell check then disables on 'eff' (invalid). 'e' becomes literal, 'ct' are literals.
    // Key thing: second 'e' in "effect" does NOT produce 'ê' (modifier blocked by spellCheckDisabled_).
    TypeString(*engine_, L"effect");
    // "ef" is composed with one f (tone escape consumed 2nd f), then "ect" added literally
    EXPECT_EQ(engine_->Peek(), L"efect");  // Raw "effect" → auto-restore at commit time
}

TEST_F(EnglishProtectionTest, SoftReject_Show) {
    // "show": 'sh' is invalid consonant prefix initially, spell check disabled.
    // 'o' typed literally.
    // 'w' typed should not modify 'o' into 'ơ'.
    TypeString(*engine_, L"show");
    EXPECT_EQ(engine_->Peek(), L"show");
}

TEST_F(EnglishProtectionTest, SoftReject_Available) {
    // "available": invalid sequence early on ("av"), modifiers (like 'a') should be literals.
    TypeString(*engine_, L"available");
    EXPECT_EQ(engine_->Peek(), L"available");
}

TEST_F(EnglishProtectionTest, SoftReject_Vietnamese) {
    // "vietnamese": invalid sequence because of mixing english string and vn string structures.
    TypeString(*engine_, L"vietnamese");
    EXPECT_EQ(engine_->Peek(), L"vietnamese");
}

// ============================================================================
// -ING + TONE BLOCK (VCPair: i + ng = Invalid)
// Vietnamese has no word with rhyme -ing + tone (tíng, kíng don't exist).
// But -inh + tone is common (tính, kính, lính).
// Requires spellCheckEnabled = true (EnglishProtectionTest fixture).
// ============================================================================

TEST_F(EnglishProtectionTest, IngTone_Ting_Blocked) {
    // "tings": -ing coda makes spellCheckDisabled_ = true, 's' is literal
    TypeString(*engine_, L"tings");
    EXPECT_EQ(engine_->Peek(), L"tings");
}

TEST_F(EnglishProtectionTest, IngTone_King_Blocked) {
    TypeString(*engine_, L"kings");
    EXPECT_EQ(engine_->Peek(), L"kings");
}

TEST_F(EnglishProtectionTest, IngTone_Thing_Blocked) {
    TypeString(*engine_, L"things");
    EXPECT_EQ(engine_->Peek(), L"things");
}

TEST_F(EnglishProtectionTest, IngTone_Ring_Blocked) {
    TypeString(*engine_, L"rings");
    EXPECT_EQ(engine_->Peek(), L"rings");
}

TEST_F(EnglishProtectionTest, IngTone_Sing_Blocked) {
    TypeString(*engine_, L"sings");
    EXPECT_EQ(engine_->Peek(), L"sings");
}

TEST_F(EnglishProtectionTest, InhTone_Tinh_StillWorks) {
    // -inh + tone is valid Vietnamese (tính, kính)
    TypeString(*engine_, L"tinhs");
    EXPECT_EQ(engine_->Peek(), L"t\xEDnh");  // tính
}

TEST_F(EnglishProtectionTest, InhTone_Kinh_StillWorks) {
    TypeString(*engine_, L"kinhs");
    EXPECT_EQ(engine_->Peek(), L"k\xEDnh");  // kính
}

TEST_F(EnglishProtectionTest, IngNoTone_Ting_PassThrough) {
    // "ting" without tone → spellCheckDisabled_ but no diacritics → commit as-is
    TypeString(*engine_, L"ting");
    EXPECT_EQ(engine_->Peek(), L"ting");
}

// ============================================================================
// ENGLISH DETECTION WITHOUT SPELL CHECK
// English detection is always active, even when spell check is OFF.
// ============================================================================

class EnglishDetectionNoSpellCheckTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.spellCheckEnabled = false;  // Spell check OFF
        config_.optimizeLevel = 0;
        engine_ = std::make_unique<TypingEngine>(config_);
    }
    TypingConfig config_;
    std::unique_ptr<TypingEngine> engine_;
};

// Cross-vowel circumflex free-marking must validate the RESULT syllable,
// not the current buffer. Typing an extra vowel after a toned diphthong
// (e.g., "vào" + 'a' → would-be "vầo") must not consume the extra vowel
// when the resulting syllable is invalid Vietnamese.
class CircumflexFreeMarkSpellOnTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::Telex;
        config_.spellCheckEnabled = true;
        config_.optimizeLevel = 0;
        engine_ = std::make_unique<TypingEngine>(config_);
    }
    TypingConfig config_;
    std::unique_ptr<TypingEngine> engine_;
};

TEST_F(CircumflexFreeMarkSpellOnTest, VaoPlusA_NoCircumflex) {
    TypeString(*engine_, L"vaofa");  // vào + extra 'a'
    EXPECT_EQ(engine_->Peek(), L"vàoa");
}

TEST_F(CircumflexFreeMarkSpellOnTest, VeoPlusE_NoCircumflex) {
    TypeString(*engine_, L"veofe");  // vèo + extra 'e'
    EXPECT_EQ(engine_->Peek(), L"vèoe");
}

// Regression: legitimate cross-vowel circumflex must still work.
TEST_F(CircumflexFreeMarkSpellOnTest, CauPlusA_StillCircumflexes) {
    TypeString(*engine_, L"caua");  // cau + a → câu
    EXPECT_EQ(engine_->Peek(), L"câu");
}

TEST_F(CircumflexFreeMarkSpellOnTest, ChieuPlusE_StillCircumflexes) {
    TypeString(*engine_, L"chieue");  // chieu + e → chiêu
    EXPECT_EQ(engine_->Peek(), L"chiêu");
}

// Free-marking across coda must speculate WITH tone relocation. Previously,
// "súat" + 'a' (= raw "susata") rejected the circumflex because the
// unrelocated speculative state had sắc on `u` of `uâ` cluster, which
// validator marks Invalid. Runtime relocates sắc → `â` after applying
// circumflex, so the real result `suất` is valid Vietnamese.
TEST_F(CircumflexFreeMarkSpellOnTest, SuatPlusA_PromotesToSuat) {
    TypeString(*engine_, L"susata");  // s u s(sắc) a t a → suất
    EXPECT_EQ(engine_->Peek(), L"suất");
}

// Adjacent-vowel circumflex (aa/ee/oo direct) must also validate the result.
// "của" + extra 'a' previously produced "củâ" (invalid syllable).
TEST_F(CircumflexFreeMarkSpellOnTest, CuaPlusA_AdjacentRejected) {
    TypeString(*engine_, L"cuara");  // của + extra 'a'
    EXPECT_EQ(engine_->Peek(), L"củaa");
}

// Regression: adjacent circumflex on a Valid (already-complete) syllable
// must relocate the tone immediately, not just on a later keystroke.
// "lụa" (tone on 'u') + 'a' → 'a' promotes to 'â'; without an immediate
// relocate the tone was left stranded on 'u' ("lụâ") until a following
// char's FinalizeRegularChar happened to fix it up.
TEST_F(TelexEngineTest, LuaPlusA_RelocatesToneImmediately) {
    TypeString(*engine_, L"luaja");  // lụa + extra 'a'
    EXPECT_EQ(engine_->Peek(), L"luậ");
    TypeString(*engine_, L"t");
    EXPECT_EQ(engine_->Peek(), L"luật");
}

TEST_F(CircumflexFreeMarkSpellOnTest, CuaPlusA_Grave_AdjacentRejected) {
    TypeString(*engine_, L"cufaa");  // cùa + extra 'a'
    EXPECT_EQ(engine_->Peek(), L"cùaa");
}

// Mark/tone typed before the coda's 'h' must still reach the ch-final word —
// "khuêc"/"huyc" are prefixes of "khuếch"/"huých", not dead ends. Rust engine
// already allowed these orders; the C++ engine latched spell check off.
TEST_F(CircumflexFreeMarkSpellOnTest, KhuechEarlyCircumflex_ReachesKhuech) {
    TypeString(*engine_, L"khuecehs");  // circumflex before the 'h'
    EXPECT_EQ(engine_->Peek(), L"khuếch");
}

TEST_F(CircumflexFreeMarkSpellOnTest, HuychEarlyTone_ReachesHuych) {
    TypeString(*engine_, L"huycsh");  // sắc before the 'h'
    EXPECT_EQ(engine_->Peek(), L"huých");
}

// Regression: legitimate adjacent circumflex still applies.
TEST_F(CircumflexFreeMarkSpellOnTest, BaPlusA_AdjacentCircumflexes) {
    TypeString(*engine_, L"baa");  // ba + a → bâ
    EXPECT_EQ(engine_->Peek(), L"bâ");
}

// VNI mode has the same split-tone/mod bug: "của" + '6' would put circumflex
// on 'a' while tone stays on 'u' → "củâ". Guard must reject and let '6' fall
// through as a literal.
class VniModifierSplitGuardTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::Combined;
        config_.spellCheckEnabled = true;
        config_.optimizeLevel = 0;
        engine_ = std::make_unique<TypingEngine>(config_);
    }
    TypingConfig config_;
    std::unique_ptr<TypingEngine> engine_;
};

TEST_F(VniModifierSplitGuardTest, Cua_Plus6_Rejected) {
    TypeString(*engine_, L"cuar6");  // của + '6' (circumflex key)
    // '6' falls through as literal — tone stays on 'u', no bogus â on 'a'.
    EXPECT_EQ(engine_->Peek(), L"của6");
}

TEST_F(VniModifierSplitGuardTest, Bao_Plus6_Rejected) {
    TypeString(*engine_, L"baos6");  // báo + '6'
    EXPECT_EQ(engine_->Peek(), L"báo6");
}

// Regression: legitimate VNI circumflex still applies when tone and mod
// target the same vowel.
TEST_F(VniModifierSplitGuardTest, Bas_Plus6_Applies) {
    TypeString(*engine_, L"bas6");  // bá + '6' → bấ (tone and mod both on 'a')
    EXPECT_EQ(engine_->Peek(), L"bấ");
}

TEST_F(EnglishDetectionNoSpellCheckTest, HardReject_FL_Cluster_BlocksTone) {
    // "fl" is impossible in Vietnamese → HardEnglish → tone keys literal
    TypeString(*engine_, L"flas");
    EXPECT_EQ(engine_->Peek(), L"flas");  // 's' NOT applied as tone
}

TEST_F(EnglishDetectionNoSpellCheckTest, HardReject_CL_Cluster_BlocksTone) {
    TypeString(*engine_, L"clas");
    EXPECT_EQ(engine_->Peek(), L"clas");  // 's' NOT applied as tone
}

TEST_F(EnglishDetectionNoSpellCheckTest, HardReject_BR_Cluster_BlocksModifier) {
    // "br" is HardEnglish → 'w' should be literal, not modifier
    TypeString(*engine_, L"brown");
    EXPECT_EQ(engine_->Peek(), L"brown");
}

TEST_F(EnglishDetectionNoSpellCheckTest, HardReject_SP_Cluster_BlocksTone) {
    TypeString(*engine_, L"spas");
    EXPECT_EQ(engine_->Peek(), L"spas");  // 's' literal
}

TEST_F(EnglishDetectionNoSpellCheckTest, HardReject_WH_Cluster_BlocksToneAndModifier) {
    // "wh" is impossible in Vietnamese (only wr was listed; wh was missing).
    // Without the guard, w-h-e-r-e leaks tone 'r' onto 'e' (→ whẻ) and the
    // trailing 'e' triggers ee→ê (→ whể). Bias must lock to HardEnglish at "wh".
    // Full Telex path: P8 rewrites 'w' → synthetic ư before the states-based
    // start-cluster check sees it, so the raw-input fallback must catch wh.
    TypeString(*engine_, L"where");
    EXPECT_EQ(engine_->Peek(), L"where");
}

TEST_F(EnglishDetectionNoSpellCheckTest, WwEscape_StillWorks) {
    // Regression guard: ww → literal w (Telex escape) must not be tripped
    // by the wh start-cluster fix — IsHardEnglishStart('w','w') is false.
    TypeString(*engine_, L"ww");
    EXPECT_EQ(engine_->Peek(), L"w");
}

TEST_F(EnglishDetectionNoSpellCheckTest, HardReject_KN_Cluster_BlocksTone) {
    // "kn" is impossible in Vietnamese (know, knee, knight, knife...)
    TypeString(*engine_, L"knas");
    EXPECT_EQ(engine_->Peek(), L"knas");  // 's' NOT applied as tone
}

TEST_F(EnglishDetectionNoSpellCheckTest, HardReject_KN_Cluster_BlocksModifier) {
    // "know" — 'w' must stay literal, not become ư
    TypeString(*engine_, L"know");
    EXPECT_EQ(engine_->Peek(), L"know");
}

TEST_F(EnglishDetectionNoSpellCheckTest, HardReject_PN_Cluster_BlocksTone) {
    // "pn" is impossible in Vietnamese (pneumonia, pneumatic...)
    TypeString(*engine_, L"pnas");
    EXPECT_EQ(engine_->Peek(), L"pnas");
}

TEST_F(EnglishDetectionNoSpellCheckTest, HardReject_PS_Cluster_BlocksTone) {
    // "ps" is impossible in Vietnamese (psycho, psalm, pseudo...)
    TypeString(*engine_, L"psas");
    EXPECT_EQ(engine_->Peek(), L"psas");
}

TEST_F(EnglishDetectionNoSpellCheckTest, GH_Cluster_IsVietnamese) {
    // Regression guard: 'gh' IS a valid Vietnamese onset (ghế, ghi, ghen, ghê).
    // Must NOT be in IsHardEnglishStart — 'ghes' should compose 'ghẹ' (nặng on 'e'
    // becomes ghẹ when nucleus is plain e; here ghes → ghệ via ee→ê + s on bare e:
    // actually just verify tone applies, not the exact diacritic shape).
    TypeString(*engine_, L"ghes");
    // 's' (Sac) should land on 'e' → ghé. If 'gh' were blocked, tone would be
    // literal and Peek would equal 'ghes'.
    EXPECT_EQ(engine_->Peek(), L"ghé");
}

// ─── Regression guards for kn/pn/ps additions: TV onsets starting with k-/p- ─
// Must NOT over-block legitimate Vietnamese words. k is always followed by a
// vowel (e/ê/i/y) in TV; p is rare standalone but appears in loanwords (pin,
// Pháp). ph is a TV digraph, not blocked.

TEST_F(EnglishDetectionNoSpellCheckTest, KConsonant_KemNotBlocked) {
    TypeString(*engine_, L"kems");  // kém = k+e+m+s(sac)
    EXPECT_EQ(engine_->Peek(), L"kém");
}

TEST_F(EnglishDetectionNoSpellCheckTest, KConsonant_KinhNotBlocked) {
    TypeString(*engine_, L"kinh");  // kinh — k+i+n+h, no tone
    EXPECT_EQ(engine_->Peek(), L"kinh");
}

TEST_F(EnglishDetectionNoSpellCheckTest, KConsonant_KeetsComposesKet) {
    TypeString(*engine_, L"keets");  // kết = k+e+e(→ê)+t+s(sac)
    EXPECT_EQ(engine_->Peek(), L"kết");
}

TEST_F(EnglishDetectionNoSpellCheckTest, KConsonant_KieenComposesKien) {
    TypeString(*engine_, L"kieen");  // kiên = k+i+e+e(→ê)+n
    EXPECT_EQ(engine_->Peek(), L"kiên");
}

TEST_F(EnglishDetectionNoSpellCheckTest, PConsonant_PinNotBlocked) {
    TypeString(*engine_, L"pin");  // loanword "pin" (battery)
    EXPECT_EQ(engine_->Peek(), L"pin");
}

TEST_F(EnglishDetectionNoSpellCheckTest, PhConsonant_PhoComposesPho) {
    TypeString(*engine_, L"phowr");  // phở = ph+o+w(→ơ)+r(hoi)
    EXPECT_EQ(engine_->Peek(), L"phở");
}

TEST_F(EnglishDetectionNoSpellCheckTest, PhConsonant_PhapsComposesPhap) {
    TypeString(*engine_, L"phaps");  // Pháp = ph+a+p+s(sac)
    EXPECT_EQ(engine_->Peek(), L"pháp");
}

TEST_F(EnglishDetectionNoSpellCheckTest, KhConsonant_KhongNotBlocked) {
    TypeString(*engine_, L"khoong");  // không = kh+o+o(→ô)+n+g
    EXPECT_EQ(engine_->Peek(), L"không");
}

TEST_F(EnglishDetectionNoSpellCheckTest, PhConsonant_PhuocsComposesPhuoc) {
    TypeString(*engine_, L"phuwowcs");  // phước = ph+u+w(→ư)+o+w(→ơ)+c+s(sac)
    EXPECT_EQ(engine_->Peek(), L"phước");
}

TEST_F(EnglishDetectionNoSpellCheckTest, WsngStaysUng) {
    // Regression: 'w' (P8 → ư) + 's' (Sac tone) + n + g must compose to ứng.
    // raw[0..1]='ws' is not in IsHardEnglishStart, so the wh/wr revert path
    // must not interfere.
    TypeString(*engine_, L"wsng");
    EXPECT_EQ(engine_->Peek(), L"ứng");
}

TEST_F(EnglishDetectionNoSpellCheckTest, WrngStaysUng_ToneBlocksRevert) {
    // Regression: 'w' (P8 → ư) + 'r' (Hoi tone) + n + g must compose to ửng,
    // even though raw[0..1]='wr' IS in IsHardEnglishStart. The synthetic-ư
    // revert is gated on tone==None precisely so this Vietnamese sequence
    // (tone applied at step 2) survives.
    TypeString(*engine_, L"wrng");
    EXPECT_EQ(engine_->Peek(), L"ửng");
}

TEST_F(EnglishDetectionNoSpellCheckTest, ValidVietnamese_StillComposes) {
    // "thương" should still compose even without spell check
    TypeString(*engine_, L"thuowng");
    EXPECT_EQ(engine_->Peek(), L"thương");
}

TEST_F(EnglishDetectionNoSpellCheckTest, ValidVietnamese_Dau) {
    // ddaauf: dd→đ, aa→â, u, f→Grave on â → đầu
    TypeString(*engine_, L"ddaauf");
    EXPECT_EQ(engine_->Peek(), L"đầu");
}

TEST_F(EnglishDetectionNoSpellCheckTest, Backspace_ResetsProtection) {
    // Type "dr" → HardEnglish, backspace twice → Unknown, then compose works
    TypeString(*engine_, L"dr");
    EXPECT_EQ(engine_->Peek(), L"dr");
    engine_->Backspace();
    engine_->Backspace();
    TypeString(*engine_, L"as");
    EXPECT_EQ(engine_->Peek(), L"á");  // Tone applied normally
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifierEscape_DropdownNoMangle) {
    // "dropddown" (9 keys): coda pre-check blocks dd→đ → all chars literal
    TypeString(*engine_, L"dropddown");
    EXPECT_EQ(engine_->Peek(), L"dropddown");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifierEscape_DownloadNoMangle) {
    // Telex escape convention: "ddd" → "dd" (same as "aaa" → "aa", "aww" → "aw").
    // "dd" → đ consumed as one modifier state; "ddd" escape adds literal 'd' back → [d, d] = "dd".
    // To type "download", just type "download" (8 keys, no dd trigger). This test verifies
    // that "dddownload" gives "ddownload" and does NOT mangle the remainder via free-mark.
    TypeString(*engine_, L"dddownload");
    EXPECT_EQ(engine_->Peek(), L"ddownload");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifierEscape_BackspaceClearsFlag) {
    // After dd escape sets toneEscaped_, backspace should clear it.
    TypeString(*engine_, L"dropddown");  // toneEscaped_ set after dd
    engine_->Backspace();               // remove 'n'
    engine_->Backspace();               // remove 'w' — also clears toneEscaped_
    engine_->PushChar(L'w');            // 'w' should now be free to apply as modifier again
    // After clearing toneEscaped_, typing 'w' after 'o' (from "dropddo") → modifier
    // But at this point bias may still be complex; just verify no crash and something is returned
    EXPECT_FALSE(engine_->Peek().empty());
}

TEST_F(EnglishDetectionNoSpellCheckTest, WModifierEscape_DownloadNoMangle) {
    // "download": d-o-w-w-n-l-o-a-d (9 keys)
    // The "oww" escape (ơ → plain 'o') must set toneEscaped_ so the later 'o' and 'a'
    // don't free-mark backward. Without fix: 'o'(7) free-marks state[1]'o' → "dôwnlad".
    TypeString(*engine_, L"dowwnload");
    EXPECT_EQ(engine_->Peek(), L"download");
}


TEST_F(EnglishDetectionNoSpellCheckTest, ToneEscape_DashboardNoMangle) {
    // "dashboard": d-a-s-s-h-b-o-a-r-d
    // After 'ss' escape, all subsequent tones/modifiers should be blocked
    engine_->PushChar(L'd');
    EXPECT_EQ(engine_->Peek(), L"d");
    engine_->PushChar(L'a');
    EXPECT_EQ(engine_->Peek(), L"da");
    engine_->PushChar(L's');  // Tone: Acute on 'a'
    EXPECT_EQ(engine_->Peek(), L"dá");
    engine_->PushChar(L's');  // Escape: remove tone
    std::wstring after_ss = engine_->Peek();
    // After escape: states = [d, a, s], toneEscaped_ = true
    EXPECT_EQ(after_ss, L"das");
    engine_->PushChar(L'h');
    EXPECT_EQ(engine_->Peek(), L"dash");
    engine_->PushChar(L'b');
    EXPECT_EQ(engine_->Peek(), L"dashb");
    engine_->PushChar(L'o');
    EXPECT_EQ(engine_->Peek(), L"dashbo");
    engine_->PushChar(L'a');  // Should NOT trigger aa→â modifier
    EXPECT_EQ(engine_->Peek(), L"dashboa");
    engine_->PushChar(L'r');  // Should NOT apply as tone
    EXPECT_EQ(engine_->Peek(), L"dashboar");
    engine_->PushChar(L'd');  // Should NOT trigger dd→đ modifier
    EXPECT_EQ(engine_->Peek(), L"dashboard");
}

TEST_F(EnglishDetectionNoSpellCheckTest, ToneEscape_BlocksSubsequentTones) {
    // After 'ss' escape, all subsequent tone keys are literal
    TypeString(*engine_, L"das");  // d-a-s: tone on 'a'
    EXPECT_EQ(engine_->Peek(), L"dá");  // Tone applied
    engine_->PushChar(L's');  // Escape: ss → clear tone, set toneEscaped_
    // After escape: 's' was consumed by the tone escape mechanism.
    // Engine has [d, a, s] states. Peek = "das"
    std::wstring afterEscape = engine_->Peek();
    EXPECT_EQ(afterEscape.substr(0, 2), L"da");  // At least "da" preserved
    // Now type 'r' — should NOT apply as Hook tone
    engine_->PushChar(L'r');
    std::wstring withR = engine_->Peek();
    // 'r' should be literal, not a tone mark
    EXPECT_TRUE(withR.back() == L'r');
}

TEST_F(EnglishDetectionNoSpellCheckTest, ToneEscape_BackspaceClearsFlag) {
    // After escape, backspace should allow Vietnamese again
    TypeString(*engine_, L"das");
    engine_->PushChar(L's');  // Escape
    engine_->Backspace();     // Undo escape
    engine_->Backspace();     // Remove 'a' (now just 'd')
    engine_->PushChar(L'a');
    engine_->PushChar(L's');  // Should apply tone again
    EXPECT_EQ(engine_->Peek(), L"dá");
}

TEST_F(EnglishDetectionNoSpellCheckTest, HardReject_GL_Cluster_BlocksModifier) {
    // "gl" is impossible in Vietnamese → HardEnglish
    // The 'o' + 'w' should NOT produce 'ơ' (modifier blocked)
    TypeString(*engine_, L"glow");
    EXPECT_EQ(engine_->Peek(), L"glow");
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_ManagerDoubleA) {
    // User types "manaager" (double 'a' to escape the free-mark circumflex).
    // After 'aa' escape: states=[m,a,n,a,g,e], V+1C+V (a+g+e) → 'r' must be literal.
    // This is the exact user-reported scenario: manaager should not give managẻ.
    TypeString(*engine_, L"manaager");
    EXPECT_EQ(engine_->Peek(), L"manager");
}

TEST_F(EnglishDetectionNoSpellCheckTest, CircumflexEscape_AdjDoubled_WithCoda_Escapes) {
    // h+i+e+e+n+e+r (7 keys): ee→ê, 'n' coda, 3rd 'e' escapes circumflex.
    // Escape works regardless of coda — user intent to undo ê is clear.
    // After escape: toneEscaped_=true → 'r' is literal.
    TypeString(*engine_, L"hieener");
    EXPECT_EQ(engine_->Peek(), L"hiener");
}

TEST_F(EnglishDetectionNoSpellCheckTest, CircumflexEscape_Xuyen) {
    // "xuyên" + 'e': escape circumflex → "xuyene"
    // x-u-y-e-e → "xuyê", n → "xuyên", e → escape ê → "xuyene"
    TypeString(*engine_, L"xuyeene");
    EXPECT_EQ(engine_->Peek(), L"xuyene");
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_Manager7Keys) {
    // "manager" (7 keys, single 'a'): 4th 'a' free-marks â, then V+2C+V (â+n,g+e).
    // Tone is blocked (not "managẻ"), but â stays (circumflex from free-mark).
    // Result: "mânger" — partial fix; full fix requires double 'a' escape or spell check.
    TypeString(*engine_, L"manager");
    EXPECT_EQ(engine_->Peek(), L"m\xE2nger");  // mânger
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_Danger) {
    // "danger": no modifier involved, pure V+2C+V (a+n,g+e) structural detection.
    TypeString(*engine_, L"danger");
    EXPECT_EQ(engine_->Peek(), L"danger");
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_Mangle) {
    // "mangle": m,a,n,g,l,e → V+3C+V → 'r' and 's' blocked
    TypeString(*engine_, L"manglers");
    EXPECT_EQ(engine_->Peek(), L"manglers");
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_Lane) {
    // "lane": l,a,n,e → V+1C+V (a+n+e) → 'r' must be literal
    TypeString(*engine_, L"laner");
    EXPECT_EQ(engine_->Peek(), L"laner");
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_ValidVN_ShortWord) {
    // Short word (count < 4): structural check doesn't apply.
    // "hỏi" = h,o,i + 'r' → 3 states, count < 4 → 'r' applies as Hook → "hỏi"
    TypeString(*engine_, L"hoir");
    EXPECT_EQ(engine_->Peek(), L"h\x1ECFi");  // h + ỏ + i
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_ValidVN_Ipon) {
    // "ipỏn": i,p,o + 'r' → 3 states, count < 4 → V+C+V skipped → free-mark works
    TypeString(*engine_, L"iporn");
    EXPECT_EQ(engine_->Peek(), L"ip\x1ECFn");  // ip + ỏ + n
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_ValidVN_Dau) {
    // "đầu": đ,â,u → 0 consonants between â and u → 'f' applies as Grave
    TypeString(*engine_, L"ddaauf");
    EXPECT_EQ(engine_->Peek(), L"đầu");
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_ValidVN_Xoai) {
    // "xoải": x,o,a,i → 0 consonants between 'a' and 'i' → 'r' applies as Hook
    // (tests that adjacent vowels don't trigger the check)
    TypeString(*engine_, L"xoair");
    EXPECT_EQ(engine_->Peek(), L"xo\x1EA3i");  // xoải: x + o + ả + i
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_SubsequentCharsLiteral) {
    // After 'r' triggers HardEnglish on "manaager", 's' should also be literal
    TypeString(*engine_, L"manaagers");
    EXPECT_EQ(engine_->Peek(), L"managers");
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_ToneS_Blocked) {
    // "bone" + 's': V+C+V (o+n+e) detected with tone key 's' (sắc).
    // Previously 's' was not in IsHardEnglishEnd → V+C+V skipped → tone leaked.
    TypeString(*engine_, L"bones");
    EXPECT_EQ(engine_->Peek(), L"bones");
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_ToneJ_Blocked) {
    // "bone" + 'j': V+C+V (o+n+e) detected with tone key 'j' (nặng).
    TypeString(*engine_, L"bonej");
    EXPECT_EQ(engine_->Peek(), L"bonej");
}

TEST_F(EnglishDetectionNoSpellCheckTest, StructuralHardEnglish_ToneS_LaneBlocked) {
    // "lane" + 's': V+1C+V (a+n+e) → 's' blocked as tone
    TypeString(*engine_, L"lanes");
    EXPECT_EQ(engine_->Peek(), L"lanes");
}

TEST_F(EnglishDetectionNoSpellCheckTest, ValidVN_ToneS_StillWorks) {
    // Vietnamese "bás" (b+a+s): no V+C+V → 's' applies as sắc tone normally
    TypeString(*engine_, L"bas");
    EXPECT_EQ(engine_->Peek(), L"b\xE1");  // bá
}

TEST_F(EnglishDetectionNoSpellCheckTest, ValidVN_ToneJ_StillWorks) {
    // Vietnamese "bạ" (b+a+j): no V+C+V → 'j' applies as nặng tone normally
    TypeString(*engine_, L"baj");
    EXPECT_EQ(engine_->Peek(), L"b\x1EA1");  // bạ
}

TEST_F(EnglishDetectionNoSpellCheckTest, ToneEscape_ThenHardEnglishEnd_S) {
    // "bass": b+a+s → tone sắc on 'a' ("bá"), then +s → escape tone + add literal 's'.
    // After escape: states=[b,a,s], RecalcEnglishBias sees vowel+'s' → HardEnglish.
    // Subsequent keys should remain literal.
    TypeString(*engine_, L"bas");
    EXPECT_EQ(engine_->Peek(), L"b\xE1");  // bá: tone applied
    engine_->PushChar(L's');  // Escape: remove tone, add 's' literal
    EXPECT_EQ(engine_->Peek(), L"bas");     // Tone cleared, 's' as literal
    engine_->PushChar(L'e');  // Should be literal (HardEnglish after escape)
    EXPECT_EQ(engine_->Peek(), L"base");
}

TEST_F(EnglishDetectionNoSpellCheckTest, FreeMarkBlocked_Solution) {
    // "solution": s-o-l-u-t-i-o-n. Second 'o' free-marks across consonant+vowel groups.
    // Path o→[l,u,t,i]→o has consonants between different vowels → blocked.
    TypeString(*engine_, L"solution");
    EXPECT_EQ(engine_->Peek(), L"solution");  // No circumflex on 'o'
}

TEST_F(EnglishDetectionNoSpellCheckTest, FreeMarkBlocked_Review) {
    // "review": r-e-v-i-e-w. Second 'e' free-marks across consonant+vowel.
    // Path e→[v,i]→e has consonant 'v' between vowels → blocked.
    TypeString(*engine_, L"review");
    EXPECT_EQ(engine_->Peek(), L"review");  // No circumflex on 'e'
}

TEST_F(EnglishDetectionNoSpellCheckTest, FullBufferVCV_Behavior) {
    // "behavior"+r: V+C+V pattern e+h+a exists in mid-buffer.
    // Full scan catches it even though tail (i,o) is adjacent vowels.
    TypeString(*engine_, L"behavior");
    EXPECT_EQ(engine_->Peek(), L"behavior");  // 'r' is literal, no tone
}

TEST_F(EnglishDetectionNoSpellCheckTest, FreeMarkBlocked_Release_CodaCheck) {
    // "release": r-e-l-e-a-s-e. Same-vowel free-mark e→[l]→e blocked because
    // 'l' is NOT a valid Vietnamese coda (only c/m/n/p/t). Sets HardEnglish.
    // All subsequent chars are literal.
    TypeString(*engine_, L"release");
    EXPECT_EQ(engine_->Peek(), L"release");
}

TEST_F(EnglishDetectionNoSpellCheckTest, FreeMarkBlocked_Advance_TwoCons) {
    // "adva" (typing "advance"): a-d-v + 'a' attempts same-vowel free-mark
    // across 2 consonants ("dv"). No valid Vietnamese syllable has two
    // non-digraph consonants between identical vowels — block and mark
    // HardEnglish so the remaining keys type literally.
    TypeString(*engine_, L"advance");
    EXPECT_EQ(engine_->Peek(), L"advance");
}

TEST_F(EnglishDetectionNoSpellCheckTest, VCV_Exception_ValidCoda_Hien) {
    // "hiên"+e+r: ê+n+e where 'n' IS a valid Vietnamese coda → exception allows.
    // But 'e' escapes circumflex (our earlier fix) → toneEscaped → 'r' literal.
    TypeString(*engine_, L"hieener");
    EXPECT_EQ(engine_->Peek(), L"hiener");
}

TEST_F(EnglishDetectionNoSpellCheckTest, FreeMarkAllowed_Chieu) {
    // "chiếu": c-h-i-e-u + 'e' (free-mark circumflex) + 's' (tone sắc).
    // Path e→[u]→e has no consonant between vowels (adjacent) → circumflex allowed.
    TypeString(*engine_, L"chieues");
    EXPECT_EQ(engine_->Peek(), L"chi\x1EBFu");  // chiếu
}

TEST_F(EnglishDetectionNoSpellCheckTest, FreeMarkAllowed_Cau) {
    // "cầu": c-a-u + 'a' (free-mark circumflex) + 'f' (tone huyền).
    // Path a→[u]→a has no consonant → allowed.
    TypeString(*engine_, L"cauaf");
    EXPECT_EQ(engine_->Peek(), L"c\x1EA7u");  // cầu
}

TEST_F(EnglishDetectionNoSpellCheckTest, FreeMarkAllowed_Tieng) {
    // "tiếng": t-i-e-n-g + 'e' (circumflex) + 's' (tone sắc).
    // Scan finds 'e' immediately after consonants → needsValidation=false → allowed.
    TypeString(*engine_, L"tienges");
    EXPECT_EQ(engine_->Peek(), L"ti\x1EBFng");  // tiếng
}

// --- Delayed circumflex VCV (spell check OFF, coda validation only) ---

TEST_F(EnglishDetectionNoSpellCheckTest, DelayedCircumflex_VCV_Toto) {
    // "toto" → same-vowel o across valid coda 't' → "tôt"
    TypeString(*engine_, L"toto");
    EXPECT_EQ(engine_->Peek(), L"tôt");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DelayedCircumflex_VCV_Ata) {
    // "ata" → same-vowel a across valid coda 't' → "ât"
    TypeString(*engine_, L"ata");
    EXPECT_EQ(engine_->Peek(), L"ât");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DelayedCircumflex_VCV_Oho_Blocked) {
    // "oho" → 'h' not valid coda → HardEnglish → no circumflex
    TypeString(*engine_, L"oho");
    EXPECT_EQ(engine_->Peek(), L"oho");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DelayedCircumflex_VCV_Olo_Blocked) {
    // "olo" → 'l' not valid coda → HardEnglish → no circumflex
    TypeString(*engine_, L"olo");
    EXPECT_EQ(engine_->Peek(), L"olo");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DelayedCircumflex_VCV_Ono_Allowed) {
    // "ono" → 'n' IS valid coda → circumflex allowed → "ôn"
    TypeString(*engine_, L"ono");
    EXPECT_EQ(engine_->Peek(), L"ôn");
}

// ============================================================================
// INVALID ADJACENT VOWEL PAIR — plain vowel pairs impossible in Vietnamese
// ============================================================================

TEST_F(EnglishDetectionNoSpellCheckTest, InvalidEANucleus_Search_RIsLiteral) {
    // "sear": plain 'e' + plain 'a' is not a Vietnamese vowel nucleus.
    // 'r' must be literal, not tone key (hỏi). Without fix: "seảr".
    TypeString(*engine_, L"sear");
    EXPECT_EQ(engine_->Peek(), L"sear");
}

TEST_F(EnglishDetectionNoSpellCheckTest, InvalidEANucleus_Search_FullWord) {
    // Full word "search": no diacritics.
    TypeString(*engine_, L"search");
    EXPECT_EQ(engine_->Peek(), L"search");
}

TEST_F(EnglishDetectionNoSpellCheckTest, InvalidEANucleus_SeaF_FIsLiteral) {
    // "seaf": plain 'ea' + tone key 'f' (huyền) → 'f' must be literal.
    TypeString(*engine_, L"seaf");
    EXPECT_EQ(engine_->Peek(), L"seaf");
}

TEST_F(EnglishDetectionNoSpellCheckTest, InvalidEANucleus_VN_Ao_Unaffected) {
    // "sao"+'r': 'ao' is valid Vietnamese (rule FIRST) → tone on 'a' → "sảo". Unaffected.
    TypeString(*engine_, L"saor");
    EXPECT_EQ(engine_->Peek(), L"s\x1EA3o");  // sảo
}

TEST_F(EnglishDetectionNoSpellCheckTest, InvalidEANucleus_VN_IponUnaffected) {
    // "ipỏn": V+C+V at count=3 is preserved (count<4 guard). Unaffected.
    TypeString(*engine_, L"iporn");
    EXPECT_EQ(engine_->Peek(), L"ip\x1ECFn");  // ipỏn
}

TEST_F(EnglishDetectionNoSpellCheckTest, InvalidEANucleus_SmartAccent_Tiensge_Unaffected) {
    // Smart accent path "tiếng" uses 'i'+'e' (plain) at tone time — must NOT be blocked.
    TypeString(*engine_, L"tienges");
    EXPECT_EQ(engine_->Peek(), L"ti\x1EBFng");  // tiếng
}

// ============================================================================
// GI CLUSTER — 'i' in gi+vowel acts as consonant, not vowel
// HasInvalidAdjacentVowelPair must NOT flag gi+vowel as invalid nucleus.
// ============================================================================

TEST_F(EnglishDetectionNoSpellCheckTest, GiCluster_GioS_GivesTone) {
    // "gios": gi+o → 'i' is consonant onset, tone 's' goes on 'o' → gió
    TypeString(*engine_, L"gios");
    EXPECT_EQ(engine_->Peek(), L"gi\x00F3");  // gió
}

TEST_F(EnglishDetectionNoSpellCheckTest, GiCluster_GioiR_GivesTone) {
    // "gioir": gi+oi → tone 'r' on 'o' → giỏi
    TypeString(*engine_, L"gioir");
    EXPECT_EQ(engine_->Peek(), L"gi\x1ECFi");  // giỏi
}

TEST_F(EnglishDetectionNoSpellCheckTest, GiCluster_GiongS_GivesTone) {
    // "giongs": gi+ong + tone 's' → gióng (sắc on plain 'o')
    // Note: "giống" (ố) requires "gioongsN" since 'oo' gives 'ô'
    TypeString(*engine_, L"giongs");
    EXPECT_EQ(engine_->Peek(), L"gi\x00F3ng");  // gióng
}

// Mixed case — uppercase G with lowercase continuation.
// Reproduces the CapsLock mid-word scenario at engine level.
TEST_F(EnglishDetectionNoSpellCheckTest, GiCluster_Giar_MixedCase_ToneOnA) {
    // "Giar": G(upper)+i+a+r → 'i' is gi-cluster consonant, tone on 'a' → Giả
    TypeString(*engine_, L"Giar");
    EXPECT_EQ(engine_->Peek(), L"Gi\x1EA3");  // Giả (not Gỉa)
}

TEST_F(EnglishDetectionNoSpellCheckTest, GiCluster_Gias_MixedCase_ToneOnA) {
    // "Gias": G(upper)+i+a+s → giá with uppercase G → Giá
    TypeString(*engine_, L"Gias");
    EXPECT_EQ(engine_->Peek(), L"Gi\x00E1");  // Giá
}

TEST_F(EnglishDetectionNoSpellCheckTest, GiCluster_GIAR_AllUpper_ToneOnA) {
    // "GIAR": all uppercase → GIẢ, tone on 'A' not 'I'
    TypeString(*engine_, L"GIAR");
    EXPECT_EQ(engine_->Peek(), L"GI\x1EA2");  // GIẢ
}

// ============================================================================
// SIMPLE TELEX TESTS
// Simple Telex: standalone 'w' is literal, 'w' after a/o/u vowel is modifier
// ============================================================================

class SimpleTelexTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::SimpleTelex;
        config_.spellCheckEnabled = false;
        config_.optimizeLevel = 0;
        engine_ = std::make_unique<TypingEngine>(config_);
    }

    TypingConfig config_;
    std::unique_ptr<TypingEngine> engine_;
};

TEST_F(SimpleTelexTest, W_Standalone_IsLiteral) {
    TypeString(*engine_, L"w");
    EXPECT_EQ(engine_->Peek(), L"w");
}

TEST_F(SimpleTelexTest, W_AfterConsonant_IsLiteral) {
    TypeString(*engine_, L"tw");
    EXPECT_EQ(engine_->Peek(), L"tw");
}

TEST_F(SimpleTelexTest, W_AfterU_IsModifier) {
    TypeString(*engine_, L"uw");
    EXPECT_EQ(engine_->Peek(), L"ư");
}

TEST_F(SimpleTelexTest, W_AfterO_IsModifier) {
    TypeString(*engine_, L"ow");
    EXPECT_EQ(engine_->Peek(), L"ơ");
}

TEST_F(SimpleTelexTest, W_AfterA_IsBruve) {
    TypeString(*engine_, L"aw");
    EXPECT_EQ(engine_->Peek(), L"ă");
}

TEST_F(SimpleTelexTest, W_InMua_IsModifier) {
    TypeString(*engine_, L"muaw");
    EXPECT_EQ(engine_->Peek(), L"mưa");
}

TEST_F(SimpleTelexTest, W_InDuoc_IsModifier) {
    TypeString(*engine_, L"dduowcj");
    EXPECT_EQ(engine_->Peek(), L"được");
}

TEST_F(SimpleTelexTest, RepeatedWModifierEscapeSurvivesEnglishWordTail) {
    for (const auto& [raw, expected] : {
             std::pair{std::wstring_view(L"dowwnload"), std::wstring_view(L"download")},
             std::pair{std::wstring_view(L"powwershell"), std::wstring_view(L"powershell")},
         }) {
        TypeString(*engine_, std::wstring(raw));
        EXPECT_EQ(engine_->Peek(), expected);
        engine_->Reset();
    }
}

TEST_F(SimpleTelexTest, W_AfterI_IsLiteral) {
    // 'i' is a vowel but not a/o/u, so 'w' should be literal
    TypeString(*engine_, L"iw");
    EXPECT_EQ(engine_->Peek(), L"iw");
}

TEST_F(SimpleTelexTest, W_AfterE_IsLiteral) {
    TypeString(*engine_, L"ew");
    EXPECT_EQ(engine_->Peek(), L"ew");
}

TEST_F(SimpleTelexTest, Circumflex_StillWorks) {
    TypeString(*engine_, L"aa");
    EXPECT_EQ(engine_->Peek(), L"â");
}

TEST_F(SimpleTelexTest, Tone_StillWorks) {
    TypeString(*engine_, L"as");
    EXPECT_EQ(engine_->Peek(), L"á");
}

TEST_F(SimpleTelexTest, DD_StillWorks) {
    TypeString(*engine_, L"dd");
    EXPECT_EQ(engine_->Peek(), L"đ");
}

TEST_F(SimpleTelexTest, RealWord_Duong) {
    TypeString(*engine_, L"dduowng");
    EXPECT_EQ(engine_->Peek(), L"đương");
}

TEST_F(SimpleTelexTest, RetypeAfterDeletingToAutoCappedInitialPreservesCase) {
    TypeString(*engine_, L"Khoong");
    ASSERT_EQ(engine_->Peek(), L"Không");

    while (engine_->Peek().size() > 1) {
        engine_->Backspace();
    }
    ASSERT_EQ(engine_->Peek(), L"K");

    TypeString(*engine_, L"hoong");
    EXPECT_EQ(engine_->Peek(), L"Không");
}

TEST_F(SimpleTelexTest, WhPrefix_IsHardEnglish) {
    // SimpleTelex keeps 'w' literal (P8 gated off), so the states-based
    // IsHardEnglishStart check is what catches wh here — no raw fallback needed.
    TypeString(*engine_, L"where");
    EXPECT_EQ(engine_->Peek(), L"where");
}

// ============================================================================
// AUTO-RESTORE TESTS
// When autoRestoreEnabled + spellCheckEnabled, invalid words return raw keys
// ============================================================================

class AutoRestoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::Telex;
        config_.spellCheckEnabled = true;
        config_.autoRestoreEnabled = true;
        config_.optimizeLevel = 0;
        config_.modernOrtho = false;
        engine_ = std::make_unique<TypingEngine>(config_);
    }

    TypingConfig config_;
    std::unique_ptr<TypingEngine> engine_;
};

TEST_F(AutoRestoreTest, InvalidWord_ReturnsRaw) {
    // "basl" → engine composes "bál" but word is invalid → return raw "basl"
    TypeString(*engine_, L"basl");
    EXPECT_EQ(engine_->Commit(), L"basl");
}

TEST_F(AutoRestoreTest, ValidWord_ReturnsComposed) {
    // "bas" → "bá" is valid Vietnamese → return composed
    TypeString(*engine_, L"bas");
    EXPECT_EQ(engine_->Commit(), L"bá");
}

TEST_F(AutoRestoreTest, NoModifications_ReturnsAsIs) {
    // "hello" → raw == composed (no Vietnamese mods) → return as-is
    TypeString(*engine_, L"hello");
    std::wstring result = engine_->Commit();
    EXPECT_EQ(result, L"hello");
}

TEST_F(AutoRestoreTest, Disabled_ReturnsComposed) {
    // autoRestore OFF → return composed even if invalid
    config_.autoRestoreEnabled = false;
    engine_ = std::make_unique<TypingEngine>(config_);

    TypeString(*engine_, L"basl");
    // With autoRestore off, returns composed text regardless
    std::wstring result = engine_->Commit();
    EXPECT_NE(result, L"basl");  // Should be composed, not raw
}

TEST_F(AutoRestoreTest, SpellCheckOff_ReturnsComposed) {
    // spellCheck OFF → no validity info, return composed
    config_.spellCheckEnabled = false;
    engine_ = std::make_unique<TypingEngine>(config_);

    TypeString(*engine_, L"basl");
    std::wstring result = engine_->Commit();
    // Without spell check, engine doesn't know it's invalid
    EXPECT_NE(result, L"basl");
}

TEST_F(AutoRestoreTest, CommitResetsState) {
    // After commit, engine state is clean
    TypeString(*engine_, L"basl");
    (void)engine_->Commit();
    EXPECT_EQ(engine_->Count(), 0u);
}

TEST_F(AutoRestoreTest, EscapedTone_NoDuplicate) {
    // "musst" → 's' applied acute, second 's' escaped → composed "must" (ASCII)
    // Auto-restore should NOT fire (composed is pure ASCII, no diacritics)
    TypeString(*engine_, L"musst");
    EXPECT_EQ(engine_->Peek(), L"must");
    EXPECT_EQ(engine_->Commit(), L"must");  // NOT "musst"
}

TEST_F(AutoRestoreTest, EscapedTone_WithDiacritics_Restores) {
    // "gôgle" has diacritics → auto-restore returns raw "google"
    TypeString(*engine_, L"google");
    EXPECT_EQ(engine_->Commit(), L"google");
}

TEST_F(AutoRestoreTest, EnglishWord_Hook_Restores) {
    // "hook" → engine composes "hôk" (oo→ô) but 'k' final after 'ô' is invalid
    // → auto-restore returns raw "hook"
    TypeString(*engine_, L"hook");
    EXPECT_EQ(engine_->Peek(), L"hôk");
    EXPECT_EQ(engine_->Commit(), L"hook");
}

TEST_F(AutoRestoreTest, EnglishWord_Book_Restores) {
    // "book" → "bôk" → invalid → restore to "book"
    TypeString(*engine_, L"book");
    EXPECT_EQ(engine_->Commit(), L"book");
}

TEST_F(AutoRestoreTest, EnglishWord_Look_Restores) {
    // "look" → "lôk" → invalid → restore to "look"
    TypeString(*engine_, L"look");
    EXPECT_EQ(engine_->Commit(), L"look");
}

TEST_F(AutoRestoreTest, EscapedTone_SpellCheckGatesInvalid) {
    // Spell check gates 'r' as literal after invalid "us" prefix.
    // u-s-s-e-r → composed "user" (all ASCII, no diacritics)
    // No auto-restore needed (raw matches composed after escape fix).
    TypeString(*engine_, L"usser");
    EXPECT_EQ(engine_->Peek(), L"user");
    EXPECT_EQ(engine_->Commit(), L"user");
}

TEST_F(AutoRestoreTest, EscapedTone_BackspaceAfterEscape) {
    // u-s-s then backspace: removes literal 's', leaves 'u' with no tone
    TypeString(*engine_, L"uss");
    EXPECT_EQ(engine_->Peek(), L"us");
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"u");
}

// TEST_F(AutoRestoreTest, BackspaceCircumflex_RawInSync) {
//     // "windoo[BS]ows" — circumflex 'oo' adds to rawInput_ without new state,
//     // backspace must trim rawInput_ correctly so auto-restore gives "windows"
//     TypeString(*engine_, L"windoo");
//     engine_->Backspace();  // removes 'ô', trims raw to "wind" (not "windo")
//     TypeString(*engine_, L"ows");
//     // Composed has ư from first 'w' → has diacritics → auto-restore fires
//     EXPECT_EQ(engine_->Commit(), L"windows");
// }

TEST_F(AutoRestoreTest, DiacriticsInvalid_Restores) {
    // "basl" → composed "bál" has diacritics + invalid → return raw "basl"
    engine_->Reset();
    TypeString(*engine_, L"basl");
    EXPECT_EQ(engine_->Commit(), L"basl");
}

TEST_F(AutoRestoreTest, FinalK_DakLak_Valid) {
    // "ddawsk" → "Đắk" — 'k' is a valid final consonant (minority-language proper nouns)
    // Should return composed text, NOT raw restore
    engine_->Reset();
    TypeString(*engine_, L"ddawsk");
    EXPECT_EQ(engine_->Commit(), L"đắk");
}

TEST_F(AutoRestoreTest, FinalK_Lak_Valid) {
    // "lawsk" → "lắk"
    engine_->Reset();
    TypeString(*engine_, L"lawsk");
    EXPECT_EQ(engine_->Commit(), L"lắk");
}

TEST_F(AutoRestoreTest, ValidPrefix_AtCommit_Restores) {
    // "user" → 's' becomes tone on 'u', 'r' becomes tone on 'e' → "úẻ"
    // During typing: ValidPrefix (could add modifier later)
    // At commit: incomplete word → auto-restore to "user"
    TypeString(*engine_, L"user");
    EXPECT_EQ(engine_->Commit(), L"user");
}

TEST_F(AutoRestoreTest, ValidPrefix_AtCommit_English) {
    // "enter" → 'r' becomes tone on 'e' → "ẻnte" + ...
    // English word should be restored
    TypeString(*engine_, L"enter");
    EXPECT_EQ(engine_->Commit(), L"enter");
}

TEST_F(AutoRestoreTest, FreeCircumflex_ChuanAR_ProducesChuẩn) {
    // "chuanar": "chuan" has vowel "ua" (canEnd=false), but "uâ" (canEnd=true)
    // allows 'n' as final consonant → spell check should stay ValidPrefix,
    // allowing free-marking circumflex (second 'a') and tone 'r' (hỏi)
    TypeString(*engine_, L"chuanar");
    EXPECT_EQ(engine_->Commit(), L"chuẩn");
}

TEST_F(AutoRestoreTest, FreeCircumflex_TuanAF_ProducesTuần) {
    // Similar case: "tuanaf" → "tuần"
    TypeString(*engine_, L"tuanaf");
    EXPECT_EQ(engine_->Commit(), L"tuần");
}

TEST_F(AutoRestoreTest, FreeCircumflex_LuatAJ_ProducesLuật) {
    // "luataj" → "luật"
    TypeString(*engine_, L"luataj");
    EXPECT_EQ(engine_->Commit(), L"luật");
}

// --- Cross-vowel free marking: circumflex across intervening vowels ---

TEST_F(AutoRestoreTest, FreeCircumflex_CrossVowel_Chieue_Chiêu) {
    // "chieue" → 'e' crosses 'u' to circumflex first 'e' → "chiêu"
    TypeString(*engine_, L"chieue");
    EXPECT_EQ(engine_->Peek(), L"chiêu");
}

TEST_F(AutoRestoreTest, FreeCircumflex_CrossVowel_Chieuef_Chiều) {
    // "chieuef" → circumflex + tone → "chiều"
    TypeString(*engine_, L"chieuef");
    EXPECT_EQ(engine_->Commit(), L"chiều");
}

TEST_F(AutoRestoreTest, FreeCircumflex_CrossVowel_Caua_Câu) {
    // "caua" → 'a' crosses 'u' to circumflex first 'a' → "câu"
    TypeString(*engine_, L"caua");
    EXPECT_EQ(engine_->Peek(), L"câu");
}

TEST_F(AutoRestoreTest, FreeCircumflex_CrossVowel_Cauas_Cấu) {
    // "cauas" → circumflex + tone → "cấu"
    TypeString(*engine_, L"cauas");
    EXPECT_EQ(engine_->Commit(), L"cấu");
}

TEST_F(AutoRestoreTest, FreeCircumflex_CrossVowel_Maya_Mây) {
    // "maya" → 'a' crosses 'y' to circumflex first 'a' → "mây"
    TypeString(*engine_, L"maya");
    EXPECT_EQ(engine_->Peek(), L"mây");
}

TEST_F(AutoRestoreTest, FreeCircumflex_CrossVowel_Mayas_Mấy) {
    // "mayas" → circumflex + tone → "mấy"
    TypeString(*engine_, L"mayas");
    EXPECT_EQ(engine_->Commit(), L"mấy");
}

TEST_F(AutoRestoreTest, FreeCircumflex_CrossVowel_Invalid_Oao) {
    // "oao" → circumflex would give "ôao" (invalid) → spell check rejects → no circumflex
    TypeString(*engine_, L"oao");
    EXPECT_EQ(engine_->Peek(), L"oao");
}

// --- Delayed circumflex VCV: same vowel across single consonant ---
// Pattern V+C+V where both V are same → circumflex on first V, consume second V
// Gõ Nhanh calls this "DelayedCircumflex" — distinct from adjacent doubling (aa→â)

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Toto_Tot) {
    // "toto" → 'o' crosses 't' to circumflex first 'o' → "tôt"
    TypeString(*engine_, L"toto");
    EXPECT_EQ(engine_->Peek(), L"tôt");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Totos_Tot_Sac) {
    // "totos" → circumflex + sắc → "tốt"
    TypeString(*engine_, L"totos");
    EXPECT_EQ(engine_->Commit(), L"tốt");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Onro_On) {
    // "onro" → 'o' crosses 'n' to circumflex, 'r' applies hỏi → "ổn"
    TypeString(*engine_, L"onro");
    EXPECT_EQ(engine_->Peek(), L"ổn");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Ata_At) {
    // "ata" → 'a' crosses 't' to circumflex first 'a' → "ât"
    TypeString(*engine_, L"ata");
    EXPECT_EQ(engine_->Peek(), L"ât");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Xepe_Xep) {
    // "xepe" → 'e' crosses 'p' to circumflex → "xêp"
    TypeString(*engine_, L"xepe");
    EXPECT_EQ(engine_->Peek(), L"xêp");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Xepes_Xep_Sac) {
    // "xepes" → circumflex + sắc → "xếp"
    TypeString(*engine_, L"xepes");
    EXPECT_EQ(engine_->Commit(), L"xếp");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Apas_Ap_Sac) {
    // "apas" → delayed circumflex (apa→âp) + sắc on 's' → "ấp"
    // Regression: English block "pas" suffix used to catch this — now prefix-only.
    TypeString(*engine_, L"apas");
    EXPECT_EQ(engine_->Peek(), L"ấp");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Epas_Ep_Sac) {
    // "epes" → delayed circumflex (epe→êp) + sắc → "ếp"
    TypeString(*engine_, L"epes");
    EXPECT_EQ(engine_->Peek(), L"ếp");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Opos_Op_Sac) {
    // "opos" → delayed circumflex (opo→ôp) + sắc → "ốp"
    TypeString(*engine_, L"opos");
    EXPECT_EQ(engine_->Peek(), L"ốp");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_NotTriggered_DiffVowel) {
    // "tote" → different vowels (o vs e) → NOT delayed circumflex, just literal
    TypeString(*engine_, L"tote");
    EXPECT_EQ(engine_->Peek(), L"tote");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_NotTriggered_InvalidCoda) {
    // "oho" → 'h' is not a valid Vietnamese coda → should NOT apply circumflex
    // (without spell check, coda validation rejects 'h')
    TypeString(*engine_, L"oho");
    EXPECT_EQ(engine_->Peek(), L"oho");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Revert_Totoo) {
    // "totoo" → first "toto" gives "tôt", then third 'o' reverts circumflex → "toto"
    TypeString(*engine_, L"totoo");
    EXPECT_EQ(engine_->Peek(), L"toto");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_NoReTriger_Totooo) {
    // "totooo" → after revert, fourth 'o' should NOT re-apply circumflex → "totoo"
    TypeString(*engine_, L"totooo");
    EXPECT_EQ(engine_->Peek(), L"totoo");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Commit_Toto_RestoresEnglish) {
    // "toto" + space → "tôt" is valid VN, should commit as "tôt" (not auto-restore)
    TypeString(*engine_, L"toto");
    EXPECT_EQ(engine_->Commit(), L"tôt");
}

TEST_F(AutoRestoreTest, DelayedCircumflex_VCV_Commit_Xepe_RestoresEnglish) {
    // "xepe" + space → "xêp" is valid VN, should commit as "xêp"
    TypeString(*engine_, L"xepe");
    EXPECT_EQ(engine_->Commit(), L"xêp");
}

// ============================================================================
// QUICK CONSONANT + AUTO-RESTORE TESTS
// ============================================================================

TEST_F(AutoRestoreTest, QuickConsonant_KK_Khuya_Valid) {
    // "kkuya" → kk→kh, so "khuya" — valid Vietnamese word
    // Should NOT auto-restore to "kkuya"
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"kkuya");
    EXPECT_EQ(engine_->Commit(), L"khuya");
}

TEST_F(AutoRestoreTest, QuickConsonant_TT_Thuy_Valid) {
    // "ttuys" → tt→th, "thuys" → "thúy" (classic: no coda → tone on first)
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"ttuys");
    EXPECT_EQ(engine_->Commit(), L"thúy");
}

TEST_F(AutoRestoreTest, QuickConsonant_GG_Alone_Restores) {
    // "gg" alone → "gi" composed, but invalid on commit → restore to "gg"
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"gg");
    EXPECT_EQ(engine_->Commit(), L"gg");
}

TEST_F(AutoRestoreTest, QuickConsonant_CC_Alone_Restores) {
    // "cc" alone → "ch" composed, invalid on commit → restore to "cc"
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"cc");
    EXPECT_EQ(engine_->Commit(), L"cc");
}

TEST_F(AutoRestoreTest, QuickConsonant_UU_Alone_Restores) {
    // "uu" alone → "ươ" composed, invalid on commit → restore to "uu"
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"uu");
    EXPECT_EQ(engine_->Commit(), L"uu");
}

TEST_F(AutoRestoreTest, QuickConsonant_GG_Alone_Restores_NoSpellCheck) {
    // "gg" alone → "gi" composed, restore even with spell check disabled
    config_.quickConsonant = true;
    config_.spellCheckEnabled = false;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"gg");
    EXPECT_EQ(engine_->Commit(), L"gg");
}

TEST_F(AutoRestoreTest, QuickConsonant_UU_Alone_Restores_NoSpellCheck) {
    // "uu" alone → "ươ" composed, restore even with spell check disabled
    config_.quickConsonant = true;
    config_.spellCheckEnabled = false;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"uu");
    EXPECT_EQ(engine_->Commit(), L"uu");
}

TEST_F(AutoRestoreTest, QuickConsonant_GG_WithVowel_Keeps) {
    // "ggia" → "gia" — valid Vietnamese word, keep composed
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"gga");
    EXPECT_EQ(engine_->Commit(), L"gia");
}

TEST_F(AutoRestoreTest, QuickConsonant_PP_Backspace_Escape) {
    // "app" → "aph"; backspace now restores in-place → "app" (not "ap")
    // The escape flag still prevents the next 'p' from re-triggering quick consonant.
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"app");
    EXPECT_EQ(engine_->Peek(), L"aph");
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"app");   // restored in-place
    engine_->PushChar(L'p');               // escape consumed → literal 'p', no re-trigger
    EXPECT_EQ(engine_->Peek(), L"appp");
}

TEST_F(AutoRestoreTest, QuickConsonant_GG_Backspace_Escape) {
    // "agg" → "agi"; backspace restores in-place → "agg"
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"agg");
    EXPECT_EQ(engine_->Peek(), L"agi");
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"agg");   // restored in-place
    engine_->PushChar(L'g');               // escape consumed, literal 'g' added
    EXPECT_EQ(engine_->Peek(), L"aggg");
}

TEST_F(AutoRestoreTest, QuickConsonant_UU_Backspace_Escape) {
    // "uu" → "ươ"; backspace restores in-place → "uu"
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"uu");
    EXPECT_EQ(engine_->Peek(), L"ươ");
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"uu");    // restored in-place
    engine_->PushChar(L'u');               // escape consumed, literal 'u', no uu→ươ
    EXPECT_EQ(engine_->Peek(), L"uuu");
}

TEST_F(AutoRestoreTest, QuickConsonant_UUUU_NoConsecutiveRetrigger) {
    // "uuuu" → uu→ươ, then uu should NOT re-trigger → "ươuu"
    config_.quickConsonant = true;
    config_.spellCheckEnabled = false;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"uuuu");
    EXPECT_EQ(engine_->Peek(), L"ươuu");
}

TEST_F(AutoRestoreTest, QuickConsonant_CCCC_NoConsecutiveRetrigger) {
    // "cccc" → cc→ch, then cc should NOT re-trigger → "chcc"
    config_.quickConsonant = true;
    config_.spellCheckEnabled = false;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"cccc");
    EXPECT_EQ(engine_->Peek(), L"chcc");
}

TEST_F(AutoRestoreTest, QuickConsonant_UU_DifferentChar_ReAllows) {
    // "uuauuu" → uu→ươ, a (clears suppression), uu→ươ, u → "ươaươu"
    config_.quickConsonant = true;
    config_.spellCheckEnabled = false;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"uuauuu");
    EXPECT_EQ(engine_->Peek(), L"ươaươu");
}

TEST_F(AutoRestoreTest, QuickConsonant_CC_DifferentChar_ReAllows) {
    // "ccaccc" → cc→ch, a (clears suppression), cc→ch, c (suppressed) → "chachc"
    // Note: after "cha", next "cc" fires as quick consonant again, then last c is suppressed
    config_.quickConsonant = true;
    config_.spellCheckEnabled = false;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"ccaccc");
    EXPECT_EQ(engine_->Peek(), L"chachc");
}

// --- Bug regression: quick consonant in word body must not be auto-restored ---
// Reported: "rienn" typed while quickConsonant + autoRestore both on
// → AutoRestore was reverting "rieng" back to "rienn" (wrong)

TEST_F(AutoRestoreTest, QuickConsonant_NN_Word_NotRestored) {
    // Bug: "rienn" → "rieng" (nn→ng), commit should keep "rieng", NOT revert to "rienn"
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"rienn");
    EXPECT_EQ(engine_->Commit(), L"rieng");
}

TEST_F(AutoRestoreTest, QuickConsonant_PP_Word_NotRestored) {
    // "ppuong" → "phuong" — not a standard tone-marked word but user chose it
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"ppuong");
    EXPECT_EQ(engine_->Commit(), L"phuong");
}

TEST_F(AutoRestoreTest, QuickConsonant_CC_Word_NotRestored) {
    // "ccuong" → "chuong" — same pattern
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"ccuong");
    EXPECT_EQ(engine_->Commit(), L"chuong");
}

TEST_F(AutoRestoreTest, QuickConsonant_NN_Backspace_ThenCommit_Restores) {
    // "rienn" → "rieng", user presses backspace (rejecting ng→nn), then spaces
    // After backspace quick consonant is cleared → next commit of "rien" is fine
    // This validates backspace properly clears the active quick consonant guard
    config_.quickConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"rienn");            // → "rieng"
    EXPECT_EQ(engine_->Peek(), L"rieng");
    engine_->Backspace();                       // revert: "rieng" → "rien"
    EXPECT_EQ(engine_->Peek(), L"rienn");
    // quickConsonantIdx_ is now SIZE_MAX → auto-restore allowed again
    // "rien" = ValidPrefix → should restore to raw "rien"
    EXPECT_EQ(engine_->Commit(), L"rienn");
}

// ============================================================================
// QUICK START CONSONANT TESTS (f→ph, j→gi, w→qu at word start)
// ============================================================================

TEST_F(AutoRestoreTest, QuickStartConsonant_F_ProducesPh) {
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    engine_->PushChar(L'f');
    EXPECT_EQ(engine_->Peek(), L"ph");
}

TEST_F(AutoRestoreTest, QuickStartConsonant_F_Backspace_RestoresF) {
    // f→ph, backspace→f (not p)
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    engine_->PushChar(L'f');
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"f");
}

TEST_F(AutoRestoreTest, QuickStartConsonant_F_Vowel_KeepsExpansion) {
    // f+a → pha (expansion confirmed by vowel)
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"fa");
    EXPECT_EQ(engine_->Peek(), L"pha");
}

TEST_F(AutoRestoreTest, QuickStartConsonant_F_Consonant_UndoesExpansion) {
    // f+t → ft (not pht, expansion undone by non-vowel)
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"ft");
    EXPECT_EQ(engine_->Peek(), L"ft");
}

TEST_F(AutoRestoreTest, QuickStartConsonant_J_Backspace_RestoresJ) {
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    engine_->PushChar(L'j');
    EXPECT_EQ(engine_->Peek(), L"gi");
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"j");
}

TEST_F(AutoRestoreTest, QuickStartConsonant_W_Backspace_RestoresW) {
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    engine_->PushChar(L'w');
    EXPECT_EQ(engine_->Peek(), L"qu");
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"w");
}

TEST_F(AutoRestoreTest, QuickStartConsonant_J_Consonant_UndoesExpansion) {
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"jt");
    EXPECT_EQ(engine_->Peek(), L"jt");
}

TEST_F(AutoRestoreTest, QuickStartConsonant_W_Consonant_UndoesExpansion) {
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"wt");
    EXPECT_EQ(engine_->Peek(), L"wt");
}

TEST_F(AutoRestoreTest, QuickStartConsonant_F_UpperCase_Backspace) {
    // F→Ph, backspace→F
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    engine_->PushChar(L'F');
    EXPECT_EQ(engine_->Peek(), L"Ph");
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"F");
}

TEST_F(AutoRestoreTest, QuickStartConsonant_F_Vowel_Backspace_KeepsPh) {
    // f+a→pha, backspace→ph (expansion already confirmed, not undone)
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"fa");
    engine_->Backspace();
    EXPECT_EQ(engine_->Peek(), L"ph");
}

TEST_F(AutoRestoreTest, QuickStartConsonant_F_FullWord_Phan) {
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"fan");
    EXPECT_EQ(engine_->Commit(), L"phan");
}

// ============================================================================
// STROKE-D ABBREVIATION TESTS (đ + consonant should not auto-restore)
// ============================================================================

TEST_F(AutoRestoreTest, StrokeD_Abbreviation_DT_KeepsComposed) {
    // "ddt" → "đt" — abbreviation for "điện thoại"
    // đ + consonant only → heuristic keeps composed
    TypeString(*engine_, L"ddt");
    EXPECT_EQ(engine_->Commit(), L"đt");
}

TEST_F(AutoRestoreTest, StrokeD_Abbreviation_DH_KeepsComposed) {
    // "ddh" → "đh" — abbreviation for "đại học"
    TypeString(*engine_, L"ddh");
    EXPECT_EQ(engine_->Commit(), L"đh");
}

TEST_F(AutoRestoreTest, StrokeD_ValidWord_StillComposed) {
    // "ddeef" → "đề" — valid Vietnamese syllable with đ
    // Should still return composed (was already working)
    TypeString(*engine_, L"ddeef");
    EXPECT_EQ(engine_->Commit(), L"đề");
}

TEST_F(AutoRestoreTest, StrokeD_NonConsecutive_Download_Restores) {
    // "download" — non-consecutive d's, auto-restore should return "download"
    TypeString(*engine_, L"download");
    EXPECT_EQ(engine_->Commit(), L"download");
}

TEST_F(AutoRestoreTest, StrokeD_NonInitial_Add_Restores) {
    // "add" — dd is non-initial (index 1), đ can only be at index 0,
    // so the second 'd' is literal → "add" auto-restores
    TypeString(*engine_, L"add");
    EXPECT_EQ(engine_->Commit(), L"add");
}

TEST_F(AutoRestoreTest, StrokeD_DdwaSimpleTelex_Restores) {
    // "ddwa" with Simple Telex: dd→đ, w=literal (no vowel context), a=literal
    // → "đwa" invalid → auto-restore to "ddwa"
    config_.inputMethod = InputMethod::SimpleTelex;
    engine_ = std::make_unique<TypingEngine>(config_);
    TypeString(*engine_, L"ddwa");
    EXPECT_EQ(engine_->Commit(), L"ddwa");
}

TEST_F(AutoRestoreTest, StrokeD_Ddp_KeepsAbbreviation) {
    // "ddp" → "đp" — đ + consonant only → treated as abbreviation
    TypeString(*engine_, L"ddp");
    EXPECT_EQ(engine_->Commit(), L"đp");
}

TEST_F(AutoRestoreTest, StrokeD_Awndd_ProducesAbbreviation) {
    // "awndd" → breve 'a' + 'n' + dd→đ bypass fires (d after consonant 'n')
    // → "ănđ". HasIntentionalStrokeD keeps it (no plain vowels after đ).
    TypeString(*engine_, L"awndd");
    EXPECT_EQ(engine_->Peek(), L"ănđ");
    EXPECT_EQ(engine_->Commit(), L"ănđ");
}

// ============================================================================
// DD→Đ ABBREVIATION BYPASS TESTS
// dd→đ bypasses spellCheckDisabled_ when 'd' follows a consonant (not vowel).
// FindStrokeDTarget already blocks vowel-preceded 'd' (e.g., "add").
// HasIntentionalStrokeD at commit protects abbreviations (đ + consonant only).
// ============================================================================

TEST_F(AutoRestoreTest, StrokeD_Bypass_HD_NoExclusion) {
    // "hdd" → "hđ" without needing exclusion entry. HasIntentionalStrokeD keeps it.
    TypeString(*engine_, L"hdd");
    EXPECT_EQ(engine_->Peek(), L"hđ");
    EXPECT_EQ(engine_->Commit(), L"hđ");
}

TEST_F(AutoRestoreTest, StrokeD_Bypass_SDT_NoExclusion) {
    // "sddt" → "sđt" — abbreviation for "số điện thoại"
    TypeString(*engine_, L"sddt");
    EXPECT_EQ(engine_->Peek(), L"sđt");
    EXPECT_EQ(engine_->Commit(), L"sđt");
}

TEST_F(AutoRestoreTest, StrokeD_Bypass_TDN_NoExclusion) {
    // "tddn" → "tđn" — abbreviation for "tên đệm ngắn"
    TypeString(*engine_, L"tddn");
    EXPECT_EQ(engine_->Peek(), L"tđn");
    EXPECT_EQ(engine_->Commit(), L"tđn");
}

TEST_F(AutoRestoreTest, StrokeD_Bypass_Add_StillBlocked) {
    // "add" → dd after vowel 'a' → FindStrokeDTarget returns SIZE_MAX → no bypass
    TypeString(*engine_, L"add");
    EXPECT_EQ(engine_->Peek(), L"add");
    EXPECT_EQ(engine_->Commit(), L"add");
}

TEST_F(AutoRestoreTest, StrokeD_Bypass_Odd_StillBlocked) {
    // "odd" → dd after vowel 'o' → no bypass
    TypeString(*engine_, L"odd");
    EXPECT_EQ(engine_->Peek(), L"odd");
    EXPECT_EQ(engine_->Commit(), L"odd");
}

TEST_F(AutoRestoreTest, StrokeD_Bypass_VNI_HD9) {
    // VNI: "hd9" → "hđ" — same bypass for VNI stroke key '9'
    TypingConfig cfg;
    cfg.inputMethod = InputMethod::VNI;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    TypingEngine eng(cfg);
    TypeString(eng, L"hd9");
    EXPECT_EQ(eng.Peek(), L"hđ");
    EXPECT_EQ(eng.Commit(), L"hđ");
}

// Capitalization preservation during auto-restore (backspace-revive scenario).
// SeedFromText decomposes Vietnamese chars into lowercase bases + isUpper flag,
// so rawInput_ contains lowercase. Without the fix, auto-restore returns the
// lowercase raw input, destroying the user's capitalization.

TEST_F(AutoRestoreTest, SeedFromText_UppercasePreserved_VKey) {
    // Simulate backspace-revive: SeedFromText("VKey") → rawInput_ = "vkey"
    // but composed = "VKey". Since raw=="VKey" after cap fix, skip restore.
    EXPECT_TRUE(engine_->SeedFromText(L"VKey"));
    // "VKey" is not a valid Vietnamese word, but after capitalizing raw[0]
    // to match composed[0], raw == composed → return composed unchanged.
    EXPECT_EQ(engine_->Commit(), L"VKey");
}

TEST_F(AutoRestoreTest, SeedFromText_LowercaseUnaffected) {
    // Lowercase input: SeedFromText("vkey") → rawInput_ = "vkey",
    // composed = "vkey". raw == composed → no restore, return as-is.
    EXPECT_TRUE(engine_->SeedFromText(L"vkey"));
    EXPECT_EQ(engine_->Commit(), L"vkey");
}

TEST_F(AutoRestoreTest, SeedFromText_UppercasePreserved_Facebook) {
    // "Facebook" → after seed, raw = "facebook", composed = "Facebook".
    // After cap fix, raw = "Facebook" == composed → return composed.
    EXPECT_TRUE(engine_->SeedFromText(L"Facebook"));
    EXPECT_EQ(engine_->Commit(), L"Facebook");
}

// ============================================================================
// SPELL CHECK EXCLUSION LIST TESTS
// Prefix-based exclusion: "hđ" in list → "hđ", "hđt" bypass spell check
// ============================================================================

class SpellExclusionTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::Telex;
        config_.spellCheckEnabled = true;
        config_.autoRestoreEnabled = true;
        config_.optimizeLevel = 0;
        config_.spellExclusions = {L"hđ", L"đp"};
        engine_ = std::make_unique<TypingEngine>(config_);
    }

    TypingConfig config_;
    std::unique_ptr<TypingEngine> engine_;
};

TEST_F(SpellExclusionTest, ExcludedPattern_NoAutoRestore) {
    // "hdd" → "hđ" (dd→đ). "hđ" is in exclusion list → no auto-restore
    TypeString(*engine_, L"hdd");
    EXPECT_EQ(engine_->Peek(), L"hđ");
    EXPECT_EQ(engine_->Commit(), L"hđ");
}

TEST_F(SpellExclusionTest, ExcludedPrefix_CoversDerivedWords) {
    // "hđ" in exclusion list → "hđt" also bypasses (prefix match)
    TypeString(*engine_, L"hddt");
    EXPECT_EQ(engine_->Peek(), L"hđt");
    EXPECT_EQ(engine_->Commit(), L"hđt");
}

TEST_F(SpellExclusionTest, NonExcluded_AutoRestores) {
    // "hhdd" → "hhđ": dd bypass fires (d after consonant h). HasIntentionalStrokeD
    // keeps "hhđ" (no plain vowels after đ). Exclusion "hđ" doesn't prefix-match
    // "hhđ", but the abbreviation heuristic is the primary safety net here.
    TypeString(*engine_, L"hhdd");
    EXPECT_EQ(engine_->Peek(), L"hhđ");
    EXPECT_EQ(engine_->Commit(), L"hhđ");
}

TEST_F(SpellExclusionTest, SecondExclusion_Works) {
    // "đp" is in exclusion list → "ddp" → "đp" stays
    TypeString(*engine_, L"ddp");
    EXPECT_EQ(engine_->Peek(), L"đp");
    EXPECT_EQ(engine_->Commit(), L"đp");
}

// issue #200: a Đ-initial chain the first-char toggle would otherwise clobber
// (ddcddt → dcđt) is recoverable by adding the word to spell exclusions. The
// leading-Đ un-stroke is skipped while the buffer still prefix-matches "đcđt".
TEST_F(SpellExclusionTest, LeadingDdChain_PreservedByExclusion) {
    TypingConfig cfg;
    cfg.inputMethod = InputMethod::Telex;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.spellExclusions = {L"đcđt"};
    TypingEngine eng(cfg);
    TypeString(eng, L"ddcddt");
    EXPECT_EQ(eng.Peek(), L"đcđt");
}

TEST_F(SpellExclusionTest, LeadingDdToggle_StillTogglesWhenNotExcluded) {
    // With an unrelated exclusion present, "dmdd" still toggles normally → dmd.
    TypingConfig cfg;
    cfg.inputMethod = InputMethod::Telex;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.spellExclusions = {L"đcđt"};
    TypingEngine eng(cfg);
    TypeString(eng, L"dmdd");
    EXPECT_EQ(eng.Peek(), L"dmd");
}

TEST_F(SpellExclusionTest, EmptyExclusionList_NormalBehavior) {
    // No exclusions → dd→đ bypass fires (d after consonant), HasIntentionalStrokeD
    // keeps "hđ" at commit time (no plain vowels after đ → abbreviation heuristic)
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    TypingEngine eng(cfg);
    TypeString(eng, L"hdd");
    EXPECT_EQ(eng.Peek(), L"hđ");
    EXPECT_EQ(eng.Commit(), L"hđ");
}

TEST_F(SpellExclusionTest, CaseInsensitive) {
    // Exclusion "hđ" should also match "Hđ" (uppercase H)
    engine_->Reset();
    engine_->PushChar(L'H');
    engine_->PushChar(L'd');
    engine_->PushChar(L'd');
    EXPECT_EQ(engine_->Peek(), L"Hđ");
    EXPECT_EQ(engine_->Commit(), L"Hđ");
}

TEST_F(SpellExclusionTest, SingleCharExclusion_Ignored) {
    // Exclusion entries < 2 chars should be ignored, but dd→đ bypass still fires
    // (d after consonant h → abbreviation). HasIntentionalStrokeD keeps "hđ".
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.spellExclusions = {L"đ"};  // Too short, should be ignored
    TypingEngine eng(cfg);
    TypeString(eng, L"hdd");
    EXPECT_EQ(eng.Peek(), L"hđ");
    EXPECT_EQ(eng.Commit(), L"hđ");
}

// PLHĐ (#221): vowel-less abbreviation chains compose WITHOUT an exclusion —
// the pl- hard-onset block is lifted when the buffer has no vowel (English
// words always carry one). Supersedes the pre-#221 "blocked unless excluded"
// design; mirrors the Rust engine rule.
TEST_F(SpellExclusionTest, PLHDD_NoExclusion_Composes) {
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    TypingEngine eng(cfg);
    TypeString(eng, L"plhdd");
    EXPECT_EQ(eng.Peek(), L"plhđ");
}

TEST_F(SpellExclusionTest, PLHDD_ComposesRegardlessOfExclusions) {
    // Pre-#221 this asserted the precise-bypass block ("hđ" doesn't prefix-match
    // "plhđ"). The vowel-less chain rule now composes independent of exclusions.
    TypeString(*engine_, L"plhdd");
    EXPECT_EQ(engine_->Peek(), L"plhđ");
    EXPECT_EQ(engine_->Commit(), L"plhđ");
}

TEST_F(SpellExclusionTest, Dropdown_ExclHD_NotBypassed) {
    // "hđ" exclusion should NOT make "dropdown" apply dd→đ
    // (FindStrokeDTarget finds 'd' at index 0, stroke → "đrop" doesn't match "hđ")
    TypeString(*engine_, L"dropdown");
    EXPECT_EQ(engine_->Peek(), L"dropdown");
    EXPECT_EQ(engine_->Commit(), L"dropdown");
}

TEST_F(SpellExclusionTest, PLHDD_ExclPLHD_Works) {
    // "plhđ" in exclusion list → dd→đ applies AND prefix matches → kept
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.spellExclusions = {L"plhđ"};
    TypingEngine eng(cfg);
    TypeString(eng, L"plhdd");
    EXPECT_EQ(eng.Peek(), L"plhđ");
    EXPECT_EQ(eng.Commit(), L"plhđ");
}

// Issue #75: Tone bypass via spell exclusion (e.g. "kà" for Kà Tum)
// Spell check blocks "ka" (k before non-front vowel) → tone key becomes literal.
// With exclusion "kà", applying grave tone tentatively matches → tone allowed through.

TEST_F(SpellExclusionTest, ToneBypass_KaGrave_ExactMatch) {
    // Exclusion "kà" → typing "kaf" should produce "kà" (grave on 'a')
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.spellExclusions = {L"kà"};
    TypingEngine eng(cfg);
    TypeString(eng, L"kaf");
    EXPECT_EQ(eng.Peek(), L"kà");
    EXPECT_EQ(eng.Commit(), L"kà");
}

TEST_F(SpellExclusionTest, ToneBypass_KaAcute_NotBypassed) {
    // Exclusion "kà" should NOT allow "ká" (different tone)
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.spellExclusions = {L"kà"};
    TypingEngine eng(cfg);
    TypeString(eng, L"kas");
    EXPECT_EQ(eng.Peek(), L"kas");  // 's' treated as literal
}

TEST_F(SpellExclusionTest, ToneBypass_CaseInsensitive) {
    // Exclusion "kà" should also allow "Kà" (uppercase K)
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.spellExclusions = {L"kà"};
    TypingEngine eng(cfg);
    eng.PushChar(L'K');
    eng.PushChar(L'a');
    eng.PushChar(L'f');
    EXPECT_EQ(eng.Peek(), L"Kà");
    EXPECT_EQ(eng.Commit(), L"Kà");
}

TEST_F(SpellExclusionTest, ToneBypass_NoExclusion_Blocked) {
    // Without exclusion, "kaf" → literal "kaf" (spell check blocks tone)
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    TypingEngine eng(cfg);
    TypeString(eng, L"kaf");
    EXPECT_EQ(eng.Peek(), L"kaf");
}

// Issue #78: Modifier bypass via spell exclusion when allowZwjf=false.
// z/w/j/f as initial → spellCheckDisabled_ blocks modifiers → exclusion can never match.
// Fix: allow modifier through (tone-tolerant match) when result heads toward an exclusion.

TEST_F(SpellExclusionTest, ZwjfOff_CircumflexBypass) {
    // Exclusion "zô", allowZwjf=false → "zoo" should produce "zô"
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.allowZwjf = false;
    cfg.spellExclusions = {L"zô"};
    TypingEngine eng(cfg);
    TypeString(eng, L"zoo");
    EXPECT_EQ(eng.Peek(), L"zô");
    EXPECT_EQ(eng.Commit(), L"zô");
}

TEST_F(SpellExclusionTest, ZwjfOff_BreveBypass_FullWord) {
    // Exclusion "zắc", allowZwjf=false → "zawsc" should produce "zắc"
    // Modifier bypass (tone-tolerant: 'ă' matches 'ắ') lets breve through,
    // then existing tone bypass lets acute through.
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.allowZwjf = false;
    cfg.spellExclusions = {L"zắc"};
    TypingEngine eng(cfg);
    TypeString(eng, L"zawsc");
    EXPECT_EQ(eng.Peek(), L"zắc");
    EXPECT_EQ(eng.Commit(), L"zắc");
}

TEST_F(SpellExclusionTest, ZwjfOff_HornBypass) {
    // Exclusion "fư", allowZwjf=false → "fuw" should produce "fư"
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.allowZwjf = false;
    cfg.spellExclusions = {L"fư"};
    TypingEngine eng(cfg);
    TypeString(eng, L"fuw");
    EXPECT_EQ(eng.Peek(), L"fư");
    EXPECT_EQ(eng.Commit(), L"fư");
}

TEST_F(SpellExclusionTest, ZwjfOff_NoExclusion_StillBlocked) {
    // No matching exclusion → modifier still blocked
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.allowZwjf = false;
    TypingEngine eng(cfg);
    TypeString(eng, L"zoo");
    EXPECT_EQ(eng.Peek(), L"zoo");
}

TEST_F(SpellExclusionTest, ZwjfOff_WrongExclusion_Blocked) {
    // Exclusion "zô" but typing "foo" → prefix mismatch → blocked
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.allowZwjf = false;
    cfg.spellExclusions = {L"zô"};
    TypingEngine eng(cfg);
    TypeString(eng, L"foo");
    EXPECT_EQ(eng.Peek(), L"foo");
}

TEST_F(SpellExclusionTest, ZwjfOff_CaseInsensitive) {
    // Exclusion "zô" should also allow "Zô" (uppercase Z)
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    cfg.allowZwjf = false;
    cfg.spellExclusions = {L"zô"};
    TypingEngine eng(cfg);
    eng.PushChar(L'Z');
    eng.PushChar(L'o');
    eng.PushChar(L'o');
    EXPECT_EQ(eng.Peek(), L"Zô");
    EXPECT_EQ(eng.Commit(), L"Zô");
}

TEST_F(SpellExclusionTest, SpellCheck_Rose_RestoresToRose) {
    TypingConfig cfg;
    cfg.spellCheckEnabled = true;
    cfg.autoRestoreEnabled = true;
    TypingEngine eng(cfg);
    TypeString(eng, L"rose");
    EXPECT_EQ(eng.Peek(), L"roé");
    EXPECT_EQ(eng.Commit(), L"rose");
}

// =============================================================================
// Non-initial dd→đ tests (abbreviation support)
// =============================================================================

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_NonInitial_AfterConsonant) {
    // "vddeef": v + dd→đ + ee→ê + f→grave = "vđề"
    // Abbreviation for "vấn đề" — dd after consonant 'v' should apply stroke
    TypeString(*engine_, L"vddeef");
    EXPECT_EQ(engine_->Peek(), L"vđề");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_NonInitial_CConsonant) {
    // "cddeef": c + dd→đ + ee→ê + f→grave = "cđề"
    TypeString(*engine_, L"cddeef");
    EXPECT_EQ(engine_->Peek(), L"cđề");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_NonInitial_BlockedAfterVowel) {
    // "added": a + dd should NOT become đ (preceded by vowel 'a')
    TypeString(*engine_, L"added");
    EXPECT_EQ(engine_->Peek(), L"added");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_NonInitial_BlockedAfterVowelU) {
    // "uddone": u + dd should NOT become đ (preceded by vowel 'u')
    TypeString(*engine_, L"uddi");
    EXPECT_EQ(engine_->Peek(), L"uddi");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_NonInitial_EscapeAfterConsonant) {
    // "vddd" → "vdd" (dd→đ then escape → vdd)
    TypeString(*engine_, L"vddd");
    EXPECT_EQ(engine_->Peek(), L"vdd");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_Initial_StillWorks) {
    // Basic dd→đ at index 0 still works
    TypeString(*engine_, L"ddi");
    EXPECT_EQ(engine_->Peek(), L"đi");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_Initial_WithCoda_Works) {
    // "docd" → "đoc": onset 'd' with vowel+coda, then stroke modifier
    // Pre-check must NOT block this — [đ,o,c] is valid Vietnamese
    TypeString(*engine_, L"docd");
    EXPECT_EQ(engine_->Peek(), L"đoc");
}

// REPRO: bug 2026-05-22 — "detail"+d → "đetail" (expected "detaild").
// Late-stroke applies to leading 'd' even though e-t-a forms V+C+V (English).
// Coda-block pre-check has "leading-d + vowel" exception that lets this slip.
TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_LateStroke_RejectVCV_detail) {
    TypeString(*engine_, L"detaild");
    EXPECT_EQ(engine_->Peek(), L"detaild");
}

// Recovery: leading-Đ escape on trailing-d when vowel sits between.
// "ddoc" (fast-typed 'doc' with key bounce) → "đoc"; user adds 'd' to recover.
// Vietnamese never has coda 'd', so trailing d = English/typo signal.
TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_LeadingDdEscape_ddocd) {
    TypeString(*engine_, L"ddocd");
    EXPECT_EQ(engine_->Peek(), L"docd");  // User then BS once → "doc"
}

// issue #200: leading Đ now toggles back unconditionally (no vowel guard).
// d-d-x-d → "dxd": dd→đ, x, then d un-strokes the leading đ and re-emits d.
// (Đ-initial all-consonant tokens like đxđ are not real Vietnamese words;
// trade-off accepted by maintainer in favor of consistent first-char toggle.)
TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_LeadingDdEscape_NoVowel_ddxd) {
    TypeString(*engine_, L"ddxd");
    EXPECT_EQ(engine_->Peek(), L"dxd");
}

// Sanity: longer recovery — "ddong" + d → "dongd" (lose đ, gain trailing d).
// Trade-off: rare "đôngd"-style typing loses, common fast-d-doc recovery wins.
TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_LeadingDdEscape_ddongd) {
    TypeString(*engine_, L"ddongd");
    EXPECT_EQ(engine_->Peek(), L"dongd");
}

TEST_F(TelexEngineTest, DModifier_Initial_WithCoda_SpellCheckOn) {
    // Same as above with spell check ON — "docd" → "đoc" (valid syllable)
    TypeString(*engine_, L"docd");
    EXPECT_EQ(engine_->Peek(), L"đoc");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_Initial_EscapeStillWorks) {
    // ddd → dd (escape) still works
    TypeString(*engine_, L"ddd");
    EXPECT_EQ(engine_->Peek(), L"dd");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_Dropdown8Keys) {
    // "dropdown" (8 keys) → "dropdown" — invalid coda "pd" blocks dd→đ modifier
    TypeString(*engine_, L"dropdown");
    EXPECT_EQ(engine_->Peek(), L"dropdown");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_DropdownLiteral9Keys) {
    // "dropddown" (9 keys): coda pre-check → HardEnglish → all literal
    TypeString(*engine_, L"dropddown");
    EXPECT_EQ(engine_->Peek(), L"dropddown");
}

// Abbreviation continuation: a NEW dd→Đ trigger after an existing Đ + consonant
// must still fire. Symptom before fix: HDDLDD → "HĐLDD" because pre-check found
// the existing Đ as a stroke-target and tripped IsStrokeDBlockedByCoda on the
// intervening L, then poisoned bias=HardEnglish for the next d. HĐLĐ is the
// common abbrev for "Hợp Đồng Lao Động"; vđtđ-style chains must also work.
TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_AllCapsAbbreviation_HDDLDD_NoSpell) {
    TypeString(*engine_, L"HDDLDD");
    EXPECT_EQ(engine_->Peek(), L"HĐLĐ");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_LowerAbbreviation_hddldd_NoSpell) {
    TypeString(*engine_, L"hddldd");
    EXPECT_EQ(engine_->Peek(), L"hđlđ");
}

TEST_F(TelexEngineTest, DModifier_AllCapsAbbreviation_HDDLDD_SpellOn) {
    TypeString(*engine_, L"HDDLDD");
    EXPECT_EQ(engine_->Peek(), L"HĐLĐ");
}

TEST_F(TelexEngineTest, DModifier_LowerAbbreviation_hddldd_SpellOn) {
    TypeString(*engine_, L"hddldd");
    EXPECT_EQ(engine_->Peek(), L"hđlđ");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_AbbreviationChain_vddtdd) {
    // v-d-d-t-d-d → vđtđ — second dd cluster fires after Đ+consonant.
    TypeString(*engine_, L"vddtdd");
    EXPECT_EQ(engine_->Peek(), L"vđtđ");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_LeadingDd_AcrossIntervening_ddxd) {
    // issue #200: leading Đ@0 escapes across an intervening consonant.
    // d-d-x-d → dxd (was đxd before the first-char toggle change).
    TypeString(*engine_, L"ddxd");
    EXPECT_EQ(engine_->Peek(), L"dxd");
}

// issue #200: dd→đ toggles the FIRST char consistently. A new d strokes the
// leading d (d→đ); the NEXT d un-strokes it (đ→d) and re-emits the literal d.
// Old bug: the un-stroke step didn't fire, leaving a stuck đ ("đmd").
TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_FirstCharToggle_dmdd) {
    // d-m-d-d: dm → (3rd d strokes leading d) → đm → (4th d un-strokes) → dmd.
    TypeString(*engine_, L"dmdd");
    EXPECT_EQ(engine_->Peek(), L"dmd");
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_FirstCharToggle_Incremental) {
    // Same typed incrementally — the đm intermediate is intended.
    TypeString(*engine_, L"dm");
    EXPECT_EQ(engine_->Peek(), L"dm");
    TypeString(*engine_, L"d");
    EXPECT_EQ(engine_->Peek(), L"đm");      // leading d strokes to đ
    TypeString(*engine_, L"d");
    EXPECT_EQ(engine_->Peek(), L"dmd");     // leading đ un-strokes, d re-emitted
}

TEST_F(EnglishDetectionNoSpellCheckTest, DModifier_FirstCharToggle_ddmd) {
    // d-d-m-d: dd → đ, then m → đm, then d un-strokes leading đ → dmd.
    TypeString(*engine_, L"ddmd");
    EXPECT_EQ(engine_->Peek(), L"dmd");
}

TEST_F(EnglishDetectionNoSpellCheckTest, WModifier_EscapeUndosBothHorns) {
    // "duoww": 2 w's to escape ươ back to "duow" (2-state cycle for non h/th/kh)
    // w(1): P2 horn both → "dươ"
    // w(2): P2 escape → clear both horns + add 'w' literal → "duow"
    TypeString(*engine_, L"duoww");
    EXPECT_EQ(engine_->Peek(), L"duow");
}

TEST_F(TelexEngineTest, CapsLock_Uppercase_TildeI) {
    // CapsLock ON: I+x → Ĩ (0x0128), was reverting to ĩ (towupper fails on Windows)
    TypeString(*engine_, L"Ix");
    EXPECT_EQ(engine_->Peek(), L"\u0128");
}

TEST_F(TelexEngineTest, CapsLock_Uppercase_TildeU) {
    // CapsLock ON: U+x → Ũ (0x0168)
    TypeString(*engine_, L"Ux");
    EXPECT_EQ(engine_->Peek(), L"\u0168");
}

TEST_F(EnglishDetectionNoSpellCheckTest, FreeMark_ReadmeBlocked) {
    // "readmee": free-mark must NOT cross diff-vowel + consonant (e←a←dm←e)
    // → HardEnglish, all literal
    TypeString(*engine_, L"readmee");
    EXPECT_EQ(engine_->Peek(), L"readmee");
}

TEST_F(EnglishDetectionNoSpellCheckTest, FreeMark_CrossVowelNoCons_StillWorks) {
    // "chiều": free-mark 'e' crosses vowel 'u' (no consonant) → circumflex must apply
    TypeString(*engine_, L"chieuef");
    EXPECT_EQ(engine_->Peek(), L"chiều");
}

TEST_F(EnglishDetectionNoSpellCheckTest, FreeMark_SameVowelValidCoda_StillWorks) {
    // "tiêng" via late circumflex: same vowel, crosses ng (valid coda)
    TypeString(*engine_, L"tienges");
    EXPECT_EQ(engine_->Peek(), L"tiếng");
}

TEST_F(TelexEngineTest, SpellCheck_ConsonantCluster_NoCrash) {
    // Verify long consonant clusters with modifiers don't crash (spell check ON)
    TypeString(*engine_, L"mtruowngf");
    EXPECT_FALSE(engine_->Peek().empty());
}

// ============================================================================
// MODIFIER SWITCHING (Circumflex ↔ Breve via 'a'/'w' across coda)
// ============================================================================

TEST_F(TelexEngineTest, ModifierSwitch_CircumflexToBreve_CodaN) {
    // "chậnw" → "chặn" (â→ă via 'w' across coda 'n')
    TypeString(*engine_, L"chaajnw");
    EXPECT_EQ(engine_->Peek(), L"chặn");
}

TEST_F(TelexEngineTest, ModifierSwitch_BreveToCircumflex_CodaN) {
    // "chặna" → "chận" (ă→â via 'a' across coda 'n')
    TypeString(*engine_, L"chawjna");
    EXPECT_EQ(engine_->Peek(), L"chận");
}

TEST_F(TelexEngineTest, ModifierSwitch_CircumflexToBreve_CodaM) {
    // "châmw" → "chăm" (â→ă via 'w' across coda 'm')
    TypeString(*engine_, L"chaamw");
    EXPECT_EQ(engine_->Peek(), L"chăm");
}

TEST_F(TelexEngineTest, ModifierSwitch_BreveToCircumflex_CodaT) {
    // "chặta" → "chật" (ă→â via 'a' across coda 't')
    TypeString(*engine_, L"chawjta");
    EXPECT_EQ(engine_->Peek(), L"chật");
}

TEST_F(TelexEngineTest, ModifierSwitch_CircumflexToBreve_WithTone) {
    // Tone must be preserved when switching â→ă
    // "tấnw" → "tắn" (â+sắc → ă+sắc)
    TypeString(*engine_, L"taasnw");
    EXPECT_EQ(engine_->Peek(), L"tắn");
}

TEST_F(TelexEngineTest, ModifierSwitch_BreveToCircumflex_WithTone) {
    // Tone must be preserved when switching ă→â
    // "tắna" → "tấn" (ă+sắc → â+sắc)
    TypeString(*engine_, L"tawsna");
    EXPECT_EQ(engine_->Peek(), L"tấn");
}

TEST_F(TelexEngineTest, ModifierSwitch_NoCoda_CircumflexToBreve) {
    // Adjacent: "âw" → "ă" (no coda, direct switch)
    TypeString(*engine_, L"aaw");
    EXPECT_EQ(engine_->Peek(), L"ă");
}

TEST_F(TelexEngineTest, ModifierSwitch_NoCoda_BreveToCircumflex) {
    // Adjacent: "ăa" → "â" (no coda, direct switch)
    TypeString(*engine_, L"awa");
    EXPECT_EQ(engine_->Peek(), L"â");
}

// ============================================================================
// REPEATED VOWEL ESCAPE — spell check must not re-apply circumflex
// ============================================================================

TEST_F(EnglishProtectionTest, RepeatedO_NoCircumflex) {
    // "oooooo" must not produce "oôoo" — circumflex escape blocks re-application.
    // 6 keys → 5 chars (escape consumes 1), but NO diacritics.
    TypeString(*engine_, L"oooooo");
    EXPECT_EQ(engine_->Peek(), L"ooooo");
}

TEST_F(EnglishProtectionTest, RepeatedA_NoCircumflex) {
    TypeString(*engine_, L"aaaaaa");
    EXPECT_EQ(engine_->Peek(), L"aaaaa");
}

TEST_F(EnglishProtectionTest, RepeatedE_NoCircumflex) {
    TypeString(*engine_, L"eeeeee");
    EXPECT_EQ(engine_->Peek(), L"eeeee");
}

// ============================================================================
// REPEATED VOWEL TONE STABILITY — tone stays where user placed it
// Circumflex trigger/escape consumes keys, so output is shorter than input.
// ============================================================================

TEST_F(EnglishProtectionTest, RepeatedA_TonePlacement_Acute) {
    // "masaaaaaaaaa" → tone stays on first 'a', doesn't slide to last.
    TypeString(*engine_, L"masaaaaaaaaa");
    EXPECT_EQ(engine_->Peek(), L"máaaaaaaaa");
}

TEST_F(EnglishProtectionTest, RepeatedO_TonePlacement_Acute) {
    TypeString(*engine_, L"mosooooooooo");
    EXPECT_EQ(engine_->Peek(), L"móoooooooo");
}

TEST_F(EnglishProtectionTest, RepeatedE_TonePlacement_Acute) {
    TypeString(*engine_, L"meseeeeeeeee");
    EXPECT_EQ(engine_->Peek(), L"méeeeeeeee");
}

// ============================================================================
// P2.1: Tone drift guard — tone must NOT slide on repeated vowels
// ============================================================================

TEST_F(EnglishProtectionTest, ToneDrift_Hoaaaa_Grave) {
    // "hofaaaa" → tone stays on ò (never slides to a).
    // Circumflex free-marking rejects "hoầ" as invalid up-front, so all
    // 4 typed a's are preserved as literal (no aa→â→escape cycle).
    TypeString(*engine_, L"hofaaaa");
    EXPECT_EQ(engine_->Peek(), L"hòaaaa");
}

TEST_F(EnglishProtectionTest, ToneDrift_Kiaaaa_Grave) {
    TypeString(*engine_, L"kifaaaa");
    EXPECT_EQ(engine_->Peek(), L"kìaaaa");
}

TEST_F(EnglishProtectionTest, ToneDrift_Grave_Repeated) {
    TypeString(*engine_, L"afaaaa");
    EXPECT_EQ(engine_->Peek(), L"àaaa");
}

TEST_F(EnglishProtectionTest, ToneDrift_Hook_Repeated) {
    TypeString(*engine_, L"araaaa");
    EXPECT_EQ(engine_->Peek(), L"ảaaa");
}

TEST_F(EnglishProtectionTest, ToneDrift_Tilde_Repeated) {
    TypeString(*engine_, L"axaaaa");
    EXPECT_EQ(engine_->Peek(), L"ãaaa");
}

TEST_F(EnglishProtectionTest, ToneDrift_Dot_Repeated) {
    TypeString(*engine_, L"ajaaaa");
    EXPECT_EQ(engine_->Peek(), L"ạaaa");
}

TEST_F(EnglishProtectionTest, ToneDrift_RepeatedI_AfterOA) {
    // "Hoaifiiiii" → "Hoàiiiiii" (tone stays on 'a', not sliding to 'i')
    // Tone key 'f' doesn't add a char → 6 i's in output (1 original + 5 typed)
    TypeString(*engine_, L"Hoaifiiiii");
    EXPECT_EQ(engine_->Peek(), L"Hoàiiiiii");
}

TEST_F(EnglishProtectionTest, ToneDrift_RepeatedI_AfterAI) {
    // "Vaixiiiii" → "Vãiiiiii" (tone stays on 'a')
    TypeString(*engine_, L"Vaixiiiii");
    EXPECT_EQ(engine_->Peek(), L"Vãiiiiii");
}

TEST_F(EnglishProtectionTest, ToneDrift_RepeatedU_Single) {
    // "Tusuuuuu" → "Túuuuuu" (tone stays on first 'u')
    TypeString(*engine_, L"Tusuuuuu");
    EXPECT_EQ(engine_->Peek(), L"Túuuuuu");
}

TEST_F(EnglishProtectionTest, ToneDrift_RepeatedI_AfterOI) {
    // "ddoifiiiii" → "đòiiiiii" (tone stays on 'o')
    TypeString(*engine_, L"ddoifiiiii");
    EXPECT_EQ(engine_->Peek(), L"đòiiiiii");
}

// ============================================================================
// P2.2: gi + tone THEN vowel → tone must relocate from i to new vowel
// ============================================================================

TEST_F(EnglishProtectionTest, GiToneRelocate_Grave_A) {
    // g-i-f(grave)-a → "già" (tone moves from ì to à)
    TypeString(*engine_, L"gifa");
    EXPECT_EQ(engine_->Peek(), L"già");
}

TEST_F(EnglishProtectionTest, GiToneRelocate_Acute_A) {
    TypeString(*engine_, L"gisa");
    EXPECT_EQ(engine_->Peek(), L"giá");
}

TEST_F(EnglishProtectionTest, GiToneRelocate_Hook_A) {
    TypeString(*engine_, L"gira");
    EXPECT_EQ(engine_->Peek(), L"giả");
}

TEST_F(EnglishProtectionTest, GiToneRelocate_Tilde_A) {
    TypeString(*engine_, L"gixa");
    EXPECT_EQ(engine_->Peek(), L"giã");
}

TEST_F(EnglishProtectionTest, GiToneRelocate_Dot_A) {
    TypeString(*engine_, L"gija");
    EXPECT_EQ(engine_->Peek(), L"giạ");
}

TEST_F(EnglishProtectionTest, GiToneRelocate_Grave_An) {
    // gì + an → giàn
    TypeString(*engine_, L"gifan");
    EXPECT_EQ(engine_->Peek(), L"giàn");
}

// ============================================================================
// MODIFIER ESCAPE THROUGH SPELL CHECK GATE
// ww should undo horn even when spellCheckDisabled_ is true
// ============================================================================

TEST_F(EnglishProtectionTest, WW_Escape_ThroughSpellCheck_EU) {
    // euw → eư (horn applied, spellCheckDisabled_ set because "eư" is invalid)
    // euww → euw (second 'w' should escape the horn)
    TypeString(*engine_, L"euww");
    EXPECT_EQ(engine_->Peek(), L"euw");
}

// ============================================================================
// ENGLISH WORD BLOCK — RAW PREFIX CHECK
// Block tone/modifier on known English prefixes when spell check is ON.
// TDD: these tests are written BEFORE the implementation — expect RED.
// ============================================================================

TEST_F(EnglishProtectionTest, EnglishBlock_Pass) {
    // "pass": p-a-s → 's' should NOT fire sắc on "pa"
    // Expected: "pas" (literal s), not "pá" (sắc applied)
    TypeString(*engine_, L"pass");
    EXPECT_EQ(engine_->Peek(), L"pass");
}

TEST_F(EnglishProtectionTest, EnglishBlock_Passing) {
    // "passing": same prefix "pas" → tone blocked, rest is literal
    TypeString(*engine_, L"passing");
    EXPECT_EQ(engine_->Peek(), L"passing");
}

TEST_F(EnglishProtectionTest, EnglishBlock_Password) {
    TypeString(*engine_, L"password");
    EXPECT_EQ(engine_->Peek(), L"password");
}

TEST_F(EnglishProtectionTest, EnglishBlock_Guess) {
    // "guess": g-u-e-s → 's' should NOT fire sắc on "gue"
    TypeString(*engine_, L"guess");
    EXPECT_EQ(engine_->Peek(), L"guess");
}

TEST_F(EnglishProtectionTest, EnglishBlock_Guessing) {
    TypeString(*engine_, L"guessing");
    EXPECT_EQ(engine_->Peek(), L"guessing");
}

TEST_F(EnglishProtectionTest, EnglishBlock_Power) {
    // "power": p-o-w → 'w' should NOT fire horn on "po" → no "pơ"
    TypeString(*engine_, L"power");
    EXPECT_EQ(engine_->Peek(), L"power");
}

TEST_F(EnglishProtectionTest, EnglishBlock_Powder) {
    TypeString(*engine_, L"powder");
    EXPECT_EQ(engine_->Peek(), L"powder");
}

TEST_F(EnglishProtectionTest, EnglishBlock_Upward) {
    // "upward": u-p-w → 'w' should NOT fire horn on "u" → no "ưp"
    TypeString(*engine_, L"upward");
    EXPECT_EQ(engine_->Peek(), L"upward");
}

TEST_F(EnglishProtectionTest, EnglishBlock_Upload) {
    TypeString(*engine_, L"upload");
    EXPECT_EQ(engine_->Peek(), L"upload");
}

TEST_F(EnglishProtectionTest, EnglishBlock_Up_ToneStillWorks) {
    // "úp" (u-p-s): tone sắc on "up" should STILL work — only horn is blocked
    TypeString(*engine_, L"ups");
    EXPECT_EQ(engine_->Peek(), L"úp");
}

TEST_F(EnglishProtectionTest, EnglishBlock_SpellCheckOff_NoBlock) {
    // With spell check OFF, English block should NOT activate
    TypingConfig offConfig;
    offConfig.spellCheckEnabled = false;
    offConfig.optimizeLevel = 0;
    auto offEngine = std::make_unique<TypingEngine>(offConfig);
    TypeString(*offEngine, L"pas");
    // Spell check off → 's' fires sắc normally → "pá"
    EXPECT_EQ(offEngine->Peek(), L"pá");
}

// ============================================================================
// Free-marking spell-check validation
// "gacha" must stay "gacha", not become "gâch" (âch is not a valid coda).
// ============================================================================

TEST_F(EnglishProtectionTest, GachaNotMarkedCircumflex) {
    TypeString(*engine_, L"gacha");
    EXPECT_EQ(engine_->Peek(), L"gacha");
}

TEST_F(EnglishProtectionTest, GachWNotMarkedBreve) {
    TypeString(*engine_, L"gachw");
    EXPECT_EQ(engine_->Peek(), L"gachw");
}

TEST_F(EnglishProtectionTest, GachSStillGivesSacTone) {
    TypeString(*engine_, L"gachs");
    EXPECT_EQ(engine_->Peek(), L"gách");
}

TEST_F(EnglishProtectionTest, GachJStillGivesNangTone) {
    TypeString(*engine_, L"gachj");
    EXPECT_EQ(engine_->Peek(), L"gạch");
}

TEST_F(EnglishProtectionTest, BachaNotMarkedCircumflex) {
    TypeString(*engine_, L"bacha");
    EXPECT_EQ(engine_->Peek(), L"bacha");
}

TEST_F(EnglishProtectionTest, MachaNotMarkedCircumflex) {
    TypeString(*engine_, L"macha");
    EXPECT_EQ(engine_->Peek(), L"macha");
}

TEST_F(EnglishProtectionTest, TiengStillFreeMarks) {
    TypeString(*engine_, L"tienge");
    EXPECT_EQ(engine_->Peek(), L"tiêng");
}

TEST_F(EnglishProtectionTest, BanwStillGivesBreve) {
    TypeString(*engine_, L"banw");
    EXPECT_EQ(engine_->Peek(), L"băn");
}

TEST_F(EnglishProtectionTest, AnwStillGivesBreve) {
    TypeString(*engine_, L"anw");
    EXPECT_EQ(engine_->Peek(), L"ăn");
}

TEST_F(EnglishProtectionTest, HungwStillGivesHorn) {
    // P5 path: u→horn applied because "hưng" is valid. Non-regression.
    TypeString(*engine_, L"hungw");
    EXPECT_EQ(engine_->Peek(), L"hưng");
}

TEST_F(EnglishProtectionTest, CongwNotHornedCong) {
    // P6 path: o→horn blocked because "cơng" is not a valid syllable.
    TypeString(*engine_, L"congw");
    EXPECT_EQ(engine_->Peek(), L"congw");
}

// ============================================================================
// SeedFromText — revive composition from committed Vietnamese text
// ============================================================================

TEST_F(TelexEngineTest, Seed_Empty) {
    EXPECT_TRUE(engine_->SeedFromText(L""));
    EXPECT_EQ(engine_->Count(), 0u);
    EXPECT_EQ(engine_->Peek(), L"");
}

TEST_F(TelexEngineTest, Seed_PlainAscii) {
    EXPECT_TRUE(engine_->SeedFromText(L"go"));
    EXPECT_EQ(engine_->Peek(), L"go");
    EXPECT_EQ(engine_->Count(), 2u);
}

TEST_F(TelexEngineTest, Seed_ThenAddTone) {
    // User typed "gõ", committed. BS back to "go". Type 's' → "gó".
    ASSERT_TRUE(engine_->SeedFromText(L"go"));
    engine_->PushChar(L's');
    EXPECT_EQ(engine_->Peek(), L"gó");
}

TEST_F(TelexEngineTest, Seed_TonedVowel_ThenChangeTone) {
    // Seed "gõ", user types 's' → sắc overrides tilde → "gó"
    ASSERT_TRUE(engine_->SeedFromText(L"gõ"));
    EXPECT_EQ(engine_->Peek(), L"gõ");
    engine_->PushChar(L's');
    EXPECT_EQ(engine_->Peek(), L"gó");
}

TEST_F(TelexEngineTest, Seed_ModifiedVowel_Backspace) {
    ASSERT_TRUE(engine_->SeedFromText(L"â"));
    EXPECT_EQ(engine_->Count(), 1u);
    engine_->Backspace();
    EXPECT_EQ(engine_->Count(), 0u);
}

TEST_F(TelexEngineTest, Seed_FullSyllable) {
    ASSERT_TRUE(engine_->SeedFromText(L"tiếng"));
    EXPECT_EQ(engine_->Peek(), L"tiếng");
    EXPECT_EQ(engine_->Count(), 5u);
}

TEST_F(TelexEngineTest, Seed_DStroke) {
    ASSERT_TRUE(engine_->SeedFromText(L"đẹp"));
    EXPECT_EQ(engine_->Peek(), L"đẹp");
    EXPECT_EQ(engine_->Count(), 3u);
}

TEST_F(TelexEngineTest, Seed_UpperCasePreserved) {
    ASSERT_TRUE(engine_->SeedFromText(L"Tiếng"));
    EXPECT_EQ(engine_->Peek(), L"Tiếng");
}

TEST_F(TelexEngineTest, Seed_RejectsNonLetter_Space) {
    EXPECT_FALSE(engine_->SeedFromText(L"go "));
    EXPECT_EQ(engine_->Count(), 0u);
}

TEST_F(TelexEngineTest, Seed_RejectsNonLetter_Digit) {
    EXPECT_FALSE(engine_->SeedFromText(L"go1"));
    EXPECT_EQ(engine_->Count(), 0u);
}

TEST_F(TelexEngineTest, Seed_RejectsNonLetter_Punct) {
    EXPECT_FALSE(engine_->SeedFromText(L"go."));
    EXPECT_EQ(engine_->Count(), 0u);
}

TEST_F(TelexEngineTest, Seed_Horn_ThenTone) {
    ASSERT_TRUE(engine_->SeedFromText(L"ư"));
    engine_->PushChar(L's');
    EXPECT_EQ(engine_->Peek(), L"ứ");
}

TEST_F(TelexEngineTest, Seed_OverridesPreviousState) {
    TypeString(*engine_, L"hello");
    ASSERT_TRUE(engine_->SeedFromText(L"go"));
    EXPECT_EQ(engine_->Peek(), L"go");
    EXPECT_EQ(engine_->Count(), 2u);
}

TEST_F(TelexEngineTest, Seed_LatinOnlyWord_Succeeds) {
    // "system" is all latin a-z → decompose OK. TSF layer decides whether to revive
    // based on spell validity; the engine just decomposes.
    EXPECT_TRUE(engine_->SeedFromText(L"system"));
    EXPECT_EQ(engine_->Peek(), L"system");
}

// ============================================================================
// ESC RESTORE RAW TESTS — PeekRaw() returns raw keys for Esc-restore feature
// ============================================================================

TEST_F(TelexEngineTest, EscRestoreRaw_BasicVirus) {
    // v-i-r-u-s composes to Vietnamese form; rawInput keeps original keys
    TypeString(*engine_, L"virus");
    EXPECT_EQ(engine_->PeekRaw(), L"virus");
}

TEST_F(TelexEngineTest, EscRestoreRaw_PeekRawClearedByCommit) {
    // Contract test for design 2026-05-17: callers MUST snapshot PeekRaw()
    // BEFORE Commit() because Commit() internally calls Reset() which clears
    // escRawHistory_. See TypingEngine.cpp:1495,1504,1539,1546 -> Reset() ->
    // escRawHistory_.clear() (line 1553).
    TypeString(*engine_, L"virus");
    ASSERT_EQ(engine_->PeekRaw(), L"virus")
        << "Sanity: raw is populated while engine has buffer";

    std::wstring committed = engine_->Commit();
    EXPECT_FALSE(committed.empty())
        << "Commit() should return non-empty composed text";
    EXPECT_EQ(engine_->PeekRaw(), L"")
        << "Commit() must call Reset() which clears escRawHistory_; "
           "callers MUST snapshot PeekRaw() BEFORE Commit().";
    EXPECT_EQ(engine_->Count(), 0u);
}

TEST_F(TelexEngineTest, EscRestoreRaw_PreservesUpperCase) {
    TypeString(*engine_, L"VIRUS");
    EXPECT_EQ(engine_->PeekRaw(), L"VIRUS");
}

TEST_F(TelexEngineTest, EscRestoreRaw_PreservesMixedCase) {
    TypeString(*engine_, L"ViRuS");
    EXPECT_EQ(engine_->PeekRaw(), L"ViRuS");
}

TEST_F(TelexEngineTest, EscRestoreRaw_QuickStartConsonant) {
    // f → ph: rawInput keeps 'f' (the actual key user pressed)
    config_.quickStartConsonant = true;
    engine_ = std::make_unique<TypingEngine>(config_);
    engine_->PushChar(L'f');
    EXPECT_EQ(engine_->Peek(), L"ph");           // composed form
    EXPECT_EQ(engine_->PeekRaw(), L"f");          // raw form
    EXPECT_EQ(engine_->Count(), 2u);              // 2 displayed chars
}

TEST_F(TelexEngineTest, EscRestoreRaw_EmptyBufferReturnsEmpty) {
    EXPECT_EQ(engine_->PeekRaw(), L"");
}

TEST_F(TelexEngineTest, EscRestoreRaw_ClearedAfterReset) {
    TypeString(*engine_, L"hello");
    ASSERT_FALSE(engine_->PeekRaw().empty());
    engine_->Reset();
    EXPECT_EQ(engine_->PeekRaw(), L"");
}

TEST_F(TelexEngineTest, EscRestoreRaw_DoubleSToneEscape) {
    // a-s-u-s: 1st 's' applies acute tone to 'a' (states="áu"), 2nd 's' is
    // tone-escape — engine clears the tone and treats 's' as literal.
    // PeekRaw must return the FULL keystroke sequence "asus" so the user can
    // recover their original word. The pre-existing auto-restore feature uses
    // a different buffer (rawInput_) which deliberately drops the consumed
    // first 's' to give "aus"; ESC-restore-raw must NOT inherit that semantics.
    TypeString(*engine_, L"asus");
    EXPECT_EQ(engine_->PeekRaw(), L"asus");
}

// Regression suite for ESC restore raw under SimpleTelex + allowZwjf=true
// (typical real-user config). Engine's display mangles many English words
// when tone/modifier keys get consumed; PeekRaw must always return the full
// keystroke sequence so ESC can recover the literal.
TEST_F(TelexEngineTest, EscRestoreRaw_SimpleTelex_EnglishWords) {
    config_.inputMethod = InputMethod::SimpleTelex;
    config_.allowZwjf = true;
    engine_ = std::make_unique<TypingEngine>(config_);

    struct Case { const wchar_t* keys; const wchar_t* expectRaw; };
    Case cases[] = {
        // Double-tone-letter English words — engine consumes 1 char from display,
        // PeekRaw must preserve full sequence so user can recover.
        { L"ass",     L"ass"     },
        { L"bass",    L"bass"    },
        { L"pass",    L"pass"    },
        { L"mass",    L"mass"    },
        { L"less",    L"less"    },
        { L"miss",    L"miss"    },
        { L"sorry",   L"sorry"   },
        { L"error",   L"error"   },
        // EnglishBias catches str- prefix; display + raw both match.
        { L"stress",  L"stress"  },
        // Non-double-tone English words with tone/modifier consumption.
        { L"where",   L"where"   },
        { L"users",   L"users"   },
        { L"perfect", L"perfect" },
        // Tone-escape gesture path (user knew r=tone, double-pressed to escape).
        // PeekRaw returns 6 chars matching the 6 keystrokes — consistent with
        // the asus case, even though display happens to show 5.
        { L"wherre",  L"wherre"  },
        // Telex-novice case — wants the original literal back.
        { L"asus",    L"asus"    },
    };
    for (const auto& c : cases) {
        engine_->Reset();
        TypeString(*engine_, c.keys);
        // c.keys is always ASCII (English test words) — explicit narrow avoids
        // MSVC /WX C4244 from std::string(wchar_t*, wchar_t*).
        std::string narrow;
        for (const wchar_t* p = c.keys; *p; ++p) narrow.push_back(static_cast<char>(*p));
        EXPECT_EQ(engine_->PeekRaw(), c.expectRaw) << "input: " << narrow;
    }
}

TEST_F(TelexEngineTest, EscRestoreRaw_BackspaceShrinks) {
    // v-i-r-u-s composes "víu"; pressing BS removes one displayed char.
    // PeekRaw must shrink by 1 to match — gives the user a coherent
    // "what I have left" view.
    TypeString(*engine_, L"virus");
    ASSERT_EQ(engine_->PeekRaw(), L"virus");
    engine_->Backspace();
    EXPECT_EQ(engine_->PeekRaw(), L"viru");
}

TEST_F(TelexEngineTest, EscRestoreRaw_BackspaceToEmpty_ClearsHistory) {
    // dd → đ (1 displayed char from 2 keystrokes). BS empties states.
    // escRawHistory must wipe too so the next word's typing doesn't
    // accumulate behind stale 'd'.
    engine_->PushChar(L'd');
    engine_->PushChar(L'd');
    ASSERT_EQ(engine_->PeekRaw(), L"dd");
    engine_->Backspace();
    EXPECT_EQ(engine_->PeekRaw(), L"");
    engine_->PushChar(L'a');
    EXPECT_EQ(engine_->PeekRaw(), L"a");  // not "da"
}

TEST_F(TelexEngineTest, EscRestoreRaw_DoesNotMutateState) {
    // PeekRaw must be const-like: calling it twice gives the same result
    // and doesn't disturb the engine.
    TypeString(*engine_, L"virus");
    auto first = engine_->PeekRaw();
    auto second = engine_->PeekRaw();
    EXPECT_EQ(first, second);
    EXPECT_EQ(first, L"virus");
    // After PeekRaw, engine still works normally
    engine_->PushChar(L's');
    EXPECT_FALSE(engine_->Peek().empty());
}

// ============================================================================
// uyê smart-accent intermediate — typing the syllable's final `e` AFTER the
// coda+tone (e.g. chuyenje, tuyensje, xuyensje) should free-mark e → ê and
// land the tone on ê via P2 priority. Mirrors the long-standing iê path
// (bietje → biệt, chiense → chiến). Bug surfaced 2026-05-16.
// ============================================================================
class UyeSmartAccentTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_.inputMethod = InputMethod::Telex;
        cfg_.spellCheckEnabled = true;
        cfg_.modernOrtho = true;
        engine_ = std::make_unique<TypingEngine>(cfg_);
    }
    TypingConfig cfg_;
    std::unique_ptr<TypingEngine> engine_;
};

TEST_F(UyeSmartAccentTest, Chuyenje_Nang)   { TypeString(*engine_, L"chuyenje");  EXPECT_EQ(engine_->Peek(), L"chuyện"); }
TEST_F(UyeSmartAccentTest, Tuyenje_Nang)    { TypeString(*engine_, L"tuyenje");   EXPECT_EQ(engine_->Peek(), L"tuyện"); }
TEST_F(UyeSmartAccentTest, Tuyetje_Nang)    { TypeString(*engine_, L"tuyetje");   EXPECT_EQ(engine_->Peek(), L"tuyệt"); }
TEST_F(UyeSmartAccentTest, Nguyenje_Nang)   { TypeString(*engine_, L"nguyenje");  EXPECT_EQ(engine_->Peek(), L"nguyện"); }
TEST_F(UyeSmartAccentTest, Xuyense_Sac)     { TypeString(*engine_, L"xuyense");   EXPECT_EQ(engine_->Peek(), L"xuyến"); }
TEST_F(UyeSmartAccentTest, Nguyenxe_Nga)    { TypeString(*engine_, L"nguyenxe");  EXPECT_EQ(engine_->Peek(), L"nguyễn"); }
// Canonical (ee first) must still work unchanged.
TEST_F(UyeSmartAccentTest, Chuyeenj_Canonical) { TypeString(*engine_, L"chuyeenj"); EXPECT_EQ(engine_->Peek(), L"chuyện"); }
// Regression guards: shouldn't disturb adjacent diphthong tone rules.
TEST_F(UyeSmartAccentTest, Chuyf_ModernYHuyen) { TypeString(*engine_, L"chuyf");  EXPECT_EQ(engine_->Peek(), L"chuỳ"); }   // modern uy=SECOND keeps tone on y — fix targets uye (3-vowel), not uy (2-vowel)
TEST_F(UyeSmartAccentTest, Khuyaj_UyaTrip)     { TypeString(*engine_, L"khuyaj"); EXPECT_EQ(engine_->Peek(), L"khuỵa"); }  // uya triphthong middle (per IsTriphthong table — tone on y unaffected by the new uye case)

// Classic ortho: same fix, different 2-vowel tone placement preserved (no coda → tone on u, with coda → tone on y).
class UyeSmartAccentClassicTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_.inputMethod = InputMethod::Telex;
        cfg_.spellCheckEnabled = true;
        cfg_.modernOrtho = false;  // CLASSIC
        engine_ = std::make_unique<TypingEngine>(cfg_);
    }
    TypingConfig cfg_;
    std::unique_ptr<TypingEngine> engine_;
};
TEST_F(UyeSmartAccentClassicTest, Chuyenje_ClassicSameResult) { TypeString(*engine_, L"chuyenje"); EXPECT_EQ(engine_->Peek(), L"chuyện"); }
TEST_F(UyeSmartAccentClassicTest, Tuyetje_ClassicSameResult)  { TypeString(*engine_, L"tuyetje");  EXPECT_EQ(engine_->Peek(), L"tuyệt"); }
TEST_F(UyeSmartAccentClassicTest, Chuyf_ClassicUHuyen)        { TypeString(*engine_, L"chuyf");    EXPECT_EQ(engine_->Peek(), L"chùy"); }    // classic uy=CODA_AWARE no-coda → FIRST = u
TEST_F(UyeSmartAccentClassicTest, Huynhf_ClassicYHuyen)       { TypeString(*engine_, L"huynhf");   EXPECT_EQ(engine_->Peek(), L"huỳnh"); }   // classic uy=CODA_AWARE with coda → SECOND = y

// ============================================================================
// PROBE: tone-middle + smart-accent  (bug 2026-05-25: vijeet → vịeet)
// ============================================================================
// Pattern: tone key arrives BETWEEN single vowel and the smart-accent doubling.
// Expected: when 2nd 'e' fires ee→ê, tone must relocate from i to ê.
// "i" + tone "j" → "ị" ; then "e" + "e" → ee should promote to "iê" and
// re-place tone (nặng) onto ê → "iệ" ; final "t" coda → "việt".
class ToneMidSmartAccentTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_.inputMethod = InputMethod::Telex;
        cfg_.spellCheckEnabled = true;  // bug only reported with spell-check ON
        engine_ = std::make_unique<TypingEngine>(cfg_);
    }
    TypingConfig cfg_;
    std::unique_ptr<TypingEngine> engine_;
};
TEST_F(ToneMidSmartAccentTest, Vijeet_ToneBeforeSmartAccent) {
    TypeString(*engine_, L"vijeet");
    EXPECT_EQ(engine_->Peek(), L"việt");
}
TEST_F(ToneMidSmartAccentTest, Vieetj_Canonical_SpellOn) {
    TypeString(*engine_, L"vieetj");
    EXPECT_EQ(engine_->Peek(), L"việt");
}
// Probe step-by-step: does ee→ê fire after a toned vowel at all?
TEST_F(ToneMidSmartAccentTest, Probe_Ije_NoCoda)    { TypeString(*engine_, L"ije");   EXPECT_EQ(engine_->Peek(), L"ịe"); }   // baseline: tone on i, +e
TEST_F(ToneMidSmartAccentTest, Probe_Ijee_NoCoda)   { TypeString(*engine_, L"ijee");  EXPECT_EQ(engine_->Peek(), L"iệ"); }   // does ee promote + relocate tone?
TEST_F(ToneMidSmartAccentTest, Probe_Vijee_NoCoda)  { TypeString(*engine_, L"vijee"); EXPECT_EQ(engine_->Peek(), L"việ"); }  // same with leading consonant

// Mirror: spell-check OFF — does the same input behave the same?
class ToneMidSmartAccentSpellOffTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_.inputMethod = InputMethod::Telex;
        cfg_.spellCheckEnabled = false;
        engine_ = std::make_unique<TypingEngine>(cfg_);
    }
    TypingConfig cfg_;
    std::unique_ptr<TypingEngine> engine_;
};
TEST_F(ToneMidSmartAccentSpellOffTest, Vijeet_SpellOff) { TypeString(*engine_, L"vijeet"); EXPECT_EQ(engine_->Peek(), L"việt"); }

// Same root pattern, 'oo' family (user-reported 2026-05-25):
//   ngufono → nguồn (free-marking 'o' works because no adjacent oo)
//   ngufoon → should also be 'nguồn' (adjacent oo case)
TEST_F(ToneMidSmartAccentTest, Ngufoon_ToneBeforeAdjacentOO) {
    TypeString(*engine_, L"ngufoon");
    EXPECT_EQ(engine_->Peek(), L"nguồn");
}
TEST_F(ToneMidSmartAccentTest, Ngufono_FreeMarkingControl) {
    TypeString(*engine_, L"ngufono");
    EXPECT_EQ(engine_->Peek(), L"nguồn");  // should already pass — free-marking, not adjacent
}
// Modern ortho variant — does modernOrtho flip the behavior?
class ToneMidSmartAccentModernTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_.inputMethod = InputMethod::Telex;
        cfg_.spellCheckEnabled = true;
        cfg_.modernOrtho = true;
        engine_ = std::make_unique<TypingEngine>(cfg_);
    }
    TypingConfig cfg_;
    std::unique_ptr<TypingEngine> engine_;
};
TEST_F(ToneMidSmartAccentModernTest, Ngufoon_Modern) {
    TypeString(*engine_, L"ngufoon");
    EXPECT_EQ(engine_->Peek(), L"nguồn");
}
TEST_F(ToneMidSmartAccentModernTest, Vijeet_Modern) {
    TypeString(*engine_, L"vijeet");
    EXPECT_EQ(engine_->Peek(), L"việt");
}

// Late-modifier on Valid pre-state — documented Vietnamese behavior
// (vietnamese-phonology-spec-distillate.md, category-10 w-priority).
// These MUST work despite c6369dd's adjacent-circumflex "Valid → reject" branch
// because they go through HandleHornW (not HandleAdjacentCircumflex).
TEST_F(ToneMidSmartAccentTest, Cuarw_LateHornAfterValid) {
    TypeString(*engine_, L"cuarw");
    EXPECT_EQ(engine_->Peek(), L"cửa");
}
TEST_F(ToneMidSmartAccentTest, Hoaw_LateBreveAfterValid) {
    TypeString(*engine_, L"hoaw");
    EXPECT_EQ(engine_->Peek(), L"hoă");
}
TEST_F(ToneMidSmartAccentTest, Muaw_LateHornAfterValidUaPair) {
    TypeString(*engine_, L"muaw");
    EXPECT_EQ(engine_->Peek(), L"mưa");
}

// ============================================================================
// DOUBLE-OO LITERAL WORDS — voọc / soóc / goòng (Telex ooo→oo escape + tone)
//
// Telex collapses "oo" → "ô" (không, quốc, sốc), so the rare native words that
// carry a literal "oo" nucleus (voọc=langur, soóc=shorts, goòng=mining cart)
// can only be reached via the triple-o escape: ooo→oo, then a tone key. The
// tone on the escaped oo is PROVISIONAL — it commits only when a valid oo-coda
// (c→ooc, n→oong) follows, and reverts to its literal keystroke otherwise, so
// "chooose" stays English "choose" rather than becoming "choóe".
// ============================================================================

class DoubleOoTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::Telex;
        config_.spellCheckEnabled = true;   // realistic Vietnamese default
        config_.optimizeLevel = 0;
        config_.modernOrtho = false;
        engine_ = std::make_unique<TypingEngine>(config_);
    }
    TypingConfig config_;
    std::unique_ptr<TypingEngine> engine_;
};

// --- The feature: toned literal-oo words via ooo→oo escape ---
TEST_F(DoubleOoTest, Vooojc_ComposesVooc) {
    TypeString(*engine_, L"vooojc");          // v + ooo(→oo) + j(dot) + c
    EXPECT_EQ(engine_->Peek(), L"voọc");      // ọ = U+1ECD on the 2nd o
}
TEST_F(DoubleOoTest, Sooosc_ComposesSooc) {
    TypeString(*engine_, L"sooosc");          // s + ooo + s(acute) + c
    EXPECT_EQ(engine_->Peek(), L"soóc");
}
TEST_F(DoubleOoTest, Gooofng_ComposesGoong) {
    TypeString(*engine_, L"gooofng");         // g + ooo + f(grave) + ng
    EXPECT_EQ(engine_->Peek(), L"goòng");
}

// --- No-tone literal-oo words still work (already did, must not regress) ---
TEST_F(DoubleOoTest, Xooong_ComposesXoong) {
    TypeString(*engine_, L"xooong");          // xoong = pot
    EXPECT_EQ(engine_->Peek(), L"xoong");
}
TEST_F(DoubleOoTest, Booong_ComposesBoong) {
    TypeString(*engine_, L"booong");          // boong = (ship) deck
    EXPECT_EQ(engine_->Peek(), L"boong");
}

// --- Provisional-tone revert: trailing vowel ⇒ stay literal (English) ---
TEST_F(DoubleOoTest, Chooose_StaysChoose) {
    // ooo→oo escape + 's' looks like a tone, but the following 'e' cannot close
    // an oo syllable, so the tone reverts and 's' returns as a literal letter.
    TypeString(*engine_, L"chooose");
    EXPECT_EQ(engine_->Peek(), L"choose");
}
TEST_F(DoubleOoTest, Pooorr_RepeatedToneKeyEscapesToPoor) {
    // The repeated tone key is the Telex escape: the revert re-emits the first
    // 'r' as a literal, so the second must be swallowed (not re-tone the pair).
    TypeString(*engine_, L"pooorr");
    EXPECT_EQ(engine_->Peek(), L"poor");
    EXPECT_EQ(engine_->Commit(), L"poor");
}
TEST_F(DoubleOoTest, Pooor_UnresolvedToneRevertsOnCommit) {
    // Provisional while typing (the 'c'/'ng' coda could still arrive)…
    TypeString(*engine_, L"pooor");
    EXPECT_EQ(engine_->Peek(), L"poỏ");
    // …but a word boundary is the last chance for it, so commit reverts. Without
    // the revert the non-ASCII "poỏ" also triggers auto-restore → "pooor".
    EXPECT_EQ(engine_->Commit(), L"poor");
}
TEST_F(DoubleOoTest, Sooosc_CommitKeepsSooc) {
    TypeString(*engine_, L"sooosc");
    EXPECT_EQ(engine_->Commit(), L"soóc");
}

TEST_F(DoubleOoTest, Choooc_NoToneStaysLiteral) {
    TypeString(*engine_, L"choooc");          // no tone key at all
    EXPECT_EQ(engine_->Peek(), L"chooc");
}

// --- Common oo→ô words MUST be unchanged (the constraint) ---
TEST_F(DoubleOoTest, Khoong_StaysKhong) {
    TypeString(*engine_, L"khoong");
    EXPECT_EQ(engine_->Peek(), L"không");
}
TEST_F(DoubleOoTest, Soocs_StaysSoc) {
    TypeString(*engine_, L"soocs");           // 2 o's → ô (shock), NOT soóc
    EXPECT_EQ(engine_->Peek(), L"sốc");
}
TEST_F(DoubleOoTest, Quoocs_StaysQuoc) {
    TypeString(*engine_, L"quoocs");
    EXPECT_EQ(engine_->Peek(), L"quốc");
}

// --- VNI mode: no oo→ô collapse, so the words type directly (no escape) ---
class DoubleOoVniTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.inputMethod = InputMethod::VNI;
        config_.spellCheckEnabled = true;
        config_.optimizeLevel = 0;
        config_.modernOrtho = false;
        engine_ = std::make_unique<TypingEngine>(config_);
    }
    TypingConfig config_;
    std::unique_ptr<TypingEngine> engine_;
};
TEST_F(DoubleOoVniTest, Voo5c_ComposesVooc) {
    TypeString(*engine_, L"voo5c");           // 5 = nặng
    EXPECT_EQ(engine_->Peek(), L"voọc");
}
TEST_F(DoubleOoVniTest, Soo1c_ComposesSooc) {
    TypeString(*engine_, L"soo1c");           // 1 = sắc
    EXPECT_EQ(engine_->Peek(), L"soóc");
}
TEST_F(DoubleOoVniTest, Goo2ng_ComposesGoong) {
    TypeString(*engine_, L"goo2ng");          // 2 = huyền
    EXPECT_EQ(engine_->Peek(), L"goòng");
}

// ============================================================================
// #221: PL-onset abbreviation chains — vowel-less buffer lifts the hard-English
// onset block for stroke-d (rule mirrors the Rust engine: plhdd→plhđ,
// pladd stays literal).
// ============================================================================

TEST_F(TelexEngineTest, StrokeD_AbbrevChain_PLHD_Upper) {
    TypeString(*engine_, L"PLHDD");
    EXPECT_EQ(engine_->Peek(), L"PLHĐ");
}

TEST_F(TelexEngineTest, StrokeD_AbbrevChain_PLHD_Lower) {
    TypeString(*engine_, L"plhdd");
    EXPECT_EQ(engine_->Peek(), L"plhđ");
}

TEST_F(TelexEngineTest, StrokeD_AbbrevChain_CLD) {
    TypeString(*engine_, L"cldd");
    EXPECT_EQ(engine_->Peek(), L"clđ");
}

TEST_F(TelexEngineTest, StrokeD_VowelBuffer_EnglishStaysProtected) {
    // "pla" carries a vowel → hard-onset protection must keep blocking
    // (play, plaid, …): dd stays literal.
    TypeString(*engine_, L"pladd");
    EXPECT_EQ(engine_->Peek(), L"pladd");
}

TEST_F(AutoRestoreTest, Issue268_UnfinishedCircumflexDoesNotBecomeACompletedWord) {
    for (auto method : {InputMethod::Telex, InputMethod::SimpleTelex, InputMethod::Combined, InputMethod::VNI}) {
        config_.inputMethod = method;
        const wchar_t* raw = method == InputMethod::VNI ? L"d9a64" : L"ddaax";
        TypingEngine engine(config_);
        TypeString(engine, raw);
        EXPECT_EQ(engine.Peek(), L"đẫ");
        EXPECT_EQ(engine.Commit(), raw);
    }
    for (const wchar_t* raw : {L"aa", L"baaf", L"DDAAX"}) {
        config_.inputMethod = InputMethod::SimpleTelex;
        TypingEngine engine(config_);
        TypeString(engine, raw);
        EXPECT_EQ(engine.Commit(), raw);
    }
}

TEST_F(AutoRestoreTest, Issue268_ExplicitFreeTypingAndExclusionsStillKeepMarkedText) {
    for (bool spell : {false, true}) {
        for (bool restore : {false, true}) {
            if (spell && restore) continue;
            config_.spellCheckEnabled = spell;
            config_.autoRestoreEnabled = restore;
            TypingEngine engine(config_);
            TypeString(engine, L"ddaax");
            EXPECT_EQ(engine.Commit(), L"đẫ");
        }
    }
    config_.spellCheckEnabled = true;
    config_.autoRestoreEnabled = true;
    config_.spellExclusions = {L"đẫ"};
    TypingEngine engine(config_);
    TypeString(engine, L"ddaax");
    EXPECT_EQ(engine.Commit(), L"đẫ");
}

TEST_F(AutoRestoreTest, Issue268_AbbreviationsAndCompleteRhymesStayComposed) {
    struct Case { const wchar_t* raw; const wchar_t* expected; };
    const Case cases[] = {{L"dd", L"đ"}, {L"ddt", L"đt"}, {L"ddh", L"đh"},
        {L"hdd", L"hđ"}, {L"vddeef", L"vddeef"}, {L"ddaam", L"đâm"},
        {L"ddaauf", L"đầu"}, {L"ddaay", L"đây"}};
    for (const auto& item : cases) {
        TypingEngine engine(config_);
        TypeString(engine, item.raw);
        EXPECT_EQ(engine.Commit(), item.expected);
    }
    EXPECT_TRUE(HasIntentionalStrokeD(std::wstring(L"vddeef"), L"vđề"));
}

}  // namespace
}  // namespace NextKey
