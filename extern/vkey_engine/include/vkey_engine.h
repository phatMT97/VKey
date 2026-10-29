/* vkey_engine.h - generic C ABI for the portable Vietnamese input engine.
 *
 * Closed-source boundary: consumers link the prebuilt vkey_engine library
 * (DLL / .so) and include this header; the Rust source is not distributed.
 * All text crosses as UTF-16 to match Windows wchar_t.
 *
 * Lifetime: vkey_engine_create() returns an owning handle; release it once
 * with vkey_engine_destroy(). Every other call borrows the handle. Calls that
 * mutate one handle must be serialized by the caller and must not overlap
 * any other access to that handle.
 */
#ifndef VKEY_ENGINE_H
#define VKEY_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when the ABI changes; check against vkey_engine_abi_version(). */
#define VKEY_ENGINE_ABI_VERSION 9u

/* ABI v6 runtime-artifact identity status. */
#define VKEY_ENGINE_RUNTIME_OK                   0u
#define VKEY_ENGINE_RUNTIME_WRONG_FILENAME       1u
#define VKEY_ENGINE_RUNTIME_IDENTITY_UNAVAILABLE 2u

/* Input methods (the `method` argument to vkey_engine_create). */
#define VKEY_METHOD_TELEX        0u
#define VKEY_METHOD_VNI          1u
#define VKEY_METHOD_SIMPLE_TELEX 2u
#define VKEY_METHOD_COMBINED     3u
#define VKEY_METHOD_USER_DEFINED 4u

/* Feature flags (OR together into the `features` argument). */
#define VKEY_FEAT_MODERN_ORTHOGRAPHY   (1u << 0)
#define VKEY_FEAT_QUICK_START_CONSONANT (1u << 1)
#define VKEY_FEAT_QUICK_CONSONANT      (1u << 2)
#define VKEY_FEAT_QUICK_END_CONSONANT  (1u << 3)
#define VKEY_FEAT_SPELL_CHECK          (1u << 4)
#define VKEY_FEAT_ALLOW_ENGLISH_BYPASS (1u << 5)
#define VKEY_FEAT_ALLOW_ZWJF           (1u << 6)
#define VKEY_FEAT_SPELL_SUGGEST        (1u << 7)
/* Opt out of commit-time raw restoration; older callers retain the default. */
#define VKEY_FEAT_DISABLE_AUTO_RESTORE (1u << 8)

/* Opaque engine handle. */
typedef struct VKeyEngine VKeyEngine;

/* Opaque immutable user-dictionary handle (ABI v8). */
typedef struct VKeyUserDictionary VKeyUserDictionary;

/* Create an engine for `method` with the `features` bitmask. Returns NULL when
 * vkey_engine_runtime_status() is not VKEY_ENGINE_RUNTIME_OK. */
VKeyEngine *vkey_engine_create(uint32_t method, uint32_t features);

/* Destroy a handle from vkey_engine_create(). NULL is ignored. */
void vkey_engine_destroy(VKeyEngine *engine);

/* Clear the active composition. */
void vkey_engine_reset(VKeyEngine *engine);

/* Push one decoded key (a Unicode scalar value, e.g. an ASCII letter). */
void vkey_engine_push_char(VKeyEngine *engine, uint32_t codepoint);

/* ABI v9: push one decoded physical scalar with independent output-case
 * intent. Hosts use this for punctuation shortcuts where Caps Lock affects
 * Vietnamese case but must not rewrite the literal physical scalar. */
void vkey_engine_push_key(VKeyEngine *engine, uint32_t codepoint, bool uppercase);

/* Remove one rendered symbol from the composition. */
void vkey_engine_backspace(VKeyEngine *engine);

/* Copy the rendered composition into `buf` as UTF-16 (up to `cap` code units)
 * and return the total UTF-16 length needed. Pass buf=NULL, cap=0 to query the
 * length. The active composition is bounded, so a 256-unit buffer always fits. */
size_t vkey_engine_peek_utf16(const VKeyEngine *engine, uint16_t *buf, size_t cap);

