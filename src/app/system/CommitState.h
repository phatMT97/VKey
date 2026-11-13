// VKey - Commit State (Wave 3 PR 3.4, 2026-05-24)
// SPDX-License-Identifier: GPL-3.0-only
//
// Owns the per-word commit / undo state machine extracted from HookEngine:
//   * State enum: Idle → Ready → Primed FSM
//   * Stack of CommitEntry snapshots (LIFO, capped at kMaxStack)
//   * Pending-trigger counter (extra commit triggers typed while Ready)
//   * Input history (user keystrokes for current composition + BS markers
//     for replay)
//   * Ready-state entry timestamp (auto-expire after kReadyTimeoutMs)
//
// HookEngine still owns the *orchestration* logic — HandleCommitUndoFsm
// (the 289 LOC step-2d FSM body) dispatches keystrokes against this state
// and engine_ in tandem. CommitState is a pure state-machine + storage
// owner with no engine dependency.
//
// Threading:
//   * All mutations and reads happen on the hook thread. Same single-
//     writer invariant as pre-PR-3.4 (no atomics needed — Rule 11.3 holds
//     because HookEngine's writers were already hook-thread-only).
//   * Public API is non-atomic; HookEngine asserts VKEY_ASSERT_HOOK_THREAD
//     at the call sites that funnel into CommitState mutators.

#pragma once

