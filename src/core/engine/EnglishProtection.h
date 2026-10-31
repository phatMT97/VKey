// VKey - English Protection Module (Header-Only)
// Copyright (c) 2024-2026 PhatMT. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-only
//
// 3-Tier English Protection System:
//   TIER 1: Hard reject impossible patterns (cl, cr, ending x/r/z/f)
//   TIER 2: Soft bias for ambiguous patterns (y + vowel)
//   TIER 3: User override via same-key insistence
//
// Always active (independent of spell check setting).
// All functions are constexpr/inline — zero runtime overhead.

#pragma once

#include <cstddef>
#include <cwctype>
#include "VietnameseTables.h"

namespace NextKey {

// =============================================================================
// Language Bias State
// =============================================================================

enum class LanguageBias : uint8_t {
    Unknown,      // Initial state, not yet determined
    HardEnglish,  // TIER 1: Definitely not Vietnamese (disable composition)
    SoftEnglish,  // TIER 2: Ambiguous, defer tone until user insists
    Vietnamese    // Confirmed Vietnamese pattern
};

// Shared state struct embedded in TypingEngine.
struct EnglishProtectionState {
    LanguageBias bias = LanguageBias::Unknown;
    wchar_t lastToneKey = 0;
    int sameToneKeyCount = 0;

