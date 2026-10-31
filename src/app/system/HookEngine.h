// VKey - Keyboard Hook Engine
// SPDX-License-Identifier: GPL-3.0-only
//
// Single-process Vietnamese input using WH_KEYBOARD_LL.
// Replaces TSF DLL for MVP — no COM registration, no admin elevation.

#pragma once

#include "core/engine/IInputEngine.h"
#include "core/engine/CodeTableConverter.h"
#include "core/config/TypingConfig.h"
#include "core/config/ConfigSnapshot.h"
#include "core/hotkey/HotkeyRegistry.h"
#include "core/AutoCapStateTransition.h"
#include "core/DorionHookReclaimPolicy.h"
#include "core/FocusApplyDecision.h"
#include "core/SmartSwitchManager.h"
#include "core/ipc/BrowserContextManager.h"
#include "app/system/HookCommandMailbox.h"
#include "app/system/HookLifecycle.h"
#include "app/system/FocusOwner.h"
#include "app/system/OutputDispatcher.h"
#include "app/system/CommitState.h"
#include "app/system/AdaptiveTick.h"
#include "core/pipeline/IBackwardEditExecutor.h"
#include "core/pipeline/ICommitUndoExecutor.h"
#include "core/pipeline/IEscRestoreRawExecutor.h"
#include "core/pipeline/IMacroExecutor.h"
#include "core/pipeline/Coordinator.h"
#include "core/pipeline/OutputChannel.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Wave 3 PR 3.3 — IOutputInjector forward declaration removed; the type is
// pulled in transitively via OutputDispatcher.h (which directly includes
// output/IOutputInjector.h since it owns std::atomic<shared_ptr<IOI>>).
// HookEngine no longer touches IOI directly — all dispatch goes through
// dispatcher_.

