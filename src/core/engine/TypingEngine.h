// VKey - Typing Engine Header (unified Telex/VNI/Combined)
// Copyright (c) 2024-2026 PhatMT. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "IInputEngine.h"
#include "EngineHelpers.h"
#include "PhonotacticsValidator.h"
#include "EnglishProtection.h"
#include "TypingAction.h"
#include "core/config/TypingConfig.h"
#include "core/engine/rule/AdjacentCircumflexProposal.h"
#include "core/engine/rule/BracketProposal.h"
#include "core/engine/rule/HornModifierProposal.h"
#include "core/engine/rule/StrokeDProposal.h"
#include "core/engine/rule/VniBreveProposal.h"
#include "core/engine/rule/VniCircumflexProposal.h"
#include "core/engine/rule/VniHornProposal.h"
#include "core/engine/rule/EngineRuleRegistry.h"
#include "core/engine/rule/IModifierExecutor.h"
#include "core/engine/rule/IModifierSubExecutor.h"
#include "core/engine/rule/IQuickConsonantExecutor.h"
#include "core/engine/rule/IToneExecutor.h"
#include <vector>
#include <string>

namespace NextKey {

// =============================================================================
// Unified types (shared by all input methods)
// =============================================================================

/// Modifier type for Vietnamese vowels
enum class Modifier : uint8_t {
    None,       // a e i o u y
    Circumflex, // â ê ô
    Breve,      // ă
    Horn,       // ơ ư
    Stroke      // đ (dd / d9)
};

/// Tone type for Vietnamese
enum class Tone : uint8_t {
    None,   // no tone
    Acute,  // sắc (Telex: s, VNI: 1)
    Grave,  // huyền (Telex: f, VNI: 2)
    Hook,   // hỏi (Telex: r, VNI: 3)
    Tilde,  // ngã (Telex: x, VNI: 4)
    Dot     // nặng (Telex: j, VNI: 5)
};

/// Internal state for each character - THE CORE ABSTRACTION
/// IME converts keys into STATE; letters are merely a consequence.
struct CharState {
    wchar_t base = 0;               // Base letter (lowercase): a e i o u y d or consonant
    Modifier mod = Modifier::None;  // Vowel modifier
    Tone tone = Tone::None;         // Tone mark
    bool isUpper = false;           // Preserve original case
    bool synthetic = false;         // True if created by P8 standalone 'w' (not a real keystroke)
    size_t rawIdx = 0;              // rawInput_ index when this state was created (for backspace sync)
    size_t toneRawIdx = SIZE_MAX;   // rawInput_ index of consumed tone key (for escape removal)

    [[nodiscard]] constexpr bool IsVowel() const noexcept {
        return base == L'a' || base == L'e' || base == L'i' ||
               base == L'o' || base == L'u' || base == L'y';
    }
    [[nodiscard]] constexpr bool CanHaveMod() const noexcept {
        // a -> â, ă; e -> ê; o -> ô, ơ; u -> ư
        return base == L'a' || base == L'e' || base == L'o' || base == L'u';
    }
    [[nodiscard]] constexpr bool IsD() const noexcept { return base == L'd'; }
    [[nodiscard]] constexpr bool IsHorn() const noexcept { return mod == Modifier::Horn; }
    [[nodiscard]] constexpr bool HasModifier() const noexcept { return mod != Modifier::None; }
    [[nodiscard]] constexpr bool HasTone() const noexcept { return tone != Tone::None; }
    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return base == 0; }
};

// =============================================================================
// TypingEngine — unified input method engine (Telex, VNI, Combined)
// =============================================================================