    constexpr void Reset() noexcept {
        bias = LanguageBias::Unknown;
        lastToneKey = 0;
        sameToneKeyCount = 0;
    }
};

// =============================================================================
// TIER 1: Hard English Pattern Detection (O(1))
// =============================================================================

/// Check if two starting consonants form an impossible Vietnamese cluster.
/// Vietnamese has: ch, gh, gi, kh, ng, ngh, nh, ph, qu, th, tr
/// All other 2-consonant starts are English-only.
[[nodiscard]] inline bool IsHardEnglishStart(wchar_t c0, wchar_t c1) noexcept {
    c0 = towlower(c0);
    c1 = towlower(c1);
    // Exhaustive list of impossible Vietnamese start clusters
    return (c0 == L'c' && c1 == L'l') ||
           (c0 == L'c' && c1 == L'r') ||
           (c0 == L'b' && c1 == L'r') ||
           (c0 == L'b' && c1 == L'l') ||
           (c0 == L'd' && c1 == L'r') ||
           (c0 == L'f' && c1 == L'r') ||
           (c0 == L'f' && c1 == L'l') ||
           (c0 == L'g' && c1 == L'r') ||
           (c0 == L'g' && c1 == L'l') ||
           (c0 == L'p' && c1 == L'r') ||
           (c0 == L'p' && c1 == L'l') ||
           (c0 == L's' && c1 == L'm') ||
           (c0 == L's' && c1 == L'n') ||
           (c0 == L's' && c1 == L'p') ||
           (c0 == L's' && c1 == L'w') ||
           (c0 == L's' && c1 == L't') ||
           (c0 == L's' && c1 == L'c') ||
           (c0 == L's' && c1 == L'k') ||
           (c0 == L's' && c1 == L'l') ||
           (c0 == L'w' && c1 == L'h') ||
           (c0 == L'w' && c1 == L'r') ||
           (c0 == L'k' && c1 == L'n') ||
           (c0 == L'p' && c1 == L'n') ||
           (c0 == L'p' && c1 == L's');
}

/// Raw-input variant: check the first two RAW keystrokes against the same
/// impossible-Vietnamese start-cluster table. Needed because Telex P8 rewrites
/// a leading standalone `w` to synthetic `ư` in states_ before CheckEnglishBias
/// sees it — so `wh`/`wr` would slip past the states-based start check.
/// rawInput_ holds keystrokes verbatim, so the cluster is still visible there.
[[nodiscard]] inline bool IsHardEnglishRawStart(
        const wchar_t* raw, size_t len) noexcept {
    if (len < 2) return false;
    return IsHardEnglishStart(raw[0], raw[1]);
}

/// Check if a consonant is impossible at the end of a Vietnamese word.
/// Vietnamese final consonants: c, ch, m, n, ng, nh, p, t
/// These NEVER end a Vietnamese word: x, r, z, f, b, d, g, h, k, l, q, s, v, w
[[nodiscard]] inline bool IsHardEnglishEnd(wchar_t c) noexcept {
    c = towlower(c);
    // Only check the most distinctive ones to avoid false positives during typing
    // (user might still type more chars). Focus on Telex tone keys (s, f, r, x, j)
    // plus z (clear-tone key) — none of which ever end Vietnamese words.
    return c == L'x' || c == L'r' || c == L'z' || c == L'f' ||
           c == L's' || c == L'j';
}

/// Q without U is impossible in Vietnamese
[[nodiscard]] inline bool IsQWithoutU(wchar_t c0, wchar_t c1) noexcept {
    return towlower(c0) == L'q' && towlower(c1) != L'u';
}

// =============================================================================
// TIER 2: Soft English Bias (y + vowel patterns)
// =============================================================================

/// Check if a y-initial sequence is a valid Vietnamese pattern.
/// Valid: yê + u/n/m/t (yêu, yên, yếu, yết...)
/// Template works structurally against CharState.
template<typename CharStateT>
[[nodiscard]] inline bool IsValidVietnameseYSequence(
        const CharStateT* states, size_t count) noexcept {
    if (count < 2) return false;
    if (towlower(states[0].base) != L'y') return false;

    // y + ê (e with circumflex) → valid start
    if (states[1].base == L'e' && states[1].HasModifier()) {
        if (count == 2) return true;  // Could still become yêu, yên...
        wchar_t c2 = towlower(states[2].base);
        return (c2 == L'u' || c2 == L'n' || c2 == L'm' || c2 == L't');
    }

    return false;
}

/// Check for soft English bias (y + vowel without valid VN continuation).
template<typename CharStateT>
[[nodiscard]] inline bool CheckSoftEnglishBias(
        const CharStateT* states, size_t count) noexcept {
    if (count < 2) return false;
    if (towlower(states[0].base) != L'y') return false;

    // y + a/e/o (ambiguous — could be English year/yes/you)
    wchar_t c1 = towlower(states[1].base);
    bool isAmbiguous = states[1].IsVowel() &&
                       (c1 == L'a' || c1 == L'o' || c1 == L'e');

    if (isAmbiguous && !IsValidVietnameseYSequence(states, count)) {
        return true;
    }
    return false;
}

// =============================================================================
// TIER 3: Same-Key Insistence
// =============================================================================

/// Update insistence tracking. Returns true if user has "insisted"
/// (pressed the same tone key twice consecutively).
inline bool UpdateToneInsistence(wchar_t toneKey,
                                 EnglishProtectionState& state) noexcept {
    if (toneKey == state.lastToneKey) {
        state.sameToneKeyCount++;
    } else {
        state.lastToneKey = toneKey;
        state.sameToneKeyCount = 1;
    }
    return state.sameToneKeyCount >= 2;
}

// =============================================================================
// Pre-Tone Hard English Context Check
// =============================================================================

/// Detect structural V+C(1+)+V pattern anywhere in the buffer.
/// Vietnamese syllables never have a consonant between two vowel groups.
/// Catches: "behavior" (e+h+a), "manager" (a+ng+e), "danger" (a+ng+e), etc.
/// Used by VNI tone gate (directly) and Telex tone gate (via IsHardEnglishToneContext).
template<typename CharStateT>
[[nodiscard]] inline bool HasStructuralVCVPattern(
        const CharStateT* states, size_t count) noexcept {
    if (count < 4) return false;  // ≥4: catches "behavior","release". <4: preserves "ipỏn" free-mark

    // Scan forward: find vowel groups separated by consonant(s).
    size_t i = 0;
    while (i < count) {
        // Skip consonants
        if (!states[i].IsVowel()) { ++i; continue; }

        // Found start of a vowel group — skip all adjacent vowels
        size_t vowelEnd = i;
        while (vowelEnd < count && states[vowelEnd].IsVowel()) ++vowelEnd;

        // Count consonants after this vowel group
        size_t consStart = vowelEnd;
        int consonants = 0;
        while (vowelEnd < count && !states[vowelEnd].IsVowel()) {
            ++vowelEnd;
            ++consonants;
        }

        // Check for next vowel group after consonant(s)
        if (consonants >= 1 && vowelEnd < count && states[vowelEnd].IsVowel()) {
            // Exception: modified vowel (ê, â, ô) + exactly 1 valid Vietnamese coda
            // consonant (c, k, m, n, p, t) is plausible nucleus+coda, not V+C+V.
            // E.g., {h,i,ê,n,e}: ê+n+e could be a typo after valid "hiên".
            bool exception = false;
            if (consonants == 1) {
                bool hasModifier = false;
                for (size_t j = i; j < consStart; ++j) {
                    if (states[j].HasModifier()) { hasModifier = true; break; }
                }
                if (hasModifier) {
                    wchar_t coda = states[consStart].base;
                    exception = (coda == L'c' || coda == L'k' || coda == L'm' ||
                                 coda == L'n' || coda == L'p' || coda == L't');
                }
            }
            if (!exception) return true;
        }

        i = vowelEnd;
    }
    return false;
}

/// Detect structural impossibility gated by tone key type.
/// For Telex: only fires when toneKey is a letter that can't end Vietnamese words
/// (s, f, r, x, j, z). For VNI digit keys, use HasStructuralVCVPattern() directly.
template<typename CharStateT>
[[nodiscard]] inline bool IsHardEnglishToneContext(
        const CharStateT* states, size_t count, wchar_t toneKey) noexcept {
    if (!IsHardEnglishEnd(toneKey)) return false;
    return HasStructuralVCVPattern(states, count);
}

// =============================================================================
// Consonant Cluster Detection — shared by tone target and vowel pair checks
// =============================================================================

/// Check if states[i] is acting as a consonant due to a Vietnamese onset cluster,
/// even though it would normally be classified as a vowel by IsVowel():
///   "gi" cluster: g + i + vowel → 'i' is consonant onset (gió, giỏi, giống...)
///   "qu" cluster: q + u → 'u' is consonant glide (quốc, quả, quê...)
///
/// Callers must verify states[i].IsVowel() before calling — this function only
/// distinguishes nucleus vowels from cluster consonants within the vowel set.
template<typename CharStateT>
[[nodiscard]] inline bool IsClusterConsonant(
        const CharStateT* states, size_t count, size_t i) noexcept {
    if (i == 0) return false;
    // "gi" cluster: 'i' is consonant when preceded by 'g' and followed by another vowel
    if (states[i].base == L'i' && states[i-1].base == L'g'
        && i + 1 < count && states[i+1].IsVowel()) return true;
    // "qu" cluster: 'u' is always a consonant glide when preceded by 'q'
    if (states[i].base == L'u' && states[i-1].base == L'q') return true;
    return false;
}

// =============================================================================
// Invalid Adjacent Vowel Pair — Pre-Tone Hard English Check
// =============================================================================

/// Detect adjacent plain vowel pairs that cannot form a Vietnamese syllable nucleus.
/// Uses diphthong tables: if BOTH classic AND modern have rule 0 for a pair,
/// it is not a recognized Vietnamese diphthong combination.
///
/// EXCEPTIONS (skipped even if rule 0):
///   i+e (2+1) — smart accent intermediate state for 'iê' (tiếng = t-i-e-n-s-g-e)
///   y+a (5+0) — part of the "uya" triphthong (khuya, đêm khuya, etc.)
///
/// Catches: "sea"+'r' (e+a=0,0), "aerial"+'r' (a+e=0,0), "boy"+'s' (o+y=0,0).
/// Does NOT catch: "oa"+'f' (rule 3), "ai"+'s' (rule 1), "ie"+'s', "uya"+'s'.
///
/// Call in the tone gate BEFORE ProcessTone() to block tone on invalid nuclei.
template<typename CharStateT>
[[nodiscard]] inline bool HasInvalidAdjacentVowelPair(
        const CharStateT* states, size_t count) noexcept {
    for (size_t i = 0; i + 1 < count; ++i) {
        if (!states[i].IsVowel() || !states[i+1].IsVowel()) continue;
        if (states[i].HasModifier() || states[i+1].HasModifier()) continue;
        // Skip cluster consonants (gi/qu onset). states[i+1].IsVowel() is already
        // guaranteed above, satisfying IsClusterConsonant's gi-cluster check.
        if (IsClusterConsonant(states, count, i)) continue;
        int v0 = DiphthongVowelIndex(states[i].base);
        int v1 = DiphthongVowelIndex(states[i+1].base);
        if (v0 < 0 || v1 < 0) continue;
        // Exception: i+e — valid smart accent intermediate state (tiê..., miê...)
        if (v0 == 2 && v1 == 1) continue;
        // Exception: y+a — part of the "uya" triphthong (khuya, đêm khuya, etc.)
        if (v0 == 5 && v1 == 0) continue;
        // Exception: y+e — smart-accent intermediate for yê / uyê triphthong
        // (yến, yêu, chuyện, nguyễn, tuyết, xuyến). Mirrors the i+e case:
        // yê is the only Vietnamese nucleus that contains y+e, so a plain
        // y+e in the buffer is an in-progress syllable, not English.
        // Free-marking via a trailing 'e' later promotes e → ê and
        // RelocateToneToTarget moves any tone onto the new ê.
        if (v0 == 5 && v1 == 1) continue;
        // Exception: o+o — the only way a literal "oo" pair reaches the buffer
        // is the ooo→oo circumflex escape (normal "oo" collapses to "ô"). "oo"
        // is a valid Vietnamese nucleus with coda c/ng (xoong, boong, voọc,
        // soóc, goòng), so a tone must be allowed to land on it rather than
        // tripping the HardEnglish guard.
        if (v0 == 3 && v1 == 3) continue;
        if (kDiphthongClassic[v0][v1] == 0 && kDiphthongModern[v0][v1] == 0) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// TIER 1: Invalid Vietnamese Coda Structure
// =============================================================================

/// Check if consonants after the last vowel form a valid Vietnamese coda.
/// Vietnamese syllable coda is strictly limited to:
///   Single consonant: c, m, n, p, t
///   Digraph:          ch, ng, nh  (only these three)
/// Any other 2-consonant cluster after a vowel (e.g. pp, mp, nd, rr) is
/// structurally impossible in Vietnamese — the word must be English/foreign.
/// 3+ consonants after a vowel are always invalid.
///
/// Called from CheckEnglishBias after each new consonant is added.
/// Template works structurally against CharState.
template<typename CharStateT>
[[nodiscard]] inline bool IsInvalidVietnameseCoda(
        const CharStateT* states, size_t count) noexcept {
    if (count < 3) return false;  // Need at least vowel + 2 consonants to check

    // Count consonants from the end back to the last vowel.
    // 'd' at position 0 (onset) is vowel-like — the scan stops there so
    // buffers like [d,d,...] or [đ,...] don't report a bogus coda.
    // A 'd' at any non-initial position is treated as a coda consonant
    // — Vietnamese never places 'd' in coda, so patterns like [a,d,v]
    // correctly surface as invalid (dv ≠ ch/ng/nh).
    size_t codaLen = 0;
    bool foundVowel = false;
    for (size_t i = count; i-- > 0;) {
        if (states[i].IsVowel() || (states[i].IsD() && i == 0)) {
            foundVowel = true; break;
        }
        ++codaLen;
    }

    if (!foundVowel) return false;   // All consonants so far = onset cluster (ngh, kh, tr...), not coda
    if (codaLen < 2) return false;   // 0-1 coda consonants are always valid
    if (codaLen >= 3) return true;   // 3+ consecutive coda consonants: never Vietnamese

    // codaLen == 2: only ch, ng, nh are valid Vietnamese final digraphs
    wchar_t c1 = towlower(states[count - 2].base);
    wchar_t c2 = towlower(states[count - 1].base);
    bool validDigraph = (c1 == L'c' && c2 == L'h') ||  // ch
                        (c1 == L'n' && c2 == L'g') ||  // ng
                        (c1 == L'n' && c2 == L'h');    // nh
    return !validDigraph;
}

// =============================================================================
// Combined Bias Check — runs Tier 1 + Tier 2
// =============================================================================

/// Analyze states and update English protection bias.
/// Call after each PushChar() that adds a new state.
/// Template works structurally against CharState.
template<typename CharStateT>
inline void CheckEnglishBias(const CharStateT* states, size_t count,
                             EnglishProtectionState& prot) noexcept {
    // Skip if already determined as Vietnamese (modifier/tone was applied)
    if (prot.bias == LanguageBias::Vietnamese) return;

    // TIER 1: Hard reject — check start clusters
    if (count >= 2 && !states[0].IsVowel() && !states[1].IsVowel() &&
        !states[0].IsD()) {
        if (IsHardEnglishStart(states[0].base, states[1].base) ||
            IsQWithoutU(states[0].base, states[1].base)) {
            prot.bias = LanguageBias::HardEnglish;
            return;
        }
    }

    // TIER 1: Hard reject — check end consonants (only at 3+ chars, after a vowel)
    if (count >= 3) {
        const auto& last = states[count - 1];
        if (!last.IsVowel() && !last.IsD()) {
            // Only flag as hard English if previous char was a vowel
            // (consonant after vowel = potential word ending)
            const auto& prev = states[count - 2];
            if (prev.IsVowel() && IsHardEnglishEnd(last.base)) {
                prot.bias = LanguageBias::HardEnglish;
                return;
            }
        }
    }

    // TIER 1: Hard reject — invalid Vietnamese coda structure.
    // Catches English words like "approved" (pp), "append" (pp), "amp" (mp),
    // "and" (nd) where 2+ consonants after a vowel don't form ch/ng/nh.
    if (IsInvalidVietnameseCoda(states, count)) {
        prot.bias = LanguageBias::HardEnglish;
        return;
    }

    // TIER 2: Soft bias — y + vowel
    if (prot.bias == LanguageBias::Unknown && CheckSoftEnglishBias(states, count)) {
        prot.bias = LanguageBias::SoftEnglish;
    }
}

// =============================================================================
// English Word Block — Raw Prefix Check (spell check required)
// =============================================================================

/// Table entry: a raw keystroke prefix that blocks tone or modifier application.
/// Patterns are lowercase.  To add a new word, append one line to the table.
struct RawPrefixRule {
    const wchar_t* pattern;
    uint8_t len;
};

/// Tone-blocking prefixes — checked BEFORE ProcessTone().
/// rawInput_ already includes the incoming key when checked.
constexpr RawPrefixRule kBlockedTonePrefixes[] = {
    {L"pas",  3},   // pass, passing, passive, password, passion...
    {L"gues", 4},   // guess, guessing...
    {L"per",  3},   // perfect, person, perform, percent, perhaps, period...
};

/// Modifier-blocking prefixes — checked BEFORE ProcessModifier().
constexpr RawPrefixRule kBlockedModifierPrefixes[] = {
    {L"pow", 3},    // power, powerful, powder...
    {L"upw", 3},    // upward, upload, update, upon...
};

/// Match the head of `raw` against a table of prefix rules.
/// Prefix (not suffix) is correct: rawInput_ is cleared at word boundaries,
/// so it holds the current word. Suffix matching produced false positives for
/// delayed-VCV words like "apas" → "ấp" (tail "pas" matched the English block).
[[nodiscard]] inline bool MatchesRawPrefix(
        const wchar_t* raw, size_t rawLen,
        const RawPrefixRule* rules, size_t ruleCount) noexcept {
    for (size_t r = 0; r < ruleCount; ++r) {
        if (rawLen < rules[r].len) continue;
        const wchar_t* pat = rules[r].pattern;
        bool match = true;
        for (uint8_t i = 0; i < rules[r].len; ++i) {
            if (towlower(raw[i]) != pat[i]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

/// Check if raw keystrokes start with a known English prefix that blocks tone.
[[nodiscard]] inline bool IsBlockedEnglishTone(
        const wchar_t* raw, size_t len) noexcept {
    return MatchesRawPrefix(raw, len, kBlockedTonePrefixes,
        sizeof(kBlockedTonePrefixes) / sizeof(kBlockedTonePrefixes[0]));
}

/// Check if raw keystrokes start with a known English prefix that blocks modifier.
[[nodiscard]] inline bool IsBlockedEnglishModifier(
        const wchar_t* raw, size_t len) noexcept {
    return MatchesRawPrefix(raw, len, kBlockedModifierPrefixes,
        sizeof(kBlockedModifierPrefixes) / sizeof(kBlockedModifierPrefixes[0]));
}

}  // namespace NextKey
