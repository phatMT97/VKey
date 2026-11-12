// VKey - Output Dispatcher (Wave 3 PR 3.3, 2026-05-24)
// SPDX-License-Identifier: GPL-3.0-only
//
// Owns the output-dispatch subsystem extracted from HookEngine:
//   * IOutputInjector RCU publish (active channel — RichEdit / Win32 / Split)
//   * Synthetic-event counter (synthEventsPending_ + lastSynthSendTime_ +
//     lastRealSynthTime_ + hadSynthInWord_)
//   * Sending re-entrant gate (sending_)
//   * Per-app output policy flags (useClipboardPaste_, skipEmptyChar_)
//   * ReplaceUnicode orchestrator — IsSyncReplaceChannel? RichEdit retry-loop,
//     ShouldUseClipboard? TryEditMessagePaste + clipboard fallback,
//     else SendInput w/ optional reinjectVk prepend
//   * SendBackspaces / SendBackspaceEvents / SendCharEvents primitives
//   * ClipboardPaste (modifier release + Ctrl+V simulation)
//   * TryEditMessagePaste (EM_REPLACESEL fast path, reads focus_ cache)
//
// HookEngine still owns the *composition* state (previousComposition_,
// previousEncodedWidths_, code-table) and the diff-then-dispatch outer
// shell of ReplaceComposition; OutputDispatcher receives the precomputed
// (bsCount, text, reinjectVk) tuple and handles the rest.
//
// Threading:
//   * All dispatch entry points run on the hook thread (HookEngine asserts
//     VKEY_ASSERT_HOOK_THREAD at ReplaceComposition + TryEscRestoreRaw +
//     SendBackspaces entries — no need to re-assert here).
//   * `sending_`, `synthEventsPending_`, `useClipboardPaste_`,
//     `skipEmptyChar_` are atomic — readers run on hook thread (key path)
//     and (for sending_) the LL keyboard callback. The atomic surface
//     mirrors pre-Wave-3 PR 3.3 hand-rolled fields in HookEngine.
//   * `lastSynthSendTime_`, `lastRealSynthTime_`, `hadSynthInWord_` are
//     hook-thread-only — plain types, no atomics.
//   * `injector_` is std::atomic<std::shared_ptr<IOutputInjector>> — RCU
//     publish on focus change, lock-free hot-path load on key path.

#pragma once

#include "core/config/TypingConfig.h"  // CodeTable enum
#include "core/FocusApplyDecision.h"
#include "output/IOutputInjector.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace NextKey {

class FocusOwner;  // forward declaration
struct FocusClassification;

class OutputDispatcher {
public:
    explicit OutputDispatcher(FocusOwner& focus);
    ~OutputDispatcher();

    OutputDispatcher(const OutputDispatcher&) = delete;
    OutputDispatcher& operator=(const OutputDispatcher&) = delete;

    /// Install — set `s_instance` + wire the Output::Internal synth-
    /// counter callback. Idempotent. Call from HookEngine::Start before
    /// the hook thread is created so the first SendInput sees a live
    /// callback.
    void Install() noexcept;
    /// Uninstall — clear the synth-counter callback BEFORE nulling
    /// `s_instance` (otherwise an in-flight TrackedSendInput could
    /// dereference s_instance after we clear it). Idempotent.
    void Uninstall() noexcept;

    // ── Injector RCU ─────────────────────────────────────────────────
    /// Publish a fresh injector. RCU pattern — readers loading mid-swap
    /// keep the old `shared_ptr` alive until they drop the reference.
    void SetInjector(std::shared_ptr<NextKey::Output::IOutputInjector> inj) noexcept;
    [[nodiscard]] std::shared_ptr<NextKey::Output::IOutputInjector> GetInjector() const noexcept;
    /// True iff active injector's SettleBudget == 0ms (RichEdit only
    /// today). Used at 4 policy-gate sites in HookEngine + here for the
    /// RichEdit retry path in ReplaceUnicode.
    [[nodiscard]] bool IsSyncReplaceChannel() const noexcept;

