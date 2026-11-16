// VKey - Shared Engine Helper Functions
// Copyright (c) 2024-2026 PhatMT. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-only
//
// Template helpers used by TypingEngine (Telex, VNI, and Combined input methods).
// Templated so call sites stay structural-only against CharState.

#pragma once

#include "PhonotacticsValidator.h"
#include "Phonotactics.h"
#include "EnglishProtection.h"
#include "VietnameseTables.h"
#include "core/config/TypingConfig.h"
#include <functional>
#include <string>

namespace NextKey {

//=============================================================================
// EscapeState — replaces toneEscaped_ + dModifierEscaped_ booleans
//=============================================================================

enum class EscapeKind : uint8_t {
    None = 0,
    Tone,           // ss, ff, rr, xx, jj (Telex) / 11, 22... (VNI)
    Circumflex,     // aa, ee, oo escape / 6-key escape
    Horn,           // ơ[], ư], w-pairs, P4 / 7-key escape
    Breve,          // ă→a via ww / 8-key escape
    Stroke,         // ddd / d99 — replaces dModifierEscaped_
    Modifier,       // VNI generic (Pass 2 fallback — covers circ/horn/breve in one pass)
};

struct EscapeState {
    EscapeKind kind = EscapeKind::None;

    [[nodiscard]] constexpr bool isEscaped() const noexcept {
        return kind != EscapeKind::None;
    }
    [[nodiscard]] constexpr bool isEscaped(EscapeKind k) const noexcept {
        return kind == k;
    }
    constexpr void escape(EscapeKind k) noexcept { kind = k; }
    constexpr void clear() noexcept { kind = EscapeKind::None; }
};

//=============================================================================
// QuickConsonantState — replaces 4 scattered QC fields
//=============================================================================

struct QuickConsonantState {
    size_t resultIndex = SIZE_MAX;  // states_ index of quick consonant result char
    wchar_t lastKey = 0;            // key that triggered last QC (suppresses consecutive re-trigger)
    bool onlyQC = false;            // true when buffer is only quick consonant expansion
    bool escaped = false;           // true after backspace undoes quick consonant

