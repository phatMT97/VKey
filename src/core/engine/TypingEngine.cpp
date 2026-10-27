// VKey - Typing Engine Implementation (unified Telex/VNI/Combined)
// Copyright (c) 2024-2026 PhatMT. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-only
//
// V3 changes: flat constexpr arrays for O(1) Compose(), stack-allocated
// FindToneTarget(), bounded ApplyAutoUO(), pre-reserved buffers.

#include "TypingEngine.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>

#include "EngineHelpers.h"
#include "Phonotactics.h"
#include "TypingAction.h"
#include "VietnameseTables.h"
#include "core/engine/rule/EngineRuleContext.h"
#include "core/engine/rule/ToneRule.h"
#include "core/engine/rule/ModifierRule.h"
#include "core/engine/rule/QuickStartConsonantRule.h"
#include "core/engine/rule/QuickEndConsonantRule.h"

namespace NextKey {

namespace {

//=============================================================================
// File-scope constants
//=============================================================================

// Vowel/state cap used wherever a stack-array snapshot of a syllable buffer
// is needed. Matches FindTonePosition's fixed vowel-sequence capacity so that
// truncation behaves identically on both sides. Vietnamese syllables max out
// around 7-8 CharStates
// (e.g. `nghiêng` = 7); 16 is generous defensive headroom.
constexpr size_t kVowelCap = 16;

//=============================================================================
// Key mapping helpers
//=============================================================================

// IsVowelChar is shared — defined in VietnameseTables.h

// --- TypingAction helpers (G-3.2 dispatch foundation) ---
// Replaces the per-mode TelexKeyToTone / VniKeyToTone / IsVniModifierKey
// helpers — `ClassifyKey` (TypingAction.h) classifies the key once and
// these helpers extract the post-classification subset PushChar still
// needs.

constexpr Tone ActionToTone(TypingAction a) noexcept {
    switch (a) {
        case TypingAction::ToneAcute: return Tone::Acute;
        case TypingAction::ToneGrave: return Tone::Grave;
        case TypingAction::ToneHook:  return Tone::Hook;
        case TypingAction::ToneTilde: return Tone::Tilde;
        case TypingAction::ToneDot:   return Tone::Dot;
        default:                       return Tone::None;
    }
}

constexpr wchar_t ActionToVowel(TypingAction action) noexcept {
    switch (action) {
        case TypingAction::CircumflexA: return L'a';
        case TypingAction::CircumflexE: return L'e';
        case TypingAction::CircumflexO: return L'o';
        default: return 0;
    }
}

constexpr Modifier ActionToVniModifier(TypingAction action) noexcept {
    switch (action) {
        case TypingAction::VniCircumflex: return Modifier::Circumflex;
        case TypingAction::VniHorn:       return Modifier::Horn;
        case TypingAction::VniBreve:      return Modifier::Breve;
        case TypingAction::VniStroke:     return Modifier::Stroke;
        default:                           return Modifier::None;
    }
}

constexpr int ModifierIndex(Modifier mod) noexcept {
    switch (mod) {
        case Modifier::Circumflex: return 0;
        case Modifier::Breve:      return 1;
        case Modifier::Horn:       return 2;
        default:                   return -1;
    }
}

constexpr int ToneIndex(Tone tone) noexcept {
    switch (tone) {
        case Tone::Acute: return 0;
        case Tone::Grave: return 1;
        case Tone::Hook:  return 2;
        case Tone::Tilde: return 3;
        case Tone::Dot:   return 4;
        default:          return -1;
    }
}

}  // namespace

//=============================================================================
// TypingEngine Implementation
//=============================================================================

TypingEngine::TypingEngine(const TypingConfig& config)
    : config_(config) {
    states_.reserve(8);
    rawInput_.reserve(12);
    escRawHistory_.reserve(12);
    ruleRegistry_.Register(std::make_unique<EngineRule::ToneRule>(*this));
    ruleRegistry_.Register(std::make_unique<EngineRule::ModifierRule>(*this));
    ruleRegistry_.Register(std::make_unique<EngineRule::QuickStartConsonantRule>(*this));
    ruleRegistry_.Register(std::make_unique<EngineRule::QuickEndConsonantRule>(*this));
    Reset();
}

//-----------------------------------------------------------------------------
// Main Entry Point
//-----------------------------------------------------------------------------

void TypingEngine::PushChar(wchar_t keyChar) {
    PushKey(keyChar, iswupper(keyChar) || keyChar == L'{' || keyChar == L'}');
}

void TypingEngine::PushKey(wchar_t keyChar, bool uppercase) {
    // No buffer cap: game-compatible Telex keeps V mode active while WASD-spamming
    // can exceed any fixed limit. Relies on Reset() hooks (focus, click, space, enter)
    // to bound growth in practice.

    rawInput_.push_back(keyChar);
    escRawHistory_.push_back(keyChar);  // Independent of tone/mod-escape — see TypingEngine.h
    qc_.onlyQC = false;  // Any new char clears the flag

    // W7.1+: build engine-rule ctx + PreClassify dispatch (QuickStartConsonant).
    const wchar_t lower = towlower(keyChar);
    const bool isUpper = uppercase;
    activeKeyUppercase_ = uppercase;

    // Resolve any provisional oo-tone from a prior ooo→oo escape before this
    // key is classified (e.g. "chooo" + 's' + 'e': the 'e' reverts the tone so
    // the result is "choose", not "choóe"; "vooo"+'j'+'c' keeps it → voọc).
    if (RevertProvisionalOoTone(lower)) {
        UpdateSpellState();
        RecalcEnglishBias(states_.data(), states_.size(), engProt_);
        return;
    }
    const EngineRule::EngineRuleContext ruleCtx{
        .keyChar            = keyChar,
        .lower              = lower,
        .isUpper            = isUpper,
        .action             = TypingAction::None,
        .spellCheckDisabled = spellCheckDisabled_,
        .allowEnglishBypass = config_.allowEnglishBypass,
        .escapeActive       = escape_.isEscaped(),
        .escapeKind         = escape_.kind,
        .bias               = engProt_.bias,
        .isVniDigitSeq      = false,
        .states             = states_,
        .rawInput           = rawInput_,
        .config             = config_,
    };
    if (ruleRegistry_.DispatchAtPhase(EngineRule::Phase::PreClassify, ruleCtx, *this)
            == EngineRule::Result::Veto) return;

    // Literal digit sequence protection (VNI mode)
    // If the user types a digit immediately following a literal digit,
    // it's highly likely they are typing a number sequence (e.g., E747).
    // Bypass VNI tone/modifier processing to insert the digit literally.
    bool isVniDigitSequence = false;
    if (IsVniMode() && keyChar >= L'0' && keyChar <= L'9' && !states_.empty()) {
        wchar_t lastBase = states_.back().base;
        if (lastBase >= L'0' && lastBase <= L'9') {
            isVniDigitSequence = true;
        }
    }

    // 1. Classify keystroke. `customKeyMap` is the user-defined input method's
    // key remapping table — it ONLY applies when `inputMethod == UserDefined`.
    // Other modes (Telex/VNI/SimpleTelex/Combined) use their own base mapping
    // via ClassifyKey; stale customKeyMap entries left over from a previous
    // UserDefined session must not silently override the base.
    TypingAction overrideAction = TypingAction::None;
    if (lower < 128 && config_.inputMethod == InputMethod::UserDefined) {
        overrideAction = config_.customKeyMap[static_cast<uint8_t>(lower)];
        const wchar_t canonical = CanonicalBracketKey(lower);
        if (overrideAction == TypingAction::None && canonical != lower) {
            overrideAction = config_.customKeyMap[static_cast<uint8_t>(canonical)];
        }
    }
    TypingAction action = (overrideAction != TypingAction::None)
        ? overrideAction
        : ClassifyKey(lower, IsTelexMode(), IsVniMode());
    if (isVniDigitSequence) action = TypingAction::None;

    // W7.1+: PostClassify dispatch (Tone:10, Modifier:20, QuickEndConsonant:30).
    // Re-snapshot engine-local gate inputs in case PreClassify mutated them.
    {
        EngineRule::EngineRuleContext postCtx = ruleCtx;
        postCtx.action             = action;
        postCtx.isVniDigitSeq      = isVniDigitSequence;
        postCtx.spellCheckDisabled = spellCheckDisabled_;
        postCtx.escapeActive       = escape_.isEscaped();
        postCtx.escapeKind         = escape_.kind;
        postCtx.bias               = engProt_.bias;
        if (ruleRegistry_.DispatchAtPhase(EngineRule::Phase::PostClassify, postCtx, *this)
                == EngineRule::Result::Veto) return;
    }

    // 3. Regular character (Tone, Modifier, QuickStart, QuickEnd all handled
    // via the rule registry above).
    ProcessChar(keyChar, lower, isUpper);
    FinalizeRegularChar();
}

//-----------------------------------------------------------------------------
// Tone subsystem entry — W7.2 ToneRule executor target.
//-----------------------------------------------------------------------------

bool TypingEngine::HandleToneFsm(TypingAction action,
                                  wchar_t keyChar,
                                  wchar_t lower,
                                  bool isUpper) {
    // "Gõ tự do" / allowEnglishBypass: when ON, treat all spell-check-driven
    // literal-treatment gates below as if spell check were OFF — user wants
    // tones/modifiers applied freely regardless of Vietnamese phonotactic
    // validity. The HardEnglish-bias gates check `!config_.allowEnglishBypass`
    // directly (separate concern: rendered-text English heuristic).
    const bool effectiveSpellCheck =
        config_.spellCheckEnabled && !config_.allowEnglishBypass;

    // 1a. Clear tone: Telex 'z' / VNI '0'
    if (action == TypingAction::ClearTone && !states_.empty()) {
        if (effectiveSpellCheck && spellCheckDisabled_) {
            ProcessChar(keyChar);
            UpdateSpellState();
            return true;
        }
        if (ProcessClearTone()) {
            UpdateSpellState();
            return true;
        }
    }

    // 1b. Tone keys — derive Tone from action.
    Tone requestedTone = states_.empty() ? Tone::None : ActionToTone(action);
    bool isTelexTone = (requestedTone != Tone::None) && (lower < L'0' || lower > L'9');

    if (requestedTone != Tone::None) {
        // All "treat as literal" paths share the same two operations.
        auto asLiteral = [&] { ProcessChar(keyChar, lower, isUpper); UpdateSpellState(); };

        // Cache FindToneTarget from spell-check gate to avoid redundant call in ProcessTone.
        size_t cachedToneTarget = SIZE_MAX;
        bool hasCachedTarget = false;

        if (effectiveSpellCheck && spellCheckDisabled_) {
            cachedToneTarget = FindToneTarget();
            hasCachedTarget = true;
            size_t targetIndex = cachedToneTarget;
            bool isEscape = (targetIndex != SIZE_MAX && states_[targetIndex].tone == requestedTone);
            bool matchesExclusion = !isEscape && ToneMatchesExclusion(targetIndex, requestedTone);
            // T5: allow tone REPLACEMENT when the current invalid buffer would
            // become Valid after swapping the existing tone for the requested one.
            bool wouldRecover = false;
            if (!isEscape && !matchesExclusion && targetIndex != SIZE_MAX && states_[targetIndex].HasTone()) {
                Tone savedTone = states_[targetIndex].tone;
                states_[targetIndex].tone = requestedTone;
                auto result = Phonology::ValidateSyllableState(
                    states_.data(), states_.size(), config_.allowZwjf);
                states_[targetIndex].tone = savedTone;
                wouldRecover = (result == Phonology::SyllableState::Valid);
            }
            if (!isEscape && !matchesExclusion && !wouldRecover) { asLiteral(); return true; }
        }
        // Tone escape: a tone/horn/breve/stroke escape blocks the next tone.
        // The ToneEscapeGate already keeps ToneRule out for those, so this is
        // mostly defense-in-depth — EXCEPT for a circumflex escape (ooo→oo),
        // which the gate deliberately lets through so a following tone applies
        // and voọc/soóc/goòng compose. Mirror that exemption here.
        if (escape_.isEscaped() && !escape_.isEscaped(EscapeKind::Circumflex)) { asLiteral(); return true; }
        // English word block: raw prefix check (Telex keys only).
        if (isTelexTone && effectiveSpellCheck &&
            IsBlockedEnglishTone(rawInput_.data(), rawInput_.size())) {
            bool overridden = false;
            // Outer !empty guard skips the FindToneTarget cache fill when there are
            // no exclusions — ToneMatchesExclusion would return false anyway, but
            // the cache fill costs an O(n) buffer scan we'd rather avoid on this path.
            if (!config_.spellExclusions.empty()) {
                if (!hasCachedTarget) { cachedToneTarget = FindToneTarget(); hasCachedTarget = true; }
                overridden = ToneMatchesExclusion(cachedToneTarget, requestedTone);
            }
            if (!overridden) { asLiteral(); return true; }
        }
        // English Protection: always active, independent of spell check.
        if (!config_.allowEnglishBypass) {
            // T6 (2026-06-28): the English-bias gates below are MONOTONIC — once
            // `bias` latches to HardEnglish it never recovers on the forward tone
            // path (only Backspace's RecalcEnglishBias / Reset clear it), so a
            // transient English-looking intermediate permanently drops EVERY
            // subsequent tone until the user deletes the whole word ("không nhận
            // diện được bộ gõ"). Before any literal-drop, tentatively apply the
            // requested tone: if the WHOLE buffer becomes a phonotactically Valid
            // Vietnamese syllable, the word is unambiguously Vietnamese — let the
            // tone land and clear the latch (the missing non-monotonic reset).
            // Self-limiting: English words stay Invalid even with a tone, so they
            // never qualify (dropdown/password/fá/jà all fail ValidateSyllableState).
            // Mirrors the spell-path wouldRecover escape at ~L257-265, reusing the
            // same ValidateSyllableState oracle — no new heuristics.
            // NOTE (2026-06-29): the POSITIVE recovery (clearing a latched
            // HardEnglish) is unreachable by engine-only typing — a HardEnglish
            // latch always leaves a non-VN char in the buffer (literal s/f/r/x/j/z,
            // bad coda, or an asLiteral'd tone key), and Backspace's
            // RecalcEnglishBias re-derives bias, so "bias==HardEnglish AND
            // buffer+tone Valid" only arises when the hook injects a latched bias
            // onto an otherwise-valid buffer (commit-undo replay, #210). A
            // 150k-sequence fix-on/off probe sweep showed 0 engine-level diff;
            // ToneLatchSelfLimitTest can therefore only lock the self-limiting
            // side (English input must never be rescued into a toned form).
            auto toneWouldValidate = [&]() -> bool {
                size_t t = hasCachedTarget ? cachedToneTarget : FindToneTarget();
                if (t == SIZE_MAX || t >= states_.size()) return false;
                if (states_[t].tone == requestedTone) return false;  // same-tone = escape, not a save
                Tone saved = states_[t].tone;
                states_[t].tone = requestedTone;
                auto r = Phonology::ValidateSyllableState(
                    states_.data(), states_.size(), config_.allowZwjf);
                states_[t].tone = saved;
                return r == Phonology::SyllableState::Valid;
            };

            if (!toneWouldValidate()) {
                if (engProt_.bias == LanguageBias::HardEnglish)         { asLiteral(); return true; }
                if (engProt_.bias == LanguageBias::SoftEnglish) {
                    if (!UpdateToneInsistence(keyChar, engProt_))              { asLiteral(); return true; }
                }
                // Structural V+C+V check:
                if (isTelexTone) {
                    if (IsHardEnglishToneContext(states_.data(), states_.size(), keyChar)) {
                        engProt_.bias = LanguageBias::HardEnglish;
                        asLiteral(); return true;
                    }
                } else if (states_.size() >= 4) {
                    if (HasStructuralVCVPattern(states_.data(), states_.size())) {
                        engProt_.bias = LanguageBias::HardEnglish;
                        asLiteral(); return true;
                    }
                }
                if (HasInvalidAdjacentVowelPair(states_.data(), states_.size())) {
                    engProt_.bias = LanguageBias::HardEnglish;
                    asLiteral(); return true;
                }
            } else {
                engProt_.bias = LanguageBias::Vietnamese;  // clear the latch
            }
        }
        // Pre-tone stop-final check (spellCheck path only):
        if (effectiveSpellCheck && !spellCheckDisabled_) {
            if (requestedTone == Tone::Grave || requestedTone == Tone::Hook ||
                    requestedTone == Tone::Tilde) {
                if (HasStopFinalCoda(states_.data(), states_.size())) {
                    asLiteral(); return true;
                }
            }
        }
        if (ProcessTone(requestedTone, keyChar, hasCachedTarget ? cachedToneTarget : SIZE_MAX)) {
            if (!escape_.isEscaped()) {
                engProt_.bias = LanguageBias::Vietnamese;
            } else {
                RecalcEnglishBias(states_.data(), states_.size(), engProt_);
                if (IsTelexMode()) CheckZwjfInitialBias(states_.data(), states_.size(), config_, engProt_);
            }
            ApplyAutoUO();
            UpdateSpellState();
            return true;
        }
    }

    return false;  // No tone path consumed the key; PushChar continues.
}

//-----------------------------------------------------------------------------
// Quick-consonant subsystem entry — W7.4 QuickStart/QuickEnd rule executor.
//-----------------------------------------------------------------------------

NextKey::EngineRule::Result
TypingEngine::HandleQuickStartConsonant(wchar_t keyChar, wchar_t lower, bool isUpper) {
    using NextKey::EngineRule::Result;

    // 0a. Quick start consonant: f→ph, j→gi, w→qu (only at word start)
    if (config_.quickStartConsonant && states_.empty()) {
        wchar_t first = 0, second = 0;
        if (lower == L'f') { first = L'p'; second = L'h'; }
        else if (lower == L'j') { first = L'g'; second = L'i'; }
        else if (lower == L'w') { first = L'q'; second = L'u'; }
        if (first) {
            ProcessChar(isUpper ? towupper(first) : first);
            ProcessChar(second);
            quickStartKey_ = keyChar;  // Remember original key for undo
            UpdateSpellState();
            return Result::Veto;
        }
    }

    // 0a-cont. Undo quick start consonant if next char is not a vowel
    // e.g., f→ph, then 't' → undo to "ft" (not "pht"). Returns Pass so
    // PushChar continues to PostClassify dispatch + step 3.
    if (quickStartKey_ != 0) {
        wchar_t savedKey = quickStartKey_;
        quickStartKey_ = 0;  // Clear before any further processing
        if (!IsVowelChar(keyChar)) {
            states_.clear();
            rawInput_.clear();
            rawInput_.push_back(savedKey);
            ProcessChar(savedKey);
            rawInput_.push_back(keyChar);
            // Fall through (Pass) to normal processing in PushChar.
        }
    }

    // 0b. Quick consonant: cc→ch, gg→gi, nn→ng, kk→kh, qq→qu, pp→ph, tt→th
    // Skip if backspace just undid a quick consonant (let user type the literal).
    bool quickEscaped = qc_.escaped;
    qc_.escaped = false;

    // Suppress consecutive re-triggering: after cc→ch, skip quick consonant
    // while the user keeps pressing the same key (e.g., cccc → chcc, not chch).
    if (qc_.lastKey != 0) {
        if (lower == qc_.lastKey) {
            quickEscaped = true;  // Reuse escape flag to skip quick consonant
        } else {
            qc_.lastKey = 0;  // Different key, allow future expansions
        }
    }

    if (config_.quickConsonant && !states_.empty() && !quickEscaped) {
        const CharState& last = states_.back();
        if (!last.IsVowel() && !last.IsD()) {
            wchar_t replacement = 0;
            if (last.base == L'c' && lower == L'c') replacement = L'h';
            else if (last.base == L'g' && lower == L'g') replacement = L'i';
            else if (last.base == L'n' && lower == L'n') replacement = L'g';
            else if (last.base == L'k' && lower == L'k') replacement = L'h';
            else if (last.base == L'q' && lower == L'q') replacement = L'u';
            else if (last.base == L'p' && lower == L'p') replacement = L'h';
            else if (last.base == L't' && lower == L't') replacement = L'h';
            if (replacement) {
                if (states_.size() == 1) qc_.onlyQC = true;
                qc_.resultIndex = states_.size();  // Index of the char about to be added
                qc_.lastKey = lower;  // Suppress re-trigger — save ORIGINAL key
                wchar_t newKey = isUpper ? towupper(replacement) : replacement;
                // Direct ProcessChar + FinalizeRegularChar replaces the
                // pre-W7.4 "mutate caller's keyChar + fall through to step 3".
                ProcessChar(newKey, towlower(newKey), iswupper(newKey));
                FinalizeRegularChar();
                return Result::Veto;
            }
        }
        // uu→ươ: apply horn to existing 'u', then insert 'ơ'.
        // Guard: don't expand if last 3 vowels form a triphthong.
        else if (last.IsVowel() && last.base == L'u' && last.mod == Modifier::None && lower == L'u') {
            size_t stateCount = states_.size();
            bool triphthong = stateCount >= 3 && states_[stateCount - 3].IsVowel()
                && states_[stateCount - 2].IsVowel()
                && IsTriphthong(states_[stateCount - 3].base, states_[stateCount - 2].base, last.base);
            if (!triphthong) {
                states_.back().mod = Modifier::Horn;  // u→ư
                CharState newState;
                newState.base = L'o';
                newState.mod = Modifier::Horn;  // ơ
                newState.isUpper = isUpper;
                newState.rawIdx = rawInput_.empty() ? 0 : rawInput_.size() - 1;
                states_.push_back(newState);
                qc_.resultIndex = states_.size() - 1;  // Index of the ơ just added
                if (states_.size() == 2) qc_.onlyQC = true;
                qc_.lastKey = lower;  // Suppress re-trigger on consecutive same key
                UpdateSpellState();
                return Result::Veto;
            }
            // Triphthong — fall through to normal processing.
        }
    }

    return Result::Pass;
}

bool TypingEngine::HandleQuickEndConsonant(wchar_t /*keyChar*/, wchar_t lower, bool /*isUpper*/) {
    // Pre-guards (config, vowel-tail, letter ∈ {g,h,k}) already done by
    // QuickEndConsonantRule::Apply — this body just performs the match.
    wchar_t first = 0, second = 0;
    if (lower == L'g') { first = L'n'; second = L'g'; }
    else if (lower == L'h') { first = L'n'; second = L'h'; }
    else if (lower == L'k') { first = L'c'; second = L'h'; }
    if (first) {
        ProcessChar(first);
        ProcessChar(second);
        UpdateSpellState();
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
// W7.4: post-ProcessChar finalization helper. Body lifted verbatim from
// pre-W7.4 PushChar lines 290-321 (step 3 tail). Called from PushChar's
// step 3 and from HandleQuickStartConsonant's 0b cc→ch path.
//-----------------------------------------------------------------------------
void TypingEngine::FinalizeRegularChar() {
    RelocateToneToTarget();
    ApplyAutoUO();
    UpdateSpellState();

    // English Protection: re-evaluate bias after adding character
    CheckEnglishBias(states_.data(), states_.size(), engProt_);
    // Full Telex only: P8 rewrites a leading 'w' to synthetic ư before the
    // states-based start-cluster check sees it, hiding `wh`/`wr` from
    // IsHardEnglishStart. Re-check against raw keystrokes here AND revert the
    // synthetic ư back to literal 'w'.
    //
    // The synthetic-ư gate (tone == None) is load-bearing: once a tone key has
    // already landed on ư (e.g. `w` then `r` → `ử`), we cannot safely revert
    // — doing so would strip the tone the user actually wanted. That covers
    // legitimate Vietnamese sequences that share the `wr` raw prefix:
    //   w-r-n-g  → ửng  (tone applied at step 2, revert skipped)
    //   w-r-i-t-e → ửite (same — pre-existing behavior preserved)
    // SimpleTelex keeps 'w' literal (P8 gated off), so the states-based
    // start-cluster check on the previous line already covers it.
    if (config_.inputMethod == InputMethod::Telex &&
        engProt_.bias != LanguageBias::HardEnglish &&
        !states_.empty() && states_[0].synthetic &&
        states_[0].base == L'u' && states_[0].mod == Modifier::Horn &&
        states_[0].tone == Tone::None &&
        IsHardEnglishRawStart(rawInput_.data(), rawInput_.size())) {
        engProt_.bias = LanguageBias::HardEnglish;
        states_[0].base = L'w';
        states_[0].mod = Modifier::None;
        states_[0].synthetic = false;
    }
    if (IsTelexMode()) CheckZwjfInitialBias(states_.data(), states_.size(), config_, engProt_);
}

bool TypingEngine::HandleModifierAction(TypingAction action, wchar_t keyChar, wchar_t lower, bool /*isUpper*/) {
    const bool isTelexModifier = IsTelexModifierAction(action);
    const bool isVniModifier = IsVniModifierAction(action);
    const bool isUserOnly = IsUserDefinedOnlyAction(action);

    if (!isTelexModifier && !isVniModifier && !isUserOnly) return false;

    // Mode gating
    bool telexGated = IsTelexMode() || config_.inputMethod == InputMethod::UserDefined;
    bool vniGated = IsVniMode() || config_.inputMethod == InputMethod::UserDefined;
    bool userGated = config_.inputMethod == InputMethod::UserDefined;

    const bool effectiveSpellCheck = config_.spellCheckEnabled && !config_.allowEnglishBypass;

    // 2a. Telex-style modifier logic
    if (telexGated && isTelexModifier) {
        if (action == TypingAction::StrokeD && engProt_.bias != LanguageBias::HardEnglish && states_.size() >= 3) {
            size_t dTarget = FindStrokeDTarget(states_.data(), states_.size());
            // Only evaluate the English pre-check when target is a RAW d that
            // would BECOME Đ. An already-stroked Đ belongs to a prior abbrev
            // segment (e.g. HĐL+d for HĐLĐ); reusing it would set HardEnglish
            // and poison the NEXT dd→đ trigger.
            if (dTarget != SIZE_MAX && states_[dTarget].mod == Modifier::None &&
                ShouldBlockStrokeDAsEnglish(states_.data(), states_.size(), dTarget)) {
                engProt_.bias = LanguageBias::HardEnglish;
            }
        }
        bool block = escape_.isEscaped() || (!config_.allowEnglishBypass && engProt_.bias == LanguageBias::HardEnglish);
        if (!block && effectiveSpellCheck && IsBlockedEnglishModifier(rawInput_.data(), rawInput_.size())) block = true;
        // Vowel-less buffer + stroke-d = abbreviation chain (PLHDD→PLHĐ, #221):
        // the hard-onset bias (pl/cl/…) exists to protect English words, and no
        // English word lacks a vowel. Mirrors the Rust engine rule (parity:
        // plhdd→plhđ, pladd stays literal).
        if (block && !escape_.isEscaped() && action == TypingAction::StrokeD &&
            !HasVowelState(states_.data(), states_.size())) {
            block = false;
        }
        if (block && !escape_.isEscaped() && !config_.spellExclusions.empty() && WouldModifierKeyMatchExclusion(lower)) block = false;

        if (!block) {
            bool canApply = true;
            if (effectiveSpellCheck && spellCheckDisabled_) {
                canApply = WouldModifierRecoverOrEscape(action, keyChar, lower);
            }
            if (canApply && ProcessModifier(action, keyChar)) {
                engProt_.bias = LanguageBias::Vietnamese;
                ApplyAutoUO();
                UpdateSpellState();
                return true;
            }
        }
    }

    // 2b. VNI-style modifier logic
    if (vniGated && isVniModifier) {
        if (action == TypingAction::VniStroke && engProt_.bias != LanguageBias::HardEnglish && states_.size() >= 3) {
            size_t dTarget = FindStrokeDTarget(states_.data(), states_.size());
            // See 2a: only RAW d (mod=None) is a stroke target. Already-stroked
            // Đ is a prior segment.
            if (dTarget != SIZE_MAX && states_[dTarget].mod == Modifier::None &&
                ShouldBlockStrokeDAsEnglish(states_.data(), states_.size(), dTarget)) {
                engProt_.bias = LanguageBias::HardEnglish;
            }
        }
        bool block = escape_.isEscaped() || (!config_.allowEnglishBypass && engProt_.bias == LanguageBias::HardEnglish);
        // See 2a: vowel-less abbreviation chain exception for stroke-d (#221).
        if (block && !escape_.isEscaped() && action == TypingAction::VniStroke &&
            !HasVowelState(states_.data(), states_.size())) {
            block = false;
        }
        if (block && !escape_.isEscaped() && !config_.spellExclusions.empty() && WouldModifierKeyMatchExclusion(lower)) block = false;

        if (!block) {
            bool canApply = true;
            if (effectiveSpellCheck && spellCheckDisabled_) {
                canApply = WouldModifierRecoverOrEscape(action, keyChar, lower);
            }
            if (canApply && ProcessModifier(action, keyChar)) {
                engProt_.bias = LanguageBias::Vietnamese;
                ApplyAutoUO();
                UpdateSpellState();
                return true;
            }
        }
    }

    // 2d. User-defined ONLY actions (HornOrInsertU, InsertABreve, ...)
    // Mirror 2a's English-protection guards: stale HardEnglish bias from
    // failed free-mark must block ư insertion just like it blocks HornW in
    // Telex (regression 2026-05-18: `revie + w` in UserDefined with
    // w=HornOrInsertUNoStart produced `revieư` because 2d had no bias check).
    if (userGated && isUserOnly) {
        bool block = escape_.isEscaped() ||
                     (!config_.allowEnglishBypass && engProt_.bias == LanguageBias::HardEnglish);
        if (!block && effectiveSpellCheck &&
            IsBlockedEnglishModifier(rawInput_.data(), rawInput_.size())) block = true;
        if (block && !escape_.isEscaped() && !config_.spellExclusions.empty() &&
            WouldModifierKeyMatchExclusion(lower)) block = false;

        if (!block && ProcessModifier(action, keyChar)) {
            engProt_.bias = LanguageBias::Vietnamese;
            ApplyAutoUO();
            UpdateSpellState();
            return true;
        }
    }

    return false;
}

bool TypingEngine::WouldModifierRecoverOrEscape(TypingAction action, wchar_t keyChar, wchar_t lower) {
    bool canEscape = false;
    if (IsTelexModifierAction(action)) {
        if (lower == L'w') {
            canEscape = HasEscapableModifier(states_.data(), states_.size(), Modifier::Horn) ||
                        HasEscapableModifier(states_.data(), states_.size(), Modifier::Breve);
        } else if (lower == L'd') {
            canEscape = HasEscapableModifier(states_.data(), states_.size(), Modifier::Stroke, true) ||
                        (FindStrokeDTarget(states_.data(), states_.size()) != SIZE_MAX);
        } else if (IsVowelChar(keyChar) && !states_.empty()) {
            const CharState& last = states_.back();
            if (last.IsVowel() && last.base == lower && last.mod == Modifier::Circumflex) canEscape = true;
        } else if ((action == TypingAction::HornInsertO || action == TypingAction::HornInsertU) &&
                   !states_.empty() && rawInput_.size() >= 2 &&
                   towlower(rawInput_[rawInput_.size() - 2]) == lower) {
            // Bracket escape recognition: `[[`/`]]` (or any UserDefined-mapped
            // duplicate trigger) must reach HandleHornInsert to undo the
            // just-inserted ơ/ư. Without this, spell-check-disabled (set after
            // first `]` makes the resulting "aư" invalid) skips ProcessModifier
            // and the second `]` lands in the literal-char path — which calls
            // RelocateToneToTarget and hijacks the hỏi tone from `a` onto ư
            // (bug 2026-05-21: tar]] → taử] instead of tả]).
            const wchar_t baseVowel = (action == TypingAction::HornInsertO) ? L'o' : L'u';
            const CharState& last = states_.back();
            if (last.base == baseVowel && last.mod == Modifier::Horn) canEscape = true;
        }
    } else { // VNI
        Modifier escMod = ActionToVniModifier(action);
        if (escMod == Modifier::Stroke) {
            canEscape = HasEscapableModifier(states_.data(), states_.size(), escMod, true) ||
                        (FindStrokeDTarget(states_.data(), states_.size()) != SIZE_MAX);
        } else if (escMod != Modifier::None) {
            canEscape = HasEscapableModifier(states_.data(), states_.size(), escMod);
        }
    }

    if (!canEscape) canEscape = WouldModifierKeyMatchExclusion(lower);
    return canEscape || IsToneStopCodaMismatch();
}

//-----------------------------------------------------------------------------
// Tone Processing
//-----------------------------------------------------------------------------

bool TypingEngine::ToneMatchesExclusion(size_t targetIdx, Tone requestedTone) const noexcept {
    if (config_.spellExclusions.empty() || targetIdx == SIZE_MAX) return false;
    CharState tentative = states_[targetIdx];
    tentative.tone = requestedTone;
    wchar_t tonedCh = Compose(tentative);
    return WouldToneMatchExclusion(states_.data(), states_.size(),
        config_.spellExclusions,
        [](const CharState& s) { return Compose(s); },
        targetIdx, tonedCh);
}

bool TypingEngine::ProcessTone(Tone newTone, wchar_t keyChar, size_t cachedTarget) {
    if (newTone == Tone::None) return false;

    size_t targetIdx = (cachedTarget != SIZE_MAX) ? cachedTarget : FindToneTarget();
    if (targetIdx == SIZE_MAX) return false;

    CharState& target = states_[targetIdx];

    // Escape: same tone → clear tone and put the consumed key back as a literal
    if (target.tone == newTone) {
        target.tone = Tone::None;
        const size_t consumedRawIdx = target.toneRawIdx;
        target.toneRawIdx = SIZE_MAX;
        // Invalidates `target` (states_ may grow) — nothing below reads it.
        RestoreConsumedKeyInPlace(consumedRawIdx, keyChar);
        escape_.escape(EscapeKind::Tone);  // Signal caller: user canceled tone
        return true;
    }

    // Apply or replace tone
    target.tone = newTone;
    target.toneRawIdx = rawInput_.size() - 1;
    escape_.clear();
    return true;
}

//-----------------------------------------------------------------------------
// Clear Tone (z key) — remove any existing tone
//-----------------------------------------------------------------------------

bool TypingEngine::ProcessClearTone() {
    size_t targetIdx = FindToneTarget();
    if (targetIdx == SIZE_MAX) return false;

    CharState& target = states_[targetIdx];
    if (target.tone == Tone::None) return false;

    target.tone = Tone::None;
    target.toneRawIdx = SIZE_MAX;
    return true;
}

//-----------------------------------------------------------------------------
// Unified modifier dispatcher — single switch(TypingAction) covering both
// Telex and VNI modifier actions. PushChar gates per-mode pre-checks (English
// protection, escape, spell-check disabled) and then routes the approved
// action here. Handler signature is uniform `(TypingAction, wchar_t)` so this
// stays a flat fan-out — foundation for G-4 customKeyMap.
//-----------------------------------------------------------------------------

bool TypingEngine::ProcessModifier(TypingAction action, wchar_t c) {
    switch (action) {
        case TypingAction::HornInsertO:
        case TypingAction::HornInsertU:   return bracketProposal_.tryApply(action, c);
        case TypingAction::HornW:         return hornModifierProposal_.tryApply(action, c);
        case TypingAction::CircumflexA:
        case TypingAction::CircumflexE:
        case TypingAction::CircumflexO:   return adjacentCircumflexProposal_.tryApply(action, c);
        case TypingAction::StrokeD:       return strokeDProposal_.tryApply(action, c);
        case TypingAction::VniCircumflex: return vniCircumflexProposal_.tryApply(action, c);
        case TypingAction::VniHorn:       return vniHornProposal_.tryApply(action, c);
        case TypingAction::VniBreve:      return vniBreveProposal_.tryApply(action, c);
        case TypingAction::VniStroke:     return HandleVniStroke(action, c);

        // -- UserDefined ONLY actions --
        case TypingAction::HornOrInsertU:
        case TypingAction::HornOrInsertUNoStart: return HandleHornOrInsertU(action, c);
        case TypingAction::UndoAllMarks:         return HandleUndoAllMarks(action, c);

        // -- Direct char insertion --
        case TypingAction::InsertABreve:
        case TypingAction::InsertABreveUpper:
        case TypingAction::InsertACircumflex:
        case TypingAction::InsertACircumflexUpper:
        case TypingAction::InsertDStroke:
        case TypingAction::InsertDStrokeUpper:
        case TypingAction::InsertECircumflex:
        case TypingAction::InsertECircumflexUpper:
        case TypingAction::InsertOCircumflex:
        case TypingAction::InsertOCircumflexUpper:
        case TypingAction::InsertOHorn:
        case TypingAction::InsertOHornUpper:
        case TypingAction::InsertUHorn:
        case TypingAction::InsertUHornUpper:
            return HandleInsertChar(action, c);

        default: return false;
    }
}

bool TypingEngine::HandleVniStroke(TypingAction action, wchar_t c) {
    return HandleStrokeD(action, c);
}

bool TypingEngine::HandleHornOrInsertU(TypingAction action, wchar_t keyChar) {
    // NoStart variant: must short-circuit BEFORE HandleHornW because HornW's
    // P8 fallback synthesises ư at empty buffer — defeating "no insert at word
    // start". With this guard, PushChar's outer fallthrough treats the key as
    // a literal char (user feedback 2026-05-17: w → Ư at word start).
    if (action == TypingAction::HornOrInsertUNoStart && states_.empty()) {
        return false;
    }

    // 1. Try to apply horn to existing vowel (same as HornW P1/P5/P6, plus P8
    //    fallback for plain HornOrInsertU at empty buffer)
    if (HandleHornW(TypingAction::HornW, keyChar)) {
        return true;
    }

    // 2. Plain variant fallback (consonant-onset-only buffer, e.g. "th" + w):
    //    insert ư as a fresh state. Mark it synthetic so a subsequent press
    //    of the same key triggers the ww-style full revert via HandleHornW
    //    P4 (synthetic + last-state → erase ư entirely, add literal).
    //    Without this, double-press lands in the regular escape path
    //    (ư → u + literal), producing e.g. `th + w + w → thuw` instead of `thw`.
    //
    // Same "no Vietnamese word has a vowel followed by standalone ư" rule as
    // HornW's P8 (see HasNonClusterVowel) — without it, English words with a
    // vowel already in the buffer (view, review, new, know...) get a bogus
    // ư appended on the first press instead of staying literal.
    if (HasNonClusterVowel()) return false;

    const size_t beforeSize = states_.size();
    if (HandleHornInsert(TypingAction::HornInsertU, keyChar)) {
        if (states_.size() > beforeSize) {
            states_.back().synthetic = true;
            // HandleHornInsert ignores case (bracket keys have none). With HornW's
            // P8 now off in UserDefined (issue #205), this fallback is the primary
            // insert path for the "Móc hoặc ư" options — preserve W→Ư as P8 did.
            states_.back().isUpper = iswupper(keyChar) != 0;
        }
        return true;
    }
    return false;
}

bool TypingEngine::HandleUndoAllMarks(TypingAction /*action*/, wchar_t /*keyChar*/) {
    if (states_.empty()) return false;

    bool modified = false;
    for (auto& s : states_) {
        if (s.mod != Modifier::None || s.tone != Tone::None) {
            s.mod = Modifier::None;
            s.tone = Tone::None;
            s.toneRawIdx = SIZE_MAX;
            modified = true;
        }
    }

    if (modified) {
        escape_.escape(EscapeKind::Tone); // Reuse tone escape to block re-trigger
        return true;
    }
    return false;
}

bool TypingEngine::HandleInsertChar(TypingAction action, wchar_t keyChar) {
    wchar_t base = 0;
    Modifier mod = Modifier::None;
    bool upper = false;

    switch (action) {
        case TypingAction::InsertABreve:           base = L'a'; mod = Modifier::Breve; break;
        case TypingAction::InsertABreveUpper:      base = L'a'; mod = Modifier::Breve; upper = true; break;
        case TypingAction::InsertACircumflex:      base = L'a'; mod = Modifier::Circumflex; break;
        case TypingAction::InsertACircumflexUpper: base = L'a'; mod = Modifier::Circumflex; upper = true; break;
        case TypingAction::InsertDStroke:          base = L'd'; mod = Modifier::Stroke; break;
        case TypingAction::InsertDStrokeUpper:     base = L'd'; mod = Modifier::Stroke; upper = true; break;
        case TypingAction::InsertECircumflex:      base = L'e'; mod = Modifier::Circumflex; break;
        case TypingAction::InsertECircumflexUpper: base = L'e'; mod = Modifier::Circumflex; upper = true; break;
        case TypingAction::InsertOCircumflex:      base = L'o'; mod = Modifier::Circumflex; break;
        case TypingAction::InsertOCircumflexUpper: base = L'o'; mod = Modifier::Circumflex; upper = true; break;
        case TypingAction::InsertOHorn:            base = L'o'; mod = Modifier::Horn; break;
        case TypingAction::InsertOHornUpper:       base = L'o'; mod = Modifier::Horn; upper = true; break;
        case TypingAction::InsertUHorn:            base = L'u'; mod = Modifier::Horn; break;
        case TypingAction::InsertUHornUpper:       base = L'u'; mod = Modifier::Horn; upper = true; break;
        default: return false;
    }

    // Escape: doubled action key (e.g. `[[` mapped to InsertOHorn) undoes the
    // just-inserted glyph and emits the key literal — mirrors the Telex
    // bracket escape in HandleHornInsert so UserDefined "Chữ ơ/ư/â/ê/ô/ă/đ"
    // bindings respect the same press-twice-to-revert UX.
    if (!states_.empty() && rawInput_.size() >= 2 &&
        towlower(rawInput_[rawInput_.size() - 2]) == towlower(keyChar)) {
        CharState& last = states_.back();
        if (last.base == base && last.mod == mod) {
            size_t consumedIdx = last.rawIdx;
            states_.pop_back();
            EraseConsumedRaw(consumedIdx);
            ProcessChar(keyChar);
            EscapeKind escapeKind = EscapeKind::Modifier;
            switch (mod) {
                case Modifier::Horn:       escapeKind = EscapeKind::Horn; break;
                case Modifier::Circumflex: escapeKind = EscapeKind::Circumflex; break;
                case Modifier::Breve:      escapeKind = EscapeKind::Breve; break;
                case Modifier::Stroke:     escapeKind = EscapeKind::Stroke; break;
                default: break;
            }
            escape_.escape(escapeKind);
            return true;
        }
    }

    CharState s;
    s.base = base;
    s.mod = mod;
    s.isUpper = upper;
    s.rawIdx = rawInput_.empty() ? 0 : rawInput_.size() - 1;
    states_.push_back(s);
    return true;
}

// Thin wrappers giving the Process* implementation methods a uniform
// `(TypingAction, wchar_t)` shape so ProcessModifier can dispatch them
// alongside the natively-uniform handlers (HandleHornInsert,
// HandleAdjacentCircumflex). The `action` arg is unused for handlers
// that are 1:1 with their action; HandleVniCircumflex/Breve forward to
// the Modifier-parameterised VNI vowel modifier helper.

bool TypingEngine::HandleVniCircumflex(TypingAction /*action*/, wchar_t c) {
    return ProcessVniVowelModifier(Modifier::Circumflex, c);
}

bool TypingEngine::HandleVniBreve(TypingAction /*action*/, wchar_t c) {
    return ProcessVniVowelModifier(Modifier::Breve, c);
}

bool TypingEngine::HandleHornInsert(TypingAction action, wchar_t c) {
    // SimpleTelex omits bracket keys — let `[`/`]` fall through to literal.
    if (config_.inputMethod == InputMethod::SimpleTelex) return false;

    const wchar_t baseVowel = (action == TypingAction::HornInsertO) ? L'o' : L'u';

    // Escape: doubled action key (e.g. `[[`, `]]`, or `qq` if user remapped
    // HornInsertO in UserDefined) undoes the just-inserted ơ/ư and produces
    // the key literal. Comparing rawInput[size-2] to `c` (instead of a
    // hardcoded `[`/`]`) lets the escape work for any key bound to the
    // action in UserDefined mode — Telex still hits via the same path
    // because ClassifyKey only routes `[`/`]` to HornInsertO/U.
    if (!states_.empty() && rawInput_.size() >= 2 &&
        towlower(rawInput_[rawInput_.size() - 2]) == towlower(c)) {
        CharState& last = states_.back();
        if (last.base == baseVowel && last.mod == Modifier::Horn) {
            size_t consumedIdx = last.rawIdx;
            states_.pop_back();
            EraseConsumedRaw(consumedIdx);
            ProcessChar(c);
            escape_.escape(EscapeKind::Horn);
            return true;
        }
    }

    CharState s;
    s.base = baseVowel;
    s.mod = Modifier::Horn;
    s.isUpper = activeKeyUppercase_;
    s.rawIdx = rawInput_.empty() ? 0 : rawInput_.size() - 1;
    states_.push_back(s);
    return true;
}

bool TypingEngine::HandleAdjacentCircumflex(TypingAction action, wchar_t c) {
    // Dispatcher only routes a/e/o here; defensively bail when there's no
    // prior state to apply circumflex against.
    if (states_.empty()) return false;
    const wchar_t lower = towlower(c);

    CharState& last = states_.back();

    // Problem 6: escape logic must match the action's target vowel in UserDefined mode
    const wchar_t targetBase = (config_.inputMethod == InputMethod::UserDefined)
        ? ActionToVowel(action)
        : lower;

    if (last.IsVowel() && last.base == targetBase) {
        // Guard: don't apply circumflex if last 3 vowels form a triphthong
        // e.g., "ngoeo" + 'o' → consume but don't modify (triphthong complete)
        size_t n = states_.size();
        if (n >= 3 && states_[n - 3].IsVowel() && states_[n - 2].IsVowel() &&
            IsTriphthong(states_[n - 3].base, states_[n - 2].base, last.base)) {
            return true;  // Consume keystroke, no state change
        }
        // Escape: already has circumflex (adjacent case — last char, no coda possible)
        if (last.mod == Modifier::Circumflex) {
            last.mod = Modifier::None;
            ProcessChar(c);
            // With spell check ON, block further modifiers to prevent
            // oo→ô→oo→ô re-trigger cycle ("oo" is ValidPrefix in spell
            // checker since "oong" is valid, so spellCheckDisabled_ alone
            // doesn't catch it — unlike "aa"/"ee" which are Invalid).
            if (config_.spellCheckEnabled) {
                escape_.escape(EscapeKind::Circumflex);
            }
            return true;
        }
        // Pre-state drives both validation and apply paths so the speculate
        // path mirrors the runtime path (invariant from ad09f15).
        //
        //  - ValidPrefix: user is mid-construction (e.g. "vịe" → "việ", "ngùo"
        //    → "nguồ"). Apply circumflex AND relocate tone so the resulting
        //    diphthong (iê, uô, …) attracts the tone to the correct vowel.
        //    Mirrors free-marking branch L1024-1030.
        //  - Valid: syllable already complete ("của", "ca") → adjacent
        //    modifier is most likely a typo. Keep mod-only path so
        //    "cuara" + 'a' rejects to "cuara" instead of over-accepting
        //    to "cuậ" (typo guard from ad09f15).
        //  - Invalid: not a syllable at all → mod-only path rejects.
        auto preState = Phonology::ValidateSyllableState(
            states_.data(), states_.size(), config_.allowZwjf);
        const bool needsRelocate =
            (preState == Phonology::SyllableState::ValidPrefix);

        if (needsRelocate) {
            if (!WouldBeValidSyllable(states_.size() - 1, Modifier::Circumflex,
                                      /*clearCircumflexIdx=*/SIZE_MAX,
                                      /*speculateRelocateTone=*/true)
                && !WouldModifierKeyMatchExclusion(c)) {
                return false;
            }
        } else {
            // Reject adjacent circumflex when the result is an invalid
            // syllable — catches split tone/mod typos ("của" + 'a' → c,ủ,â)
            // via SpellCheck's tone/mod invariant and structural invalidity
            // ("hò" + 'a' + 'a' → h,ò,â with "âo" not in vowel table).
            if (ShouldRejectModifier(states_.size() - 1,
                                     Modifier::Circumflex, targetBase)) {
                return false;  // Fall through to ProcessChar — add vowel literally
            }
        }
        // Apply circumflex - PRESERVE FIRST LETTER CASE
        last.mod = Modifier::Circumflex;
        // A replacement ươ -> uô changes both quality marks, just as the
        // free-marking and VNI switch paths do.
        UndoHornU(states_.data(), states_.size() - 1);
        // Always relocate (not just when needsRelocate/ValidPrefix): with
        // spell check off, the Valid branch above skips WouldBeValidSyllable
        // entirely, so a tone already sitting on the pre-modifier vowel (e.g.
        // "lụa" + 'a' → tone on 'u') would otherwise stay stranded there
        // ("lụâ") until a later keystroke's FinalizeRegularChar relocated it.
        // RelocateToneToTarget() is already a no-op when the tone is absent
        // or already on the right vowel, so this is safe for the mod-only
        // (spell-check-on, non-relocate) path too.
        RelocateToneToTarget();
        return true;
    }

    // Free marking: backward scan for circumflex across intervening chars
    // e.g., "tieng" + 'e' → "tiêng", "cau" + 'a' → "câu", "chieu" + 'e' → "chiêu"
    // Crosses consonants freely; crosses vowels only with spell-check validation (if enabled)

    // GUARD: don't apply cross-vowel circumflex if it completes a contiguous triphthong
    // e.g., "ngoe" + 'o' -> forms "o e o" triphthong, so let it be "ngoeo" instead of "ngôe"
    size_t n = states_.size();
    if (n >= 2 && states_[n - 1].IsVowel() && states_[n - 2].IsVowel() &&
        IsTriphthong(states_[n - 2].base, states_[n - 1].base, targetBase)) {
        return false;
    }

    bool needsValidation = false;  // True when crossing different vowels
    int consonantsCrossed = 0;     // Count consonants in path (for same-vowel coda check)
    wchar_t singleCoda = 0;        // Base of single consonant crossed (for coda validation)
    for (auto it = states_.rbegin(); it != states_.rend(); ++it) {
        if (it->IsVowel() && it->base != targetBase) { needsValidation = true; continue; }
        if (!it->IsVowel()) {
            if (consonantsCrossed == 0) singleCoda = it->base;
            ++consonantsCrossed;
        }
        if (it->IsVowel() && it->base == targetBase) {
            // Reject cross-vowel if: unsupported modifier
            // Horn undo (ươ→uô) always allowed across vowels
            if (needsValidation && it->mod != Modifier::Horn &&
                 (it->mod != Modifier::None && it->mod != Modifier::Circumflex)) break;

            if (it->mod == Modifier::Circumflex) {
                it->mod = Modifier::None;
                ProcessChar(c);
                escape_.escape(EscapeKind::Circumflex);
                return true;
            }
            if (it->mod == Modifier::Breve && targetBase == L'a') {
                it->mod = Modifier::Circumflex;
                RelocateToneToTarget();
                return true;
            }
            if (it->mod == Modifier::Horn && targetBase == L'o') {
                auto oIndex = static_cast<size_t>(states_.rend() - it - 1);
                it->mod = Modifier::Circumflex;
                UndoHornU(states_.data(), oIndex);
                RelocateToneToTarget();
                return true;
            }
            if (it->mod == Modifier::None) {
                // Spell check ON: validate the RESULT of applying circumflex.
                //   Rejects "vào" + 'a' → "vầo" (invalid syllable) while
                //   allowing "cau" + 'a' → "câu" and "chieu" + 'e' → "chiêu".
                // Spell check OFF, cross-vowel + consonant: always reject.
                //   Vietnamese circumflex never crosses diff-vowel + consonant
                //   (dấu mũ ở SAU trong iê/uô → match trước needsValidation;
                //    dấu mũ ở TRƯỚC trong âu/ây/êu/ôi → không có coda).
                //   Catches "readme" (e←a←dm←e) and "review" (e←v←i←e).
                // Spell check OFF, same-vowel: reject if single consonant is not
                //   a valid Vietnamese coda (c/m/n/p/t). Catches "release" (e→l→e)
                //   while allowing "hiên" (e→n→e) and "tiêng" (e→ng→e).
                if (needsValidation) {
                    if (config_.spellCheckEnabled) {
                        size_t targetIdx =
                            static_cast<size_t>(states_.rend() - it - 1);
                        if (ShouldRejectModifier(targetIdx, Modifier::Circumflex, targetBase))
                            break;
                    } else if (consonantsCrossed >= 1) {
                        engProt_.bias = LanguageBias::HardEnglish;
                        break;
                    }
                } else if (!config_.spellCheckEnabled && consonantsCrossed == 1) {
                    bool validCoda = (singleCoda == L'c' || singleCoda == L'k' ||
                                      singleCoda == L'm' || singleCoda == L'n' ||
                                      singleCoda == L'p' || singleCoda == L't');
                    if (!validCoda) {
                        engProt_.bias = LanguageBias::HardEnglish;
                        break;
                    }
                } else if (config_.spellCheckEnabled && consonantsCrossed >= 1) {
                    // Same-vowel free-marking across consonants: reject
                    // transformations that produce invalid syllables.
                    // Catches "gacha" → "gâch", "bacha" → "bâch", etc.
                    // — âch/ăch are not valid Vietnamese codas.
                    // Speculate WITH tone relocation because the runtime
                    // below calls RelocateToneToTarget after applying the
                    // modifier — without it, "súat" + 'a' speculates as
                    // sắc-on-`u` of `uâ` (Invalid) and the legitimate
                    // promotion to "suất" gets rejected.
                    size_t targetIdx = static_cast<size_t>(states_.rend() - it - 1);
                    if (!WouldBeValidSyllable(targetIdx, Modifier::Circumflex,
                                              /*clearCircumflexIdx=*/SIZE_MAX,
                                              /*speculateRelocateTone=*/true)) break;
                }
                it->mod = Modifier::Circumflex;
                RelocateToneToTarget();
                return true;
            }
            break;
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
// HasNonClusterVowel — true if the buffer already has a vowel that isn't the
// 'i' of a potential "gi" consonant cluster. Shared by HandleHornW's P8 and
// HandleHornOrInsertU's insert fallback: standalone ư never follows a vowel
// like this in real Vietnamese (no word has a vowel + standalone ư).
//-----------------------------------------------------------------------------

bool TypingEngine::HasNonClusterVowel() const noexcept {
    for (size_t i = 0; i < states_.size(); ++i) {
        if (!states_[i].IsVowel()) continue;
        // 'i' after 'g' is a potential "gi" cluster consonant — don't count it
        if (states_[i].base == L'i' && i > 0 && states_[i - 1].base == L'g') continue;
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
// HandleHornW — Telex `w` modifier (EXPLICIT PRIORITY ORDER P1-P8)
//-----------------------------------------------------------------------------

bool TypingEngine::HandleHornW(TypingAction /*action*/, wchar_t c) {
    // Simple Telex: 'w' only acts as modifier when preceded by a/o/u vowel
    if (config_.inputMethod == InputMethod::SimpleTelex) {
        bool hasVowelContext = false;
        for (const auto& s : states_) {
            if (s.IsVowel() && (s.base == L'a' || s.base == L'o' || s.base == L'u')) {
                hasVowelContext = true;
                break;
            }
        }
        if (!hasVowelContext) return false;  // Let PushChar add 'w' as literal
    }

    // Check if 'u' at position i is part of QU consonant cluster
    // QU-cluster 'u' should not be treated as a modifiable vowel
    // Only unmodified 'u' qualifies — a horned ư (e.g., P8 synthetic) is not a cluster 'u'.
    auto isQUClusterU = [this](size_t i) -> bool {
        return i > 0 && states_[i].base == L'u' && states_[i].mod == Modifier::None
            && states_[i - 1].base == L'q';
    };

    // PRIORITY ORDER for 'w':
    // P1: "ua" pattern → apply horn to 'u' (mưa, được)
    // P2: "uo" pair horn cycle — default ươ, edge prefixes (h/th/kh) get 3-state
    // P3: "oa" pattern → apply breve to 'a' (hoặc)
    // P4: Escape - if already have horn/breve, second 'w' clears it
    // P5: Standalone 'u' → horn
    // P6: Standalone 'o' (not in oa/uo) → horn
    // P7: Standalone 'a' → breve

    // Analyze current state (skip QU-cluster 'u' for modification targets)
    bool hasUA = false, hasOA = false, hasUO = false, hasUU = false;
    size_t uIdx = SIZE_MAX, firstUIdx = SIZE_MAX, oIdx = SIZE_MAX, aIdx = SIZE_MAX;
    size_t hornedIdx = SIZE_MAX, brevedIdx = SIZE_MAX;

    for (size_t i = 0; i < states_.size(); ++i) {
        if (!states_[i].IsVowel()) continue;

        // Skip 'u' in QU cluster — it's a consonant, not modifiable
        if (isQUClusterU(i)) continue;

        wchar_t base = states_[i].base;
        Modifier mod = states_[i].mod;

        if (base == L'u') {
            if (mod == Modifier::Horn) hornedIdx = i;
            else if (mod == Modifier::None) {
                if (firstUIdx == SIZE_MAX) firstUIdx = i;  // first unmodified u
                uIdx = i;  // last unmodified u
            }
        } else if (base == L'o') {
            if (mod == Modifier::Horn) hornedIdx = i;
            else if (mod == Modifier::None || mod == Modifier::Circumflex) oIdx = i;
        } else if (base == L'a') {
            if (mod == Modifier::Breve) brevedIdx = i;
            else if (mod == Modifier::None || mod == Modifier::Circumflex) aIdx = i;
        }
    }

    // Detect vowel patterns and uo pair indices (skip QU-cluster 'u')
    size_t pairU = SIZE_MAX, pairO = SIZE_MAX;
    for (size_t i = 0; i + 1 < states_.size(); ++i) {
        if (states_[i].IsVowel() && states_[i+1].IsVowel()) {
            if (isQUClusterU(i)) continue;
            wchar_t first = states_[i].base;
            wchar_t second = states_[i+1].base;
            if (first == L'u' && second == L'a') hasUA = true;
            if (first == L'o' && second == L'a') hasOA = true;
            if (first == L'u' && second == L'o') { hasUO = true; pairU = i; pairO = i + 1; }
            if (first == L'u' && second == L'u') hasUU = true;
        }
    }

    // P1: "ua" pattern → horn on 'u' (mưa, được, thưa)
    if (hasUA && uIdx != SIZE_MAX) {
        states_[uIdx].mod = Modifier::Horn;
        if (aIdx != SIZE_MAX && states_[aIdx].mod == Modifier::Circumflex) {
            states_[aIdx].mod = Modifier::None;
        }
        RelocateToneToHornVowel();
        return true;
    }

    // P2: "uo" pair — horn cycle
    // Non-edge: uo → ươ → uo (two-press cycle).
    // Edge (h/th/kh): uo → uơ → ươ → uo (three-press cycle, uơ first).
    // ApplyAutoUO() will auto-complete uơ → ươ when followed by another char.
    if (hasUO && pairU != SIZE_MAX) {
        Modifier uMod = states_[pairU].mod;
        Modifier oMod = states_[pairO].mod;
        bool isEdge = IsUOEdgeCasePrefix(states_.data(), states_.size(), pairU);

        // Forward (non-edge): uo/uô → ươ (first press, covers circumflex replacement)
        // Forward (edge h/th/kh): uo → uơ (first press — default to uơ for huơ/khuơ)
        if (uMod != Modifier::Horn && (oMod == Modifier::None || oMod == Modifier::Circumflex)) {
            if (isEdge) {
                states_[pairO].mod = Modifier::Horn;
            } else {
                states_[pairU].mod = Modifier::Horn;
                states_[pairO].mod = Modifier::Horn;
            }
            RelocateToneToHornVowel();
            return true;
        }

        // Forward: ưo → ươ (u already horned via P5, now complete pair)
        if (uMod == Modifier::Horn && oMod == Modifier::None) {
            states_[pairO].mod = Modifier::Horn;
            RelocateToneToHornVowel();
            return true;
        }

        // Forward (edge): uơ → ươ (h/th/kh second press — complete the pair)
        if (uMod == Modifier::None && oMod == Modifier::Horn && isEdge) {
            states_[pairU].mod = Modifier::Horn;
            RelocateToneToHornVowel();
            return true;
        }

        // Escape: ươ → uo (third press for h/th/kh, second press for others)
        if (uMod == Modifier::Horn && oMod == Modifier::Horn) {
            states_[pairU].mod = Modifier::None;
            states_[pairO].mod = Modifier::None;
            ProcessChar(c);
            escape_.escape(EscapeKind::Horn);
            return true;
        }

        // Escape: uơ → uo (non h/th/kh, or any remaining uơ state)
        if (uMod == Modifier::None && oMod == Modifier::Horn) {
            states_[pairO].mod = Modifier::None;
            ProcessChar(c);
            escape_.escape(EscapeKind::Horn);
            return true;
        }
    }

    // P3: "oa" pattern → breve on 'a' (hoặc)
    if (hasOA && aIdx != SIZE_MAX) {
        states_[aIdx].mod = Modifier::Breve;
        return true;
    }

    // P4: Escape - clear existing modifier and add 'w' as literal
    // Must be before standalone applications (P5-P7) so that second 'w'
    // escapes the first modification.
    if (hornedIdx != SIZE_MAX) {
        // Special case: P8-synthesized ư (ww → w escape)
        // Only erase when synthetic ư is the last state (immediate ww sequence).
        // If other chars were typed after the synthetic ư (e.g., "window"),
        // it's now part of a word — do regular escape instead.
        if (states_[hornedIdx].synthetic && hornedIdx == states_.size() - 1) {
            // Preserve the case of the first keystroke (the 'W' that produced Ư).
            // Passing the raw second keystroke drops uppercase intent: W + w → w
            // instead of W. Force case on the escape output to match the original.
            bool origUpper = states_[hornedIdx].isUpper;
            states_.erase(states_.begin() + static_cast<ptrdiff_t>(hornedIdx));
            ProcessChar(origUpper ? towupper(c) : towlower(c));
            escape_.escape(EscapeKind::Horn);
            return true;
        }
        UndoHornU(states_.data(), hornedIdx);  // Undo companion 'u' horn BEFORE clearing 'o'
        states_[hornedIdx].mod = Modifier::None;
        ProcessChar(c);
        escape_.escape(EscapeKind::Horn);
        return true;
    }
    if (brevedIdx != SIZE_MAX) {
        states_[brevedIdx].mod = Modifier::None;
        ProcessChar(c);
        escape_.escape(EscapeKind::Breve);
        return true;
    }

    // When a modifier would apply to a vowel with intervening non-vowel state(s)
    // before end of buffer, validate that the resulting syllable is not Invalid.
    // Rejects "gachw" → "găch", "congw" → "cơng", "hunw" → "hưn", etc.
    auto hasNonVowelAfter = [this](size_t idx) {
        for (size_t i = idx + 1; i < states_.size(); ++i) {
            if (!states_[i].IsVowel()) return true;
        }
        return false;
    };

    // P5: Standalone 'u' → horn
    // For "uu" pattern, horn goes on FIRST 'u' (lưu, cưu, hưu): the second 'u' is the glide.
    if (uIdx != SIZE_MAX) {
        size_t targetU = (hasUU && firstUIdx != SIZE_MAX) ? firstUIdx : uIdx;
        // Pass aIdx so the validation matches the real mutation (u→horn and,
        // if present, strip sister â back to a). Example: "uaan" + w.
        if (hasNonVowelAfter(targetU) &&
            !WouldBeValidSyllable(targetU, Modifier::Horn, aIdx)) {
            return false;
        }
        states_[targetU].mod = Modifier::Horn;
        if (aIdx != SIZE_MAX && states_[aIdx].mod == Modifier::Circumflex) {
            states_[aIdx].mod = Modifier::None;
        }
        RelocateToneToHornVowel();
        return true;
    }

    // P6: Standalone 'o' (not in oa pattern) → horn (replaces circumflex: ô→ơ)
    if (oIdx != SIZE_MAX && !hasOA) {
        if (hasNonVowelAfter(oIdx) && !WouldBeValidSyllable(oIdx, Modifier::Horn)) {
            return false;
        }
        states_[oIdx].mod = Modifier::Horn;
        RelocateToneToHornVowel();
        return true;
    }

    // P7: Standalone 'a' → breve (or switch circumflex → breve: â→ă)
    if (aIdx != SIZE_MAX && (states_[aIdx].mod == Modifier::None ||
                              states_[aIdx].mod == Modifier::Circumflex)) {
        if (hasNonVowelAfter(aIdx) && !WouldBeValidSyllable(aIdx, Modifier::Breve)) {
            return false;
        }
        states_[aIdx].mod = Modifier::Breve;
        return true;
    }

    // P8: Full Telex only — standalone 'w' with no modifiable vowel → insert ư
    // Only fires when there are no vowels yet in the buffer (onset consonants or empty),
    // OR when the only vowel is 'i' in a potential "gi" consonant cluster (giữ, giữa, giường).
    // After a non-cluster vowel (e.g., "re" + w), 'w' is treated as literal — no Vietnamese
    // word has a vowel followed by standalone ư via P8.
    // In QU cluster, don't insert standalone ư (let 'w' be literal: "quew" → "quew")
    //
    // UserDefined excluded (issue #205): there, HornW backs the "Móc chung
    // (ă,ư,ơ)" option, which is combine-ONLY — `a/u/o + w` → `ă/ư/ơ`, and a
    // standalone `w` stays literal. Inserting a fresh ư is the job of the two
    // "Móc hoặc ư" options (HornOrInsertU / HornOrInsertUNoStart), which call
    // HandleHornW for the combine step and then run their own HandleHornInsert
    // fallback. Full Telex / Combined keep P8 so word-initial ư types as `w`.
    if (config_.inputMethod != InputMethod::SimpleTelex &&
        config_.inputMethod != InputMethod::UserDefined && !IsInQUCluster()) {
        if (!HasNonClusterVowel()) {
            CharState s;
            s.base = L'u';
            s.mod = Modifier::Horn;
            s.isUpper = iswupper(c);
            s.synthetic = true;  // Mark as P8-synthesized (ww escape removes it entirely)
            s.rawIdx = rawInput_.empty() ? 0 : rawInput_.size() - 1;
            states_.push_back(s);
            return true;
        }
    }

    return false;
}

//-----------------------------------------------------------------------------
// HandleStrokeD — Telex `dd` / VNI `9` (dd → đ)
// Scan logic via FindStrokeDTarget (EngineHelpers.h)
//-----------------------------------------------------------------------------

bool TypingEngine::HandleStrokeD(TypingAction /*action*/, wchar_t c) {
    if (escape_.isEscaped(EscapeKind::Stroke)) return false;
    size_t dIdx = FindStrokeDTarget(states_.data(), states_.size());
    if (dIdx == SIZE_MAX) return false;

    CharState& target = states_[dIdx];
    if (target.mod == Modifier::None) {
        target.mod = Modifier::Stroke;
        return true;
    } else if (target.mod == Modifier::Stroke) {
        if (!IsStrokeDEscapeAllowed(states_.data(), states_.size(), dIdx)) return false;
        // Spell-exclusion escape hatch (issue #200): the leading-Đ@0 toggle-back
        // would clobber Đ-initial chains the user explicitly allowed (e.g. đcđt
        // typed as ddcddt). If keeping Đ still matches an exclusion prefix, treat
        // this d as a fresh literal so the chain can finish building the excluded
        // word instead of un-stroking the leading Đ.
        if (dIdx == 0 && !config_.spellExclusions.empty() &&
            WouldModifierKeyMatchExclusion(towlower(c))) {
            return false;
        }
        target.mod = Modifier::None;
        escape_.escape(EscapeKind::Stroke);
        ProcessChar(c);
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
// QU Cluster Detection
//-----------------------------------------------------------------------------

bool TypingEngine::IsInQUCluster() const {
    if (states_.size() < 2) return false;

    for (size_t i = 0; i + 1 < states_.size(); ++i) {
        if (states_[i].base == L'q' && states_[i+1].base == L'u'
            && states_[i+1].mod == Modifier::None) {
            return true;
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
// Remove Consumed Raw Entry (for tone escape auto-restore fix)
//-----------------------------------------------------------------------------

void TypingEngine::EraseConsumedRaw(size_t idx) {
    if (idx >= rawInput_.size()) return;
    rawInput_.erase(rawInput_.begin() + static_cast<ptrdiff_t>(idx));
    // Adjust all indices that reference positions after the erased entry
    for (auto& s : states_) {
        if (s.rawIdx > idx) s.rawIdx--;
        if (s.toneRawIdx != SIZE_MAX && s.toneRawIdx > idx) s.toneRawIdx--;
    }
}

void TypingEngine::RestoreConsumedKeyInPlace(size_t consumedRawIdx, wchar_t escapeKey) {
    // No snapshot of the consumed key, or this keypress is not the one sitting at
    // the tail of rawInput_ — keep the old append-at-end behaviour.
    if (consumedRawIdx == SIZE_MAX || consumedRawIdx >= rawInput_.size() ||
        rawInput_.empty() || towlower(rawInput_.back()) != towlower(escapeKey)) {
        if (consumedRawIdx != SIZE_MAX && consumedRawIdx < rawInput_.size()) {
            EraseConsumedRaw(consumedRawIdx);
        }
        ProcessChar(escapeKey);
        return;
    }

    // Position-only fix: WHICH character surfaces is unchanged (the escaping key,
    // so `Te`+`r`+`R` still yields "TeR" and Combined-mode `a`+`s`+`1` still
    // yields "a1") — it just lands in the consumed key's slot instead of the tail.
    // Erasing the tail can't shift any state's rawIdx — no state was built from it.
    EraseConsumedRaw(rawInput_.size() - 1);
    rawInput_[consumedRawIdx] = escapeKey;  // keep rawInput_ in the rendered order

    CharState s;
    s.base = towlower(escapeKey);
    s.isUpper = iswupper(escapeKey) != 0;
    s.rawIdx = consumedRawIdx;

    // Everything typed after the consumed key stays after it.
    size_t at = states_.size();
    for (size_t i = 0; i < states_.size(); ++i) {
        if (states_[i].rawIdx > consumedRawIdx) { at = i; break; }
    }
    states_.insert(states_.begin() + static_cast<ptrdiff_t>(at), s);
}

//-----------------------------------------------------------------------------
// Character Processing
//-----------------------------------------------------------------------------

void TypingEngine::ProcessChar(wchar_t /*c*/, wchar_t lower, bool isUpper) {
    CharState s;
    s.base = lower;
    s.isUpper = isUpper;
    s.rawIdx = rawInput_.empty() ? 0 : rawInput_.size() - 1;
    states_.push_back(s);
}

bool TypingEngine::RevertProvisionalOoTone(wchar_t lower) {
    const size_t count = states_.size();
    if (count < 2) return false;
    CharState& firstO  = states_[count - 2];
    CharState& secondO = states_[count - 1];
    // Two LITERAL o's with no modifier only arise from the ooo→oo escape; a
    // normal "oo" collapses to a single Circumflex ô, so this never matches a
    // regular syllable.
    if (firstO.base != L'o' || secondO.base != L'o') return false;
    if (firstO.mod != Modifier::None || secondO.mod != Modifier::None) return false;
    // The escape-oo tone always lands on the SECOND o (voọc/soóc/goòng). A tone
    // on the FIRST o is an ordinary toned vowel trailed by a repeated 'o'
    // (Telex "mó"+o, VNI "ó"+o) and must NOT be disturbed.
    if (!secondO.HasTone() || firstO.HasTone()) return false;
    // 'c' (→ ooc) and 'n' (→ oong) are the only valid continuations; keep the
    // tone for those so voọc/soóc/goòng compose.
    if (lower == L'c' || lower == L'n') return false;
    // Otherwise revert: drop the tone and re-emit the consumed tone key as a
    // literal char, recovered from rawInput_ via its tracked index. All reads
    // off secondO happen before the push_back below — a realloc there would
    // dangle the reference, so nothing touches it afterward.
    const size_t consumedRawIdx = secondO.toneRawIdx;
    const wchar_t toneKey =
        (consumedRawIdx != SIZE_MAX && consumedRawIdx < rawInput_.size())
            ? rawInput_[consumedRawIdx] : 0;
    secondO.tone = Tone::None;
    secondO.toneRawIdx = SIZE_MAX;
    if (toneKey != 0) {
        CharState literalToneKey;
        literalToneKey.base = towlower(toneKey);
        literalToneKey.isUpper = iswupper(toneKey) != 0;
        literalToneKey.rawIdx = consumedRawIdx;
        states_.push_back(literalToneKey);
    }
    // Same tone key again = the Telex escape (rr, jj, …). The revert above already
    // put that keystroke back as a literal, so the repeat must be swallowed rather
    // than re-toning the pair, and the escape latch keeps a third press literal.
    if (toneKey != 0 && towlower(toneKey) == lower) {
        escape_.escape(EscapeKind::Tone);
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
// Auto ươ Transformation — O(1) bounded scan
//-----------------------------------------------------------------------------

void TypingEngine::ApplyAutoUO() {
    // Requires 3+ chars: both patterns need a char after the uo/ưo pair.
    if (states_.size() < 3) return;

    // Scan backward, bounded to last 4 positions
    size_t start = (states_.size() > 4) ? states_.size() - 4 : 0;
    for (size_t i = start; i + 2 < states_.size(); ++i) {
        // Skip QU cluster: 'u' in "qu" is a consonant glide, not a modifiable vowel
        if (i > 0 && states_[i].base == L'u' && states_[i - 1].base == L'q') continue;

        // Pattern 1: u(no horn) + ơ(has horn) → horn the u to complete ươ
        // When followed by another character, always auto-complete uơ → ươ
        // (including h/th/kh prefixes: huơn → hươn, thuở typed as thuowr).
        if (states_[i].base == L'u' && states_[i].mod == Modifier::None &&
            states_[i+1].base == L'o' && states_[i+1].mod == Modifier::Horn) {
            states_[i].mod = Modifier::Horn;
        }

        // Pattern 2: ư(has horn) + o(no horn) → horn the o to complete ươ
        // Tone relocation needed: ư was horned by ApplyW P5 when only 'u' existed,
        // so any tone placed before 'o' arrived landed on ư and must now move to ơ.
        if (states_[i].base == L'u' && states_[i].mod == Modifier::Horn &&
            states_[i+1].base == L'o' && states_[i+1].mod == Modifier::None) {
            states_[i+1].mod = Modifier::Horn;
            RelocateToneToTarget();
        }
    }
}

//-----------------------------------------------------------------------------
// Tone Relocation (after horn applied)
//-----------------------------------------------------------------------------

void TypingEngine::RelocateToneToHornVowel() {
    size_t hornIdx = SIZE_MAX;
    size_t tonedIdx = SIZE_MAX;

    for (size_t i = 0; i < states_.size(); ++i) {
        if (states_[i].IsVowel()) {
            if (states_[i].mod == Modifier::Horn) hornIdx = i;
            if (states_[i].tone != Tone::None) tonedIdx = i;
        }
    }

    if (hornIdx != SIZE_MAX && tonedIdx != SIZE_MAX && hornIdx != tonedIdx) {
        // Allow relocation from unmodified vowels and from horn vowels
        // (handles "ươ" diphthong: tone moves from ư to ơ). Must stay within
        // the same syllable — never relocate across a consonant.
        if ((states_[tonedIdx].mod == Modifier::None ||
             states_[tonedIdx].mod == Modifier::Horn) &&
            !HasConsonantBetween(states_.data(), tonedIdx, hornIdx)) {
            states_[hornIdx].tone = states_[tonedIdx].tone;
            states_[tonedIdx].tone = Tone::None;
        }
    }
}

//-----------------------------------------------------------------------------
// Tone Relocation (after any modifier changes priority)
//-----------------------------------------------------------------------------

void TypingEngine::RelocateToneToTarget() {
    // Find where the tone currently is
    size_t tonedIdx = FindTonedVowelIndex(states_.data(), states_.size());
    if (tonedIdx == SIZE_MAX) return;

    // Find where the tone should be now (modifier may have changed priority)
    size_t targetIdx = FindToneTarget();
    if (targetIdx == SIZE_MAX || targetIdx == tonedIdx) return;

    // Never relocate across a consonant — Vietnamese syllables keep all
    // vowels contiguous, so a consonant between tonedIdx and targetIdx
    // means they belong to different syllables.
    if (HasConsonantBetween(states_.data(), tonedIdx, targetIdx)) return;

    if (IsToneRelocBlockedByP4(states_.data(), states_.size(), targetIdx, config_.modernOrtho))
        return;

    states_[targetIdx].tone = states_[tonedIdx].tone;
    states_[targetIdx].toneRawIdx = states_[tonedIdx].toneRawIdx;
    states_[tonedIdx].tone = Tone::None;
    states_[tonedIdx].toneRawIdx = SIZE_MAX;
}

//-----------------------------------------------------------------------------
// Tone Target Finding
// Builds a vowel sequence + state-index map from states_ (skipping cluster
// consonants like the 'i' in "gi" and the 'u' in "qu"), composes each vowel
// state without its tone diacritic so FindTonePosition sees only the
// modifier+base char, then maps Phonotactics' returned vowel-sequence index
// back to a state index.
//-----------------------------------------------------------------------------

bool TypingEngine::IsToneStopCodaMismatch() const noexcept {
    // T5 (anh 2026-05-07): "tone-stop-coda mismatch" = a syllable invalid
    // SOLELY because the existing tone (huyền/hỏi/ngã) is incompatible with
    // a stop final coda (c/ch/p/t). Vietnamese phonotactics: stop codas only
    // permit sắc and nặng tones; the other three tones force re-evaluation.
    //
    // Used by tone and modifier gates as a "user is mid-correction" predicate.
    // When true, the gate lets through a tone or modifier that would otherwise
    // be treated as literal — covers the `cafcs → các` and `cafcwj → cặc`
    // chains where the user mistypes huyền then corrects.
    if (!HasStopFinalCoda(states_.data(), states_.size())) return false;
    for (const auto& state : states_) {
        if (!state.HasTone()) continue;
        // First tone-bearing vowel determines mismatch — sắc/nặng = compatible,
        // huyền/hỏi/ngã = mismatch.
        return state.tone == Tone::Grave
            || state.tone == Tone::Hook
            || state.tone == Tone::Tilde;
    }
    return false;
}

size_t TypingEngine::FindToneTarget() const noexcept {
    // Cap matches FindTonePosition's fixed vowel capacity (see file-scope
    // kVowelCap), so the index map stays consistent for pathological inputs.
    // Stack-only buffers — Pillar Nhanh: no heap alloc on hook hot path.
    std::array<size_t, kVowelCap> vowelStateIdx{};
    std::array<wchar_t, kVowelCap> vowelSeq{};
    size_t vowelCount = 0;
    size_t lastVowelStateIdx = SIZE_MAX;

    for (size_t i = 0; i < states_.size(); ++i) {
        if (!states_[i].IsVowel()) continue;
        if (IsClusterConsonant(states_.data(), states_.size(), i)) continue;
        if (vowelCount >= kVowelCap) break;

        // Compose without tone and without case — FindTonePosition
        // matches lowercase rendered modifier+base (e.g. L'\x01B0' for ư).
        // Using towlower() on Vietnamese chars is locale-dependent and
        // unreliable on Linux; clearing isUpper produces the canonical
        // lowercase form directly.
        CharState canonical = states_[i];
        canonical.tone = Tone::None;
        canonical.isUpper = false;
        wchar_t composed = Compose(canonical);
        if (composed == 0) continue;

        vowelSeq[vowelCount] = composed;
        vowelStateIdx[vowelCount++] = i;
        lastVowelStateIdx = i;
    }

    if (vowelCount == 0) return SIZE_MAX;

    // Coda: any state past the last nucleus vowel.
    std::array<wchar_t, kVowelCap> coda{};
    size_t codaLen = 0;
    for (size_t i = lastVowelStateIdx + 1; i < states_.size(); ++i) {
        if (codaLen >= kVowelCap) break;  // bounds guard for pathological inputs
        CharState canonical = states_[i];
        canonical.isUpper = false;
        wchar_t composed = Compose(canonical);
        if (composed != 0) coda[codaLen++] = composed;
    }

    size_t vowelIdx = Phonology::FindTonePosition(
        std::wstring_view{vowelSeq.data(), vowelCount},
        std::wstring_view{coda.data(), codaLen},
        config_.modernOrtho);
    if (vowelIdx == SIZE_MAX || vowelIdx >= vowelCount) return SIZE_MAX;
    return vowelStateIdx[vowelIdx];
}

//-----------------------------------------------------------------------------
// Composition (State → Unicode) — O(1) flat array lookups
//-----------------------------------------------------------------------------

wchar_t TypingEngine::Compose(const CharState& s) {
    if (s.IsEmpty()) return 0;

    wchar_t ch = s.base;

    // Special: đ
    if (s.IsD() && s.mod == Modifier::Stroke) {
        return s.isUpper ? L'Đ' : L'đ';
    }

    // Step 1: Apply modifier — O(1) array lookup
    if (s.mod != Modifier::None && s.IsVowel()) {
        int bi = VowelBaseIndex(s.base);
        int mi = ModifierIndex(s.mod);
        if (bi >= 0 && mi >= 0) {
            wchar_t modified = kModifiedVowel[bi][mi];
            if (modified) ch = modified;
        }
    }

    // Step 2: Apply tone — O(1) array lookup
    if (s.tone != Tone::None) {
        int ti = ToneBaseIndex(ch);
        int si = ToneIndex(s.tone);
        if (ti >= 0 && si >= 0) {
            ch = kTonedVowel[ti][si];
        }
    }

    // Step 3: Apply case
    if (s.isUpper) {
        ch = ToUpperVietnamese(ch);
    }

    return ch;
}

const std::wstring& TypingEngine::ComposeAll() const {
    composeBuf_.clear();
    for (const auto& s : states_) {
        wchar_t ch = Compose(s);
        if (ch != 0) composeBuf_ += ch;
    }
    return composeBuf_;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

void TypingEngine::Backspace() {
    if (states_.empty()) return;
    escape_.clear();  // Allow đ re-trigger + Vietnamese re-trigger after user edits

    // Undo quick start consonant: ph→f, gi→j, qu→w (collapse both chars to original)
    if (quickStartKey_ != 0 && states_.size() == 2) {
        states_.clear();
        rawInput_.clear();
        rawInput_.push_back(quickStartKey_);
        ProcessChar(quickStartKey_);
        quickStartKey_ = 0;
        UpdateSpellState();
        return;
    }
    quickStartKey_ = 0;

    // Undo quick consonant expansion — restore original key in-place.
    // e.g., "aph" + BS → "app" (not "ap"), "rieng" + BS → "rienn".
    // The triggering key is still in rawInput_; re-add it as an escaped literal
    // so the user sees the full original sequence without having to retype.
    if (qc_.hasActive() && states_.size() - 1 == qc_.resultIndex) {
        // For uu→ươ: undo the horn on the preceding 'u' before popping ơ
        UndoHornU(states_.data(), states_.size() - 1);

        wchar_t originalKey = qc_.lastKey;
        qc_.markEscaped();  // escaped=true, clearActive (idx+lastKey)

        states_.pop_back();  // Remove the converted char (h in ph, g in ng, i in gi, ơ in ươ)
        // rawInput_ already contains the original triggering key — don't trim it.
        // Re-add it as a literal char; qc_.escaped prevents re-conversion.
        if (originalKey != 0) {
            ProcessChar(originalKey);
        }

        UpdateSpellState();
        RecalcEnglishBias(states_.data(), states_.size(), engProt_);
        if (IsTelexMode()) CheckZwjfInitialBias(states_.data(), states_.size(), config_, engProt_);
        return;
    }
    qc_.clearActive();

    // Trim rawInput_ to the position when this state was created.
    // This correctly handles modifier keys (circumflex, tone) that add to rawInput_
    // without creating new states — backspace removes all associated raw entries.
    size_t rawTarget = states_.back().rawIdx;
    states_.pop_back();
    if (rawInput_.size() > rawTarget) {
        rawInput_.resize(rawTarget);
    }
    // escRawHistory tracks keystrokes 1:1. Normal BS pops 1 entry — matches
    // a user BS removing 1 displayed char. When the composition fully empties
    // (BS reached the start of word), wipe escRawHistory_ so the next word's
    // keystrokes don't accumulate behind stale history. Quick-start / quick-
    // consonant undo branches return earlier and intentionally don't touch
    // escRawHistory_ — those keep display length unchanged.
    if (!escRawHistory_.empty()) escRawHistory_.pop_back();
    if (states_.empty()) escRawHistory_.clear();
    UpdateSpellState();

    // English Protection: recalculate bias after backspace
    RecalcEnglishBias(states_.data(), states_.size(), engProt_);
    if (IsTelexMode()) CheckZwjfInitialBias(states_.data(), states_.size(), config_, engProt_);
}

const std::wstring& TypingEngine::Peek() const {
    return ComposeAll();
}

std::wstring TypingEngine::Commit() {
    // Word boundary = last chance for the oo-coda, so an unresolved provisional
    // oo-tone can never close validly (voọc/soóc/goòng already have their 'c'/
    // 'ng' by now). Drop it so "pooor" commits as "poor", not "poỏ" — and so
    // the auto-restore below sees plain ASCII and keeps the escaped form
    // instead of replaying every raw keystroke ("pooor").
    (void)RevertProvisionalOoTone(L' ');
    std::wstring composed = ComposeAll();

    // Single quick-start consonant alone (f->ph, j->gi, w->qu) — always restore
    // This allows "j " -> "j " instead of "gi ", since "gi" is a valid word and wouldn't auto-restore naturally.
    if (quickStartKey_ != 0 && rawInput_.size() == 1) {
        std::wstring raw(rawInput_.begin(), rawInput_.end());
        if (ShouldAutoRestore(raw, composed)) {
            Reset();
            return raw;
        }
    }

    // Quick consonant alone (gg, uu) — always restore regardless of spell check setting
    if (qc_.onlyQC) {
        std::wstring raw(rawInput_.begin(), rawInput_.end());
        if (ShouldAutoRestore(raw, composed)) {
            Reset();
            return raw;
        }
    }

    // Guard: skip auto-restore when any of these are true:
    //  - spell check / auto-restore not enabled
    //  - an active quick consonant (nn→ng, cc→ch, etc.) was applied — user did
    //    this intentionally; restoring would undo the conversion they wanted.
    //    (qc_.hasActive() when quick consonant is active)
    bool skipAutoRestore = !config_.spellCheckEnabled
                        || !config_.autoRestoreEnabled
                        || qc_.hasActive();
    if (!skipAutoRestore) {
        bool shouldRestore = spellCheckDisabled_;  // Already known Invalid
        bool unfinishedNucleus = false;

        // Also restore ValidPrefix at commit time — incomplete words like "úẻ"
        // (from typing "user") are ValidPrefix during typing (allowing future
        // modifiers) but should auto-restore when the user commits.
        if (!shouldRestore && !states_.empty()) {
            auto result = Phonology::ValidateSyllableState(states_.data(), states_.size(), config_.allowZwjf);
            shouldRestore = (result == Phonology::SyllableState::ValidPrefix);
            unfinishedNucleus = shouldRestore && std::any_of(states_.begin(), states_.end(),
                [](const CharState& state) { return state.IsVowel(); });
        }

        if (shouldRestore) {
            // Spell exclusion list takes priority. HasIntentionalStrokeD heuristic
            // also protects abbreviations like đt, đh while restoring đwa, ăndd.
            bool excluded = IsSpellExcluded(states_.data(), states_.size(),
                                            config_.spellExclusions,
                                            [](const CharState& s) { return Compose(s); });
            // A plausible but unfinished nucleus is a typing prefix, not a
            // consonant abbreviation. Explicit exclusions still take priority.
            bool keepComposed = excluded ||
                                (!unfinishedNucleus && HasIntentionalStrokeD(rawInput_, composed));
            if (!keepComposed) {
                std::wstring raw(rawInput_.begin(), rawInput_.end());
                // Preserve first-letter capitalization from composed form.
                // SeedFromText (backspace-revive) stores lowercase bases in
                // rawInput_, so without this the auto-restore would return
                // "vkey" when the user typed "VKey". Match the capitalization
                // of the composed string's first character.
                if (!raw.empty() && !composed.empty() && std::iswupper(composed[0])) {
                    raw[0] = std::towupper(raw[0]);
                }
                // If raw now matches composed (capitalization was the only
                // difference), skip the restore — the word is effectively
                // unchanged and should be returned as-is.
                if (raw == composed) {
                    Reset();
                    return composed;
                }
                if (ShouldAutoRestore(raw, composed)) {
                    Reset();
                    return raw;
                }
            }
        }
    }

    Reset();
    return composed;
}

void TypingEngine::Reset() {
    states_.clear();
    rawInput_.clear();
    escRawHistory_.clear();
    spellCheckDisabled_ = false;
    qc_.Reset();
    escape_.clear();
    quickStartKey_ = 0;
    engProt_.Reset();
}

bool TypingEngine::SeedFromText(const std::wstring& text) {
    Reset();
    states_.reserve(text.size());
    rawInput_.reserve(text.size());
    for (wchar_t ch : text) {
        wchar_t base = 0;
        int modIdx = -1, toneIdx = -1;
        bool isUpper = false;
        if (!DecomposeVietChar(ch, base, modIdx, toneIdx, isUpper)) {
            Reset();
            return false;
        }
        CharState st;
        st.base = base;
        st.isUpper = isUpper;
        switch (modIdx) {
            case 0: st.mod = Modifier::Circumflex; break;
            case 1: st.mod = Modifier::Breve; break;
            case 2: st.mod = Modifier::Horn; break;
            case 3: st.mod = Modifier::Stroke; break;
            default: st.mod = Modifier::None; break;
        }
        switch (toneIdx) {
            case 0: st.tone = Tone::Acute; break;
            case 1: st.tone = Tone::Grave; break;
            case 2: st.tone = Tone::Hook; break;
            case 3: st.tone = Tone::Tilde; break;
            case 4: st.tone = Tone::Dot; break;
            default: st.tone = Tone::None; break;
        }
        // Synthetic raw: only the base letter — we don't have original keystrokes.
        // This is enough for Backspace/PushChar to work correctly after seeding.
        st.rawIdx = rawInput_.size();
        st.toneRawIdx = SIZE_MAX;
        rawInput_.push_back(isUpper ? std::towupper(base) : base);
        states_.push_back(st);
    }
    UpdateSpellState();
    // Populate engProt_.bias so IsEnglishWord() can report on seeded text.
    CheckEnglishBias(states_.data(), states_.size(), engProt_);
    return true;
}



//-----------------------------------------------------------------------------
// Spell Exclusion — modifier key match
//-----------------------------------------------------------------------------

bool TypingEngine::WouldModifierKeyMatchExclusion(wchar_t lower) const {
    auto compose = [](const CharState& s) { return Compose(s); };
    // Telex modifier keys
    if (lower == L'd') {
        return WouldStrokeDMatchExclusion(states_.data(), states_.size(),
            config_.spellExclusions, compose);
    }
    if (lower == L'w') {
        return WouldAnyModifierMatchExclusion(states_.data(), states_.size(),
            config_.spellExclusions, compose, Modifier::Horn, L"uo") ||
            WouldAnyModifierMatchExclusion(states_.data(), states_.size(),
            config_.spellExclusions, compose, Modifier::Breve, L"a");
    }
    if (lower == L'a' || lower == L'e' || lower == L'o') {
        return WouldAnyModifierMatchExclusion(states_.data(), states_.size(),
            config_.spellExclusions, compose, Modifier::Circumflex, L"aeo");
    }
    // VNI modifier keys
    if (lower == L'6') {
        return WouldAnyModifierMatchExclusion(states_.data(), states_.size(),
            config_.spellExclusions, compose, Modifier::Circumflex, L"aeo");
    }
    if (lower == L'7') {
        return WouldAnyModifierMatchExclusion(states_.data(), states_.size(),
            config_.spellExclusions, compose, Modifier::Horn, L"uo");
    }
    if (lower == L'8') {
        return WouldAnyModifierMatchExclusion(states_.data(), states_.size(),
            config_.spellExclusions, compose, Modifier::Breve, L"a");
    }
    if (lower == L'9') {
        return WouldStrokeDMatchExclusion(states_.data(), states_.size(),
            config_.spellExclusions, compose);
    }
    return false;
}

//-----------------------------------------------------------------------------
// HandleVniHorn — VNI `7` (uo pair cycle + standalone u/o horn)
// HandleVniCircumflex / HandleVniBreve are thin wrappers over
// ProcessVniVowelModifier below; HandleStrokeD covers VNI `9`.
//-----------------------------------------------------------------------------

bool TypingEngine::HandleVniHorn(TypingAction /*action*/, wchar_t c) {
    if (states_.empty()) return false;

    // --- uo pair cycle (same logic as Telex HandleHornW P2) ---
    size_t uIdx = SIZE_MAX, oIdx = SIZE_MAX;
    for (size_t i = 0; i < states_.size(); ++i) {
        if (!states_[i].IsVowel()) continue;
        if (IsClusterConsonant(states_.data(), states_.size(), i)) continue;
        if (states_[i].base == L'u') uIdx = i;
        if (states_[i].base == L'o') oIdx = i;
    }

    // uo pair detected (adjacent)
    if (uIdx != SIZE_MAX && oIdx != SIZE_MAX && oIdx == uIdx + 1) {
        bool isEdge = IsUOEdgeCasePrefix(states_.data(), states_.size(), uIdx);
        bool uHorn = states_[uIdx].IsHorn();
        bool oHorn = states_[oIdx].IsHorn();

        if (!uHorn && !oHorn) {
            // uo → apply horn
            if (isEdge) {
                states_[oIdx].mod = Modifier::Horn;  // Edge: horn o first
            } else {
                states_[uIdx].mod = Modifier::Horn;
                states_[oIdx].mod = Modifier::Horn;
            }
            RelocateToneToTarget();
            return true;
        }
        // Forward: ưo → ươ (u already horned via explicit hu7, complete pair)
        if (uHorn && !oHorn) {
            states_[oIdx].mod = Modifier::Horn;
            RelocateToneToTarget();
            return true;
        }
        if (isEdge && !uHorn && oHorn) {
            // Edge step 2: uơ → ươ (horn u too)
            states_[uIdx].mod = Modifier::Horn;
            RelocateToneToTarget();
            return true;
        }
        // Escape: both horned — clear pair
        // (isEdge && uHorn && !oHorn already handled above by ưo→ươ forward)
        if (uHorn && oHorn) {
            states_[uIdx].mod = Modifier::None;
            states_[oIdx].mod = Modifier::None;
            ProcessChar(c);
            escape_.escape(EscapeKind::Horn);
            return true;
        }
    }

    // --- uu pattern (horn first u, like Telex P5 — 2-press cycle) ---
    size_t firstU = SIZE_MAX, lastU = SIZE_MAX;
    int uCount = 0;
    for (size_t i = 0; i < states_.size(); ++i) {
        if (states_[i].base == L'u' && states_[i].IsVowel()) {
            if (IsClusterConsonant(states_.data(), states_.size(), i)) continue;
            if (firstU == SIZE_MAX) firstU = i;
            lastU = i;
            ++uCount;
        }
    }
    if (uCount >= 2 && firstU != lastU) {
        if (states_[firstU].mod == Modifier::None && states_[lastU].mod == Modifier::None) {
            // Apply: horn first u (lưu, cưu, hưu)
            states_[firstU].mod = Modifier::Horn;
            RelocateToneToTarget();
            return true;
        }
        if (states_[firstU].mod == Modifier::Horn) {
            // Escape: clear horn on first u, add '7' literal (2-press cycle, same as Telex P4)
            states_[firstU].mod = Modifier::None;
            ProcessChar(c);
            escape_.escape(EscapeKind::Horn);
            return true;
        }
    }

    // --- Generic: rightmost eligible o/u → apply horn ---
    return ProcessVniVowelModifier(Modifier::Horn, c);
}

bool TypingEngine::ProcessVniVowelModifier(Modifier targetMod, wchar_t key) {
    if (states_.empty()) return false;

    // Eligible base vowels for each modifier type
    auto isEligible = [targetMod](wchar_t base) -> bool {
        switch (targetMod) {
            case Modifier::Circumflex: return base == L'a' || base == L'e' || base == L'o';
            case Modifier::Horn:       return base == L'o' || base == L'u';
            case Modifier::Breve:      return base == L'a';
            default:                   return false;
        }
    };

    // Pass 1: rightmost unmodified eligible vowel → apply.
    // Pre-state drives validation + apply paths so the speculate path mirrors
    // the runtime path (invariant from ad09f15; W8.5 ports c6369dd template
    // from HandleAdjacentCircumflex to fix the VNI mirror: `vi5e6t → việt`,
    // `ngu2oo6n → nguồn`).
    //
    //  - ValidPrefix: user is mid-construction (e.g. "vịe" → "việ" via 6).
    //    Apply modifier AND relocate tone so the resulting diphthong
    //    (iê, uô, …) attracts the tone to the correct vowel.
    //  - Valid: syllable already complete ("cua" + 6 → cuâ as legitimate
    //    ValidPrefix to cuấp/cuấn). Mod-only validate; don't relocate.
    //    Catches split tone/mod typos ("của" + 6 → c,ủ,â) via spell check.
    //  - Invalid: not a syllable at all → mod-only path rejects.
    auto preState = Phonology::ValidateSyllableState(
        states_.data(), states_.size(), config_.allowZwjf);
    const bool needsRelocate =
        (preState == Phonology::SyllableState::ValidPrefix);

    for (size_t i = states_.size(); i-- > 0;) {
        if (!states_[i].IsVowel() || !isEligible(states_[i].base)) continue;
        if (IsClusterConsonant(states_.data(), states_.size(), i)) continue;
        if (states_[i].mod == Modifier::None) {
            if (needsRelocate) {
                if (!WouldBeValidSyllable(i, targetMod,
                                          /*clearCircumflexIdx=*/SIZE_MAX,
                                          /*speculateRelocateTone=*/true)
                    && !WouldModifierKeyMatchExclusion(key)) {
                    return false;
                }
            } else {
                if (ShouldRejectModifier(i, targetMod, key)) return false;
            }
            states_[i].mod = targetMod;
            if (targetMod == Modifier::Circumflex) UndoHornU(states_.data(), i);
            if (needsRelocate) RelocateToneToTarget();
            return true;
        }
    }

    // Pass 1.5: modifier switching on compatible vowels
    // â+8→ă, ă+6→â, ô+7→ơ, ơ+6→ô
    for (size_t i = states_.size(); i-- > 0;) {
        if (!states_[i].IsVowel() || !states_[i].HasModifier()) continue;
        if (IsClusterConsonant(states_.data(), states_.size(), i)) continue;
        wchar_t base = states_[i].base;
        Modifier curMod = states_[i].mod;
        bool canSwitch = false;
        // a: Circumflex ↔ Breve
        if (base == L'a' && ((curMod == Modifier::Circumflex && targetMod == Modifier::Breve) ||
                             (curMod == Modifier::Breve && targetMod == Modifier::Circumflex))) {
            canSwitch = true;
        }
        // o: Circumflex ↔ Horn
        if (base == L'o' && ((curMod == Modifier::Circumflex && targetMod == Modifier::Horn) ||
                             (curMod == Modifier::Horn && targetMod == Modifier::Circumflex))) {
            canSwitch = true;
        }
        if (canSwitch) {
            UndoHornU(states_.data(), i);
            states_[i].mod = targetMod;
            RelocateToneToTarget();
            return true;
        }
    }

    // Pass 2: escape — rightmost vowel with matching modifier → clear
    for (size_t i = states_.size(); i-- > 0;) {
        if (!states_[i].IsVowel()) continue;
        if (IsClusterConsonant(states_.data(), states_.size(), i)) continue;
        if (states_[i].mod == targetMod) {
            UndoHornU(states_.data(), i);
            states_[i].mod = Modifier::None;
            ProcessChar(key);
            escape_.escape(EscapeKind::Modifier);
            return true;
        }
    }

    return false;
}

//-----------------------------------------------------------------------------
// Spell Check State Update
//-----------------------------------------------------------------------------

void TypingEngine::UpdateSpellState() {
    UpdateSpellCheck(states_.data(), states_.size(), config_, spellCheckDisabled_,
                     [](const CharState& s) { return Compose(s); });
}

bool TypingEngine::ShouldRejectModifier(size_t targetIdx, Modifier newMod,
                                        wchar_t key) {
    if (!config_.spellCheckEnabled) return false;
    return !WouldBeValidSyllable(targetIdx, newMod) &&
           !WouldModifierKeyMatchExclusion(key);
}

bool TypingEngine::WouldBeValidSyllable(size_t targetIdx, Modifier newMod,
                                        size_t clearCircumflexIdx,
                                        bool speculateRelocateTone) {
    if (!config_.spellCheckEnabled) return true;
    if (targetIdx >= states_.size()) return true;

    // Bulk snapshot path — only when the caller's runtime relocates the tone
    // after applying the modifier. Validating without the relocation
    // misclassifies legitimate promotions like "súat" + 'a' → "suất"
    // (free-marking circumflex across coda 't'): unrelocated, sắc on `u` of
    // `uâ` reads as Invalid; relocated, sắc on `â` is Valid. The stack-array
    // snapshot is sized to the file-scope kVowelCap so the same truncation
    // contract applies as in FindToneTarget — no heap traffic on the modifier
    // hot path. CharState is trivially copyable; only RelocateToneToTarget
    // mutates fields beyond .mod, and only on two indices, so a bulk snapshot/
    // restore covers every mutation.
    if (speculateRelocateTone) {
        const size_t bufferLen = states_.size();
        if (bufferLen > kVowelCap) return true;  // Defensive: oversized buffer → permissive.

        std::array<CharState, kVowelCap> saved;
        std::copy_n(states_.begin(), bufferLen, saved.begin());

        states_[targetIdx].mod = newMod;
        if (newMod == Modifier::Circumflex) UndoHornU(states_.data(), targetIdx);
        if (clearCircumflexIdx < bufferLen &&
            saved[clearCircumflexIdx].mod == Modifier::Circumflex) {
            states_[clearCircumflexIdx].mod = Modifier::None;
        }
        RelocateToneToTarget();

        auto result = Phonology::ValidateSyllableState(states_.data(), bufferLen, config_.allowZwjf);
        bool recoverableMismatch = (result == Phonology::SyllableState::Invalid)
                                   && IsToneStopCodaMismatch();

        std::copy_n(saved.begin(), bufferLen, states_.begin());
        return result != Phonology::SyllableState::Invalid || recoverableMismatch;
    }

    // Mod-only path — for callers whose runtime keeps the tone on its
    // current vowel after applying the modifier. Speculating relocation
    // here would over-accept: validator returns Valid by re-aligning a
    // tone the runtime then leaves stranded, opening the door to typos
    // (e.g. "của" + 'a' → cuẩ, ad09f15).
    //
    // Callers that DO relocate at runtime MUST opt in via
    // speculateRelocateTone=true to keep the speculate/apply invariant
    // (mirrors what the runtime actually mutates). See free-marking
    // circumflex L1052 and adjacent circumflex ValidPrefix branch L944
    // for examples. ProcessVniVowelModifier L1953 still uses this
    // mod-only path despite relocating at runtime — tracked in
    // docs/TODO.md as a known parity gap.
    Modifier saved = states_[targetIdx].mod;
    Modifier savedCompanion = targetIdx > 0 ? states_[targetIdx - 1].mod : Modifier::None;
    states_[targetIdx].mod = newMod;
    if (newMod == Modifier::Circumflex) UndoHornU(states_.data(), targetIdx);
    bool didClear = false;
    if (clearCircumflexIdx < states_.size() &&
        states_[clearCircumflexIdx].mod == Modifier::Circumflex) {
        states_[clearCircumflexIdx].mod = Modifier::None;
        didClear = true;
    }
    auto result = Phonology::ValidateSyllableState(states_.data(), states_.size(), config_.allowZwjf);
    // T5 case 2 (anh 2026-05-07): if the speculative state is Invalid SOLELY
    // because of tone-stop-coda mismatch (huyền/hỏi/ngã + stop coda c/ch/p/t),
    // accept the modifier — the user is mid-correction and the next tone
    // keystroke recovers via the T5 case 1 branch in the tone gate. Without
    // this, P5/P6/P7 in HandleHornW reject `càc + w` because `cằc` is still
    // invalid, and `w` falls through to literal even though the user clearly
    // wants to type `cặc`/`cẳc`/etc.
    bool recoverableMismatch = (result == Phonology::SyllableState::Invalid)
                               && IsToneStopCodaMismatch();
    if (didClear) states_[clearCircumflexIdx].mod = Modifier::Circumflex;
    states_[targetIdx].mod = saved;
    if (targetIdx > 0) states_[targetIdx - 1].mod = savedCompanion;
    return result != Phonology::SyllableState::Invalid || recoverableMismatch;
}

}  // namespace NextKey