/* Commit the composition, reset the engine, and copy the committed text into
 * `buf` as UTF-16; returns the committed UTF-16 length. Unlike the peek calls,
 * this CONSUMES the composition, so the buf=NULL,cap=0 length-query idiom does
 * not work (it would commit and discard the text). If cap is shorter than the
 * return value the copy is silently truncated and the rest is unrecoverable, so
 * pass a buffer large enough up front; a 256-unit buffer always fits. */
size_t vkey_engine_commit_utf16(VKeyEngine *engine, uint16_t *buf, size_t cap);

/* Number of Unicode scalars in the current rendered composition. */
size_t vkey_engine_count(const VKeyEngine *engine);

/* ABI revision implemented by this library. */
uint32_t vkey_engine_abi_version(void);

/* Cached runtime-artifact identity status. A dynamically loaded artifact must
 * be named vkey_engine.dll on Windows (ASCII case-insensitive) or
 * libvkey_engine.so on Linux (exact case). Static linkage remains supported.
 * vkey_engine_create() returns NULL whenever this status is not OK. */
uint32_t vkey_engine_runtime_status(void);

/* --- ABI v2: host query surface --------------------------------------------
 * Added in ABI 2. Present only when vkey_engine_abi_version() >= 2. */

/* Whether the current rendered composition is flagged as a non-Vietnamese
 * (English) shape by the engine's English-protection heuristic. */
bool vkey_engine_is_english_word(const VKeyEngine *engine);

/* Whether the active composition carries a tone/modifier escape (a repeated
 * tone/modifier key forced literal, or a cancelled tone). */
bool vkey_engine_is_tone_escaped(const VKeyEngine *engine);

/* Whether a quick-consonant expansion (e.g. nn->ng, cc->ch) is present in the
 * active composition. */
bool vkey_engine_has_active_quick_consonant(const VKeyEngine *engine);

/* Copy the case-preserved physical input scalars of the active composition into
 * `buf` as UTF-16 (up to `cap` code units); returns the total UTF-16 length.
 * Pass buf=NULL, cap=0 to query the length. */
size_t vkey_engine_peek_raw_utf16(const VKeyEngine *engine, uint16_t *buf, size_t cap);

/* Seed the active composition from already-rendered UTF-16 text (for
 * backspace-revive into a committed word). The text is replayed as canonical
 * telex keys when they render back to it exactly, so a later tone/modifier key
 * edits it; otherwise every scalar is seeded literally (ADR-0013). Either way
 * peek_raw returns the seeded keys, not keys the user physically typed. Returns
 * true on success, false (leaving the engine reset) on invalid UTF-16 or
 * capacity overflow. */
bool vkey_engine_seed_text_utf16(VKeyEngine *engine, const uint16_t *buf, size_t len);

/* --- ABI v3: lexicon smart-restore -----------------------------------------
 * Added in ABI 3. Present only when vkey_engine_abi_version() >= 3. The
 * library ships with an embedded lexicon (VKEY_FEAT_SPELL_SUGGEST just needs
 * to be set); hosts normally never call vkey_engine_set_lexicon. */

/* Install the process-wide lexicon (VKLX format, see vkey_lexicon) used by
 * engines created AFTER this call; existing engines are unchanged. This is
 * one-shot: only the first successful call installs an override. `len` must
 * be 1..1,048,576 bytes. The library validates before making its sole
 * process-lifetime copy, so the caller may free `blob` immediately. Returns
 * false for an empty, oversized, malformed, or already-installed blob. */
bool vkey_engine_set_lexicon(const uint8_t *blob, size_t len);

/* Whether the most recent vkey_engine_commit_utf16() emitted a lexicon
 * correction instead of a raw restore. Cleared by the next commit. */
bool vkey_engine_last_commit_was_corrected(const VKeyEngine *engine);

/* Diagnostics/UI surface: candidates computed for the last corrected commit.
 * Count is 0 unless vkey_engine_last_commit_was_corrected() is true. */
size_t vkey_engine_suggest_count(const VKeyEngine *engine);

/* Copy the `index`-th ranked correction candidate (index 0 is the word that
 * was actually emitted) into `buf` as UTF-16 (up to `cap` code units);
 * returns the total UTF-16 length. Pass buf=NULL, cap=0 to query the length.
 * Returns 0 if index >= vkey_engine_suggest_count(). */