    // Prepare on the worker; publish on the hook using the same ordering
    // decision as the composition transaction. Neither step edits the word.
    static void PrepareFocusOutput(FocusClassification& cls) noexcept;
    [[nodiscard]] bool ApplyFocusOutput(const FocusClassification& cls,
                                       FocusApplyDisposition disposition,
                                       bool suggestKeepChars) noexcept;
    [[nodiscard]] bool WantsGameReinject() const noexcept {
        return wantsGameReinject_;
    }

    // ── Per-app output policy (publishers — hook thread on focus change) ─
    void SetUseClipboardPaste(bool v) noexcept {
        useClipboardPaste_.store(v, std::memory_order_release);
    }
    void SetSkipEmptyChar(bool v) noexcept {
        skipEmptyChar_.store(v, std::memory_order_release);
    }
    [[nodiscard]] bool SkipEmptyChar() const noexcept {
        return skipEmptyChar_.load(std::memory_order_acquire);
    }
    /// True iff focused app needs clipboard paste fallback AND current
    /// code table is Unicode (TCVN3 / VNI-Win skip clipboard).
    [[nodiscard]] bool ShouldUseClipboard(CodeTable currentTable) const noexcept;

    // ── Sending re-entrant gate ─────────────────────────────────────
    [[nodiscard]] bool IsSending() const noexcept {
        return sending_.load(std::memory_order_acquire);
    }
    void SetSending(bool sending) noexcept {
        sending_.store(sending, std::memory_order_release);
    }

    /// RAII guard for sending state (Issue 2). Ensures IsSending() remains true
    /// during replay or injection, and restores previous state upon destruction.
    class ScopedSending {
    public:
        explicit ScopedSending(OutputDispatcher& dispatcher) noexcept
            : dispatcher_(dispatcher),
              prev_(dispatcher.sending_.exchange(true, std::memory_order_acq_rel)) {}

        ~ScopedSending() noexcept {
            dispatcher_.sending_.store(prev_, std::memory_order_release);
        }

        ScopedSending(const ScopedSending&) = delete;
        ScopedSending& operator=(const ScopedSending&) = delete;

    private:
        OutputDispatcher& dispatcher_;
        bool prev_;
    };

    // ── Synth tracking ──────────────────────────────────────────────
    [[nodiscard]] int SynthEventsPending() const noexcept {
        return synthEventsPending_.load(std::memory_order_relaxed);
    }
    /// Called from LowLevelKeyboardProc passthrough (own synthetic event
    /// — VKEY_EXTRA_INFO marker). Single-decrement per event.
    void DecrementSynthEvents() noexcept {
        // Pre-PR-3.3 behaviour preserved: only decrement when > 0 (caller
        // checked, but cheap to re-verify here so the public API is safe).
        int v = synthEventsPending_.load(std::memory_order_relaxed);
        if (v > 0) synthEventsPending_.fetch_sub(1, std::memory_order_relaxed);
    }
    /// Reset counter — used by ClearWordState + watchdog (stuck > 500ms).
    void ResetSynthEvents() noexcept {
        synthEventsPending_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] DWORD LastSynthSendTime() const noexcept { return lastSynthSendTime_; }
    [[nodiscard]] DWORD LastRealSynthTime() const noexcept { return lastRealSynthTime_; }
    /// Word-reset hook (ClearWordState) — match pre-PR-3.3 behaviour
    /// (`lastRealSynthTime_ = 0` only; lastSynthSendTime_ stays — it's the
    /// hard watchdog timestamp).
    void ResetLastRealSynthTime() noexcept { lastRealSynthTime_ = 0; }

    [[nodiscard]] bool HadSynthInWord() const noexcept { return hadSynthInWord_; }
    void SetHadSynthInWord(bool v) noexcept { hadSynthInWord_ = v; }