    constexpr void Reset() noexcept {
        resultIndex = SIZE_MAX; lastKey = 0; onlyQC = false; escaped = false;
    }
    constexpr void clearActive() noexcept {
        resultIndex = SIZE_MAX; lastKey = 0;
    }
    constexpr void markEscaped() noexcept {
        escaped = true; clearActive();
    }
    [[nodiscard]] constexpr bool hasActive() const noexcept {
        return resultIndex != SIZE_MAX;
    }
};

/// Check if the composed buffer matches any spell exclusion prefix.
/// Exclusion entries must be >= 2 chars and pre-lowercased (done at config load time).
/// Match is prefix-based: "hđ" covers "hđt", "hđqt".
template<typename CharStateT, typename ComposeFunc>
inline bool IsSpellExcluded(const CharStateT* states, size_t count,
                            const std::vector<std::wstring>& exclusions,
                            ComposeFunc compose) {
    if (exclusions.empty() || count == 0) return false;
    // Stack buffer — Vietnamese words max ~8 chars, 16 is generous.
    // Avoids heap allocation on every PushChar call.
    constexpr size_t kMaxBuf = 16;
    wchar_t buf[kMaxBuf];
    size_t bufLen = 0;
    for (size_t i = 0; i < count && bufLen < kMaxBuf; ++i) {
        wchar_t ch = compose(states[i]);
        if (ch != 0) buf[bufLen++] = towlower(ch);
    }
    // Inline prefix match (same logic as MatchExclusionBuf but avoids forward-decl issue)
    for (const auto& pat : exclusions) {
        if (pat.size() < 2 || bufLen < pat.size()) continue;
        bool match = true;
        for (size_t i = 0; i < pat.size(); ++i) {
            if (pat[i] != buf[i]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

/// Update spell check state. Template over CharState.
/// Call after every PushChar/Backspace to revalidate the syllable.
template<typename CharStateT, typename ComposeFunc>
inline void UpdateSpellCheck(const CharStateT* states, size_t count,
                             const TypingConfig& config,
                             bool& spellCheckDisabled,
                             ComposeFunc compose) {
    if (!config.spellCheckEnabled || count == 0) {
        spellCheckDisabled = false;
        return;
    }
    if (IsSpellExcluded(states, count, config.spellExclusions, compose)) {
        spellCheckDisabled = false;
        return;
    }
    auto result = Phonology::ValidateSyllableState(states, count, config.allowZwjf);
    spellCheckDisabled = (result == Phonology::SyllableState::Invalid);
}

/// Check if auto-restore should return raw input instead of composed text.
/// Restores when composed has diacritics or same/shorter length (meaning
/// modifiers changed letters without escape expanding the string).
inline bool ShouldAutoRestore(const std::wstring& raw, const std::wstring& composed) noexcept {
    if (raw == composed) return false;
    for (wchar_t ch : composed) {
        if (ch > 0x7F) return true;  // Has diacritics → restore raw
    }
    return raw.length() <= composed.length();
}

/// Check if the user intentionally typed đ and the result looks like an abbreviation.
/// Returns true (keep composed) when ALL of:
///   1. Raw input has consecutive "dd"/"DD" or "d9" (Telex/VNI stroke pattern)
///   2. Composed output actually contains đ/Đ (the stroke fired, not blocked)
///   3. No plain ASCII vowels (a,e,i,o,u,y) appear after the last đ in composed
///      (abbreviations are đ + consonants like "đt"; typing attempts have unmodified
///       vowels like "đwa" with plain 'a')
/// This protects abbreviations (đt, đh) while letting failed typing attempts
/// (đwa, đp+vowel) auto-restore. For full control, use the spell exclusion list.
template<typename RawT>
inline bool HasIntentionalStrokeD(const RawT& rawInput,
                                  const std::wstring& composed) noexcept {
    // Step 1: raw input must have consecutive dd or d9
    bool hasRawDD = false;
    for (size_t i = 0; i + 1 < rawInput.size(); ++i) {
        wchar_t ch = rawInput[i];
        if (ch == L'd' || ch == L'D') {
            wchar_t next = rawInput[i + 1];
            if (next == L'd' || next == L'D' || next == L'9') { hasRawDD = true; break; }
        }
    }
    if (!hasRawDD) return false;

    // Step 2: composed must contain đ/Đ — if the stroke was blocked (e.g.,
    // spellCheckDisabled_ prevented dd→đ), there's nothing to protect.
    size_t lastStrokeD = std::wstring::npos;
    for (size_t i = 0; i < composed.size(); ++i) {
        if (composed[i] == L'\u0111' || composed[i] == L'\u0110') lastStrokeD = i;
    }
    if (lastStrokeD == std::wstring::npos) return false;

    // Step 3: after the last đ, only consonants (no plain vowels).
    // Abbreviations: đt, đh, hđ, vđề (ề is > 0x7F, not plain ASCII vowel).
    // Typing attempts: đwa (plain 'a'), đia (plain 'i','a').
    for (size_t i = lastStrokeD + 1; i < composed.size(); ++i) {
        wchar_t ch = towlower(composed[i]);
        if (ch == L'a' || ch == L'e' || ch == L'i' || ch == L'o' || ch == L'u' || ch == L'y') {
            return false;
        }
    }
    return true;
}

/// Check if the consonant onset before 'u' at uIdx forms an edge case prefix
/// where uơ (not ươ) is a valid Vietnamese word: h (huơ), th (thuở), kh (khuơ).
/// Used by the uo horn cycle to determine 3-state vs 2-state toggle.
template<typename CharStateT>
[[nodiscard]] inline bool IsUOEdgeCasePrefix(
        const CharStateT* states, size_t /*count*/, size_t uIdx) noexcept {
    if (uIdx == 1 && towlower(states[0].base) == L'h') return true;       // h + uo
    if (uIdx == 2) {
        wchar_t c0 = towlower(states[0].base);
        wchar_t c1 = towlower(states[1].base);
        if (c1 == L'h' && (c0 == L't' || c0 == L'k')) return true;        // th/kh + uo
    }
    return false;
}

/// Undo ươ pair: if 'o' at oIndex has a modifier and preceding 'u' also has one,
/// clear the u's modifier. Handles: ươ→uô (horn undo), uu→ươ backspace, w escape.
/// Works with both Telex and Vni CharState.
template<typename CharStateT>
inline void UndoHornU(CharStateT* states, size_t oIndex) noexcept {
    if (oIndex > 0 && states[oIndex].base == L'o' && states[oIndex].HasModifier() &&
        states[oIndex - 1].base == L'u' && states[oIndex - 1].HasModifier()) {
        states[oIndex - 1].mod = {};  // Clear to None (value-initialized enum = 0)
    }
}

/// Recalculate English protection bias after backspace.
/// Resets bias, then REPLAYS CheckEnglishBias incrementally over every
/// prefix (as if each remaining char were just typed) instead of a single
/// pass over the full buffer. CheckEnglishBias's invalid-coda check
/// (IsInvalidVietnameseCoda) only inspects the TRAILING consonant run, so a
/// one-shot pass over the whole buffer misses an offending cluster that is
/// no longer trailing — e.g. "android" (correctly HardEnglish-latched at
/// "and") minus a trailing backspace leaves "androi", where "nd" is now
/// buried before "roi" and a one-shot check would silently un-latch it,
/// letting a retyped 'd' misfire dd→đ ("anđroi", #209 2026-07-05). Replaying
/// incrementally re-derives the exact bias forward typing would have
/// produced, catching "nd" while it was still trailing at length 3.
/// Call after Backspace() modifies the state buffer.
template<typename CharStateT>
inline void RecalcEnglishBias(const CharStateT* states, size_t count,
                              EnglishProtectionState& engProt) noexcept {
    engProt.Reset();
    for (size_t len = 2; len <= count; ++len) {
        CheckEnglishBias(states, len, engProt);
    }
}

/// When allowZwjf is disabled AND spell check is on, treat w/z/j/f as initial
/// consonant → HardEnglish.  When spell check is off, z/w/j/f are always
/// allowed (the toggle has no effect).
/// Call after CheckEnglishBias() or RecalcEnglishBias() to layer this check.
template<typename CharStateT>
inline void CheckZwjfInitialBias(const CharStateT* states, size_t count,
                                  const TypingConfig& config,
                                  EnglishProtectionState& engProt) noexcept {
    if (config.allowZwjf || !config.spellCheckEnabled || count == 0) return;
    if (engProt.bias == LanguageBias::Vietnamese) return;  // Respect confirmed VN intent
    if (states[0].IsVowel()) return;
    wchar_t initialChar = states[0].base;
    if (initialChar == L'w' || initialChar == L'z' || initialChar == L'j' || initialChar == L'f') {
        engProt.bias = LanguageBias::HardEnglish;
    }
}

/// Find the target 'd' for stroke modifier (dd→đ / d9→đ).
/// Returns index of the 'd' to modify, or SIZE_MAX if none found or blocked.
/// Scans backward for last 'd', then checks that the contiguous 'd' cluster
/// is NOT preceded by a vowel (blocks "added"→"ađed", allows "vdd"→"vđ").
/// Template over CharState.
template<typename CharStateT>
[[nodiscard]] inline size_t FindStrokeDTarget(
        const CharStateT* states, size_t count) noexcept {
    if (count == 0) return SIZE_MAX;

    size_t dIdx = SIZE_MAX;
    for (size_t i = count; i-- > 0;) {
        if (states[i].IsD()) { dIdx = i; break; }
    }
    if (dIdx == SIZE_MAX) return SIZE_MAX;

    // A non-initial 'd' (dIdx > 0) can only be targeted if it is immediately
    // preceding the new key (dIdx == count - 1). Delayed stroke-D across
    // intervening characters is only valid for initial onset 'd' at index 0
    // (e.g. "doc"+d → "đoc", issue #200 "dm"+d → "đm").
    if (dIdx > 0 && dIdx != count - 1) return SIZE_MAX;

    // For non-initial 'd', scan past contiguous 'd' cluster to find real predecessor.
    // Block if preceded by vowel (prevents "added"→"ađed", "oddly"→"ođly").
    if (dIdx > 0) {
        size_t checkIdx = dIdx;
        while (checkIdx > 0 && states[checkIdx - 1].IsD()) --checkIdx;
        if (checkIdx > 0 && states[checkIdx - 1].IsVowel()) return SIZE_MAX;
    }
    return dIdx;
}

/// Does the composed buffer contain any vowel? Vowel-less buffers are
/// abbreviation chains (PLHĐ, CLĐ, HĐLĐ) — no English word lacks a vowel,
/// so English-protection bias carries no signal for them.
template<typename CharStateT>
[[nodiscard]] inline bool HasVowelState(const CharStateT* states, size_t count) noexcept {
    for (size_t i = 0; i < count; ++i) {
        if (states[i].IsVowel()) return true;
    }
    return false;
}

/// Pre-check for stroke-D modifier: returns true if applying đ would create an
/// invalid consonant cluster (e.g., "drop" + d → "đrop" with coda "p" already present).
/// When onset 'd' (position 0) is immediately followed by a vowel (e.g. "doc"),
/// applying stroke gives [đ + vowel + coda] — perfectly valid Vietnamese structure.
/// Template over CharState.
template<typename CharStateT>
[[nodiscard]] inline bool IsStrokeDBlockedByCoda(
        const CharStateT* states, size_t count, size_t dTarget) noexcept {
    bool isSimpleOnsetVowelCoda = (dTarget == 0 && count > 1 && states[1].IsVowel());
    if (isSimpleOnsetVowelCoda) return false;

    size_t codaLen = 0;
    for (size_t j = count; j-- > 0;) {
        if (states[j].IsVowel() || states[j].IsD()) break;
        ++codaLen;
    }
    return codaLen >= 1;
}

/// Combined pre-check: should a stroke-D attempt mark the buffer as HardEnglish?
/// Caller has already verified dTarget != SIZE_MAX and states[dTarget].mod == None.
///   - Coda-block heuristic catches "drop"+d (invalid coda 'p').
///   - V+C+V structural pattern catches "detail"+d (e-t-a) which the
///     coda-block heuristic misses due to its "leading-d + vowel coda"
///     exception (kept so legitimate "doc"+d → "đoc" still works).
template<typename CharStateT>
[[nodiscard]] inline bool ShouldBlockStrokeDAsEnglish(
        const CharStateT* states, size_t count, size_t dTarget) noexcept {
    if (IsStrokeDBlockedByCoda(states, count, dTarget)) return true;
    if (dTarget == 0 && HasStructuralVCVPattern(states, count)) return true;
    return false;
}

/// Decide whether a stroke-D escape (Đ → d) should fire at the given target.
/// Two allowed cases:
///   (a) Trailing Đ (ddd → dd, vddd → vdd): user double-tapped to undo.
///   (b) Leading Đ at index 0 (issue #200 toggle): a new d toggles the
///       FIRST char back to d, mirroring "d→đ on the first char" in reverse.
///       Examples: đm + d → dmd, đoc + d → docd. This fires regardless of
///       what sits between Đ and the new d — there is NO vowel guard.
///       Trade-off (accepted by maintainer): Đ-initial abbreviation chains
///       typed as one token (e.g. ddxd, ddcddt) lose the leading Đ. Such
///       all-consonant Đ-initial tokens are not real Vietnamese words; users
///       who need them type the syllable, then space → backspace.
/// Intermediate Đ (e.g. HĐL+d for HĐLĐ) returns false — Đ belongs to a prior
/// segment; the new d is a fresh literal so the NEXT dd composes a new Đ.
template<typename CharStateT>
[[nodiscard]] inline bool IsStrokeDEscapeAllowed(
        const CharStateT* /*states*/, size_t count, size_t dIdx) noexcept {
    // Trailing Đ (ddd→dd) or leading Đ@0 (issue #200 first-char toggle).
    // `states` is unused now that the leading case has no vowel guard.
    if (count > 0 && dIdx == count - 1) return true;
    if (dIdx == 0 && count >= 2) return true;
    return false;
}

/// Does a composed buffer match any spell exclusion prefix?
/// Buffer may be shorter than exclusion (typing in progress) or longer (prefix match).
/// Exclusion entries are pre-lowercased at config load time; buffer is lowercased by caller.
/// @param Compare  Binary predicate for char equality. Default: std::equal_to (exact match).
///                 Pass a tone-stripping comparator for modifier bypass (see overload below).
template<typename Compare = std::equal_to<wchar_t>>
[[nodiscard]] inline bool MatchExclusionBuf(
        const wchar_t* buf, size_t bufLen,
        const std::vector<std::wstring>& exclusions,
        Compare cmp = {}) {
    for (const auto& pat : exclusions) {
        if (pat.size() < 2) continue;
        size_t cmpLen = std::min(bufLen, pat.size());
        if (cmpLen == 0) continue;
        bool match = true;
        for (size_t i = 0; i < cmpLen; ++i) {
            if (!cmp(pat[i], buf[i])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

/// Like MatchExclusionBuf but strips tones before comparing.
/// Used by modifier bypass: at modifier time the tone hasn't been applied yet,
/// so 'ă' (untoned) must match 'ắ' (toned) in the exclusion pattern.
[[nodiscard]] inline bool MatchExclusionBufIgnoreTone(
        const wchar_t* buf, size_t bufLen,
        const std::vector<std::wstring>& exclusions) {
    return MatchExclusionBuf(buf, bufLen, exclusions,
        [](wchar_t a, wchar_t b) { return StripTone(a) == StripTone(b); });
}

/// Build composed buffer with one character overridden at overrideIdx.
/// Returns buffer length. Caller provides a stack-allocated wchar_t[16].
template<typename CharStateT, typename ComposeFunc>
[[nodiscard]] inline size_t BuildExclusionBuf(
        const CharStateT* states, size_t count,
        ComposeFunc compose,
        wchar_t* buf,
        size_t overrideIdx, wchar_t overrideChar) {
    size_t bufLen = 0;
    for (size_t i = 0; i < count && bufLen < 16; ++i) {
        wchar_t ch = (i == overrideIdx) ? overrideChar : compose(states[i]);
        if (ch == 0) continue;
        buf[bufLen++] = towlower(ch);
    }
    return bufLen;
}

/// Typing-time check: would applying dd→đ produce a word matching a spell exclusion?
/// Simulates stroke on the d-target, builds composed buffer with 'd' replaced by 'đ',
/// then does bidirectional prefix match (buffer may be shorter than exclusion while typing).
template<typename CharStateT, typename ComposeFunc>
[[nodiscard]] inline bool WouldStrokeDMatchExclusion(
        const CharStateT* states, size_t count,
        const std::vector<std::wstring>& exclusions,
        ComposeFunc compose) {
    if (exclusions.empty() || count == 0) return false;
    size_t dIdx = FindStrokeDTarget(states, count);
    if (dIdx == SIZE_MAX) return false;

    wchar_t buf[16];
    size_t bufLen = BuildExclusionBuf(states, count, compose, buf, dIdx, L'\u0111');
    return MatchExclusionBuf(buf, bufLen, exclusions);
}

/// Typing-time check: would applying a tone produce a word matching a spell exclusion?
/// Simulates tone on the target vowel, builds composed buffer with the toned char,
/// then does bidirectional prefix match (buffer may be shorter than exclusion while typing).
/// Caller provides the target index and the pre-composed toned character.
template<typename CharStateT, typename ComposeFunc>
[[nodiscard]] inline bool WouldToneMatchExclusion(
        const CharStateT* states, size_t count,
        const std::vector<std::wstring>& exclusions,
        ComposeFunc compose,
        size_t toneIdx, wchar_t tonedChar) {
    if (exclusions.empty() || count == 0 || toneIdx >= count) return false;

    wchar_t buf[16];
    size_t bufLen = BuildExclusionBuf(states, count, compose, buf, toneIdx, tonedChar);
    return MatchExclusionBuf(buf, bufLen, exclusions);
}

/// Typing-time check: would applying a modifier at targetIdx produce a word matching
/// a spell exclusion? Uses tone-tolerant matching because tone hasn't been applied yet
/// (e.g. tentative 'ă' must match exclusion 'ắ').
template<typename CharStateT, typename ComposeFunc, typename ModifierT>
[[nodiscard]] inline bool WouldModifierMatchExclusion(
        const CharStateT* states, size_t count,
        const std::vector<std::wstring>& exclusions,
        ComposeFunc compose,
        size_t targetIdx, ModifierT mod) {
    if (exclusions.empty() || count == 0 || targetIdx >= count) return false;

    CharStateT tentative = states[targetIdx];
    tentative.mod = mod;
    wchar_t modChar = compose(tentative);

    wchar_t buf[16];
    size_t bufLen = BuildExclusionBuf(states, count, compose, buf, targetIdx, modChar);
    return MatchExclusionBufIgnoreTone(buf, bufLen, exclusions);
}

/// Typing-time check: would applying a modifier to any eligible vowel match an exclusion?
/// Horn: tries u/o. Breve: tries a. Circumflex: tries a/e/o.
/// Used by Telex 'w' key and VNI modifier keys (6/7/8) to bypass spellCheckDisabled_.
/// @param eligibleBases  Null-terminated string of base vowels eligible for this modifier
///                       (e.g. L"ao" for circumflex a/e/o → pass L"aeo").
template<typename CharStateT, typename ComposeFunc, typename ModifierT>
[[nodiscard]] inline bool WouldAnyModifierMatchExclusion(
        const CharStateT* states, size_t count,
        const std::vector<std::wstring>& exclusions,
        ComposeFunc compose,
        ModifierT mod, const wchar_t* eligibleBases) {
    if (exclusions.empty() || count == 0) return false;

    for (size_t i = 0; i < count; ++i) {
        if (!states[i].IsVowel() || states[i].mod != static_cast<ModifierT>(0)) continue;

        wchar_t base = states[i].base;
        bool eligible = false;
        for (const wchar_t* p = eligibleBases; *p; ++p) {
            if (base == *p) { eligible = true; break; }
        }
        if (!eligible) continue;
        if (WouldModifierMatchExclusion(states, count, exclusions, compose, i, mod))
            return true;
    }
    return false;
}

/// Returns true if the buffer ends with a stop-final consonant (c, ch, k, p, t)
/// preceded by at least one vowel. Scans backward from end.
/// Stop finals only accept Acute and Dot tones in Vietnamese phonology.
/// Used by pre-tone check to block Grave/Hook/Tilde before ProcessTone().
template<typename CharStateT>
[[nodiscard]] inline bool HasStopFinalCoda(
        const CharStateT* states, size_t count) noexcept {
    if (count < 2) return false;

    // Scan backward from end to find coda start (stop at first vowel)
    size_t codaEnd = count;
    size_t codaStart = codaEnd;
    while (codaStart > 0 && !states[codaStart - 1].IsVowel()) {
        --codaStart;
    }
    size_t codaLen = codaEnd - codaStart;

    // Must be preceded by a vowel (loop exit guarantees this when codaStart > 0)
    if (codaStart == 0) return false;

    if (codaLen == 1) {
        wchar_t c = towlower(states[codaStart].base);
        return c == L'c' || c == L'k' || c == L'p' || c == L't';
    }
    if (codaLen == 2) {
        wchar_t c0 = towlower(states[codaStart].base);
        wchar_t c1 = towlower(states[codaStart + 1].base);
        return (c0 == L'c' && c1 == L'h');  // ch is the only stop digraph
    }
    return false;
}

/// Check if states_ contains a modifier that the given key could escape.
/// Used by TypingEngine to bypass the spellCheckDisabled_ gate
/// for modifier escape (ww undoes horn, dd undoes stroke, etc.).
/// @param mod  The modifier type the key would apply/escape.
/// @param isStroke  True if checking for stroke-d (searches IsD() instead of IsVowel()).
template<typename CharStateT, typename ModifierT>
[[nodiscard]] inline bool HasEscapableModifier(
        const CharStateT* states, size_t count, ModifierT mod,
        bool isStroke = false) noexcept {
    for (size_t i = 0; i < count; ++i) {
        if (isStroke) {
            if (states[i].IsD() && states[i].mod == mod) return true;
        } else {
            if (states[i].IsVowel() && states[i].mod == mod) return true;
        }
    }
    return false;
}

/// Returns the index of the first vowel that currently carries a tone, or
/// SIZE_MAX if none. Valid Vietnamese syllables have at most one toned vowel.
template<typename CharStateT>
[[nodiscard]] inline size_t FindTonedVowelIndex(
        const CharStateT* states, size_t count) noexcept {
    for (size_t i = 0; i < count; ++i) {
        if (states[i].IsVowel() && states[i].HasTone())
            return i;
    }
    return SIZE_MAX;
}

/// Returns true if the open range (fromIdx, toIdx) — exclusive on both ends —
/// contains at least one non-vowel state. Used to detect that two vowel
/// indices belong to different syllables (a consonant closes one syllable and
/// starts another), which must block tone relocation across the boundary.
template<typename CharStateT>
[[nodiscard]] inline bool HasConsonantBetween(
        const CharStateT* states, size_t fromIdx, size_t toIdx) noexcept {
    if (fromIdx > toIdx) {
        size_t tmp = fromIdx;
        fromIdx = toIdx;
        toIdx = tmp;
    }
    for (size_t i = fromIdx + 1; i < toIdx; ++i) {
        if (!states[i].IsVowel()) return true;
    }
    return false;
}

/// P4 guard for tone relocation: returns true if the target vowel was selected
/// by FindToneTarget's P4 default (rightmost vowel, no diphthong rule for the
/// last two vowels). Prevents tone sliding when repeated vowels are typed,
/// e.g., "Kìaaaa" — tone stays on 'i', doesn't drift to the last 'a'.
/// Legitimate P3 relocations (coda changing a rule-3 target) have rule != 0.
template<typename CharStateT>
[[nodiscard]] inline bool IsToneRelocBlockedByP4(
        const CharStateT* states, size_t count,
        size_t targetIdx, bool modernOrtho) noexcept {
    // P1/P2 targets (horn, circumflex, breve) always attract the tone.
    if (states[targetIdx].HasModifier()) return false;

    // Find last two non-cluster vowels
    size_t vLast = SIZE_MAX, v2nd = SIZE_MAX;
    for (size_t i = count; i-- > 0;) {
        if (!states[i].IsVowel()) continue;
        if (IsClusterConsonant(states, count, i)) continue;
        if (vLast == SIZE_MAX) { vLast = i; continue; }
        v2nd = i;
        break;
    }
    if (targetIdx != vLast || v2nd == SIZE_MAX || vLast != v2nd + 1) return false;

    int fi = DiphthongVowelIndex(states[v2nd].base);
    int li = DiphthongVowelIndex(states[vLast].base);
    if (fi < 0 || li < 0) return false;

    const auto& table = modernOrtho ? kDiphthongModern : kDiphthongClassic;
    return table[fi][li] == 0;  // No diphthong rule → P4 default → block
}

}  // namespace NextKey