size_t vkey_engine_suggest_utf16(const VKeyEngine *engine, size_t index,
                                  uint16_t *buf, size_t cap);

/* --- ABI v4: user-defined custom keymap ------------------------------------
 * Added in ABI 4. Present only when vkey_engine_abi_version() >= 4. Only
 * consulted when the engine was created with VKEY_METHOD_USER_DEFINED. */

/* One canonical Vietnamese input action a key can be bound to under
 * VKEY_METHOD_USER_DEFINED. Every action already exists as a fixed Telex or
 * VNI key; binding it to another key changes nothing about how it behaves.
 * Values are pinned and cross the ABI as raw bytes: never renumber existing
 * codes when adding new ones. */
#define VKEY_KEY_ACTION_NONE            0u  /* Unbound: types as a literal. */
#define VKEY_KEY_ACTION_CLEAR_TONE      1u
#define VKEY_KEY_ACTION_TONE_ACUTE      2u
#define VKEY_KEY_ACTION_TONE_GRAVE      3u
#define VKEY_KEY_ACTION_TONE_HOOK       4u
#define VKEY_KEY_ACTION_TONE_TILDE      5u
#define VKEY_KEY_ACTION_TONE_DOT        6u
#define VKEY_KEY_ACTION_CIRCUMFLEX_A    7u
#define VKEY_KEY_ACTION_CIRCUMFLEX_E    8u
#define VKEY_KEY_ACTION_CIRCUMFLEX_O    9u
#define VKEY_KEY_ACTION_HORN_W          10u /* Telex-style: horn on u/o, breve-on-lone-a and standalone-insert-ư fallback. */
#define VKEY_KEY_ACTION_HORN_INSERT_O   11u /* Always inserts a fresh o-horn (o+). */
#define VKEY_KEY_ACTION_HORN_INSERT_U   12u /* Always inserts a fresh u-horn (u+). */
#define VKEY_KEY_ACTION_STROKE_D        13u
#define VKEY_KEY_ACTION_VNI_CIRCUMFLEX  14u
#define VKEY_KEY_ACTION_VNI_HORN        15u /* Same transform as HORN_W; no standalone-insert fallback. */
#define VKEY_KEY_ACTION_VNI_BREVE       16u
#define VKEY_KEY_ACTION_VNI_STROKE      17u
#define VKEY_KEY_ACTION_HORN_OR_INSERT_U           18u
#define VKEY_KEY_ACTION_HORN_OR_INSERT_U_NO_START  19u
#define VKEY_KEY_ACTION_UNDO_ALL_MARKS             20u
#define VKEY_KEY_ACTION_INSERT_A_BREVE             21u
#define VKEY_KEY_ACTION_INSERT_A_BREVE_UPPER       22u
#define VKEY_KEY_ACTION_INSERT_A_CIRCUMFLEX        23u
#define VKEY_KEY_ACTION_INSERT_A_CIRCUMFLEX_UPPER  24u
#define VKEY_KEY_ACTION_INSERT_D_STROKE            25u
#define VKEY_KEY_ACTION_INSERT_D_STROKE_UPPER      26u
#define VKEY_KEY_ACTION_INSERT_E_CIRCUMFLEX        27u
#define VKEY_KEY_ACTION_INSERT_E_CIRCUMFLEX_UPPER  28u
#define VKEY_KEY_ACTION_INSERT_O_CIRCUMFLEX        29u
#define VKEY_KEY_ACTION_INSERT_O_CIRCUMFLEX_UPPER  30u
#define VKEY_KEY_ACTION_INSERT_O_HORN              31u
#define VKEY_KEY_ACTION_INSERT_O_HORN_UPPER        32u
#define VKEY_KEY_ACTION_INSERT_U_HORN              33u
#define VKEY_KEY_ACTION_INSERT_U_HORN_UPPER        34u
/* Byte codes 35-255 are reserved for future actions; this build maps any of
 * them to VKEY_KEY_ACTION_NONE rather than rejecting them. */