    // ── Dispatch entry points (hook thread) ─────────────────────────
    /// Unicode-path dispatch with full retry+fallback orchestration:
    ///   1. IsSyncReplaceChannel → RichEdit retry-loop (30ms retry
    ///      budget; an in-flight synchronous attempt may overrun it), then
    ///      fall through on exhaust.
    ///   2. ShouldUseClipboard → TryEditMessagePaste + clipboard fallback
    ///      chain (BS-adjust for reinjectVk).
    ///   3. Generic — optional reinjectVk SendInput prepend + injector
    ///      Replace.
    /// Returns nothing — failures are logged + best-effort.
    void ReplaceUnicode(size_t backspaceCount,
                        std::wstring_view text,
                        std::uint16_t reinjectVk) noexcept;

    /// Raw injector dispatch — no retry, no fallback, no reinjectVk.
    /// Used by encoded-path ReplaceComposition + TryEscRestoreRaw.
    /// Returns the injector's success flag so callers (TryEscRestoreRaw)
    /// can react.
    [[nodiscard]] bool ReplaceRaw(size_t backspaceCount,
                                  std::wstring_view text) noexcept;

    /// Public BS-only path. Single-call into the active injector.
    void SendBackspaces(size_t count) noexcept;

    /// Re-inject a single VK keystroke (game-compat replay / commit-trigger
    /// re-ordering). Wraps `sending_` re-entrant gate + injector SendKey +
    /// `lastSynthSendTime_` watchdog timestamp. Does NOT update
    /// `lastRealSynthTime_` — InjectKey is replay, not "real" typing.
    void InjectKey(std::uint16_t vkCode) noexcept;

    /// Macro expansion clipboard fast path. Used by TryExpandMacro when the
    /// expansion exceeds the clipboard threshold. Wraps modifier release +
    /// Ctrl+V + RecordSynthDispatch.
    void ClipboardPasteText(const std::wstring& text) noexcept {
        ClipboardPaste(text);
    }

private:
    /// Raw SendInput BS events (no injector). Used by clipboard fallback.
    void SendBackspaceEvents(size_t count) noexcept;
    /// Raw SendInput Unicode char events. Used by clipboard fallback when
    /// SetClipboardText fails.
    void SendCharEvents(const std::wstring& text) noexcept;
    /// Clipboard paste with modifier release + Ctrl+V simulation.
    void ClipboardPaste(const std::wstring& text) noexcept;
    /// EM_REPLACESEL direct paste — primary VB6/ANSI path. Reads
    /// `focus_.CachedFocusedHwnd()` / `focus_.CachedFocusedClass()`;
    /// refreshes the cache on miss.
    [[nodiscard]] bool TryEditMessagePaste(const std::wstring& text,
                                            size_t backspaceCount) noexcept;
    /// Post-dispatch timestamps (lastSynthSendTime_ + lastRealSynthTime_).
    void RecordSynthDispatch() noexcept;

    /// Output::Internal::g_synthCounterCallback target. Wired in
    /// Install / cleared in Uninstall.
    static void OnSynthDispatched(int delta) noexcept;

    // Non-const ref: TryEditMessagePaste calls focus_.RefreshFocusCache()
    // on cache miss (non-const — writes cachedFocusedHwnd_/Class). All
    // other dispatcher reads of focus_ (CachedFocusedHwnd, CachedFocusedClass)
    // are const-safe and compatible with either const- or non-const ref.
    FocusOwner& focus_;

    std::atomic<std::shared_ptr<NextKey::Output::IOutputInjector>> injector_;
    std::atomic<int>  synthEventsPending_{0};
    std::atomic<bool> sending_{false};
    std::atomic<bool> useClipboardPaste_{false};
    std::atomic<bool> skipEmptyChar_{false};
    bool wantsGameReinject_{false};  // hook-thread-owned output policy

    DWORD lastSynthSendTime_ = 0;
    DWORD lastRealSynthTime_ = 0;
    bool  hadSynthInWord_    = false;

    /// Singleton for the static OnSynthDispatched callback dispatch
    /// (Output::Internal::g_synthCounterCallback is a plain function
    /// pointer — no userdata). Mirrors HookLifecycle / FocusOwner
    /// per-instance approach. Set in Install, cleared in Uninstall.
    static std::atomic<OutputDispatcher*> s_instance;
};

}  // namespace NextKey