/// Unified input method engine - STATE-BASED ARCHITECTURE
///
/// Key principle: IME tracks STATE, not precomposed Unicode.
/// Unicode is only produced when Peek() or Commit() is called.
///
/// This makes:
/// - Backspace clean (pop entire char state)
/// - Escape clean (retype tone/mod to clear)
/// - No reverse maps needed
/// - TSF composition state always in sync
class TypingEngine : public IInputEngine,
                     public EngineRule::IToneExecutor,
                     public EngineRule::IModifierExecutor,
                     public EngineRule::IModifierSubExecutor,
                     public EngineRule::IQuickConsonantExecutor {
public:
    TypingEngine() : TypingEngine(TypingConfig{}) {}
    explicit TypingEngine(const TypingConfig& config);
    ~TypingEngine() override = default;

    TypingEngine(const TypingEngine&) = delete;
    TypingEngine& operator=(const TypingEngine&) = delete;

    // IInputEngine implementation
    void PushChar(wchar_t c) override;
    void PushKey(wchar_t physicalChar, bool uppercase) override;
    void Backspace() override;
    [[nodiscard]] const std::wstring& Peek() const override;
    [[nodiscard]] std::wstring Commit() override;
    void Reset() override;
    [[nodiscard]] size_t Count() const override { return states_.size(); }
    [[nodiscard]] bool HasActiveQuickConsonant() const override { return qc_.hasActive(); }
    [[nodiscard]] bool SeedFromText(const std::wstring& text) override;
    [[nodiscard]] bool IsEnglishWord() const override {
        return engProt_.bias == LanguageBias::HardEnglish;
    }
    [[nodiscard]] bool IsToneEscaped() const override {
        return escape_.isEscaped();
    }
    [[nodiscard]] std::wstring PeekRaw() const override {
        return std::wstring(escRawHistory_.data(), escRawHistory_.size());
    }
    [[nodiscard]] std::wstring_view PeekRawView() const noexcept override {
        return std::wstring_view(escRawHistory_.data(), escRawHistory_.size());
    }

    // IToneExecutor — drives the W7.2 ToneRule plugin. Returns true if the
    // tone/ClearTone action was handled (caller stops processing this key),
    // false if no tone path applied and dispatch should fall through.
    [[nodiscard]] bool HandleToneFsm(TypingAction action,
                                      wchar_t keyChar,
                                      wchar_t lower,
                                      bool isUpper) override;

    // IModifierExecutor — drives the W7.3 ModifierRule plugin. Returns true
    // if the modifier action consumed the key, false to fall through to
    // quick-end-consonant / regular char dispatch. Body unchanged from the
    // pre-W7.3 private dispatch helper.
    [[nodiscard]] bool HandleModifierAction(TypingAction action,
                                             wchar_t keyChar,
                                             wchar_t lower,
                                             bool isUpper) override;

    // IQuickConsonantExecutor — drives the W7.4 QuickStart/QuickEnd rules.
    // HandleQuickStartConsonant returns Veto on consumption (0a, 0b cc→ch,
    // 0b uu→ươ); Pass otherwise (0a-cont fall-through and no-match).
    // HandleQuickEndConsonant returns true on 2c consumption; false to
    // fall through to regular char.
    [[nodiscard]] EngineRule::Result HandleQuickStartConsonant(
        wchar_t keyChar, wchar_t lower, bool isUpper) override;
    [[nodiscard]] bool HandleQuickEndConsonant(
        wchar_t keyChar, wchar_t lower, bool isUpper) override;

private:
    // Mode helpers
    [[nodiscard]] bool IsTelexMode() const noexcept {
        return config_.inputMethod != InputMethod::VNI &&
               config_.inputMethod != InputMethod::UserDefined;
    }
    [[nodiscard]] bool IsVniMode() const noexcept {
        return config_.inputMethod == InputMethod::VNI ||
               config_.inputMethod == InputMethod::Combined;
    }

    // Tone keys (Telex z/s/f/r/x/j, VNI 0/1/2/3/4/5) — handled inline in PushChar.
    bool ProcessTone(Tone tone, wchar_t keyChar, size_t cachedTarget = SIZE_MAX);
    bool ProcessClearTone();

    // Tone gates ask this twice (spell-disabled gate and English-block gate)
    // to decide whether a user spell-exclusion entry should override a literal
    // rejection. Returns false on empty exclusions or SIZE_MAX target — caller
    // can drop the empty/sentinel guards.
    [[nodiscard]] bool ToneMatchesExclusion(size_t targetIdx, Tone requestedTone) const noexcept;

    // Internal processing
    void ProcessChar(wchar_t keyChar, wchar_t lower, bool isUpper);
    void ProcessChar(wchar_t keyChar) { ProcessChar(keyChar, towlower(keyChar), iswupper(keyChar)); }

    // Escape restore: emits `escapeKey` as a literal in the slot the consumed
    // tone/modifier keystroke occupied, rather than appending it after everything
    // typed since — t-e-r-i-r is "teri", not "teir" (#209). Which character
    // surfaces is unchanged; only its position is. Falls back to appending when
    // `consumedRawIdx` is unusable.
    void RestoreConsumedKeyInPlace(size_t consumedRawIdx, wchar_t escapeKey);

    // A tone applied to an `ooo→oo`-escaped literal "oo" is provisional: it only
    // commits if the next key is a valid oo-coda start ('c'→ooc, 'n'→oong).
    // Any other next key (notably a vowel) means the syllable can never close
    // validly, so the tone reverts to its literal keystroke. This is what makes
    // "vooojc"→voọc / "gooofng"→goòng work while "chooose"→choose (not choóe).
    // Called at PushChar entry with the incoming key's lowercase form, and at
    // Commit() with a sentinel (a word boundary is the last chance for a coda);
    // mutates states_ in place when a revert is needed (no-op otherwise).
    // Returns true when `lower` is the same tone key again — that repeat is the
    // Telex escape, already re-emitted by the revert, so PushChar must swallow
    // it instead of re-toning the pair ("pooorr" → poor, not poỏr).
    [[nodiscard]] bool RevertProvisionalOoTone(wchar_t lower);

    // W7.4: post-ProcessChar finalization (relocate tone / autoUO /
    // UpdateSpellState / English-bias / P8 revert / ZWJF). Lifted from
    // PushChar step 3 so 0b's cc→ch path can finalize without leaking
    // keyChar mutation back into PushChar's locals.
    void FinalizeRegularChar();

    // Unified modifier dispatch (G-3.5). Single entry point for every
    // user-mappable modifier action — both Telex and VNI. Switches on
    // TypingAction and forwards to the per-action handlers below. The
    // Telex/VNI split lives in PushChar's gating (English protection,
    // escape, spell-check disabled paths) — once the action is approved,
    // it's the same dispatch. Foundation for G-4 customKeyMap.
    bool ProcessModifier(TypingAction action, wchar_t keyChar);

    // Dispatch helpers (HandleModifierAction moved to public IModifierExecutor override above)
    bool WouldModifierRecoverOrEscape(TypingAction action, wchar_t keyChar, wchar_t lower);

    // Per-action modifier handlers. Uniform `(TypingAction, wchar_t)`
    // signature so ProcessModifier can dispatch them by action without
    // per-handler thunks. Each returns true on consumption, false to
    // fall through to ProcessChar (literal). `action` selects sub-
    // behaviour where one handler covers multiple actions (HornInsert,
    // AdjacentCircumflex); ignored where it's 1:1.
    // W8.4: IModifierSubExecutor override — body unchanged, called via
    // bracketProposal_.tryApply() from ProcessModifier dispatch.
    [[nodiscard]] bool HandleHornInsert(TypingAction action, wchar_t keyChar) override; // Telex `[`/`]`
    // W8.1: IModifierSubExecutor override — body unchanged, called via
    // adjacentCircumflexProposal_.tryApply() from ProcessModifier dispatch.
    [[nodiscard]] bool HandleAdjacentCircumflex(TypingAction action, wchar_t keyChar) override;  // Telex aa/ee/oo + cross-vowel
    // W8.2: IModifierSubExecutor override — body unchanged, called via
    // hornModifierProposal_.tryApply() from ProcessModifier dispatch.
    [[nodiscard]] bool HandleHornW(TypingAction action, wchar_t keyChar) override;               // Telex w (P1-P8)
    [[nodiscard]] bool HasNonClusterVowel() const noexcept;  // shared by HornW P8 + HornOrInsertU fallback
    // W8.3: IModifierSubExecutor override — body unchanged, called via
    // strokeDProposal_.tryApply() from ProcessModifier dispatch.
    [[nodiscard]] bool HandleStrokeD(TypingAction action, wchar_t keyChar) override;             // Telex dd / VNI 9
    // W8.5: IModifierSubExecutor overrides — bodies unchanged, called via
    // VNI proposal members from ProcessModifier dispatch.
    [[nodiscard]] bool HandleVniCircumflex(TypingAction action, wchar_t keyChar) override; // VNI 6
    [[nodiscard]] bool HandleVniHorn(TypingAction action, wchar_t keyChar) override;       // VNI 7
    [[nodiscard]] bool HandleVniBreve(TypingAction action, wchar_t keyChar) override;      // VNI 8
    bool HandleVniStroke(TypingAction action, wchar_t keyChar);            // VNI 9

    // New User-defined handlers
    bool HandleHornOrInsertU(TypingAction action, wchar_t keyChar);       // Horn/Insert U hybrid
    bool HandleUndoAllMarks(TypingAction action, wchar_t keyChar);        // Escape all
    bool HandleInsertChar(TypingAction action, wchar_t keyChar);          // Direct char insertion

    // ProcessVniVowelModifier is the shared backing impl for VniCircumflex
    // and VniBreve — kept Modifier-parameterised so the cross-vowel scan
    // isn't duplicated. HandleVniCircumflex / HandleVniBreve are the
    // dispatch entry points that supply the right Modifier.
    bool ProcessVniVowelModifier(Modifier targetMod, wchar_t key);

    // Find target for tone/modifier application.
    [[nodiscard]] size_t FindToneTarget() const noexcept;

    // T5 (anh 2026-05-07): true when the buffer is invalid because the existing
    // tone (huyền/hỏi/ngã) is incompatible with a stop final coda (c/ch/p/t).
    // Used by tone and modifier gates as a "mid-correction" predicate — when
    // true, the user is presumed to be fixing the syllable so we let through
    // tone replacement / modifier even if the current snapshot is invalid.
    [[nodiscard]] bool IsToneStopCodaMismatch() const noexcept;

    // Auto ươ: convert 'uơ' to 'ươ' when followed by another character
    void ApplyAutoUO();

    // Move tone to horn vowel when horn modifier is added
    // Example: "cuả" + w → "cửa" (tone moves from a to ư)
    void RelocateToneToHornVowel();

    // Move tone to correct target after modifier changes priority
    // Example: "chuanr" + a → tone moves from u to â (circumflex has higher priority)
    void RelocateToneToTarget();

    // QU Cluster detection (qu is consonant cluster, u is not vowel)
    bool IsInQUCluster() const;

    // Compose single CharState to Unicode
    static wchar_t Compose(const CharState& s);

    // Compose all states to string
    const std::wstring& ComposeAll() const;

    // Remove consumed raw entry and adjust all indices
    void EraseConsumedRaw(size_t idx);

    // Spell exclusion: would the modifier key produce an excluded word?
    bool WouldModifierKeyMatchExclusion(wchar_t lower) const;

    // Spell check: validate syllable structure after each keystroke
    void UpdateSpellState();

    // Would applying `newMod` to states_[targetIdx] produce a syllable that is
    // not Invalid per Phonology::ValidateSyllableState? Returns true when
    // spell-check is disabled (no validation performed). If `clearCircumflexIdx`
    // is a valid index and that state has a Circumflex, it is temporarily
    // cleared for the check (models W-modifier P5 which strips the sister â
    // when applying horn to u). Restores state before returning.
    // The validator enforces the tone/mod same-vowel invariant, so this also
    // catches typos like "của" + circumflex on 'a' → c,ủ,â.
    // `speculateRelocateTone` mirrors callers whose runtime applies the
    // modifier AND immediately runs `RelocateToneToTarget()` (free-marking
    // circumflex). Callers whose runtime keeps the tone on its current vowel
    // (adjacent circumflex via ShouldRejectModifier, Breve P7) leave it false.
    [[nodiscard]] bool WouldBeValidSyllable(size_t targetIdx, Modifier newMod,
                                            size_t clearCircumflexIdx = SIZE_MAX,
                                            bool speculateRelocateTone = false);

    // Common guard used by modifier sites (Telex adjacent/cross-vowel,
    // VNI Pass 1): reject when applying `newMod` at `targetIdx` produces an
    // invalid syllable AND the result doesn't match a user spell exclusion.
    // Returns false when spell check is disabled (no validation performed).
    [[nodiscard]] bool ShouldRejectModifier(size_t targetIdx, Modifier newMod,
                                            wchar_t key);

    void SetVowelMod(size_t idx, Modifier mod);

    // State
    std::vector<CharState> states_;   // Internal state buffer
    std::vector<wchar_t> rawInput_;   // Internal raw buffer — consumed tone/mod keys
                                      // are erased so auto-restore yields "user" for u-s-s-e-r.
                                      // Do NOT use for Esc-restore-raw — use escRawHistory_ instead.
    std::vector<wchar_t> escRawHistory_;  // Full keystroke history for PeekRaw() / Esc-restore-raw.
                                          // Push on PushChar, pop on Backspace, clear on Reset —
                                          // never trimmed by tone/mod-escape paths.
    TypingConfig config_;
    bool spellCheckDisabled_ = false; // true when buffer is invalid syllable
    QuickConsonantState qc_;              // Quick consonant expansion state
    EscapeState escape_;                  // Replaces toneEscaped_ + dModifierEscaped_
    bool activeKeyUppercase_ = false;     // Explicit case intent for the current PushKey call.
    wchar_t quickStartKey_ = 0;          // original key for quick start consonant (f/j/w), 0 if none
    EnglishProtectionState engProt_;     // 3-tier English protection state
    mutable std::wstring composeBuf_;    // Reusable buffer for ComposeAll() — avoids heap alloc per Peek()
    EngineRule::EngineRuleRegistry ruleRegistry_;  // W7.1: empty; W7.2+ registers rules
    // W8.1: ModifierProposal for CircumflexA/E/O dispatch. Routes through
    // IModifierSubExecutor — body lives in this engine, proposal layer is
    // documentation + future-extension slot for W8.2-W8.5.
    EngineRule::AdjacentCircumflexProposal adjacentCircumflexProposal_{*this};
    // W8.2: ModifierProposal for HornW (Telex `w`, P1-P8). Metadata declares
    // RelocationKind::HornVowel (apply path calls RelocateToneToHornVowel).
    EngineRule::HornModifierProposal hornModifierProposal_{*this};
    // W8.3: ModifierProposal for StrokeD (Telex `dd`, VNI `d9` → đ).
    // Metadata declares RelocationKind::None (pure mod toggle, no relocate).
    EngineRule::StrokeDProposal strokeDProposal_{*this};
    // W8.4: ModifierProposal for HornInsert (Telex `[`/`]` → direct ơ/ư).
    // Metadata declares RelocationKind::None (append + escape pop).
    EngineRule::BracketProposal bracketProposal_{*this};
    // W8.5: VNI proposals — Circumflex (6), Horn (7), Breve (8).
    // All declare TargetTone or HornVowel per their RelocateToneTo* call.
    // ProcessVniVowelModifier Pass 1 received the c6369dd ValidPrefix
    // speculate-relocate fix in this wave (resolves TODO 2026-05-25
    // vi5e6t / ngu2o6n wrong-reject).
    EngineRule::VniCircumflexProposal vniCircumflexProposal_{*this};
    EngineRule::VniHornProposal vniHornProposal_{*this};
    EngineRule::VniBreveProposal vniBreveProposal_{*this};
};

}  // namespace NextKey