namespace NextKey {

class SharedStateManager;  // Forward declaration (defined in core/ipc/SharedStateManager.h)
class HotkeyManager;       // Forward declaration
#ifdef VKEY_USE_RUST_ENGINE
class RustUserDictionarySnapshot;
#endif

/// Callback when Vietnamese/English mode changes.
/// `sharedMode`  = logical V/E to persist into SharedState (drives the DLL,
///                 OnTickPoll sync, and per-app restoration).
/// `displayMode` = what the tray / floating / settings icon should show; equals
///                 `sharedMode` except in a TSF app whose TIP is not active,
///                 where the icon is forced to English while the logical mode is
///                 preserved. Conflating the two clobbered the logical flag and
///                 made V/E unrecoverable in TSF apps (issue #209).
using ModeChangeCallback = std::function<void(bool sharedMode, bool displayMode)>;

/// Keyboard hook engine — intercepts keystrokes, routes the passive application
/// hotkey matcher, processes Vietnamese input, and outputs via SendInput
/// backspace+retype.
class HookEngine
    : public NextKey::Pipeline::IBackwardEditExecutor
    , public NextKey::Pipeline::ICommitUndoExecutor
    , public NextKey::Pipeline::IEscRestoreRawExecutor
    , public NextKey::Pipeline::IMacroExecutor {
public:
    HookEngine();
    ~HookEngine() override;

    HookEngine(const HookEngine&) = delete;
    HookEngine& operator=(const HookEngine&) = delete;

    // Pipeline::IBackwardEditExecutor — Wave 2 adapter for BackwardEditFeature.
    // Thin wrapper around the existing ReplaceComposition; Wave 3+ will split the
    // pure-diff phase out into the feature and execute via OutputChannel.
    void ExecuteReplace(std::wstring_view newText,
                        std::uint16_t reinjectVk) override;

    // Pipeline::ICommitUndoExecutor — Wave 3 adapter for CommitUndoFeature.
    // Thin wrapper around the existing HandleCommitUndoFsm FSM body. Reads
    // vnMode internally from vietnameseMode_ atomic. Maps KeyOutcome →
    // CommitUndoOutcome at the boundary. Wave N+ will lift the FSM body
    // into CommitUndoFeature::Try for true single-owner state.
    [[nodiscard]] NextKey::Pipeline::CommitUndoOutcome HandleCommitUndo(
        std::uint16_t vkCode,
        bool shift, bool capsLock, bool ctrl, bool alt, bool win) override;

    // Pipeline::IEscRestoreRawExecutor — Wave 4a adapter for EscRestoreRawFeature.
    // Resolves hotkey registry (CancelComposition intent) + live/primed-commit
    // gates internally, then calls the existing TryEscRestoreRaw body if the
    // gate passes. Returns Eat on consume, Fallthrough otherwise.
    [[nodiscard]] NextKey::Pipeline::EscRestoreOutcome TryEscRestore(
        std::uint16_t vkCode,
        bool shift, bool ctrl, bool alt, bool win) override;

    // Pipeline::IMacroExecutor — Wave 4b adapter for MacroFeature.
    // Owns macro tracking (rawMacroBuffer_ accumulation) + dispatch:
    // EN mode (English macro), SkipMacro hotkey, expansion via TryExpandMacro.
    // Body transcribed 1:1 from pre-W4b HandlePreDispatch macro blocks; logic
    // unchanged. Reads vnMode/macroOn/macroEng atomics + RCU configSnapshot
    // internally so the feature interface stays decoupled from those fields.
    [[nodiscard]] NextKey::Pipeline::MacroOutcome HandleMacro(
        std::uint16_t vkCode,
        bool shift, bool capsLock, bool ctrl, bool alt, bool win) override;

    /// Start the hook engine (installs keyboard hook + focus hook)
    bool Start(HINSTANCE hInstance, const TypingConfig& config,
               bool initialVietnamese = true);

    /// Stop and unhook everything
    void Stop();

    /// Toggle Vietnamese/English mode
    void ToggleVietnameseMode() noexcept;

    /// Commit any pending composition — called by hotkey callbacks before firing actions
    /// (e.g., Quick Convert) so the text in the document reflects what's on screen.
    void CommitPending();

    /// Attach the passive application-hotkey matcher during startup. This
    /// wires its no-throw deferred-fire sink into HookLifecycle and also makes
    /// it available for focus-time physical-modifier reconciliation.
    void SetHotkeyManager(HotkeyManager* manager) noexcept;

    /// Set callback for mode changes (to update tray icon)
    void SetModeChangeCallback(ModeChangeCallback callback) { modeChangeCallback_ = std::move(callback); }

    /// Set callback for config reload (notifies main to update QuickConvert etc.)
    void SetConfigReloadCallback(std::function<void()> callback) { configReloadCallback_ = std::move(callback); }

    /// Wave 3 PR 3.8 — fast path for toggle-hotkey rebinding from SharedState.
    ///
    /// Pre-3.8 the only propagation path was `configReloadCallback_` fired
    /// after `ReloadFromToml()`. But `SettingsDialog::syncToSharedState`
    /// writes the new hotkey into SharedState immediately while deferring
    /// the TOML save by 30 s — so HookEngine's ReloadFromToml read STALE
    /// disk data and `HotkeyManager::UpdateHotkey` got the old binding
    /// until the user closed the dialog (which forces TOML save).
    ///
    /// This callback fires from `QuickSyncFromSharedState`'s slow body
    /// whenever the SharedState hotkey field differs from the previously-
    /// observed value, BEFORE `ReloadFromToml()` runs. Doctrine alignment:
    /// SharedState is the live config bus; TOML is the persistence layer.
    /// Hotkey changes hit the live bus instantly and propagate the same way.
    ///
    /// Threading: invoked from QuickSync's CAS-claimed slow body — that
    /// body runs on the worker thread (typical: workHandler signaled by
    /// SharedState change) or the main thread (atypical: a menu command
    /// like SpellCheck-toggle calls ApplyConfigChange which bumps
    /// configGeneration and triggers QuickSync inline). Both paths reach
    /// `HotkeyManager::UpdateHotkey` from a non-hook thread; UpdateHotkey
    /// publishes one packed configuration with a lock-free atomic store, so
    /// cross-thread invocation is safe. Post-PR-3.6 the hook thread bails BEFORE the
    /// slow body (worker-thread doctrine §12.4), so this callback can
    /// never fire on the LL hook thread.
    using HotkeyChangedCallback = std::function<void(const HotkeyConfig&)>;
    void SetHotkeyChangedCallback(HotkeyChangedCallback callback) {
        hotkeyChangedCallback_ = std::move(callback);
    }

    /// Callback fired when focus/config changes TSF ownership.
    /// Args: (tsfActive, tsfReadonly, shouldActivateProfile).
    ///   tsfActive   = foreground app is in TSF list (full TIP consumes keys).
    ///   tsfReadonly = Hook handles keys; TSF DLL should publish doc anchor.
    ///   shouldActivateProfile = focus entered a new TSF HWND, or config made
    ///                           the current app a TSF target.
    /// Fires on every applied focus change (SetOrClearFlag is idempotent).
    using TsfModeCallback = std::function<void(bool, bool, bool)>;
    void SetTsfModeCallback(TsfModeCallback callback) {
        tsfModeCallback_ = std::move(callback);
    }

    /// Worker-thread callback fired after a current real-app classification.
    /// Args: (exeName, ruleText, isTsf, isRustEngine). Implementations may
    /// allocate because this never runs in the keyboard-hook callback.
    using FocusAppContextCallback =
        std::function<void(std::wstring_view, std::wstring_view, bool, bool)>;
    void SetFocusAppContextCallback(FocusAppContextCallback callback) {
        focusAppContextCallback_ = std::move(callback);
    }

    /// Re-read SharedState and reload TOML if configGeneration changed.
    /// Safe cross-process: uses the configGeneration counter, not the Named Event
    /// (which is auto-reset and reserved for the TSF DLL).
    void SyncConfigFromSharedState();

    /// Periodic poll: CJK layout change detection + foreground PID
    /// fallback (catches missed/phantom focus events). Sprint 1 D10
    /// migrated this off `SetTimer(200ms, FocusPollTimerProc)` and onto
    /// `MainThreadWorker`'s tick branch. The body is unchanged from the
    /// retired `FocusPollTimerProc`; the call site is the only difference.
    /// Safe to call from any thread that is not the LL hook thread —
    /// stateMutex_ serializes against main-thread writers.
    void OnTickPoll() noexcept;

    /// Set SharedState pointer for direct reading (must be the global instance from main.cpp)
    void SetSharedStateReader(SharedStateManager* ptr) { sharedStatePtr_ = ptr; }

    /// Return the dedicated hook thread id for diagnostics and thread-affinity
    /// assertions. Returns 0 before Start() completes or after Stop().
    [[nodiscard]] DWORD GetHookThreadId() const noexcept { return lifecycle_.ThreadId(); }

    /// Doctrine §12.4 (worker-thread doctrine): the wiring `main.cpp` uses to
    /// give HookEngine a way to signal the off-hook worker thread without
    /// taking a dependency on `MainThreadWorker`. Producers (WinEventProc
    /// handler, hook-thread QuickSync slow-path detection) latch state into
    /// `pendingClassifyRequest_` (or rely on the Signal itself to mark the
    /// QuickSync re-run) then invoke this fn so the worker drains on its
    /// next wake. Wired once at startup, never changed.
    using WorkerSignalFn = std::function<void()>;
    void SetWorkerSignalFn(WorkerSignalFn fn) noexcept { workerSignalFn_ = std::move(fn); }

    /// Doctrine §12.4 worker drain. Runs on the MainThreadWorker thread from
    /// the workHandler wired in `main.cpp`. Dequeues `pendingClassifyRequest_`
    /// and runs the heavy classify body (QuickSync + focus_.Classify + mailbox
    /// post). Safe to call when nothing is pending — early-returns.
    /// PUBLIC so `main.cpp` can wire the workHandler without exposing
    /// internal private members; intended call site is one — the workHandler
    /// lambda. Doctrine §12.6 audit-allow not required: not an atomic field.
    void DrainClassifyOnWorker();

    /// Doctrine §12.4 worker drain, same contract as DrainClassifyOnWorker:
    /// the hook thread only latches a flag + calls workerSignalFn_, and this
    /// does the actual work off the hot path. Toggles the FOREGROUND app in
    /// the per-app override table between send method 5 (game) and no entry,
    /// then persists + signals. File I/O is why it cannot run inline in the
    /// hook (Rule 11.1's 1 ms budget); SaveAppOverrides writes TOML
    /// immediately rather than through the deferred 30 s save, which is what
    /// the caller wants — a game that exits seconds later must not lose it.
    ///
    /// The foreground app is re-derived here rather than captured at press
    /// time: passing a wstring across the hook boundary would need a lock the
    /// hook thread is not allowed to take. The user is looking at the app they
    /// mean to tag, and the worker wakes within a tick, so the window where
    /// this could tag the wrong app is not reachable in practice.
    void DrainGameModeToggleOnWorker();

    /// Adaptive-tick backoff (2026-05-27, plan docs/plans/2026-05-27-
    /// adaptive-tick-idle-backoff.md). Wired once at startup from main.cpp /
    /// main_lite.cpp; the lambda calls g_mainThreadWorker.SetTickInterval(ms).
    /// Same callback-injection pattern as SetWorkerSignalFn — keeps HookEngine
    /// independent of MainThreadWorker.
    using TickRetuneFn = std::function<void(std::chrono::milliseconds)>;
    void SetTickRetuneFn(TickRetuneFn fn) noexcept { tickRetuneFn_ = std::move(fn); }

    /// Adaptive-tick — called by ProcessKeyDown (hook thread, post synth-event
    /// filter) and FocusOwner focus-change bridge (main thread). Rule 11.2
    /// compliant on the hook hot path: one relaxed atomic store + one
    /// relaxed atomic load + branch ≈ 5 ns when already in active cadence.
    /// When the gate trips (idle → active transition), one workerSignalFn_()
    /// call wakes the worker so workHandler runs RetuneCadenceIfNeeded.
    /// Safe to call from any thread.
    void MarkActivity() noexcept;

    /// Adaptive-tick — compares idleMs since last MarkActivity against the
    /// AdaptiveTick thresholds; if the desired interval differs from the
    /// currently-published one, store the new value and invoke tickRetuneFn_.
    /// Called from BOTH OnTickPoll (the worker tick path) AND the workHandler
    /// wired in main.cpp (the post-Signal wake path). PUBLIC for the same
    /// reason DrainClassifyOnWorker is — main.cpp's workHandler needs to call
    /// it without poking private members. Worker-thread only.
    void RetuneCadenceIfNeeded() noexcept;

    /// Change code table (commits pending composition, updates per-app map)
    void SetCodeTable(CodeTable ct);

    /// Get effective code table (checks manual per-app override map)
    [[nodiscard]] CodeTable GetCodeTable() const noexcept;

    [[nodiscard]] bool IsVietnameseMode() const noexcept {
        return vietnameseMode_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool ShouldUseNativeQuickConvert() const noexcept;
    [[nodiscard]] bool IsRunning() const noexcept { return lifecycle_.IsRunning(); }

    // Magic number to mark our own SendInput events (prevents other hooks from processing them)
    static constexpr ULONG_PTR VKEY_EXTRA_INFO = 0x4E4B;  // "NK"

    // Wave 3 PR 3.2 — `GetExeNameForHwnd` migrated to FocusOwner alongside the
    // rest of the focus-classification subsystem. Callers reach it via
    // `NextKey::FocusOwner::GetExeNameForHwnd(hwnd)`.

private:
    struct FocusClassifyRequest {
        HWND triggerHwnd{nullptr};
        std::uint64_t requestSerial{0};
        std::uint64_t inputEpochAtRequest{0};
    };

    // Hook callbacks (static → instance dispatch). WinEventProc moved to
    // FocusOwner (Wave 3 PR 3.2).
    //
    // Each public proc is a thin SEH wrapper (no unwindable locals) around an
    // *Impl that holds the real body + its C++ try/catch. The wrapper exists
    // because under /EHsc `catch(...)` does NOT catch structured exceptions
    // (access violations): an AV in hook code would otherwise unwind through
    // KiUserCallbackDispatcher and terminate VKeyApp — which the watchdog then
    // respawns, feeding the AV self-defense signal. __try/__except and C++
    // unwinding can't coexist in one function (C2712), hence the split.
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT LowLevelKeyboardProcImpl(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT LowLevelMouseProcImpl(int nCode, WPARAM wParam, LPARAM lParam);
    static void PostMatchedHotkey(void* context, std::size_t slot) noexcept;

    // Config application (shared between Start and SyncConfigFromSharedState)
    void ApplyConfig(const TypingConfig& config);

    /// Publish a new hotkey registry to the hook hot path. Atomic RCU swap —
    /// caller may call from main thread; the hook callback picks up the new
    /// snapshot on its next ProcessKeyDown. The previous registry stays alive
    /// until any in-flight key event finishes its load().
    void ApplyHotkeyRegistry(HotkeyRegistry registry);

    // Core processing. preDrain* is a modifier snapshot the caller (hook
    // callback) took BEFORE DrainHookCommands() ran — see call site and
    // ProcessKeyDown's body comment for why re-reading GetKeyState inside
    // this function is unsafe.
    bool ProcessKeyDown(DWORD vkCode, DWORD scanCode, DWORD flags,
                        bool preDrainShift, bool preDrainCapsLock, bool preDrainCtrl,
                        bool preDrainAlt, bool preDrainWin);
    bool ProcessKeyUp(DWORD vkCode, DWORD flags);

    // Outcome of ProcessKeyDown step extracts (H1a/H1b/H1c).
    //   Eat         → ProcessKeyDown returns true (key consumed by step).
    //   Pass        → ProcessKeyDown returns false (key passes through to app).
    //   Fallthrough → continue with subsequent ProcessKeyDown steps.
    enum class KeyOutcome : uint8_t { Eat, Pass, Fallthrough };

    // H1b (extracted from ProcessKeyDown steps 0/0b/1/1b/1c): top-of-pipeline
    // guards — QuickSync from SharedState, TSF early-out, modifier tracking,
    // toggle-key passthrough (Caps/Num/Scroll), excluded-app passthrough with
    // PID verification. Falls through only when the keystroke should reach
    // the Vietnamese pipeline. Bookkeeping (otherKeyPressed_/modTapCount_/
    // synth watchdog) stays in ProcessKeyDown after the switch — only runs
    // on Fallthrough by design (excluded-app's same-PID/still-excluded paths
    // set otherKeyPressed_ themselves before returning Pass).
    [[nodiscard]] KeyOutcome RunTopGuards(DWORD vkCode);

    // H1a (extracted from ProcessKeyDown step 2d): commit-undo state machine
    // (Idle/Ready/Primed) — handles backspace-into-committed-word replay.
    // Mutates commitUndoState_/pendingTriggerCount_/macroCrossCommit_/rawMacroBuffer_
    // and may call HandleAlphaKey/HandleVniDigitKey/HandleBackspace/InjectKey.
    [[nodiscard]] KeyOutcome HandleCommitUndoFsm(DWORD vkCode, bool vnMode,
                                                 bool shift, bool capsLock,
                                                 bool ctrl, bool alt, bool win);

    // H1c (extracted from ProcessKeyDown steps 3 / 3a-3d): English-mode short
    // circuit + Vietnamese-mode pre-dispatch tracking. When !vnMode, runs the
    // English-mode macro tracking block and returns Pass. When vnMode, updates
    // auto-caps FSM, accumulates the macro buffer, handles temp-off-by-Esc, and
    // attempts macro expansion on commit triggers — returning Eat/Pass on
    // expansion match, Fallthrough otherwise so the dispatch chain runs next.
    /// Esc-restore-raw: end composition with raw keys and inject them (víu → virus).
    /// Returns KeyOutcome::Eat on success, Fallthrough if buffer empty / disabled.
    [[nodiscard]] KeyOutcome TryEscRestoreRaw();

    [[nodiscard]] KeyOutcome HandlePreDispatch(DWORD vkCode, bool vnMode,
                                                bool cachedShift,
                                                bool cachedCtrl, bool cachedAlt,
                                                bool cachedWin);

    // H1c (extracted from ProcessKeyDown steps 4b-10): action dispatch chain.
    // Late guards (tempEngineOff_ bypass, Ctrl/Alt/Win shortcut skip), then
    // alpha / Telex bracket / VNI digit / backspace / commit-trigger / "other
    // key with pending composition" / synth-pending BS re-inject. Returns Eat
    // or Pass for every keystroke (final fallthrough returns Pass — original
    // ProcessKeyDown's tail `return false`).
    [[nodiscard]] KeyOutcome DispatchKeyAction(DWORD vkCode, bool cachedShift,
                                                bool cachedCapsLock, bool cachedCtrl,
                                                bool cachedAlt, bool cachedWin, bool macroOn);

    // Input engine interaction
    [[nodiscard]] bool HandleAlphaKey(DWORD vkCode, bool shift, bool capsLock);
    [[nodiscard]] bool HandleVniDigitKey(DWORD vkCode); // VNI/UserDefined digit 0-9: push to engine, replace composition
    void HandleBackspace();
    // Returns true if auto-restore changed text. `verbatim` drops a lexicon
    // correction (IsVerbatimCommitTrigger); raw restore still applies.
    bool CommitComposition(bool verbatim = false);
    void ResetComposition();
    void CancelCommitUndo();   // commitUndoState_ = Idle + commitStack_.clear()
    void SetCommitUndoReady(); // commitUndoState_ = Ready + timestamp

    // Output — universal SendInput with KEYEVENTF_UNICODE. Wave 3 PR 3.3
    // shrank the body to a thin shim: prefix-diff + code-table encoding +
    // previousComposition_/Widths_ update, then dispatcher_.ReplaceUnicode
    // (Unicode path) or dispatcher_.ReplaceRaw (encoded path) handles the
    // RichEdit retry / clipboard fallback / SendInput orchestration.
    void ReplaceComposition(const std::wstring& newText, DWORD reinjectVk = 0);

    // Wave 2 — pipeline dispatch helper. Builds a HookCompositionSession +
    // KeyContext from current HookEngine state, hands them to coordinator_,
    // then drains any inadvertent intents from outputChannel_. In W2 the only
    // registered feature is BackwardEditFeature which delegates synchronously
    // to ExecuteReplace, so the batch is always empty after HandleKey returns.
    void DispatchCoordinator(DWORD vkCode, DWORD reinjectVk,
                              const std::wstring& composition);

    // Wave 3 PR 3.3 — SendBackspaces / SendBackspaceEvents / SendCharEvents,
    // OnSynthDispatched, IsSyncReplaceChannel, ShouldUseClipboard,
    // ClipboardPaste, TryEditMessagePaste, RecordSynthDispatch moved to
    // OutputDispatcher. Public surface via `dispatcher_.X()`.

    // Modifier state tracking (used by double-Alt and layout change detection)
    void TrackModifier(DWORD vkCode, bool isDown);

    // Restore a committed word for another edit. Backspace prefers the visible
    // glyph state; tone/modifier keys preserve raw replay provenance.
    [[nodiscard]] bool ReplayCommittedChars(bool preferVisibleText = false);

    // Commit trigger check
    static bool IsCommitTrigger(DWORD vkCode);
    bool IsMacroTrigger(DWORD vkCode) const;

    // OEM punctuation subset of IsCommitTrigger (DispatchKeyAction step 6d).
    static bool IsOemPunctVk(DWORD vkCode);

    // Convert VK code to macro-usable char (for special-char macro keys).
    // Uses MapVirtualKeyW — returns unshifted character only (Shift state ignored).
    [[nodiscard]] static wchar_t VkToMacroChar(DWORD vkCode) noexcept;

    // Result of TryExpandMacro
    enum class MacroResult { NoMatch, ExpandedEatTrigger, ExpandedPassTrigger };

    // Shared macro expansion logic (used by both English and Vietnamese mode paths).
    [[nodiscard]] MacroResult TryExpandMacro(wchar_t triggerChar);

    // Re-inject a key after auto-restore replacement
    void InjectKey(DWORD vkCode);

    // Clear per-word engine state (shared by CommitComposition, ResetComposition, TryExpandMacro)
    void ClearWordState();

    // Drop the raw macro buffer. wasFirstCharAutoCapped_ is only meaningful while the
    // buffer still starts at the word's first character, so the two must die together —
    // a surviving flag would capitalize a macro matched by a restarted buffer.
    void ClearMacroBuffer() noexcept;

    // Wave 3 PR 3.2 — IsTrayOrTaskbarWindow + IsWebView2App migrated to
    // FocusOwner (focus-classification helpers; no engine state).
    void NotifyModeChange() noexcept;  // Fire modeChangeCallback_ with effective mode
    bool VerifyExcludedState();        // Check if foreground is still excluded; clears stale flag if not
    // Wave 3 PR 3.6 — focus-classify routing under the worker-thread doctrine
    // (docs/CODING_RULES/12-worker-thread-doctrine.md §12.4).
    //
    //   Producers (WinEventProc on main, OnTickPoll's PID-change branch on
    //   worker) capture an immutable request containing the trigger HWND,
    //   request serial and current physical-input epoch. OnFocusChanged is
    //   produce-only: latch that request + invoke
    //   `workerSignalFn_` to wake the worker. The worker's workHandler
    //   then calls `DrainClassifyOnWorker()` (public, see top of class)
    //   which runs the heavy body — QuickSync + `focus_.Classify` +
    //   `Post(kFocusChanged)` — on the worker thread only.
    //
    //   Why this matters: pre-3.6 `focus_.Classify()` ran on whichever
    //   thread called `OnFocusChanged`. That meant main (WinEventProc) and
    //   worker (OnTickPoll PID branch) could concurrently mutate the plain
    //   `appProfileCache_` and `webView2PositiveCache_` containers on
    //   FocusOwner. Single-writer is restored by routing both producers
    //   through the worker.
    //
    //   NOT for same-window control changes (mouse click / Tab): the whole
    //   classify+apply chain is too heavy for the input path — see
    //   LowLevelMouseProcImpl, which clears the auto-cap latch directly
    //   instead.
    void OnFocusChanged(HWND triggerHwnd = nullptr);

    // Worker-only entry point used by OnTickPoll's PID-change branch — that
    // branch ALREADY runs on the worker thread (it's a tick-handler body),
    // so it can call this directly without the latch+signal hop. Doctrine
    // §12.5 names this the single legitimate exemption. Same body as the
    // drain consumes; sharing prevents drift between paths.
    [[nodiscard]] FocusClassifyRequest CaptureFocusClassifyRequest(
        HWND triggerHwnd) noexcept;
    void OnFocusChangedSyncOnWorker(const FocusClassifyRequest& request);
    void OnLayoutChanged(bool isCompatibleNow);
    void CheckLayoutChange();  // Query current layout and call OnLayoutChanged if it changed
    void FlushSmartSwitchOnStop();      // Force-flush smart-switch map to TOML on shutdown (bypass debounce).

    // Phase 3d — single source of truth for snapshot rebuild. Parses
    // overrides/excluded/TSF/macros fresh from TOML, derives
    // spaceMacroKeys via ConfigSnapshot::Build, and atomic-publishes the
    // result. Replaces the four legacy Reload* methods + the dual-write
    // PublishConfigSnapshot bridge from P3b. The 2026-05-19 follow-up
    // also folded `appSendMethodOverrides` into the snapshot, closing
    // the last variable-size config map that was racing across threads.
    //
    // NOT `noexcept`: STL container allocations + `std::make_shared`
    // here can throw `std::bad_alloc`. Callers (ReloadFromToml,
    // QuickSync macro-toggle path, OnTickPoll drain) are reached from
    // sites with outer try/catch (LL callback catch for hook-thread
    // path; OnTickPoll's own try for worker path), so a throw unwinds
    // gracefully instead of `std::terminate`-ing the process.
    void RebuildSnapshotFromToml(std::uint32_t generation);

    // Engine state
    std::unique_ptr<IInputEngine> engine_;
    // Sprint 1 D6: migrated to std::atomic<std::shared_ptr<const TypingConfig>>
    // (Rule #11.3 RCU pattern). Writers (main thread): Start, QuickSyncFromSharedState,
    // ReloadFromToml — create a fresh shared_ptr with the new config and store with
    // release ordering. Readers (hook hot path): IsMacroTrigger loads once per
    // call and dereferences the loaded shared_ptr. The old config object stays
    // alive while readers hold their loaded shared_ptr, so no use-after-free is
    // possible even when a writer publishes mid-keystroke. Initialized with a
    // default TypingConfig so the field is never nullptr — Start overwrites it
    // before the hook thread is spawned, but the default-init guards against
    // any pre-Start IsMacroTrigger access path.
    std::atomic<std::shared_ptr<const TypingConfig>> config_{
        std::make_shared<const TypingConfig>()
    };  // Last applied config (for per-app engine recreation)
#ifdef VKEY_USE_RUST_ENGINE
    // Worker/main threads compile `user_dictionary.txt`; the hook thread only
    // exchanges this immutable snapshot at an empty-word config boundary.
    std::atomic<std::shared_ptr<const RustUserDictionarySnapshot>>
        pendingUserDictionary_;
    std::shared_ptr<const RustUserDictionarySnapshot> userDictionary_;
#endif

    // Unified hotkey registry (cancel-composition / skip-macro / toggle-enabled).
    // RCU pattern matching config_ above: writers (main thread) call
    // hotkeys_.store(std::make_shared<...>(newRegistry), release); readers
    // (hook hot path) load() once per ProcessKeyDown to dispatch all three
    // intents against the same snapshot. Default = factory bindings
    // (Esc/Esc/Ctrl-alone/2×Alt) so the field is never nullptr and hot path
    // can dereference unconditionally even before ApplyConfig has run.
    std::atomic<std::shared_ptr<const HotkeyRegistry>> hotkeys_{
        std::make_shared<const HotkeyRegistry>(HotkeyRegistry::Defaults())
    };
    // Phase 3 — RCU snapshot of variable-size config data. Holds the
    // excluded-apps / TSF-apps / macro / per-app override maps that the
    // hook hot path needs to read. Single producer is
    // `RebuildSnapshotFromToml` (worker thread or main; never hook —
    // hook side defers via `pendingConfigReload_`). Readers are
    // lock-free `configSnapshot_.load(acquire)`. Default-init to an
    // empty snapshot so the first reader before any rebuild publishes
    // still gets a dereferenceable pointer.
    std::atomic<std::shared_ptr<const ConfigSnapshot>> configSnapshot_{
        std::make_shared<const ConfigSnapshot>()
    };
    // Sprint 2 T3: Output channel strategy. RCU-published shared_ptr to the
    // active IOutputInjector, same pattern as config_ above. Writers (main
    // thread on focus change): two-phase classify → atomic_store. Readers
    // (hook hot path): atomic load → 1 virtual call (~11 ns total overhead).
    // Initialized in HookEngine ctor via Output::Create({}) so the field is
    // never nullptr — hot path's atomic_load can rely on a usable injector
    // even before any focus event has fired.
    // See docs/plans/sprint-2-output-injector.md §1 for the data-flow contract.
    // Wave 3 PR 3.3 — injector_ moved to OutputDispatcher. Read via
    // dispatcher_.GetInjector(); publish via dispatcher_.SetInjector().
    // Sprint 1 D5.1: migrated to std::atomic for hook-thread-safe read without
    // stateMutex_ (Rule #11.3 acquire/release). Writers: ApplyConfig (main),
    // QuickSyncFromSharedState (hook — same thread as readers), ReloadFromToml
    // (main), OnFocusChanged app-method override (main, via WinEventProc).
    // Readers: ProcessKeyDown punct branch + HandleAlphaKey VNI/Combined gates.
    std::atomic<InputMethod> currentMethod_{InputMethod::Telex};
    std::wstring previousComposition_;  // What's currently displayed in the app
    std::vector<uint8_t> previousEncodedWidths_;  // Output unit count per Unicode char (for non-Unicode code tables)
    // Sprint 1 D5: migrated to std::atomic for hook-thread-safe read without
    // stateMutex_ (Rule #11.3 acquire/release pattern). Hook callback paths
    // (ProcessKeyDown/Up, CheckLayoutChange) use .load(acquire); main thread
    // (ApplyConfig, ToggleVietnameseMode, SettingsDialog WM_VKEY_MODE_CHANGED
    // → HookEngine via callback) uses .store(release).
    std::atomic<bool> vietnameseMode_{true};
    // #221: carries the definitive post-toggle mode from ToggleVietnameseMode()
    // (main/tray thread) to ApplyToggleVNOnHookThread's drain (hook thread).
    // -1 = no pending explicit toggle; 0/1 = target mode. Needed because the
    // drain used to re-read SharedFlags::VIETNAMESE_MODE, which a
    // NotifyModeChange() landing between the eager SharedState flip and the
    // drain (e.g. from an intervening focus event) could have already
    // overwritten with the stale pre-toggle value — silently reverting the
    // toggle. Consumed via exchange(-1) at the top of the drain.
    std::atomic<int8_t> pendingToggleMode_{-1};
    // Wave 3 PR 3.3 — sending_, synthEventsPending_, lastSynthSendTime_,
    // lastRealSynthTime_, hadSynthInWord_ moved to OutputDispatcher.
    // Readers go through dispatcher_.IsSending() / SynthEventsPending() /
    // LastSynthSendTime() / LastRealSynthTime() / HadSynthInWord().
    // Wave 2 (2026-05-23) — formerly cached bool fields (beepOnSwitch_, smartSwitch_,
    // excludeApps_, tsfApps_, cjkAutoSwitch_) deleted. Single source of truth is
    // `config_` RCU. Hot-path readers load once per function via
    //   `const auto cfg = config_.load(std::memory_order_acquire);`
    // then read `cfg->beepOnSwitch` etc. Eliminates the cache/sync surface that
    // forced ApplyConfig to run under stateMutex_.
    // Sprint 1 D5.2: config-derived flags read on the hook callback path
    // (ProcessKeyDown / HandleAlphaKey / TryExpandMacro). Writers: ApplyConfig
    // (main thread). Readers: hook hot path uses .load(acquire); other call
    // sites also use .load(acquire) for uniform pattern (cost = MOV on x86).
    std::atomic<bool> autoCaps_{false};
    // tempEngineOff_ / digitLedWord_: per-word "treat as English" flags.
    // Plain bool — written from hook thread (DispatchKeyAction, ProcessKeyUp)
    // AND main/WinEvent thread (OnFocusChanged, ToggleVietnameseMode, ClearWordState).
    // Benign race accepted: word-aligned bool reads/writes are atomic on x86/x64,
    // and a stale read at most delays the per-word reset by one keystroke (which
    // the next boundary key recovers from). Promoting to std::atomic<bool> would
    // require similar treatment of tempMacroOff_ / autoCapState_ etc. — out of
    // scope for this change.
    bool tempEngineOff_ = false;       // True = Vietnamese bypassed for current word (user-initiated via double-Alt / Ctrl-toggle)
    bool digitLedWord_ = false;        // True = current word started with a digit (VNI/Combined/UserDefined) → auto-bypass (no user action)
    // Per-modifier tap tracking for HotkeyRegistry ToggleEnabled detection.
    // Indexed by ModIdx{Ctrl=0, Shift=1, Alt=2, Win=3}. Generalizes the
    // legacy altTapCount_/lastAltReleaseTime_ pair that only tracked Alt —
    // the unified registry can bind 2× to any modifier, so we need 4 slots.
    static constexpr int kModCount = 4;
    int   modTapCount_[kModCount]   = {0, 0, 0, 0};
    DWORD modTapLastTs_[kModCount]  = {0, 0, 0, 0};
    static constexpr DWORD kDoubleTapTimeoutMs = 400;
    /// Keystroke-based auto-capitalize state machine (used when no TSF anchor truth).
    /// Enum + transition rule live in core/AutoCapStateTransition.h so Linux GTest
    /// can exercise the modifier-gate contract without depending on Win32.
    AutoCapState autoCapState_ = AutoCapState::Idle;
    // Set from FocusClassification::isPasswordFieldFocused on every focus
    // change (ApplyFocusOnHookThread, hook thread only). Suppresses the
    // keystroke-based auto-cap FSM in HandleAlphaKey for the current focus —
    // the FSM itself has no per-field awareness (see AutoCapStateTransition.h).
    // Also cleared directly (permissive direction) by the mouse-click and Tab
    // paths, which can move focus to another control with no WinEvent fired;
    // HandleAlphaKey re-suppresses synchronously if the new control is itself
    // a password field.
    bool suppressAutoCapForPasswordSafety_ = false;
    // Phase 3d: legacy `excludedAppSet_` removed — readers go through
    // configSnapshot_.load()->excludedAppSet. Same migration for
    // tsfAppSet_, macroTable_, spaceMacroKeys_, appEncodingOverrides_,
    // appInputMethodOverrides_, and `appSendMethodOverrides_`
    // (2026-05-19 follow-up — all six variable-size config maps now
    // publish through the snapshot).
    // Sprint 1 D5.2: per-app cached + macro config flags read on hook callback
    // path. Writers: ApplyConfig (main), ReloadFromToml (main),
    // OnFocusChanged + RefreshFocusCache (main, via WinEventProc),
    // VerifyExcludedState (main), ToggleVietnameseMode (main).
    // Readers: ProcessKeyDown / ProcessKeyUp / HandleAlphaKey / output dispatch
    // (DispatchSendInput, SendCharEvents, SendBackspaces) on the hook hot path
    // — all use .load(acquire). Same-thread reads on writer paths use the
    // same idiom for uniformity (cost = MOV on x86).
    std::atomic<bool> isExcludedApp_{false};      // cached: current app is excluded
    std::atomic<DWORD> excludedPid_{0};           // PID of excluded app (fast check in ProcessKeyDown)
    // Per-app hard-V lock. Same acquire/release contract as isExcludedApp_:
    // single-writer on the hook thread (ApplyFocusOnHookThread store(release));
    // reader is the toggle-lock gate (ApplyToggleVNOnHookThread load(acquire)).
    // NOT read on the per-keystroke hot path — forced-V just keeps
    // vietnameseMode_ true, so RunTopGuards/ProcessKeyDown are unchanged.
    // Init false: a toggle before the first focus classify is a benign no-op.
    std::atomic<bool> isForcedVnApp_{false};      // cached: current app is locked to Vietnamese
    std::atomic<DWORD> forcedVnPid_{0};           // PID of forced-V app (toggle-lock stale check)
    // (tsfAppSet_ removed — see Phase 3d note above)
    // Sprint 1 D5.1: migrated to std::atomic. Writers: ReloadFromToml (main) +
    // OnFocusChanged (main, via WinEventProc). Readers: ProcessKeyDown +
    // ProcessKeyUp early-return gates on the hook hot path.
    std::atomic<bool> isTsfApp_{false};       // cached: is current foreground app in TSF list?
    std::atomic<bool> tsfFeatureEnabled_{false}; // TIP is registered/enabled for domain routing
    // Browser extension overlay. The manager maps one fixed-size seqlock state;
    // the hook hot path normally reads only its 32-bit generation.
    BrowserContextManager browserContext_;
    std::atomic<BrowserRoute> browserRoute_{BrowserRoute::Default};
    bool browserContextActive_{false}; // hook-thread owned, includes default/default
    std::uint32_t lastBrowserContextGeneration_{0}; // hook-thread owned
    // Hook-thread owned. Focus polls can classify the same HWND every 200 ms;
    // only the first application may request the expensive TIP re-activation.
    TsfFocusActivationState tsfFocusActivationState_{};
    // Hook-thread owned: tracks active window and focused control so spurious
    // background WinEvents (e.g. FastStone Capture timer UI, issue #273)
    // do not wipe live composition when typing in the same context.
    std::uintptr_t lastAppliedHwnd_{0};
    std::uintptr_t lastAppliedEffectiveFocusHwnd_{0};
    // vk of the key-down currently being processed (and possibly injecting for).
    // Stamped before ProcessKeyDown; read on the re-entrant `sending_` branch to
    // identify a physical key that leaked into the injection window as the
    // in-flight key's own auto-repeat / key-up (issue #206 reorder fix). 0 = none.
    // Hook-thread write, hook-thread (re-entrant) read — relaxed atomic suffices.
    std::atomic<DWORD> sendingForVk_{0};
    // Sprint 2 D3 deleted: dispatch flag isConsoleApp_ — Console hosts now
    // selected via WindowClassification.isConsole → SplitDispatchInjector(5)
    // by the factory; no remaining HookEngine reader. Sprint 2 D4 deleted
    // useEditMsgPath_ — replaced by IsSyncReplaceChannel() (SettleBudget==0
    // proxy). Post-T3 ChannelTraits cleanup deleted isElectronApp_ +
    // needBaitChar_ — both flags moved onto IOutputInjector
    // (HasMultiProcessRenderer() / RequiresSyntheticAlphaLockstep() /
    // NeedsBaitCharPrefix()). Single source
    // of truth on the injector itself.
    // Wave 3 PR 3.2 — webView2PositiveCache_ moved to FocusOwner.
    // Wave 3 PR 3.3 — skipEmptyChar_, useClipboardPaste_ moved to OutputDispatcher.
    // Wave 3 PR 3.2 — lastForegroundPid_, appModeMap_/appModeDirty_/smartSwitchMgr_,
    // activeExe_/lastRealExe_/previousExe_ moved to FocusOwner. Readers go through
    // focus_.LastForegroundPid()/AppModeMap()/Smart()/ActiveExe()/LastRealExe()/PreviousExe().
    // Writers: worker thread (ReloadFromToml → ApplyConfig) AND hook thread
    // (SetCodeTable, QuickSyncFromSharedState, focus override). Readers: hook
    // hot path (HandleAlphaKey, CommitComposition, ClassifyFocusedWindow,
    // ReplaceComposition, macro expansion). Atomic load/store with
    // acquire/release ordering — uint8_t underlying is lock-free on x64.
    std::atomic<CodeTable> currentCodeTable_{CodeTable::Unicode};
    std::atomic<CodeTable> globalCodeTable_{CodeTable::Unicode};   // config value, restored when no override
    // (appEncodingOverrides_, appInputMethodOverrides_, and
    // appSendMethodOverrides_ all removed — Phase 3d + 2026-05-19
    // follow-up. Readers go through configSnapshot_.load()->...)
    std::atomic<InputMethod> globalInputMethod_{InputMethod::Telex}; // config value, restored when no override

    // Wave 3 PR 3.2 — AppProfile struct + appProfileCache_ + LookupAppProfile/
    // StoreAppProfile moved to FocusOwner alongside Classify.

    // Smart-switch capacity — runtime in-memory cap. ConfigManager's
    // disk-load uses the more generous kMaxAppListEntries (1000); the
    // overflow-vs-runtime-cap behavior is "clear and re-learn" — silent
    // data loss only if a user accumulates >200 distinct apps, which
    // hasn't happened in practice.
    static constexpr size_t kMaxSmartSwitchEntries = 200;

    // Smart-switch persistence — worker-thread-only state. No atomic needed
    // because only OnTickPoll reads/writes (single-thread invariant). Tracks
    // the prior Flush outcome so we log only on state transitions instead
    // of every 200 ms retry (see design §5).
    bool lastFlushFailed_ = false;

    // Worker-thread owned: tracks dispatched focus target to drop spurious
    // WinEvents from background tool windows whose target hasn't changed (#273).
    std::uintptr_t lastDispatchedHwnd_{0};
    std::uintptr_t lastDispatchedTargetHwnd_{0};

    // Smart-switch off→on cross-thread handoff. ReloadFromToml runs on the
    // worker thread; it can't mutate appModeMap_ directly (hook-thread-owned).
    // Pattern: worker loads from TOML, atomic-stashes here; hook's
    // ApplyConfigOnHookThread exchange-consumes + assigns into the live map +
    // publishes a fresh snapshot. nullptr = "no pending load".
    std::atomic<std::shared_ptr<const std::unordered_map<std::wstring, bool>>>
        pendingAppModeMap_;
    // Sprint 2 D5: kSynthSettleMs (was 100 ms hardcoded for all hosts) replaced
    // by per-injector budget — `dispatcher_.GetInjector()->SettleBudget()`
    // returns 0 ms for RichEdit (sent message drains synchronously), 30 ms
    // for Win32 batch, and 100 ms for Split (Electron/Console). Read inline
    // at the gate sites so a focus change (re-publishing a different
    // injector) takes effect on the next keystroke without staleness.

    // Wave 3 PR 3.4 — commit-undo state machine extracted to CommitState.
    // Backspace-into-committed-word (re-enter composition after commit + BS)
    // is preserved byte-identical; HandleCommitUndoFsm orchestrates against
    // `commitState_` instead of scattered fields. Only the aliases actually
    // referenced in HookEngine.cpp survive PR 3.5 cleanup; everything else
    // goes through `CommitState::` directly.
    using CommitEntry     = CommitState::Entry;
    static constexpr wchar_t kBackspaceMarker     = CommitState::kBackspaceMarker;
    static constexpr DWORD   kCommitUndoTimeoutMs = CommitState::kReadyTimeoutMs;
    CommitState commitState_;

    // Macro expansion
    // Sprint 1 D5.2: macroEnabled_, macroInEnglish_ migrated to std::atomic —
    // read on hook hot path (ProcessKeyDown step 2c, alpha key path,
    // TryExpandMacro). tempMacroOff_ / macroCrossCommit_ are per-word runtime
    // state on the hook thread only — no atomic needed.
    // tempOffMacroByEsc_ / escRestoreRawEnabled_ removed in v3 cleanup
    // (SkipMacro / CancelComposition triggers now live in HotkeyRegistry).
    std::atomic<bool> macroEnabled_{false};
    std::atomic<bool> macroInEnglish_{false};
    bool tempMacroOff_ = false;       // Runtime: macro disabled for current word; same-thread (hook) only
    bool macroCrossCommit_ = false;   // rawMacroBuffer_ spans multiple engine commits; same-thread (hook) only
    bool wasFirstCharAutoCapped_ = false;  // First char of current word was auto-capitalized by AutoCaps
    // (macroTable_ + spaceMacroKeys_ removed — Phase 3d. Live in
    // configSnapshot_->macroTable / ->spaceMacroKeys now.)
    std::wstring rawMacroBuffer_;

    // Macro undo on Backspace (issue #264)
    struct MacroUndoState {
        std::wstring shortcut;
        std::size_t  expansionLength{0};
        bool         hasTrailingTrigger{false};
        DWORD        timestamp{0};
        bool         active{false};
    };
    MacroUndoState macroUndoState_{};
    static constexpr DWORD kMacroUndoTimeoutMs = 4000;
    void ClearMacroUndo() noexcept;

    // Hooks
    // Wave 3 PR 3.1 — hook handles, thread, mailbox moved to HookLifecycle.
    // Wave 3 PR 3.2 — focusHook_ / minimizeHook_ moved to FocusOwner.
    // Sprint 1 D10: 200 ms focus / CJK poll moved off SetTimer onto
    // MainThreadWorker's tick branch. The body lives in OnTickPoll().

    // Wave 3 PR 3.2 — FocusOwner declared BEFORE HookLifecycle so destruction
    // order (reverse of declaration) runs ~lifecycle_ first (joins hook
    // thread), then ~focus_ (Uninstalls WinEvent hooks on main). Mirrors the
    // Stop() sequence: lifecycle_.Stop() → focus_.Uninstall().
    FocusOwner focus_;

    // Wave 3 PR 3.3 — OutputDispatcher declared AFTER focus_ (uses const
    // ref to focus_ for TryEditMessagePaste's CachedFocusedHwnd/Class
    // reads) and BEFORE lifecycle_ (so destruction order joins the hook
    // thread first, then tears down dispatch state). Stop() mirrors:
    // lifecycle_.Stop() → dispatcher_.Uninstall() → focus_.Uninstall().
    OutputDispatcher dispatcher_{focus_};

    // Wave 3 PR 3.1 — dedicated hook thread + WH_KEYBOARD_LL/WH_MOUSE_LL hook
    // handles + cross-thread mailbox moved into HookLifecycle. HookEngine
    // accesses them via `lifecycle_.ThreadId()` / `lifecycle_.IsRunning()` /
    // `lifecycle_.Mailbox()` / `lifecycle_.PostHotkey()`. Drain and
    // deferred-hotkey callbacks are registered together at lifecycle_.Start().
    HookLifecycle lifecycle_;

    // Wave 3 PR 3.6 worker latch, extended with generation evidence for
    // delayed-focus safety. A request is larger than one pointer, so doctrine
    // §12.4's immutable atomic<shared_ptr> RCU pattern keeps HWND + both
    // generations consistent without a lock or a torn two-atomic pair.
    // Writers: OnFocusChanged (WinEvent/main). Reader: worker.
    // Last-writer-wins coalescing is deliberate under focus storms.
    //
    // QuickSync side intentionally uses no separate dirty bit: the worker
    // workHandler already re-runs `SyncConfigFromSharedState` on every
    // Signal, and its epoch check observes any change the hook thread saw.
    // The Signal IS the latch.
    std::atomic<std::shared_ptr<const FocusClassifyRequest>>
        pendingClassifyRequest_;
    WorkerSignalFn workerSignalFn_;
    // Latched by the hook thread when Intent::ToggleGameMode fires; drained by
    // DrainGameModeToggleOnWorker. Plain flag, no payload — see that method.
    std::atomic<bool> pendingGameModeToggle_{false};
    /// Hook-thread producer half of the game-mode toggle. Latch + signal only.
    void RequestGameModeToggle() noexcept;

    // Delayed focus-classification ordering:
    //   latestFocusRequestSerial_ — incremented at request publication on
    //     main/worker; hook rejects results for any older foreground.
    //   physicalInputEpoch_ — hook increments once per real key-down AFTER
    //     DrainHookCommands, so the current key cannot falsely overtake a
    //     fresh result while earlier physical keys are detected precisely.
    //   deferredFocusApply_ — hook-thread-owned latest typing-context
    //     transaction retained until engine_->Count() reaches zero.
    std::atomic<std::uint64_t> latestFocusRequestSerial_{0};
    std::atomic<std::uint64_t> physicalInputEpoch_{0};
    std::shared_ptr<const FocusClassification> deferredFocusApply_;
    // Hook-thread-owned. One delayed successful transaction is allowed per
    // verified Dorion process lifetime; Alt+Tab re-entry never churns it.
    DorionHookReclaimPolicy dorionHookReclaimPolicy_;

    // Adaptive-tick state (2026-05-27, plan docs/plans/2026-05-27-
    // adaptive-tick-idle-backoff.md). Both atomics are written from multiple
    // threads (hook, main, worker) and read from worker; relaxed ordering is
    // correct because the value is a hint that resolves on the next tick if
    // a momentary stale read occurs — no synchronization-with required.
    std::atomic<std::uint64_t> lastActivityTickMs_{0};
    std::atomic<std::uint32_t> currentTickIntervalMs_{NextKey::kTickActiveMs};
    TickRetuneFn tickRetuneFn_;

    void DrainHookCommands();                                 // hook thread only
    void RouteFocusOnHookThread(std::shared_ptr<const FocusClassification> cls);
    void TryApplyDeferredFocusOnHookThread();
    void ReconcileModifierStateOnHookThread() noexcept;
    void ApplyFocusOnHookThread(std::shared_ptr<const FocusClassification> cls);
    void RefreshBrowserRouteOnHookThread(bool forceRead, bool notifyCallback);
    [[nodiscard]] bool IsEffectiveTsf() const noexcept;
    void ApplyConfigOnHookThread();
    void ApplyTickPollOnHookThread();
    void ApplyToggleVNOnHookThread();
    // Sprint 1 D11: downgraded from recursive_mutex to plain mutex. After Phase B
    // (D5–D7), all hook-read state is atomic — hook callbacks no longer acquire
    // this mutex for reads. The remaining users are main-thread / worker-thread
    // writers (ApplyConfig requires caller-held; QuickSyncFromSharedState
    // self-locks; ReloadFromToml/Toggle/SetCodeTable/CommitPending
    // lock at their public entry; OnTickPoll — formerly FocusPollTimerProc, now
    // driven by MainThreadWorker per D10 — locks for the layout check + PID
    // update phase, releases before invoking OnFocusChanged so the inner
    // QuickSync self-lock isn't recursive). Pillar #2 (Nhẹ): smaller primitive
    // when recursion is no longer required.
    mutable std::mutex stateMutex_;

    // Modifier tracking state (for double-Alt and layout change detection)
    bool modCtrlDown_ = false;
    bool modShiftDown_ = false;
    bool modAltDown_ = false;
    bool modWinDown_ = false;
    bool otherKeyPressed_ = false;
    // Latched true the moment a *second* modifier joins an existing modifier
    // hold (e.g. Shift pressed while Ctrl is down), cleared when every modifier
    // is released. Distinguishes the trailing release of a multi-modifier combo
    // from a genuine single-modifier tap, so releasing Ctrl after a Ctrl+Shift
    // gesture does NOT fire a modifier-alone intent. Fixes #189: Ctrl+Shift
    // mode-toggle left the trailing Ctrl release toggling tempEngineOff_ (the
    // default single-Ctrl toggle-enabled binding), mangling the next word.
    bool modComboSeen_ = false;

    // Wave 3 PR 3.2 — CJK layout state (layoutSuppressed_/modeBeforeCjk_/
    // cachedIsCompatLayout_) and focused-child cache (cachedFocusedHwnd_/
    // cachedFocusedClass_) moved to FocusOwner. Readers go through
    // focus_.LayoutSuppressed()/ModeBeforeCjk()/CachedIsCompatLayout()/
    // CachedFocusedHwnd()/CachedFocusedClass().

    // Direct SharedState reader — pointer to the global SharedStateManager (same process)
    SharedStateManager* sharedStatePtr_ = nullptr;
    // Wave 2 (2026-05-23) — atomized so QuickSync slow path + OnTickPoll TOML
    // drain no longer need stateMutex_ to serialize these fields with writers.
    // Reader/writer pairs are all .load(acquire) / .store(release).
    std::atomic<uint32_t> lastFeatureFlags_{0};
    std::atomic<uint8_t> lastSpellCheck_{0};
    std::atomic<uint8_t> lastInputMethod_{0};
    std::atomic<uint8_t> lastCodeTable_{0};
    void QuickSyncFromSharedState();
    void ReloadFromToml();  // Full TOML reload (macros, excluded apps, hotkeys, etc.)
    // Pre-T3 Minor 2 fix (Rule #11.3): atomic for lock-free hot-path read
    // in QuickSyncFromSharedState. Writer (slow path inside stateMutex_):
    // release-store after applying SharedState. Reader (lock-free fast path
    // on hook thread): acquire-load + epoch compare; equal → early-return
    // without ever taking stateMutex_. Initialised to 0 so the first call
    // always enters the slow path (any valid SharedState epoch mismatches).
    std::atomic<uint32_t> lastEpoch_{0};  // Epoch fast path — skip full Read() when unchanged
    std::atomic<uint32_t> lastFlags_{0};  // Tracks SharedState.flags for TSF_TIP_ACTIVE transitions (tray icon sync)
    std::atomic<uint8_t> lastConfigGeneration_{0};   // Tracks configGeneration from SharedState (Wave 2 atomized)
    // Phase 3c: cross-thread signal from hook slow path to worker tick.
    // When `QuickSyncFromSharedState` is entered on the hook thread and
    // detects a `configGeneration` bump, it MUST NOT run `ReloadFromToml`
    // (Rule 11.2 — TOML parse on hook). Instead, it sets this flag; the
    // next `OnTickPoll` (worker thread, 200ms cadence) drains it and
    // runs `ReloadFromToml` off-hook. Worker / main entries to QuickSync
    // still run Reload inline — they're already on a safe thread.
    std::atomic<bool> pendingConfigReload_{false};
    // Phase 3f: hook-thread latch — set when ApplyConfigOnHookThread can't
    // run yet because engine_->Count() > 0 (user mid-word). Drain checks
    // this every cycle and runs the apply once the engine empties (after
    // commit / backspace-clear / focus reset). Without this, config
    // reloads landing mid-word committed partial words via the engine
    // recreate path (surfaced by chaos `-InjectConfigReloadMs 50` as
    // `uongs` → `uôngs` instead of `uống`).
    std::atomic<bool> deferredConfigApply_{false};

    // External managers
    HotkeyManager* hotkeyManager_ = nullptr;

    // Callbacks
    ModeChangeCallback modeChangeCallback_;
    std::function<void()> configReloadCallback_;
    TsfModeCallback tsfModeCallback_;
    FocusAppContextCallback focusAppContextCallback_;
    HotkeyChangedCallback hotkeyChangedCallback_;

    // Wave 3 PR 3.8 — cached SharedState toggle-hotkey value. Seeded in
    // Start() from the initial SharedState read so the first QuickSync
    // slow body doesn't fire a spurious callback. Subsequently written
    // ONLY by `QuickSyncFromSharedState`'s CAS-claimed slow body (single
    // writer at a time per Wave 2 CAS lastEpoch_ contract), so plain
    // storage is safe. Read in the same body to compare with the freshly-
    // observed `state.GetHotkey()`.
    HotkeyConfig lastToggleHotkey_{};

    // Singleton for static callback dispatch (read from hook callback thread)
    static std::atomic<HookEngine*> s_instance;

    // Wave 2 — feature pipeline. Coordinator owns registered gates/features
    // (one of each in W2: EnglishBiasGate + BackwardEditFeature). outputChannel_
    // is the IntentSink for emitted intents — kept as a value member so the
    // batch buffer survives across keystrokes (TakeBatch drains it between
    // calls). Both are constructed in the HookEngine ctor body after the
    // injector is seeded; features are registered there with `*this` as the
    // IBackwardEditExecutor backing.
    NextKey::Pipeline::Coordinator   coordinator_;
    NextKey::Pipeline::OutputChannel outputChannel_;
};

}  // namespace NextKey