#include <Windows.h>  // DWORD, GetTickCount
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace NextKey {

class CommitState {
public:
    /// Backspace-into-committed-word state machine.
    /// Pre-PR-3.4 name was HookEngine::CommitUndoState — same semantics.
    enum class State : std::uint8_t {
        Idle   = 0,  ///< No pending undo
        Ready  = 1,  ///< Just committed with Space/Enter — waiting for first BS
        Primed = 2,  ///< Space deleted — next Alpha/BS triggers replay
    };

    /// Per-commit snapshot pushed when CommitComposition fires. Pre-PR-3.4
    /// name was HookEngine::CommitEntry — same fields.
    struct Entry {
        std::vector<wchar_t> history;       ///< User keystrokes for replay
        std::wstring         text;          ///< What was on screen when committed
        std::wstring         rawInput;      ///< engine_->PeekRaw() snapshot — for Esc-restore-raw post-BS (design 2026-05-17)
        std::vector<uint8_t> widths;        ///< Encoded widths for non-Unicode code tables
        uint8_t              extraLeadingTriggers = 0;
            ///< Extra trigger chars typed between previous commit and this
            ///< word's body — must be backspaced before this entry's commit
            ///< trigger can be primed during multi-word undo.
    };

    /// Backspace marker in inputHistory_ — flags a BS keystroke for replay
    /// (replay must reproduce exact engine state including erasures).
    static constexpr wchar_t kBackspaceMarker = L'\b';
    /// Max stacked commits (LIFO). Limits backward-undo depth and memory.
    static constexpr std::size_t kMaxStack = 12;
    /// Cold-reserved capacity for engine-supported committed text/history.
    static constexpr std::size_t kReplayHistoryReserve = 256;
    /// Auto-expire Ready after this many ms — cheap insurance against any
    /// cursor-movement event that bypasses ResetComposition (e.g. future
    /// edge cases).
    static constexpr DWORD kReadyTimeoutMs = 4000;

    CommitState() {
        inputHistory_.reserve(kReplayHistoryReserve);
        stack_.reserve(kMaxStack);
    }
    ~CommitState() = default;

    CommitState(const CommitState&) = delete;
    CommitState& operator=(const CommitState&) = delete;

    // ── State accessors ───────────────────────────────────────────────
    [[nodiscard]] State Current() const noexcept { return state_; }
    [[nodiscard]] bool  IsIdle()  const noexcept { return state_ == State::Idle; }
    [[nodiscard]] bool  IsReady() const noexcept { return state_ == State::Ready; }
    [[nodiscard]] bool  IsPrimed() const noexcept { return state_ == State::Primed; }

    // ── State transitions ─────────────────────────────────────────────
    void SetIdle() noexcept {
        state_ = State::Idle;
    }
    /// Enter Ready: bumps `readyTime_` to GetTickCount() so the 4-sec
    /// timeout starts from this transition.
    void SetReady() noexcept {
        state_ = State::Ready;
        readyTime_ = GetTickCount();
    }
    void SetPrimed() noexcept {
        state_ = State::Primed;
    }

    [[nodiscard]] DWORD ReadyTime() const noexcept { return readyTime_; }
    [[nodiscard]] bool ReadyExpired(DWORD now) const noexcept {
        return state_ == State::Ready && (now - readyTime_) > kReadyTimeoutMs;
    }

    // ── Pending-trigger counter (extra triggers typed while Ready) ────
    [[nodiscard]] uint8_t PendingTriggerCount() const noexcept { return pendingTriggerCount_; }
    void IncrementPendingTriggers() noexcept {
        if (pendingTriggerCount_ < 0xFF) ++pendingTriggerCount_;
    }
    void DecrementPendingTriggers() noexcept {
        if (pendingTriggerCount_ > 0) --pendingTriggerCount_;
    }
    void SetPendingTriggers(uint8_t v) noexcept { pendingTriggerCount_ = v; }
    void ResetPendingTriggers() noexcept { pendingTriggerCount_ = 0; }

    // ── Leading triggers snapshot (survives Ready→Idle and replay pops) ─
    [[nodiscard]] uint8_t LeadingTriggersForCurrentWord() const noexcept {
        return leadingTriggersForCurrentWord_;
    }
    void SetLeadingTriggersForCurrentWord(uint8_t v) noexcept {
        leadingTriggersForCurrentWord_ = v;
    }

    // ── Gap shape (see DecideReadyGapKey) ─────────────────────────────
    /// Trigger that armed Ready, so ordinary punctuation can be distinguished
    /// from host-transformable emoticon openers. Zero after backward re-arm.
    [[nodiscard]] wchar_t GapGlyph() const noexcept { return gapGlyph_; }
    void SetGapGlyph(wchar_t glyph) noexcept { gapGlyph_ = glyph; }

    // ── Word opener (see IsHostReplacedWord) ──────────────────────────
    /// Punctuation char of the previous key (0 for anything else).
    [[nodiscard]] wchar_t PrevKeyGlyph() const noexcept { return prevKeyGlyph_; }
    void SetPrevKeyGlyph(wchar_t c) noexcept { prevKeyGlyph_ = c; }
    /// PrevKeyGlyph() captured when the current word's first letter was typed.
    [[nodiscard]] wchar_t WordOpenGlyph() const noexcept { return wordOpenGlyph_; }
    void SetWordOpenGlyph(wchar_t c) noexcept { wordOpenGlyph_ = c; }

    // ── Input history (keystrokes for replay; includes BS markers) ────
    [[nodiscard]] const std::vector<wchar_t>& History() const noexcept { return inputHistory_; }
    [[nodiscard]] std::vector<wchar_t>&       History()       noexcept { return inputHistory_; }
    void AppendHistory(wchar_t c) { inputHistory_.push_back(c); }
    /// Append the BS sentinel marker (replay sees it as "erase last char").
    void AppendBackspaceMarker() { inputHistory_.push_back(kBackspaceMarker); }
    void ClearHistory() noexcept { inputHistory_.clear(); }

    // ── Commit stack (LIFO of past commits) ───────────────────────────
    [[nodiscard]] bool        StackEmpty() const noexcept { return stack_.empty(); }
    [[nodiscard]] std::size_t StackSize()  const noexcept { return stack_.size(); }
    [[nodiscard]] const Entry& StackTop() const noexcept { return stack_.back(); }
    [[nodiscard]] Entry&       StackTop()       noexcept { return stack_.back(); }

    /// Push a fresh entry. Drops the oldest if over kMaxStack (FIFO eviction
    /// preserves the youngest 3 — matches pre-PR-3.4 behavior).
    void PushEntry(Entry e) {
        if (stack_.size() >= kMaxStack) {
            stack_.erase(stack_.begin());
        }
        stack_.push_back(std::move(e));
    }
    void PopStackTop() noexcept {
        if (!stack_.empty()) stack_.pop_back();
    }
    void ClearStack() noexcept { stack_.clear(); }

    [[nodiscard]] bool PushedToStack() const noexcept { return pushedToStack_; }
    void SetPushedToStack(bool v) noexcept { pushedToStack_ = v; }

    // ── Composite resets ──────────────────────────────────────────────
    /// CancelCommitUndo (HookEngine pre-PR-3.4): Idle + clear pending +
    /// clear stack. Used when an unrelated key cancels the commit-undo
    /// FSM mid-window.
    void Cancel() noexcept {
        state_ = State::Idle;
        pendingTriggerCount_ = 0;
        leadingTriggersForCurrentWord_ = 0;
        gapGlyph_ = L'\0';
        stack_.clear();
    }

    /// Strong discard for a replay-context boundary (Enter / Tab — see
    /// core/CommitUndoArmDecision.h IsReplayContextBoundary). Everything
    /// Cancel() drops, PLUS the in-progress keystroke history and the
    /// pushedToStack_ latch.
    ///
    /// Clearing inputHistory_ is what makes this safe to call at the TOP of a
    /// keystroke: CommitComposition only pushes a replay entry when
    /// History() is non-empty, so a boundary key that goes on to commit the
    /// current word later in the same keystroke cannot repopulate the very
    /// stack this discard exists to drop. Does NOT touch live engine
    /// composition — the word still commits and renders normally; only its
    /// replay entry is forfeited.
    void DiscardReplayContext() noexcept {
        Cancel();
        inputHistory_.clear();
        pushedToStack_ = false;
    }

private:
    State                  state_                          = State::Idle;
    std::vector<Entry>     stack_;          ///< LIFO; max kMaxStack entries
    bool                   pushedToStack_                  = false;
    wchar_t                gapGlyph_                       = L'\0';
    wchar_t                prevKeyGlyph_                   = L'\0';
    wchar_t                wordOpenGlyph_                  = L'\0';
    uint8_t                pendingTriggerCount_            = 0;
    uint8_t                leadingTriggersForCurrentWord_  = 0;
    DWORD                  readyTime_                      = 0;
    std::vector<wchar_t>   inputHistory_;   ///< Keystrokes + BS markers
};

}  // namespace NextKey
