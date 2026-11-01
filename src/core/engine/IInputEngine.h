// VKey - Input Engine Interface
// Copyright (c) 2024-2026 PhatMT. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace NextKey {

/// Abstract interface for input method engines (Telex, VNI, etc.)
/// NFR7: No global state - all state is instance-based
class IInputEngine {
public:
    virtual ~IInputEngine() = default;

    /// Push a character to the engine for processing
    virtual void PushChar(wchar_t c) = 0;

    /// Push a decoded physical character with independent output-case intent.
    /// The default preserves legacy engines; bracket-aware engines override it
    /// so Caps Lock can request uppercase without changing `[` into `{` in raw
    /// history and escape literals.
    virtual void PushKey(wchar_t physicalChar, bool uppercase) {
        (void)uppercase;
        PushChar(physicalChar);
    }

    /// Handle backspace - remove last character
    virtual void Backspace() = 0;

    /// Get current composition (without committing).
    /// Returns const ref to internal buffer — valid until next mutation (PushChar/Backspace/Reset).
    [[nodiscard]] virtual const std::wstring& Peek() const = 0;

    /// Commit composition and get final text, then reset state
    [[nodiscard]] virtual std::wstring Commit() = 0;

    /// Reset engine state (clear composition)
    virtual void Reset() = 0;

    virtual void ClearContext() noexcept {}

    /// Get number of characters in current composition
    [[nodiscard]] virtual size_t Count() const = 0;

    /// Check if a quick consonant expansion (e.g., nn->ng, cc->ch) is currently active
    [[nodiscard]] virtual bool HasActiveQuickConsonant() const = 0;

    /// Seed engine state from already-rendered text (e.g., when the user backspaces
    /// into a committed word). State is fully reset before seeding. Backends may
    /// restore glyphs literally rather than reconstructing their original modifier
    /// provenance, so callers should prefer exact raw-history replay and verify that
    /// it reproduces the visible text before falling back to this method.
    ///
    /// Returns false when the backend cannot represent the supplied text. On failure
    /// the engine is left in Reset() state.
    [[nodiscard]] virtual bool SeedFromText(const std::wstring& text) = 0;

    /// Whether current buffer is flagged as a hard-English word (e.g., "hello",
    /// "approved", "system") by the engine's English-protection heuristics.
    /// Call AFTER SeedFromText — caller decides whether to revive composition.
    [[nodiscard]] virtual bool IsEnglishWord() const = 0;

    /// Whether the engine has an active escape (tone, circumflex, horn, breve,
    /// stroke, or modifier). When true, features that would re-apply the
    /// escaped transform (e.g. a tone re-emit) should defer to user intent.
    /// Default false for engines that don't track escape state.
    [[nodiscard]] virtual bool IsToneEscaped() const { return false; }

    /// Raw keys typed by the user (case-preserved), independent of any Vietnamese
    /// transformation in the composed buffer. Used by the Esc-restore feature to
    /// recover the original keystrokes (e.g., composed "víu" ← raw "virus").
    /// Default no-op for engines that don't track raw input.
    [[nodiscard]] virtual std::wstring PeekRaw() const { return {}; }

    /// Zero-copy view over the same raw key history as PeekRaw(), without the
    /// owning allocation. Valid only until the next engine mutation
    /// (PushChar/Backspace/Reset/Commit). Callers that must outlive a mutation
    /// — e.g. snapshot the raw input BEFORE Commit() resets it — MUST use the
    /// owning PeekRaw() instead. Provided for the keyboard-hook hot path, where
    /// per-keystroke heap allocation is forbidden (CODING_RULES Rule 11).
    [[nodiscard]] virtual std::wstring_view PeekRawView() const noexcept { return {}; }

    /// Whether a committed physical-key snapshot should be reopened before
    /// applying a decoded key. Backends that do not expose a bounded
    /// phonology-aware preview keep the conservative false default.
    [[nodiscard]] virtual bool ShouldReplayCommittedKey(
        std::wstring_view rawInput, wchar_t key) const {
        (void)rawInput;
        (void)key;
        return false;
    }

    /// Whether the most recently committed text was silently auto-corrected
    /// against the engine's dictionary (e.g. a mistyped Telex sequence
    /// restored to the intended Vietnamese word). Cleared by the next
    /// commit. Default false for engines without a correction concept.
    [[nodiscard]] virtual bool LastCommitWasCorrected() const { return false; }
};

}  // namespace NextKey
