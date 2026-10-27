// VKey - PhonotacticsValidator Implementation
// Copyright (c) 2024-2026 PhatMT. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-only
//
// Vietnamese syllable structure: [C₁] + V + [C₂]
// Uses greedy consonant matching and packed-key vowel nucleus table (linear scan).

#include "PhonotacticsValidator.h"
#include "TypingEngine.h"
#include "VietnamesePhonologyData.h"
#include <algorithm>
#include <cwctype>

namespace NextKey {
namespace Phonology {

namespace {

// Packed-key encoding and VCPair rule data live in
// VietnamesePhonologyData.h. CharState-specific scanners stay local here.

//=============================================================================
// Vowel nucleus table
//=============================================================================

// Each entry: { key, canHaveEndConsonant }
struct VowelEntry {
    uint32_t key;
    bool canEnd;  // Can be followed by a final consonant?
    bool requiresEnd = false;
};

// Sorted by key (currently searched linearly; ~50 entries so O(n) is fine)
constexpr VowelEntry kVowelTable[] = {
    // === Single vowels (can have end consonant) ===
    { Key1(VowelSlot(kA, kNone)), true },   // a
    { Key1(VowelSlot(kA, kCirc)), true, true },   // â requires rhyme closure
    { Key1(VowelSlot(kA, kBrev)), true },   // ă
    { Key1(VowelSlot(kE, kNone)), true },   // e
    { Key1(VowelSlot(kE, kCirc)), true },   // ê
    { Key1(VowelSlot(kI, kNone)), true },   // i
    { Key1(VowelSlot(kO, kNone)), true },   // o
    { Key1(VowelSlot(kO, kCirc)), true },   // ô
    { Key1(VowelSlot(kO, kHorn)), true },   // ơ
    { Key1(VowelSlot(kU, kNone)), true },   // u
    { Key1(VowelSlot(kU, kHorn)), true },   // ư
    { Key1(VowelSlot(kY, kNone)), true },   // y

    // === Double vowels WITHOUT end consonant ===
    { Key2(VowelSlot(kA, kNone), VowelSlot(kI, kNone)), false },  // ai
    { Key2(VowelSlot(kA, kNone), VowelSlot(kO, kNone)), false },  // ao
    { Key2(VowelSlot(kA, kNone), VowelSlot(kU, kNone)), false },  // au
    { Key2(VowelSlot(kA, kNone), VowelSlot(kY, kNone)), false },  // ay
    { Key2(VowelSlot(kA, kCirc), VowelSlot(kU, kNone)), false },  // âu
    { Key2(VowelSlot(kA, kCirc), VowelSlot(kY, kNone)), false },  // ây
    { Key2(VowelSlot(kE, kNone), VowelSlot(kO, kNone)), false },  // eo
    { Key2(VowelSlot(kE, kCirc), VowelSlot(kU, kNone)), false },  // êu
    { Key2(VowelSlot(kI, kNone), VowelSlot(kA, kNone)), false },  // ia
    { Key2(VowelSlot(kI, kNone), VowelSlot(kU, kNone)), false },  // iu
    { Key2(VowelSlot(kO, kNone), VowelSlot(kA, kNone)), true },   // oa (can have end: loan, hoàn)
    { Key2(VowelSlot(kO, kNone), VowelSlot(kI, kNone)), false },  // oi
    { Key2(VowelSlot(kO, kCirc), VowelSlot(kI, kNone)), false },  // ôi
    { Key2(VowelSlot(kO, kHorn), VowelSlot(kI, kNone)), false },  // ơi
    { Key2(VowelSlot(kU, kNone), VowelSlot(kA, kNone)), false },  // ua
    { Key2(VowelSlot(kU, kNone), VowelSlot(kI, kNone)), false },  // ui
    { Key2(VowelSlot(kU, kHorn), VowelSlot(kA, kNone)), false },  // ưa
    { Key2(VowelSlot(kU, kHorn), VowelSlot(kI, kNone)), false },  // ưi
    { Key2(VowelSlot(kU, kHorn), VowelSlot(kU, kNone)), false },  // ưu

    // === Double vowels WITH end consonant ===
    { Key2(VowelSlot(kI, kNone), VowelSlot(kE, kCirc)), true },   // iê
    { Key2(VowelSlot(kO, kNone), VowelSlot(kA, kBrev)), true },   // oă
    { Key2(VowelSlot(kO, kNone), VowelSlot(kE, kNone)), true },   // oe
    { Key2(VowelSlot(kO, kNone), VowelSlot(kO, kNone)), true },   // oo
    { Key2(VowelSlot(kU, kNone), VowelSlot(kA, kCirc)), true },   // uâ
    { Key2(VowelSlot(kU, kNone), VowelSlot(kE, kCirc)), true },   // uê
    { Key2(VowelSlot(kU, kNone), VowelSlot(kO, kCirc)), true },   // uô
    { Key2(VowelSlot(kU, kNone), VowelSlot(kO, kHorn)), true },   // uơ (huơ, quơ — horn only on o)
    { Key2(VowelSlot(kU, kNone), VowelSlot(kY, kNone)), true },   // uy
    { Key2(VowelSlot(kU, kHorn), VowelSlot(kO, kHorn)), true },   // ươ
    { Key2(VowelSlot(kY, kNone), VowelSlot(kE, kCirc)), true },   // yê

    // === Triple vowels WITHOUT end consonant ===
    { Key3(VowelSlot(kI, kNone), VowelSlot(kE, kCirc), VowelSlot(kU, kNone)), false },  // iêu
    { Key3(VowelSlot(kO, kNone), VowelSlot(kA, kNone), VowelSlot(kI, kNone)), false },  // oai
    { Key3(VowelSlot(kO, kNone), VowelSlot(kA, kNone), VowelSlot(kY, kNone)), false },  // oay
    { Key3(VowelSlot(kO, kNone), VowelSlot(kE, kNone), VowelSlot(kO, kNone)), false },  // oeo
    { Key3(VowelSlot(kU, kNone), VowelSlot(kA, kCirc), VowelSlot(kY, kNone)), false },  // uây
    { Key3(VowelSlot(kU, kNone), VowelSlot(kO, kCirc), VowelSlot(kI, kNone)), false },  // uôi
    { Key3(VowelSlot(kU, kNone), VowelSlot(kY, kNone), VowelSlot(kA, kNone)), false },  // uya (khuya)
    { Key3(VowelSlot(kU, kNone), VowelSlot(kY, kNone), VowelSlot(kU, kNone)), false },  // uyu (khuỷu)
    { Key3(VowelSlot(kU, kHorn), VowelSlot(kO, kHorn), VowelSlot(kI, kNone)), false },  // ươi
    { Key3(VowelSlot(kU, kHorn), VowelSlot(kO, kHorn), VowelSlot(kU, kNone)), false },  // ươu

    // === Triple vowels WITH end consonant ===
    { Key3(VowelSlot(kU, kNone), VowelSlot(kY, kNone), VowelSlot(kE, kCirc)), true },   // uyê

    // === yêu is triple no-end ===
    { Key3(VowelSlot(kY, kNone), VowelSlot(kE, kCirc), VowelSlot(kU, kNone)), false },  // yêu
};

constexpr size_t kVowelTableSize = sizeof(kVowelTable) / sizeof(kVowelTable[0]);

// Encode a parsed final consonant as a bitmask
template<typename CharStateT>
uint16_t FinalConsonantBit(const CharStateT* states, size_t finalLen) {
    if (finalLen == 0) return 0;
    wchar_t c0 = states[0].base;
    if (finalLen == 2) {
        wchar_t c1 = states[1].base;
        if (c0 == L'c' && c1 == L'h') return F_ch;
        if (c0 == L'n' && c1 == L'g') return F_ng;
        if (c0 == L'n' && c1 == L'h') return F_nh;
        return 0;
    }
    // Single char
    switch (c0) {
        case L'c': return F_c;
        case L'k': return F_k;
        case L'm': return F_m;
        case L'n': return F_n;
        case L'p': return F_p;
        case L't': return F_t;
        default:   return 0;
    }
}

// Linear search the vowel table for exact match (~50 entries, fast enough)
const VowelEntry* FindVowel(uint32_t key) {
    for (size_t i = 0; i < kVowelTableSize; ++i) {
        if (kVowelTable[i].key == key) return &kVowelTable[i];
    }
    return nullptr;
}

// Check if key is a prefix of any entry in the vowel table
bool IsVowelPrefix(uint32_t key, int numSlots) {
    // Extract the slot byte from the key (strip length prefix)
    uint8_t keySlot0 = static_cast<uint8_t>(key & 0xFF);
    if (numSlots == 2) keySlot0 = static_cast<uint8_t>((key >> 8) & 0xFF);

    for (size_t i = 0; i < kVowelTableSize; ++i) {
        uint32_t entryKey = kVowelTable[i].key;
        uint8_t entryLen = static_cast<uint8_t>(entryKey >> 24);

        if (numSlots == 1 && entryLen >= 2) {
            // Check if first slot of 2+ entry matches our 1-slot key
            uint8_t entryFirst;
            if (entryLen == 2) entryFirst = static_cast<uint8_t>((entryKey >> 8) & 0xFF);
            else entryFirst = static_cast<uint8_t>((entryKey >> 16) & 0xFF);  // 3-slot
            if (entryFirst == keySlot0) return true;
        } else if (numSlots == 2 && entryLen == 3) {
            // Check if first 2 slots of 3-slot entry match our 2-slot key
            uint16_t entryFirst2 = static_cast<uint16_t>((entryKey >> 8) & 0xFFFF);
            uint16_t keySlots = static_cast<uint16_t>(key & 0xFFFF);  // strip length prefix
            if (entryFirst2 == keySlots) return true;
        }
    }
    return false;
}

//=============================================================================
// Initial consonant validation
//=============================================================================

// Valid initial consonants (greedy match 3→2→1):
// 3-char: ngh
// 2-char: ch, gh, gi, kh, ng, nh, ph, qu, th, tr
// 1-char: b, c, d, g, h, k, l, m, n, p, q, r, s, t, v, x
// đ: represented as d with mod != None

template<typename CharStateT>
size_t ParseInitialConsonant(const CharStateT* states, size_t count, bool allowZwjf = false) {
    if (count == 0) return 0;

    // Check đ (d with modifier)
    if (states[0].IsD() && states[0].mod != decltype(states[0].mod){}) {
        return 1;  // đ is a single consonant
    }

    wchar_t c0 = states[0].base;
    if (states[0].IsVowel()) return 0;  // Vowel, no initial consonant

    // Try 3-char: ngh
    if (count >= 3 && c0 == L'n' && states[1].base == L'g' && states[2].base == L'h' &&
        !states[1].IsVowel() && !states[2].IsVowel()) {
        return 3;
    }

    // Try 2-char consonants
    if (count >= 2 && !states[1].IsVowel() && !states[1].IsD()) {
        wchar_t c1 = states[1].base;
        if ((c0 == L'c' && c1 == L'h') ||
            (c0 == L'g' && c1 == L'h') ||
            (c0 == L'g' && c1 == L'i') ||
            (c0 == L'k' && c1 == L'h') ||
            (c0 == L'n' && c1 == L'g') ||
            (c0 == L'n' && c1 == L'h') ||
            (c0 == L'p' && c1 == L'h') ||
            (c0 == L'q' && c1 == L'u') ||
            (c0 == L't' && c1 == L'h') ||
            (c0 == L't' && c1 == L'r')) {
            return 2;
        }
    }

    // Try 2-char: "gi" where second char is vowel 'i' — special case
    // "gi" is a consonant cluster when followed by another vowel
    // But "gi" alone can also be consonant g + vowel i
    // Handle at call site with dual decomposition
    if (count >= 2 && c0 == L'g' && states[1].base == L'i' && states[1].IsVowel()) {
        // This is ambiguous — caller handles gi/qu decomposition
        // For now, don't consume the 'i' as consonant here
    }

    // Try 1-char consonants
    if (c0 == L'b' || c0 == L'c' || c0 == L'd' || c0 == L'g' || c0 == L'h' ||
        c0 == L'k' || c0 == L'l' || c0 == L'm' || c0 == L'n' || c0 == L'p' ||
        c0 == L'q' || c0 == L'r' || c0 == L's' || c0 == L't' || c0 == L'v' ||
        c0 == L'x' ||
        // allowZwjf: z/j≡gi, w≡qu, f≡ph as valid initial consonants
        (allowZwjf && (c0 == L'z' || c0 == L'j' || c0 == L'w' || c0 == L'f'))) {
        return 1;
    }

    return 0;  // Not a valid consonant
}

//=============================================================================
// Final consonant validation
//=============================================================================

// Valid final consonants: c, ch, k, m, n, ng, nh, p, t
// ('k' for minority-language proper nouns: Đắk Lắk, Đắk Nông, etc.)
// Returns number of states consumed, 0 if no valid final consonant

template<typename CharStateT>
size_t ParseFinalConsonant(const CharStateT* states, size_t count) {
    if (count == 0) return 0;

    wchar_t c0 = states[0].base;
    if (states[0].IsVowel()) return 0;

    // Try 2-char finals
    if (count >= 2 && !states[1].IsVowel()) {
        wchar_t c1 = states[1].base;
        if ((c0 == L'c' && c1 == L'h') ||
            (c0 == L'n' && c1 == L'g') ||
            (c0 == L'n' && c1 == L'h')) {
            return 2;
        }
    }

    // Try 1-char finals (k = same stop as c, used in minority-language proper nouns)
    if (c0 == L'c' || c0 == L'k' || c0 == L'm' || c0 == L'n' || c0 == L'p' || c0 == L't') {
        return 1;
    }

    return 0;
}

// Check if final consonant is a "stop" final (c, ch, k, p, t)
// Stop finals only allow Acute and Dot tones
template<typename CharStateT>
bool IsStopFinal(const CharStateT* states, size_t finalLen) {
    if (finalLen == 0) return false;
    wchar_t c0 = states[0].base;
    if (c0 == L'p' || c0 == L't') return true;
    if (c0 == L'c' || c0 == L'k') return true;  // c, ch, k are all stops
    return false;
}

//=============================================================================
// Tone extraction
//=============================================================================

template<typename CharStateT>
auto GetTone(const CharStateT* states, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (states[i].tone != decltype(states[i].tone){}) {
            return states[i].tone;
        }
    }
    return decltype(states[0].tone){};  // None
}

//=============================================================================
// Modifier ordinal extraction (works for both Telex and VNI modifier enums)
//=============================================================================

template<typename ModT>
uint8_t ModOrdinal(ModT mod) {
    // Modifier exposes:
    // None=0, Circumflex=1, Breve=2, Horn=3
    // Vni also has Stroke=4 but that's only for đ
    auto val = static_cast<uint8_t>(mod);
    if (val > 3) return 0;  // Stroke → treat as None for vowel encoding
    return val;
}

//=============================================================================
// Try modified vowels — check if adding a modifier to any unmodified vowel
// slot would produce a valid nucleus satisfying the predicate.
//=============================================================================

uint32_t BuildModifiedKey(const uint8_t* bases, const uint8_t* mods,
                          size_t vowelLen, size_t vi, uint8_t tryMod) {
    auto slot = [&](size_t i) -> uint8_t {
        return VowelSlot(bases[i], (i == vi) ? tryMod : mods[i]);
    };
    if (vowelLen == 1) return Key1(slot(0));
    if (vowelLen == 2) return Key2(slot(0), slot(1));
    return Key3(slot(0), slot(1), slot(2));
}

template<typename CharStateT, typename Pred>
bool TryModifiedVowels(const CharStateT* vowelStates, size_t vowelLen, Pred pred) {
    if (vowelLen < 1 || vowelLen > 3) return false;

    uint8_t bases[3], mods[3];
    for (size_t i = 0; i < vowelLen; ++i) {
        bases[i] = BaseIndex(vowelStates[i].base);
        if (bases[i] == 0xFF) return false;
        mods[i] = ModOrdinal(vowelStates[i].mod);
    }

    for (size_t vi = 0; vi < vowelLen; ++vi) {
        if (mods[vi] != kNone) continue;
        for (uint8_t tryMod = 1; tryMod <= 3; ++tryMod) {
            uint32_t tryKey = BuildModifiedKey(bases, mods, vowelLen, vi, tryMod);
            const VowelEntry* tryEntry = FindVowel(tryKey);
            if (tryEntry && pred(tryEntry)) return true;
        }
    }

    // u+o moves together under horn (ư+ơ) — a raw "uo" pair inside a 3-vowel
    // nucleus (uou→ươu, uoi→ươi) only becomes valid when BOTH slots flip at
    // once. The single-slot loop above can't find that: a lone o→ơ or u→ư
    // never matches those table entries, so "ruou"+w wrongly validated
    // Invalid and blocked the horn modifier (issue: ruouwj).
    if (vowelLen == 3 && bases[0] == kU && bases[1] == kO &&
        mods[0] == kNone && mods[1] == kNone) {
        uint32_t tryKey = Key3(VowelSlot(bases[0], kHorn), VowelSlot(bases[1], kHorn),
                                VowelSlot(bases[2], mods[2]));
        const VowelEntry* tryEntry = FindVowel(tryKey);
        if (tryEntry && pred(tryEntry)) return true;
    }
    return false;
}

//=============================================================================
// Core validation (single decomposition attempt)
//=============================================================================

template<typename CharStateT>
SyllableState ValidateDecomposition(const CharStateT* states, size_t count,
                             size_t initialLen) {
    size_t pos = initialLen;
    size_t remaining = count - pos;

    // All chars consumed by consonant → ValidPrefix (awaiting vowel)
    if (remaining == 0) {
        return SyllableState::ValidPrefix;
    }

    // Parse vowel nucleus
    size_t vowelStart = pos;
    size_t vowelLen = 0;
    while (pos < count && states[pos].IsVowel()) {
        ++vowelLen;
        ++pos;
    }

    if (vowelLen == 0) {
        // After initial consonant, next char must be a vowel
        // Check if it could be a valid 2/3-char consonant prefix
        // e.g., "th" → valid prefix, "bk" → invalid
        return SyllableState::Invalid;
    }

    // Encode vowel slots
    uint32_t vowelKey = 0;
    const CharStateT* vowelStates = &states[vowelStart];

    if (vowelLen == 1) {
        uint8_t bi = BaseIndex(vowelStates[0].base);
        if (bi == 0xFF) return SyllableState::Invalid;
        uint8_t mi = ModOrdinal(vowelStates[0].mod);
        vowelKey = Key1(VowelSlot(bi, mi));
    } else if (vowelLen == 2) {
        uint8_t b0 = BaseIndex(vowelStates[0].base);
        uint8_t b1 = BaseIndex(vowelStates[1].base);
        if (b0 == 0xFF || b1 == 0xFF) return SyllableState::Invalid;
        vowelKey = Key2(VowelSlot(b0, ModOrdinal(vowelStates[0].mod)),
                        VowelSlot(b1, ModOrdinal(vowelStates[1].mod)));
    } else if (vowelLen == 3) {
        uint8_t b0 = BaseIndex(vowelStates[0].base);
        uint8_t b1 = BaseIndex(vowelStates[1].base);
        uint8_t b2 = BaseIndex(vowelStates[2].base);
        if (b0 == 0xFF || b1 == 0xFF || b2 == 0xFF) return SyllableState::Invalid;
        vowelKey = Key3(VowelSlot(b0, ModOrdinal(vowelStates[0].mod)),
                        VowelSlot(b1, ModOrdinal(vowelStates[1].mod)),
                        VowelSlot(b2, ModOrdinal(vowelStates[2].mod)));
    } else {
        // 4+ consecutive vowels → invalid
        return SyllableState::Invalid;
    }

    // Look up vowel in table
    const VowelEntry* entry = FindVowel(vowelKey);

    if (!entry) {
        // Not an exact match — check if prefix of a valid combo
        if (vowelLen <= 2 && IsVowelPrefix(vowelKey, static_cast<int>(vowelLen))) {
            // Could still become valid with more vowels
            if (pos == count) return SyllableState::ValidPrefix;
        }

        // Check if adding a modifier to any vowel slot would produce a valid nucleus.
        // e.g., "ie" (i/None + e/None) → "iê" (i/None + e/Circ) with future modifier key.
        size_t finalRemaining = count - pos;
        bool found = TryModifiedVowels(vowelStates, vowelLen, [&](const VowelEntry* e) {
            if (finalRemaining == 0) return true;
            if (e->canEnd) {
                size_t finalLen = ParseFinalConsonant(&states[pos], finalRemaining);
                return finalLen == finalRemaining;
            }
            return false;
        });
        if (found) return SyllableState::ValidPrefix;

        return SyllableState::Invalid;
    }

    // Vowel found — check what follows
    remaining = count - pos;

    if (remaining == 0) {
        return entry->requiresEnd ? SyllableState::ValidPrefix : SyllableState::Valid;
    }

    // Try to parse final consonant
    if (!entry->canEnd) {
        // This vowel nucleus cannot have a final consonant as-is.
        // But a future modifier might change it to one that can.
        // e.g., "ua" (canEnd=false) → "uâ" (canEnd=true) for chuẩn, tuấn, luật
        bool canEndWithMod = TryModifiedVowels(vowelStates, vowelLen, [&](const VowelEntry* e) {
            if (!e->canEnd) return false;
            size_t finalLen = ParseFinalConsonant(&states[pos], remaining);
            return finalLen == remaining;
        });
        if (canEndWithMod) return SyllableState::ValidPrefix;
        return SyllableState::Invalid;
    }

    size_t finalLen = ParseFinalConsonant(&states[pos], remaining);
    if (finalLen == 0) {
        // What follows isn't a valid final consonant
        return SyllableState::Invalid;
    }

    if (pos + finalLen < count) {
        // Extra characters after final consonant
        return SyllableState::Invalid;
    }

    // 'k' as final consonant is non-standard Vietnamese — only appears in
    // minority-language proper nouns (Đắk Lắk, Đắk Nông). Restrict to ắ vowel
    // to prevent English words like "hook"→"hôk", "book"→"bôk" from passing.
    if (finalLen == 1 && states[pos].base == L'k') {
        bool validK = (vowelLen == 1 && vowelStates[0].base == L'a' &&
                       ModOrdinal(vowelStates[0].mod) == kBrev);
        if (!validK) return SyllableState::Invalid;
    }

    // Check vowel + final consonant pair validity (VCPairList)
    // Only restrict if the vowel is in our rules table; unknown vowels pass through.
    uint16_t allowed = GetAllowedFinals(vowelKey);
    if (allowed != 0) {
        uint16_t finalBit = FinalConsonantBit(&states[pos], finalLen);
        if (finalBit != 0 && !(allowed & finalBit)) {
            // A lone c/n can still grow into an allowed 2-letter final
            // (c→ch, n→ng/nh). "khuêc"/"huyc" are no syllables, but they are
            // the only route to "khuếch"/"huých" when the mark or tone is
            // typed before the coda's 'h' — Invalid here latches spell check
            // off and the promotion never happens (Rust engine allows it).
            uint16_t grown = 0;
            if (finalLen == 1) {
                if (states[pos].base == L'c') grown = F_ch;
                else if (states[pos].base == L'n') grown = F_ng | F_nh;
            }
            if (grown == 0 || !(allowed & grown)) return SyllableState::Invalid;
            return SyllableState::ValidPrefix;
        }
    }

    // Check tone restriction for stop finals
    if (IsStopFinal(&states[pos], finalLen)) {
        auto tone = GetTone(states, count);
        // Stop finals (c, ch, p, t) only allow None, Acute, Dot
        using ToneT = decltype(tone);
        if (tone != ToneT{}) {  // Not None
            // Acute is value 1, Dot is value 5 in both enums
            auto toneVal = static_cast<uint8_t>(tone);
            if (toneVal != 1 && toneVal != 5) {
                return SyllableState::Invalid;
            }
        }
    }

    return SyllableState::Valid;
}

//=============================================================================
// Check if sequence is a valid consonant-only prefix
//=============================================================================

template<typename CharStateT>
bool IsValidConsonantPrefix(const CharStateT* states, size_t count, bool allowZwjf = false) {
    if (count == 0) return true;

    // Single consonant — always a valid prefix (including z/j/w/f when allowZwjf)
    if (count == 1 && !states[0].IsVowel()) {
        wchar_t c0 = states[0].base;
        // Standard consonants
        if (c0 == L'b' || c0 == L'c' || c0 == L'd' || c0 == L'g' || c0 == L'h' ||
            c0 == L'k' || c0 == L'l' || c0 == L'm' || c0 == L'n' || c0 == L'p' ||
            c0 == L'q' || c0 == L'r' || c0 == L's' || c0 == L't' || c0 == L'v' ||
            c0 == L'x' ||
            (allowZwjf && (c0 == L'z' || c0 == L'j' || c0 == L'w' || c0 == L'f'))) {
            return true;
        }
        // đ
        if (states[0].IsD() && states[0].mod != decltype(states[0].mod){}) return true;
        return false;
    }

    // Check for valid 2-char initial consonant prefixes
    if (count == 2) {
        wchar_t c0 = states[0].base, c1 = states[1].base;
        if (!states[0].IsVowel() && !states[1].IsVowel()) {
            // Valid 2-char initials
            if ((c0 == L'c' && c1 == L'h') || (c0 == L'g' && c1 == L'h') ||
                (c0 == L'k' && c1 == L'h') || (c0 == L'n' && c1 == L'g') ||
                (c0 == L'n' && c1 == L'h') || (c0 == L'p' && c1 == L'h') ||
                (c0 == L't' && c1 == L'h') || (c0 == L't' && c1 == L'r')) {
                return true;
            }
            return false;  // "bk", "bl" etc. are invalid
        }
    }

    // Check for "ngh" prefix
    if (count == 3 && states[0].base == L'n' && states[1].base == L'g' && states[2].base == L'h' &&
        !states[0].IsVowel() && !states[1].IsVowel() && !states[2].IsVowel()) {
        return true;
    }

    return false;
}

//=============================================================================
// Main validation with gi/qu dual decomposition
//=============================================================================

template<typename CharStateT>
SyllableState ValidateImpl(const CharStateT* states, size_t count, bool allowZwjf = false) noexcept {
    if (count == 0) return SyllableState::ValidPrefix;

    // First, try standard decomposition
    size_t initialLen = ParseInitialConsonant(states, count, allowZwjf);

    // Check for invalid consonant sequences
    if (initialLen == 0 && !states[0].IsVowel()) {
        // Not a valid initial consonant and not a vowel
        // Check if it could be đ
        if (states[0].IsD() && states[0].mod == decltype(states[0].mod){}) {
            // Plain 'd' is a valid consonant
            initialLen = 1;
        } else {
            return SyllableState::Invalid;
        }
    }

    // Special: if only consonants consumed and no vowels follow, check prefix validity
    if (initialLen > 0 && initialLen == count) {
        if (IsValidConsonantPrefix(states, count, allowZwjf)) {
            return SyllableState::ValidPrefix;
        }
        return SyllableState::Invalid;
    }

    // After initial consonant, if next char is not a vowel, it might be invalid
    if (initialLen > 0 && initialLen < count && !states[initialLen].IsVowel()) {
        // Check if the first chars form a valid consonant-only prefix
        if (IsValidConsonantPrefix(states, count, allowZwjf)) {
            return SyllableState::ValidPrefix;
        }
        return SyllableState::Invalid;
    }

    // Initial consonant + vowel compatibility (Vietnamese phonology):
    //
    // k:   only before front vowels e/ê/i/y. ka/ko/ku → use c instead.
    // c:   only before back/central vowels a/ă/â/o/ô/ơ/u/ư. ce/ci/cy → use k instead.
    // gh:  only before front vowels e/ê/i. gha/gho → use ga/go instead.
    // ngh: only before front vowels e/ê/i. ngha → use nga instead.
    // q:   must be followed by u (qu cluster). qa/qe → invalid.
    //
    // kh/ng/nh are separate 2-3 char consonants and are NOT affected.

    if (initialLen < count && states[initialLen].IsVowel()) {
        wchar_t firstVowel = states[initialLen].base;
        bool isFrontVowel = Phonology::IsFrontBaseVowel(firstVowel);

        if (initialLen == 1) {
            wchar_t c0 = states[0].base;
            // k only before front vowels
            if (c0 == L'k' && !isFrontVowel) return SyllableState::Invalid;
            // c not before front vowels (use k instead)
            if (c0 == L'c' && isFrontVowel) return SyllableState::Invalid;
            // q must be part of qu cluster — qa/qe/qi invalid, qu handled by
            // dual decomposition below. Only block when next char is NOT 'u'.
            if (c0 == L'q' && firstVowel != L'u') return SyllableState::Invalid;
        } else if (initialLen == 2) {
            // gh only before front vowels
            if (states[0].base == L'g' && states[1].base == L'h' && !isFrontVowel)
                return SyllableState::Invalid;
        } else if (initialLen == 3) {
            // ngh only before front vowels
            if (states[0].base == L'n' && states[1].base == L'g' && states[2].base == L'h' && !isFrontVowel)
                return SyllableState::Invalid;
        }

        // oe and oă only allow specific initial consonants:
        // "", ch, h, kh, l, ng, nh, t, tr, x
        if (initialLen + 1 < count && states[initialLen].base == L'o' && states[initialLen].mod == Modifier::None) {
            bool isOeOrOa = false;
            if (states[initialLen + 1].base == L'e' && states[initialLen + 1].mod == Modifier::None) {
                isOeOrOa = true;
            } else if (states[initialLen + 1].base == L'a' && states[initialLen + 1].mod == Modifier::Breve) {
                isOeOrOa = true;
            }

            if (isOeOrOa) {
                bool allowed = false;
                if (initialLen == 0) {
                    allowed = true;
                } else if (initialLen == 1) {
                    // đ decomposes to base 'd', which is never in this set, so no
                    // stroke-d guard is needed here.
                    wchar_t c0 = states[0].base;
                    if (c0 == L'h' || c0 == L'l' || c0 == L't' || c0 == L'x') {
                        allowed = true;
                    }
                } else if (initialLen == 2) {
                    wchar_t c0 = states[0].base;
                    wchar_t c1 = states[1].base;
                    if ((c0 == L'c' && c1 == L'h') ||
                        (c0 == L'k' && c1 == L'h') ||
                        (c0 == L'n' && c1 == L'g') ||
                        (c0 == L'n' && c1 == L'h') ||
                        (c0 == L't' && c1 == L'r')) {
                        allowed = true;
                    }
                }
                if (!allowed) {
                    return SyllableState::Invalid;
                }
            }
        }
    }

    SyllableState result = ValidateDecomposition(states, count, initialLen);

    // Handle gi/qu dual decomposition
    // "gi" + vowel → try C₁="gi" (2-char consonant) + V=rest
    //            AND C₁="g" (1-char consonant) + V="i"+rest
    if (count >= 2 && states[0].base == L'g' && states[1].base == L'i' && states[1].IsVowel()) {
        // Decomposition 1: g + i... (initialLen=1, vowel starts at 1)
        SyllableState r1 = ValidateDecomposition(states, count, 1);

        // Decomposition 2: gi + ... (initialLen=2, vowel starts at 2)
        if (count > 2 && states[2].IsVowel()) {
            SyllableState r2 = ValidateDecomposition(states, count, 2);
            // Return most permissive
            if (r2 == SyllableState::Valid || r1 == SyllableState::Valid) {
                result = SyllableState::Valid;
            } else if (r2 == SyllableState::ValidPrefix || r1 == SyllableState::ValidPrefix) {
                result = SyllableState::ValidPrefix;
            }
        } else if (count == 2) {
            // "gi" alone: g + vowel_i → valid (e.g., the word "gì")
            r1 = ValidateDecomposition(states, count, 1);
            if (r1 == SyllableState::Valid) result = SyllableState::Valid;
            else if (r1 == SyllableState::ValidPrefix && result != SyllableState::Valid) result = SyllableState::ValidPrefix;
        } else {
            // "gi" + consonant: try g + i + consonant
            if (r1 == SyllableState::Valid) result = SyllableState::Valid;
            else if (r1 == SyllableState::ValidPrefix && result != SyllableState::Valid) result = SyllableState::ValidPrefix;
        }

        // Override initial result with best
        return result;
    }

    // "qu" + vowel → try C₁="qu" (2-char) + V=rest
    if (count >= 2 && states[0].base == L'q' && states[1].base == L'u') {
        if (count > 2 && states[2].IsVowel()) {
            SyllableState r2 = ValidateDecomposition(states, count, 2);
            if (r2 == SyllableState::Valid) return SyllableState::Valid;
            if (r2 == SyllableState::ValidPrefix && result != SyllableState::Valid) return SyllableState::ValidPrefix;
        } else if (count == 2) {
            // "qu" alone → valid prefix
            return SyllableState::ValidPrefix;
        }
        return result;
    }

    return result;
}

// Tone/modifier same-vowel invariant: valid Vietnamese syllables place the
// tone mark on the vowel that carries the modifier (or on the canonical tone
// target when no vowel is modified). If ANY vowel in the buffer has a
// modifier, the tone MUST sit on a modified vowel. Also rejects buffers
// carrying more than one tone, which valid syllables never do.
// Catches typos like "của" + circumflex on 'a' → c,ủ,â (split tone/mod)
// that the structural nucleus lookup would otherwise accept because "uâ"
// itself is a legal vowel combo (chuẩn, tuấn, luật).
template<typename CharStateT>
bool CheckToneModInvariant(const CharStateT* states, size_t count) noexcept {
    bool anyMod = false;
    size_t toneIdx = SIZE_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (!states[i].IsVowel()) continue;
        if (states[i].HasModifier()) anyMod = true;
        if (states[i].HasTone()) {
            if (toneIdx != SIZE_MAX) return false;  // multiple tones
            toneIdx = i;
        }
    }
    if (toneIdx != SIZE_MAX && anyMod && !states[toneIdx].HasModifier()) {
        return false;
    }
    return true;
}

}  // namespace

//=============================================================================
// Template instantiations
//=============================================================================

template<typename CharStateT>
SyllableState ValidateSyllableState(const CharStateT* states, size_t count, bool allowZwjf) noexcept {
    SyllableState r = ValidateImpl(states, count, allowZwjf);
    if (r != SyllableState::Invalid && !CheckToneModInvariant(states, count)) {
        r = SyllableState::Invalid;
    }
    return r;
}

// Explicit instantiation — single CharState type shared across Telex/VNI/Combined modes.
template SyllableState ValidateSyllableState<CharState>(const CharState* states, size_t count, bool allowZwjf) noexcept;

}  // namespace Phonology
}  // namespace NextKey