/* Installs the per-key action table for `engine` (one VKEY_KEY_ACTION_* byte
 * per ASCII key code, index = lowercased key). Bytes beyond the first 128 are
 * ignored (and need not be readable); a shorter `len` leaves the remaining
 * keys unbound. Pass NULL/0 to clear all bindings back to
 * VKEY_KEY_ACTION_NONE (pure literal passthrough).
 * Resets active composition, same as changing any other engine setting.
 * Ignored while the engine's method is not VKEY_METHOD_USER_DEFINED. */
void vkey_engine_set_custom_keymap(VKeyEngine *engine, const uint8_t *entries, size_t len);

/* --- ABI v5: spell-check exclusions --------------------------------------------
 * Added in ABI 5. Present only when vkey_engine_abi_version() >= 5. */

/* Install a process-global spell-check exclusion list (UTF-16 words,
 * newline-delimited; one word per line). The engine parses and copies the
 * bounded list during this call, so the caller may free `buf` immediately.
 * Pass NULL/0 to clear it. Only engines created AFTER the call receive the
 * new list; existing engines are unchanged. Only affects behavior when
 * VKEY_FEAT_SPELL_CHECK is set. */
bool vkey_engine_set_spell_exclusions_utf16(const uint16_t *buf, size_t len);

/* --- ABI v7: committed-word replay eligibility ----------------------------
 * Added in ABI 7. Present only when vkey_engine_abi_version() >= 7. */

/* Whether a session owner should reopen a committed word before applying
 * `codepoint`. `raw` is the case-preserved physical-key snapshot captured at
 * commit time. Returns true when the key transforms the word or appends a
 * literal that keeps a live Vietnamese shape; false for invalid UTF-16,
 * invalid codepoints, capacity overflow, or an unrelated new-word key.
 * The engine and caller buffer are not mutated. */
bool vkey_engine_should_replay_key_after_raw_utf16(const VKeyEngine *engine,
                                                    const uint16_t *raw,
                                                    size_t raw_len,
                                                    uint32_t codepoint);

/* --- ABI v8: user exact-protection dictionary ----------------------------
 * Added in ABI 8. Present only when vkey_engine_abi_version() >= 8. */

/* Construction result codes. */
#define VKEY_USER_DICTIONARY_OK                0u
#define VKEY_USER_DICTIONARY_INVALID_ARGUMENT  1u
#define VKEY_USER_DICTIONARY_INVALID_UTF16     2u
#define VKEY_USER_DICTIONARY_PAYLOAD_TOO_LARGE 3u
#define VKEY_USER_DICTIONARY_TOO_MANY_WORDS    4u
#define VKEY_USER_DICTIONARY_WORD_TOO_LONG     5u
#define VKEY_USER_DICTIONARY_INVALID_WORD      6u

/* Construction limits, exposed so hosts can reject oversized files before
 * allocating or crossing the ABI. */
#define VKEY_USER_DICTIONARY_MAX_UTF16_UNITS 65536u
#define VKEY_USER_DICTIONARY_MAX_WORDS       1024u
#define VKEY_USER_DICTIONARY_MAX_WORD_SCALARS  64u

/* Compile newline-delimited UTF-16 text into an owning immutable dictionary.
 * Blank lines and lines whose first non-whitespace character is '#' are
 * ignored. A word is normalized and matched case-insensitively. Limits are
 * 65,536 UTF-16 units total, 1,024 distinct words, and 64 Unicode scalars per
 * word. Returns NULL on failure. `out_status` and `out_error_line` are optional;
 * error_line is one-based for word errors and zero otherwise. The source buffer
 * is copied during this call and may be freed immediately. */
VKeyUserDictionary *vkey_user_dictionary_create_utf16(
    const uint16_t *buf, size_t len, uint32_t *out_status,
    size_t *out_error_line);

/* Destroy a dictionary handle. NULL is ignored. Engines successfully attached
 * to it retain their own immutable shared snapshot. */
void vkey_user_dictionary_destroy(VKeyUserDictionary *dictionary);

/* Replace an engine's exact-protection snapshot. Returns false and changes
 * nothing if `engine` is NULL or has an active composition. Pass dictionary=NULL
 * to clear at a word boundary. On success the engine keeps its own reference,
 * so the caller may destroy `dictionary` immediately. */
bool vkey_engine_set_user_dictionary(
    VKeyEngine *engine, const VKeyUserDictionary *dictionary);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VKEY_ENGINE_H */
