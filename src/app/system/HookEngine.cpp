// VKey - Keyboard Hook Engine Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "HookEngine.h"
#include "HotkeyManager.h"  // passive application-hotkey matcher
#include "Win32CaseMapper.h"
#include "PerfHistogram.h"  // Phase 1 — per-stage histogram (compiles to no-op when VKEY_PERF_HIST undef)
#include "helpers/AppHelpers.h"
#include "output/Internal.h"  // Sprint 2 D5 — g_synthCounterCallback bridge
#include "core/engine/CodeTableConverter.h"
#include "core/engine/CommittedTextRestore.h"
#include "core/engine/EngineFactory.h"
#ifdef VKEY_USE_RUST_ENGINE
#include "core/engine/RustInputEngine.h"
#endif
#include "core/config/ConfigManager.h"
#include "core/config/ConfigSnapshotBuilder.h"
#include "core/AutoCapDecision.h"
#include "core/CjkSwitchDecision.h"
#include "core/CommitUndoExemption.h"
#include "core/CommitUndoArmDecision.h"
#include "core/DigitLedWordDecision.h"
#include "core/DorionHookReclaimSequence.h"
#include "core/ExternalInjectedKeyDecision.h"
#include "core/FocusApplyDecision.h"
#include "core/LeakedKeyDuringSendDecision.h"
#include "core/MacroCase.h"
#include "core/MacroPrefix.h"
#include "core/QuickConvertLogic.h"
#include "core/ipc/SharedStateManager.h"
#include "core/Debug.h"
#include "core/CrashLog.h"
#include "core/pipeline/BackwardEditFeature.h"
#include "core/pipeline/CommitUndoFeature.h"
#include "core/pipeline/EscRestoreRawFeature.h"
#include "core/pipeline/MacroFeature.h"
#include "core/pipeline/HookCompositionSession.h"
#include "core/pipeline/Intent.h"
#include "core/pipeline/KeyContext.h"
#include "core/pipeline/gates/EnglishBiasGate.h"
#include "core/pipeline/gates/SpellCheckGate.h"
#include "core/pipeline/gates/ToneEscapeGate.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <exception>
#include <tlhelp32.h>
#include <vector>

namespace NextKey {

// ═══════════════════════════════════════════════════════════
// VKEY_ASSERT_HOOK_THREAD — Phase 2d single-writer invariant.
//
// Composition-state mutation entry points (ResetComposition,
// CommitComposition, ClearWordState, ReplaceComposition,
// ReplayCommittedChars, HandleAlphaKey, HandleBackspace,
// ApplyConfigOnHookThread) MUST run on the hook thread. The macro is
// debug-only (NDEBUG elides it) — Release builds pay nothing. Pre-Start
// is a free pass: hookThreadId_ is 0 until HookThreadProc claims it,
// and Start() legitimately runs composition setup on main before the
// hook thread spawns.
// ═══════════════════════════════════════════════════════════
#ifdef NDEBUG
  #define VKEY_ASSERT_HOOK_THREAD() ((void)0)
#else
  #define VKEY_ASSERT_HOOK_THREAD()                                            \
      do {                                                                     \
          const DWORD _expected = lifecycle_.ThreadId();                        \
          if (_expected != 0) {                                                 \
              const DWORD _current = GetCurrentThreadId();                      \
              if (_current != _expected) {                                      \
                  HOOK_LOG(L"VKEY_ASSERT_HOOK_THREAD violated: tid=%lu, expected hook tid=%lu — %hs", \
                           _current, _expected, __func__);                      \
                  assert(_current == _expected &&                               \
                         "composition-state mutation must run on the hook thread"); \
              }                                                                 \
          }                                                                     \
      } while (0)
#endif

// ═══════════════════════════════════════════════════════════
// HOOK_LOG → unified runtime-gated Logger (core/Logger.h).
// Enable from Settings → System → "Bật debug log". Output file is shared
// with NEXTKEY_LOG / TSF_LOG: VKey_<process>_<pid>.log next to
// VKeyApp.exe (falls back to %APPDATA%\VKey\logs\ if install dir is
// read-only). Flushing/closing is owned by the Logger (DLL detach + EXE
// process exit) — hook Start/Stop does NOT toggle the logger lifecycle.
// ═══════════════════════════════════════════════════════════
#define HOOK_LOG(fmt, ...) do {                                              \
    if (::NextKey::Logger::IsEnabled())                                      \
        ::NextKey::Logger::Log(L"[Hook] " fmt, ##__VA_ARGS__);               \
} while (0)

std::atomic<HookEngine*> HookEngine::s_instance{nullptr};

HookEngine::HookEngine() {
    // Wave 3 PR 3.3 — default injector seed moved into OutputDispatcher
    // ctor (dispatcher_'s own NSDMI handles the empty-classification
    // factory call). The hook hot path's `dispatcher_.GetInjector()` is
    // safe to call before any focus event has fired.

    // Wave 2: register the feature pipeline. vietnameseMode_ already has its
    // in-class initializer (true), so the gate's stored reference is live as
    // soon as this ctor body runs. *this is passed as IBackwardEditExecutor;
    // BackwardEditFeature only stores the reference and calls ExecuteReplace
    // later (per-keystroke), never during construction — safe even though the
    // derived HookEngine is still mid-construction here.
    coordinator_.RegisterGate(
        std::make_unique<NextKey::Pipeline::EnglishBiasGate>(vietnameseMode_));  // audit-allow: gate stores const ref, IsRaised() uses .load(acquire)
    // Wave 5: SpellCheckGate raised when engine_->IsEnglishWord() (3-tier
    // English protection bias == HardEnglish). ToneEscapeGate raised when
    // any EscapeKind is active. Both hold ref to the engine_ unique_ptr —
    // safe because the ref stays valid across config reloads; only the
    // pointee swaps. Gate dereferences on every IsRaised call.
    coordinator_.RegisterGate(
        std::make_unique<NextKey::Pipeline::SpellCheckGate>(engine_));
    coordinator_.RegisterGate(
        std::make_unique<NextKey::Pipeline::ToneEscapeGate>(engine_));
    coordinator_.Register(
        std::make_unique<NextKey::Pipeline::BackwardEditFeature>(*this));
    // Wave 3: CommitUndoFeature owns step 2d FSM dispatch. Stage::PreEngine
    // prio 20. *this is the ICommitUndoExecutor backing — feature delegates
    // synchronously to HandleCommitUndo(vk) which adapts to HandleCommitUndoFsm.
    coordinator_.Register(
        std::make_unique<NextKey::Pipeline::CommitUndoFeature>(*this));
    // Wave 4a: EscRestoreRawFeature handles hotkey-triggered ESC (or any
    // CancelComposition trigger) at PreEngine prio 40 — fires AFTER
    // CommitUndoFeature so the FSM's ESC-exemption check runs first.
    coordinator_.Register(
        std::make_unique<NextKey::Pipeline::EscRestoreRawFeature>(*this));
    // Wave 4b: MacroFeature owns macro tracking + expansion at PreEngine
    // prio 30 (between CommitUndo 20 and EscRestoreRaw 40). Adapter contains
    // EN-mode + VN-mode macro logic transcribed from HandlePreDispatch.
    coordinator_.Register(
        std::make_unique<NextKey::Pipeline::MacroFeature>(*this));
}

HookEngine::~HookEngine() {
    Stop();
}

void HookEngine::SetHotkeyManager(HotkeyManager* manager) noexcept {
    hotkeyManager_ = manager;
    if (hotkeyManager_) {
        hotkeyManager_->SetDeferredFireSink(&HookEngine::PostMatchedHotkey, this);
    }
}

void HookEngine::PostMatchedHotkey(void* context, std::size_t slot) noexcept {
    static_cast<HookEngine*>(context)->lifecycle_.PostHotkey(slot);
}

void HookEngine::CommitPending() {
    std::lock_guard<std::mutex> _lock(stateMutex_);
    if (engine_ && engine_->Count() > 0) {
        CommitComposition();
    }
}

// Wave 2 (2026-05-23): lock-free. Formerly required caller-held stateMutex_
// (Sprint 1 D11 contract) because it wrote plain-bool cache fields. Those
// fields are gone; remaining work is atomic stores + RCU-published settings
// (injector_, Logger). Safe to call from any thread.
void HookEngine::ApplyConfig(const TypingConfig& config) {
    tsfFeatureEnabled_.store(config.tsfApps, std::memory_order_release);
    autoCaps_.store(config.autoCaps, std::memory_order_release);
    macroEnabled_.store(config.macroEnabled, std::memory_order_release);
    macroInEnglish_.store(config.macroInEnglish, std::memory_order_release);
    // Push the suggestKeepChars flag to the live injector so ShouldEmitBait
    // sees the latest user choice without needing a focus change to swap
    // injectors. Focus-change paths re-apply this from the config snapshot.
    if (auto inj = dispatcher_.GetInjector(); inj) {
        inj->SetSuggestKeepChars(config.suggestKeepChars);
    }
    // Runtime file-logger gate (Settings → System → "Bật debug log").
    ::NextKey::Logger::SetEnabled(config.debugLogEnabled);
}

void HookEngine::ApplyHotkeyRegistry(HotkeyRegistry registry) {
    // RCU publish — readers (hook hot path) pick up on next load(). Old
    // registry stays alive until any in-flight Matches() returns.
    hotkeys_.store(std::make_shared<const HotkeyRegistry>(std::move(registry)),
                   std::memory_order_release);
}

namespace {

// Map L/R modifier VK variants down to their canonical form so HotkeyRegistry
// triggers (stored as canonical VK_CONTROL/VK_MENU/VK_SHIFT/VK_LWIN) match
// hook events (which report VK_LCONTROL/VK_RCONTROL etc.).
[[nodiscard]] uint32_t CanonicalModifierVk(DWORD vk) noexcept {
    switch (vk) {
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU:    case VK_RMENU:    return VK_MENU;
    case VK_LSHIFT:   case VK_RSHIFT:   return VK_SHIFT;
    case VK_RWIN:                       return VK_LWIN;  // collapse to one Win
    default:                            return vk;
    }
}

// Pack the cached modifier booleans into a HotkeyRegistry-style bitmask.
[[nodiscard]] uint32_t ComputeModMask(bool ctrl, bool shift, bool alt, bool win) noexcept {
    uint32_t mask = 0;
    if (ctrl)  mask |= kModCtrl;
    if (shift) mask |= kModShift;
    if (alt)   mask |= kModAlt;
    if (win)   mask |= kModWin;
    return mask;
}

// Canonical-VK → modTapCount_[] / modTapLastTs_[] index. Returns -1 for any
// VK that isn't one of the 4 modifiers we track. Keeps the slots stable so
// the array can be a flat fixed-size buffer.
[[nodiscard]] int ModIdxFor(uint32_t canonicalVk) noexcept {
    switch (canonicalVk) {
    case VK_CONTROL: return 0;
    case VK_SHIFT:   return 1;
    case VK_MENU:    return 2;
    case VK_LWIN:    return 3;
    default:         return -1;
    }
}

}  // namespace

bool HookEngine::Start(HINSTANCE hInstance, const TypingConfig& config,
                        bool initialVietnamese) {
    // Pump ownership survives a runtime replacement whose new hook fails to
    // install. Reject a repeated Start before rebuilding/finalizing any state;
    // assigning over that still-joinable lifecycle would terminate.
    if (lifecycle_.HasOwnerThread()) return false;
    dorionHookReclaimPolicy_.Reset();

    // Enable the file logger before the first HOOK_LOG so the start banner is
    // captured when the user already had the toggle on. ApplyConfig() re-asserts
    // this below for subsequent config reloads.
    ::NextKey::Logger::SetEnabled(config.debugLogEnabled);
    HOOK_LOG(L"=== HookEngine::Start ===");

    s_instance = this;
    // Wave 3 PR 3.3 — OutputDispatcher owns the synth-counter wiring.
    // Install sets dispatcher's s_instance + g_synthCounterCallback in
    // one step. Idempotent. Wired AFTER HookEngine::s_instance so a hook
    // thread already racing wouldn't see a half-initialized dispatcher
    // (we're still on main here; hook thread starts further down).
    dispatcher_.Install();
    currentMethod_.store(config.inputMethod, std::memory_order_release);
    config_.store(std::make_shared<const TypingConfig>(config), std::memory_order_release);
    // Wave 2 (2026-05-23): ApplyConfig is now lock-free (all writes are atomic
    // or RCU-publish). Sprint 1 D11's "caller holds stateMutex_" contract
    // dropped. Single-threaded init here; ApplyConfig safe to call directly.
    ApplyConfig(config);
    if (!browserContext_.Create()) {
        HOOK_LOG(L"BrowserContext: shared mapping unavailable; extension routing disabled");
    }
    // Load unified hotkey registry. On first launch after v3 upgrade, the
    // `[[hotkeys]]` section is missing — migrate reads legacy `[features]`
    // toggles directly from TOML and persists `[hotkey_state]` so future
    // launches read the new schema directly.
    ApplyHotkeyRegistry(ConfigManager::MigrateLegacyHotkeysIfNeeded(
        ConfigManager::GetConfigPath()));
    autoCapState_ = AutoCapState::Idle;
    lastAppliedHwnd_ = 0;
    lastAppliedEffectiveFocusHwnd_ = 0;
    lastDispatchedHwnd_ = 0;
    lastDispatchedTargetHwnd_ = 0;
#ifdef VKEY_USE_RUST_ENGINE
    if (config.spellSuggestEnabled) {
        auto loaded = RustInputEngine::LoadUserDictionary(ConfigManager::GetConfigPath());
        if (loaded.Succeeded()) {
            userDictionary_ = std::move(loaded.snapshot);
            HOOK_LOG(L"  UserDictionary: %s '%s'",
                     loaded.created ? L"created and loaded" : L"loaded",
                     loaded.path.c_str());
        } else {
            HOOK_LOG(L"  UserDictionary: load failed status=%d engineStatus=%u line=%zu path='%s'",
                     static_cast<int>(loaded.status), loaded.engineStatus,
                     loaded.errorLine, loaded.path.c_str());
        }
    }
#endif
    engine_ = EngineFactory::Create(config);
#ifdef VKEY_USE_RUST_ENGINE
    if (userDictionary_ && EngineFactory::WillUseRustEngine(config)) {
        const bool attached = static_cast<RustInputEngine*>(engine_.get())
                                  ->SetUserDictionary(userDictionary_);
        HOOK_LOG(L"  UserDictionary: initial attach %s", attached ? L"ok" : L"failed");
    }
#endif
    vietnameseMode_.store(initialVietnamese, std::memory_order_release);

    // Create shared memory for smart switch and load persisted English-mode apps.
    // Bug C fix (2026-05-26): persistence used to be gated on startupMode_==2
    // (Remember) — but smart_switch is conceptually independent of the initial
    // mode choice. With smart_switch on, the user expects per-app state to
    // survive restarts regardless of whether startup begins in V/E/Remember.
    if (config.smartSwitch) {
        (void)focus_.Smart().Create();
        // V2 schema (with legacy [smart_switch].english_mode_apps fallback
        // for one-time silent migration). See
        // docs/plans/2026-05-28-smart-switch-persistence-design.md §2.
        auto persisted = ConfigManager::LoadSmartSwitchApps(ConfigManager::GetConfigPath());
        focus_.AppModeMap() = std::move(persisted);
        if (!focus_.AppModeMap().empty()) {
            focus_.Smart().LoadFromMap(focus_.AppModeMap());
        }
        // Publish initial snapshot so the worker has something to flush
        // if dirty fires before the first map mutation.
        focus_.PublishAppModesSnapshot();
    }

    currentCodeTable_.store(config.codeTable, std::memory_order_release);
    globalCodeTable_.store(config.codeTable, std::memory_order_release);
    globalInputMethod_.store(config.inputMethod, std::memory_order_release);

    // Cache initial SharedState values (pointer set by main.cpp via SetSharedStateReader)
    if (sharedStatePtr_) {
        SharedState state = sharedStatePtr_->Read();
        if (state.IsValid()) {
            lastFeatureFlags_.store(state.GetFeatureFlags(), std::memory_order_release);
            lastSpellCheck_.store(state.spellCheck, std::memory_order_release);
            lastInputMethod_.store(state.inputMethod, std::memory_order_release);
            lastCodeTable_.store(state.codeTable, std::memory_order_release);
            lastConfigGeneration_.store(state.configGeneration, std::memory_order_release);
            // Wave 3 PR 3.8 — seed the toggle-hotkey cache so the first
            // QuickSync slow body doesn't fire a spurious callback. The
            // initial HotkeyManager binding came from TOML via WireHotkeys
            // at startup; SharedState's hotkey field matches that on a
            // clean run (Settings dialog writes both paths in sync).
            lastToggleHotkey_ = state.GetHotkey();
        }
    }

    // Phase 3d — single rebuild: TOML parse for overrides/excluded/TSF/
    // macros + atomic snapshot publish. Replaces the four legacy
    // Reload* + PublishConfigSnapshot calls from earlier.
    RebuildSnapshotFromToml(
        static_cast<std::uint32_t>(lastConfigGeneration_.load(std::memory_order_acquire)));

    // Seal the startup-only matcher topology before the lifecycle installs
    // the sole keyboard hook. Runtime rebinding only updates the manager's
    // lock-free packed configurations.
    if (hotkeyManager_) hotkeyManager_->FinalizeBindings();

    // Wave 3 PR 3.1: HookLifecycle owns the dedicated hook thread + LL hooks
    // + mailbox. We pass our LL callbacks (still HookEngine statics via
    // s_instance), a command-drain callback, and pump-owned hotkey dispatch.
    if (!lifecycle_.Start(hInstance, LowLevelKeyboardProc, LowLevelMouseProc,
                          [this] { DrainHookCommands(); },
                          [this](std::size_t slot) {
                              if (hotkeyManager_) hotkeyManager_->Dispatch(slot);
                          },
                          [this] {
                              LASTINPUTINFO lastInput{
                                  .cbSize = sizeof(LASTINPUTINFO),
                              };
                              const bool lastInputQuerySucceeded =
                                  GetLastInputInfo(&lastInput) != FALSE;
                              bool anyPhysicalKeyDown = false;
                              for (int vk = 1; vk < 256; ++vk) {
                                  if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
                                      anyPhysicalKeyDown = true;
                                      break;
                                  }
                              }
                              return IsDorionReclaimQuiet({
                                  .lastInputQuerySucceeded =
                                      lastInputQuerySucceeded,
                                  .nowTickMs = GetTickCount(),
                                  .lastInputTickMs = lastInput.dwTime,
                                  .anyPhysicalKeyDown = anyPhysicalKeyDown,
                                  .syntheticDispatchActive =
                                      dispatcher_.IsSending()
                                      || dispatcher_.SynthEventsPending() > 0,
                                  .compositionActive =
                                      (engine_ && engine_->Count() > 0)
                                      || !previousComposition_.empty()
                                      || !rawMacroBuffer_.empty(),
                              });
                          },
                          [this](DorionProcessIdentity identity,
                                 DorionKeyboardReclaimResult result) {
                              const bool replaced =
                                  result == DorionKeyboardReclaimResult::Replaced;
                              // Record the terminal result before state repair;
                              // an unexpected repair exception must not strand
                              // this identity in the pending set.
                              dorionHookReclaimPolicy_.Complete(
                                  identity, replaced);
                              if (replaced) {
                                  // The gate proved the editing state empty;
                                  // reset remaining FSM/latches before the new
                                  // hook begins the next physical gesture.
                                  ResetComposition();
                                  ReconcileModifierStateOnHookThread();
                                  if (hotkeyManager_) {
                                      hotkeyManager_->ReconcileLatchedKeysAfterHookReplacement(
                                          [](const void*, std::uint32_t vk) noexcept {
                                              return (GetAsyncKeyState(
                                                          static_cast<int>(vk))
                                                      & 0x8000) != 0;
                                          },
                                          nullptr);
                                  }
                              }
                          })) {
        HOOK_LOG(L"FAILED to install keyboard hook (lifecycle Start returned false)");
        return false;
    }

    // Wave 3 PR 3.2: WinEvent hook lifecycle moved into FocusOwner.
    if (!focus_.Install([this](HWND hwnd) { OnFocusChanged(hwnd); })) {
        HOOK_LOG(L"FAILED to install WinEvent hook");
        // Don't return false — focus events are nice-to-have; LL hook still works.
    }

    // Sprint 1 D10: 200 ms focus / CJK poll is no longer driven by SetTimer.
    // The owning EXE wires MainThreadWorker::SetTickHandler([](){ OnTickPoll(); })
    // and SetTickInterval(200ms); Start does not own the cadence anymore.

    // Phase 1 perf histogram (docs/plans/2026-05-19-architecture-review-design.md).
    // Path: %APPDATA%\VKey\perf-histogram-<pid>-<startTs>.log. The Enabled() gate
    // is sourced from SharedState.diagFlags inside QuickSyncFromSharedState; we
    // seed it here from the TOML toggle so the very first keystroke is captured.
    {
        wchar_t pathBuf[MAX_PATH];
        const DWORD startTs = GetTickCount();
        const DWORD pid = GetCurrentProcessId();
        const std::wstring base = ConfigManager::GetAppDataDirectory();
        const int n = swprintf_s(pathBuf, MAX_PATH,
            L"%ls\\perf-histogram-%lu-%lu.log", base.c_str(), pid, startTs);
        if (n > 0) {
            Perf::Histogram::SetLogPath(pathBuf);
        }
        Perf::Histogram::SetEnabled(config.perfHistogramEnabled);
    }

    // Adaptive-tick (#2): seed the activity clock to LAUNCH time. lastActivityTickMs_
    // is otherwise 0, so the FIRST worker tick computes idleMs = system uptime
    // ≥ kIdleStopThreshMs and STOPS the worker ~200ms after launch — killing the
    // CJK/Win+Space poll, the foreground-PID fallback, the config-reload drain
    // and smart-switch persistence until the first keystroke re-arms it. Seeding
    // here makes "idle" measure from launch, not boot. See AdaptiveTick.h.
    lastActivityTickMs_.store(GetTickCount64(), std::memory_order_relaxed);

    NEXTKEY_LOG(L"HookEngine started (method=%d, vietnamese=%d)",
                static_cast<int>(currentMethod_.load(std::memory_order_acquire)),
                vietnameseMode_.load(std::memory_order_acquire));
    HOOK_LOG(L"Hook installed OK (method=%d, vietnamese=%d)",
             static_cast<int>(currentMethod_.load(std::memory_order_acquire)),
             vietnameseMode_.load(std::memory_order_acquire));
    return true;
}

void HookEngine::Stop() {
    HOOK_LOG(L"=== HookEngine::Stop ===");
    // Phase 1 perf histogram: final flush before we tear down so the
    // last 60s window of samples reaches disk. Idempotent.
    Perf::Histogram::Stop();
    // Wave 3 PR 3.1: HookLifecycle handles thread shutdown + LL hook teardown
    // (unhook MUST happen on the installer thread per MSDN — lifecycle owns
    // that thread). Wave 3 PR 3.2: WinEvent hooks moved to FocusOwner.
    // Wave 3 PR 3.3: dispatcher owns synth-counter wiring.
    // Order: join hook thread → tear down dispatch (no in-flight SendInput
    // can race once the thread is gone) → tear down focus state. Mirrors
    // reverse-declaration destruction order (lifecycle → dispatcher → focus).
    lifecycle_.Stop();
    dorionHookReclaimPolicy_.Reset();
    dispatcher_.Uninstall();
    focus_.Uninstall();

    // Smart-switch force-flush — MUST run after lifecycle_.Stop() so the
    // hook thread is joined and reading focus_.AppModeMap() directly is
    // safe (no concurrent mutation possible). Bug 3 fix (2026-05-28):
    // pre-fix code ran the legacy save BEFORE lifecycle_.Stop() →
    // cross-thread live-map read race. See design §1 + §3.
    FlushSmartSwitchOnStop();

    // Sprint 1 D10: focusPollTimer_ retired — owner stops its
    // MainThreadWorker (which owns the 200 ms tick) before us.
    if (s_instance == this) {
        s_instance = nullptr;
    }
    focus_.ResetLayoutState();
    lastAppliedHwnd_ = 0;
    lastAppliedEffectiveFocusHwnd_ = 0;
    lastDispatchedHwnd_ = 0;
    lastDispatchedTargetHwnd_ = 0;
    NEXTKEY_LOG(L"HookEngine stopped");
}

// Wave 3 PR 3.1 (2026-05-23) — HookThreadProc body moved to
// HookLifecycle::ThreadProc. HookLifecycle invokes our DrainHookCommands via
// a callback registered at lifecycle_.Start().

void HookEngine::ToggleVietnameseMode() noexcept {
    // Phase 2c/First-word fix: immediately toggle the SharedState flag
    // so that the TSF DLL in target applications sees the new mode instantly,
    // avoiding the race condition where the first keystroke of the word is typed
    // as English.
    //
    // #221: capture the definitive post-toggle value from ToggleFlag's return
    // (not a separate re-read) and stash it in pendingToggleMode_ for the
    // drain. A re-read at drain time could observe a value some intervening
    // NotifyModeChange() already clobbered back to the pre-toggle state,
    // silently no-op'ing the user's toggle.
    if (sharedStatePtr_) {
        const uint32_t newFlags = sharedStatePtr_->ToggleFlag(SharedFlags::VIETNAMESE_MODE);
        pendingToggleMode_.store((newFlags & SharedFlags::VIETNAMESE_MODE) ? 1 : 0,
                                  std::memory_order_release);
    }

    // Phase 2c: ToggleVietnameseMode is called from any thread (tray menu
    // on main, hotkey on either main or the hook pump itself when fired
    // via HotkeyRegistry, modifier-only double-tap). All composition-state
    // writes (CommitComposition, vietnameseMode_, appModeMap_, etc.) must
    // happen on the hook thread (Rule 11.3 single-writer). Post the bit
    // and let the drain do the work.
    lifecycle_.Mailbox().Post(HookCommand::kToggleVN);
}

void HookEngine::SetCodeTable(CodeTable ct) {
    std::lock_guard<std::mutex> _lock(stateMutex_);
    // Commit any pending composition before switching
    if (ct != currentCodeTable_.load(std::memory_order_acquire) && engine_->Count() > 0) {
        CommitComposition(true);
    }

    currentCodeTable_.store(ct, std::memory_order_release);

}

CodeTable HookEngine::GetCodeTable() const noexcept {
    // Priority 1: Manual per-app override (set explicitly by user)
    // Check previousExe_ first as a fallback: on the first focus event after startup,
    // activeExe_ may not yet reflect the typing app.
    // Phase 3c: read from RCU snapshot — lock-free, safe on any thread.
    auto snap = configSnapshot_.load(std::memory_order_acquire);
    auto lookupOverride = [&](const std::wstring& exe) -> const CodeTable* {
        if (exe.empty() || !snap) return nullptr;
        auto it = snap->appEncodingOverrides.find(exe);
        return (it != snap->appEncodingOverrides.end()) ? &it->second : nullptr;
    };
    if (auto* v = lookupOverride(focus_.PreviousExe())) return *v;
    if (auto* v = lookupOverride(focus_.ActiveExe()))   return *v;

    return currentCodeTable_.load(std::memory_order_acquire);
}

bool HookEngine::ShouldUseNativeQuickConvert() const noexcept {
    const bool isTsfApp = IsEffectiveTsf();
    const uint32_t flags = sharedStatePtr_ ? sharedStatePtr_->ReadFlags() : 0;
    const uint32_t ownerPid = sharedStatePtr_
        ? sharedStatePtr_->ReadNativeConvertOwnerProcessId()
        : 0;
    return DecideQuickConvertBackend(
        isTsfApp,
        (flags & SharedFlags::TSF_NATIVE_CONVERT_READY) != 0
            && ownerPid != 0
            && ownerPid == focus_.LastForegroundPid())
        == QuickConvertBackend::TsfNative;
}

void HookEngine::QuickSyncFromSharedState() {
    // Pre-T3 Minor 2 fix (Rule #11.3): hot path is lock-free. The common
    // case — no SharedState change since the last call — returns before
    // any atomic ops, eliminating the per-keystroke contention with
    // main-thread writers (ToggleVietnameseMode, SetCodeTable, …) that
    // showed up as p99 jitter under chaos.
    //
    // Wave 2 (2026-05-23): slow path is now ALSO lock-free. Pre-Wave-2 the
    // slow-path body ran under stateMutex_ to serialise concurrent QuickSync
    // callers. Post-Wave-2 the body uses a CAS on lastEpoch_ to claim
    // exclusive processing of each SharedState epoch transition — at most
    // one caller's full apply-and-publish runs per epoch claim. Concurrent
    // callers whose CAS fails return early; newer epochs they observed are
    // re-processed on the next QuickSync call (bounded recovery ≤200 ms via
    // worker tick or sooner via keystroke). The mutex is preserved on
    // CommitPending / SetCodeTable for engine-state mutation; QuickSync no
    // longer touches it.
    if (!sharedStatePtr_) return;

    // Fast path: lock-free atomic epoch check. SharedState::ReadEpoch is
    // a memory-mapped 32-bit seqlock counter; lastEpoch_ is std::atomic.
    // Common case under steady-state typing: epoch unchanged → return
    // without any stateMutex_ acquire. Cost: ~5 ns total.
    uint32_t epoch = sharedStatePtr_->ReadEpoch();
    uint32_t seenEpoch = lastEpoch_.load(std::memory_order_acquire);
    if (epoch == seenEpoch && (epoch & 1) == 0) return;

    // Wave 3 PR 3.6 — Rule 11.2 + doctrine §12.4 (worker-thread doctrine):
    // hook thread MUST NOT execute the slow body. Two heap-allocating ops
    // live below — `std::make_shared<TypingConfig>` (line ~518) and the
    // `ReloadFromToml` branch behind the configGeneration check — and
    // running either from `LowLevelKeyboardProc` violates Rule 11.2's
    // "no malloc on hook" ceiling. Signal the worker thread so it re-runs
    // QuickSync on its own thread; intentionally do NOT advance lastEpoch_
    // so the worker still observes the change.
    //
    // Coalescing property (§12.4): a burst of SharedState changes between
    // worker wakes collapses into one drain. The slow body sees the latest
    // state on its single execution.
    if (const DWORD _hookTid = lifecycle_.ThreadId();
        _hookTid != 0 && GetCurrentThreadId() == _hookTid) {
        if (workerSignalFn_) workerSignalFn_();
        return;
    }

    // Slow path. Wave 2 (2026-05-23) — stateMutex_ DROPPED. The full body
    // (last* updates, config_ publish, ApplyConfig) was serialised by the
    // mutex pre-Wave-2; now it's serialised by a CAS on lastEpoch_ that
    // atomically claims each epoch transition. At most one caller's CAS
    // succeeds per (seenEpoch → state.epoch) edge; losers return without
    // publishing. This eliminates the lost-update race possible if both
    // callers raced their unconditional .store + RCU publishes (older
    // store could land last, overwriting newer published config).
    //
    // Trade-off: a CAS loser that observed a NEWER state.epoch than the
    // winner is dropped — but recovery is bounded ≤200 ms because the
    // next QuickSync caller (worker tick, hook keystroke, main public-API)
    // observes lastEpoch < SharedState.epoch and claims the missed epoch.
    epoch = sharedStatePtr_->ReadEpoch();
    seenEpoch = lastEpoch_.load(std::memory_order_acquire);
    if (epoch == seenEpoch && (epoch & 1) == 0) return;

    SharedState state = sharedStatePtr_->Read();
    if (!state.IsValid()) return;

    // CAS claim — exclusive entry to the slow-path body for this epoch
    // transition. If `seenEpoch` is stale (another caller already claimed),
    // the CAS fails and we return. The expected-value contract on
    // compare_exchange_strong overwrites `seenEpoch` with the observed
    // value on failure; we don't use it after, so the side-effect is
    // harmless.
    if (!lastEpoch_.compare_exchange_strong(seenEpoch, state.epoch,
            std::memory_order_release, std::memory_order_acquire)) {
        return;
    }

    // Phase 1: surface SharedState.diagFlags bit 0 into the perf histogram
    // gate. SetEnabled is lock-free.
    Perf::Histogram::SetEnabled((state.diagFlags & DiagFlags::PERF_HISTOGRAM) != 0);

    // ── Config generation check: detect TOML changes from Settings/subdialogs ──
    // When configGeneration changes, do a full TOML reload (macros, excluded apps, etc.).
    // Replaces the old ConfigEvent (Named Event + WaitForSingleObject syscall).
    //
    // Phase 3c thread-aware routing:
    //   • Hook thread → defer to worker (Rule 11.2 — TOML parse is forbidden
    //     here, ~1-10 ms). Set `pendingConfigReload_`; the next OnTickPoll
    //     drains it and runs ReloadFromToml on the worker thread.
    //   • Worker / main → run inline. Already on a thread where TOML parse
    //     is acceptable, no point bouncing through another tick.
    if (state.configGeneration != lastConfigGeneration_.load(std::memory_order_acquire)) {
        if (const DWORD _hookTid = lifecycle_.ThreadId(); _hookTid != 0 && GetCurrentThreadId() == _hookTid) {
            pendingConfigReload_.store(true, std::memory_order_release);
            NEXTKEY_LOG(L"HookEngine: configGeneration bump (%u) seen on hook — deferring Reload to worker tick",
                        state.configGeneration);
        } else {
            lastConfigGeneration_.store(state.configGeneration, std::memory_order_release);
            NEXTKEY_LOG(L"HookEngine: configGeneration changed (%u), full TOML reload", state.configGeneration);
            ReloadFromToml();
        }
    }

    // Wave 3 PR 3.8 — toggle-hotkey live propagation from SharedState.
    //
    // SettingsDialog::syncToSharedState writes the new hotkey into
    // SharedState immediately (state.SetHotkey) but defers the TOML save
    // by 30 s. Pre-3.8 the only reload path was `ReloadFromToml()` fired
    // from the configGeneration check above, which read STALE TOML data
    // and `HotkeyManager::UpdateHotkey` got the old binding until the
    // user closed the Settings dialog (WM_CLOSE forces flush).
    //
    // Doctrine: SharedState is the live config bus, TOML is the
    // persistence layer. The hotkey field lives on both — read from
    // SharedState here so HotkeyManager sees the fresh binding within
    // one QuickSync cycle (~ms latency vs 30 s).
    //
    // Must run BEFORE the ff/sc/im/ct early-return below — those four
    // are engine-state flags; the hotkey doesn't depend on any of them,
    // so a Settings change that only touches the hotkey would short-
    // circuit through the early-return without our diff running.
    {
        const HotkeyConfig newHk = state.GetHotkey();
        if (newHk != lastToggleHotkey_) {
            NEXTKEY_LOG(L"HookEngine: toggle hotkey changed (mods=C%dS%dA%dW%d vk=0x%02X → C%dS%dA%dW%d vk=0x%02X)",
                        lastToggleHotkey_.ctrl, lastToggleHotkey_.shift,
                        lastToggleHotkey_.alt, lastToggleHotkey_.win,
                        lastToggleHotkey_.vk,
                        newHk.ctrl, newHk.shift, newHk.alt, newHk.win, newHk.vk);
            lastToggleHotkey_ = newHk;
            if (hotkeyChangedCallback_) {
                hotkeyChangedCallback_(newHk);
            }
        }
    }

    uint32_t ff = state.GetFeatureFlags();
    uint8_t sc = state.spellCheck;
    uint8_t im = state.inputMethod;
    uint8_t ct = state.codeTable;

    // No change → no-op (cheap: integer compares on mapped memory)
    if (ff == lastFeatureFlags_.load(std::memory_order_acquire) &&
        sc == lastSpellCheck_.load(std::memory_order_acquire) &&
        im == lastInputMethod_.load(std::memory_order_acquire) &&
        ct == lastCodeTable_.load(std::memory_order_acquire)) return;
    lastFeatureFlags_.store(ff, std::memory_order_release);
    lastSpellCheck_.store(sc, std::memory_order_release);
    lastInputMethod_.store(im, std::memory_order_release);
    lastCodeTable_.store(ct, std::memory_order_release);

    NEXTKEY_LOG(L"HookEngine: SharedState changed (ff=0x%04X, spell=%d, method=%d, ct=%d)", ff, sc, im, ct);

    TypingConfig cfg = *config_.load(std::memory_order_acquire);
    DecodeFeatureFlags(ff, cfg);
    cfg.SetSpellCheckLevel(static_cast<SpellCheckLevel>(sc));
    cfg.inputMethod = static_cast<InputMethod>(im);
    cfg.codeTable = static_cast<CodeTable>(ct);

    bool methodChanged = (currentMethod_.load(std::memory_order_acquire) != cfg.inputMethod);
    bool codeTableChanged = (currentCodeTable_.load(std::memory_order_acquire) != cfg.codeTable);
    ApplyConfig(cfg);
    config_.store(std::make_shared<const TypingConfig>(cfg), std::memory_order_release);

    // P3e fix: defer engine recreate to ApplyConfigOnHookThread. QuickSync's
    // slow path runs on whichever thread called it (worker via OnTickPoll →
    // OnFocusChanged, or main via SyncConfigFromSharedState). The engine
    // swap + CommitComposition must run on the hook thread to avoid the
    // race that surfaced under `-InjectConfigReloadMs 50` chaos.
    if (methodChanged) {
        lifecycle_.Mailbox().Post(HookCommand::kConfigApply);
    }

    if (codeTableChanged) {
        currentCodeTable_.store(cfg.codeTable, std::memory_order_release);
        globalCodeTable_.store(cfg.codeTable, std::memory_order_release);
    }

    {
        // Phase 3d: macroEnabled toggled but configGeneration didn't bump
        // (typical case — user flips the macro feature switch). Compare
        // the snapshot's macro presence against the new desired state;
        // if they disagree, rebuild + republish. On the hook thread this
        // path is now deferred via pendingConfigReload_ (same as the
        // configGeneration-bump path) so TOML parse stays off-hook.
        const bool macroOn = macroEnabled_.load(std::memory_order_acquire);
        auto snap = configSnapshot_.load(std::memory_order_acquire);
        const bool snapHasMacros = snap && !snap->macroTable.empty();
        if (macroOn != snapHasMacros) {
            if (const DWORD _hookTid = lifecycle_.ThreadId(); _hookTid != 0 && GetCurrentThreadId() == _hookTid) {
                pendingConfigReload_.store(true, std::memory_order_release);
            } else {
                RebuildSnapshotFromToml(
                    static_cast<std::uint32_t>(lastConfigGeneration_.load(std::memory_order_acquire)));
            }
        }
    }
}

void HookEngine::SyncConfigFromSharedState() {
    QuickSyncFromSharedState();
}

void HookEngine::ReloadFromToml() {
    PERF_SCOPE(::NextKey::Perf::Stage::ConfigReload);
    NEXTKEY_LOG(L"HookEngine: full TOML reload");

    // Read TOML for fields not in SharedState (beep, smartSwitch, excludeApps, hotkey)
    auto config = ConfigManager::LoadOrDefault();

    // Override with SharedState for fields that Settings updates immediately
    // (TOML may be stale due to deferred save)
    if (sharedStatePtr_) {
        SharedState state = sharedStatePtr_->Read();
        if (state.IsValid()) {
            config.inputMethod = static_cast<InputMethod>(state.inputMethod);
            config.SetSpellCheckLevel(static_cast<SpellCheckLevel>(state.spellCheck));
            DecodeFeatureFlags(state.GetFeatureFlags(), config);
            NEXTKEY_LOG(L"HookEngine: read SharedState (epoch=%u, featureFlags=0x%04X)",
                        state.epoch, state.GetFeatureFlags());
        }
    }

#ifdef VKEY_USE_RUST_ENGINE
    // Cold path: read UTF-8 and compile the immutable Rust snapshot on this
    // worker/main thread. The hook thread only swaps the ready shared_ptr.
    if (config.spellSuggestEnabled) {
        auto loaded = RustInputEngine::LoadUserDictionary(ConfigManager::GetConfigPath());
        if (loaded.Succeeded()) {
            pendingUserDictionary_.store(std::move(loaded.snapshot),
                                         std::memory_order_release);
            HOOK_LOG(L"  UserDictionary: reload ready path='%s'", loaded.path.c_str());
        } else {
            // Invalid/unreadable is fail-stale: keep the last valid snapshot.
            HOOK_LOG(L"  UserDictionary: reload rejected; retaining prior snapshot (status=%d, engineStatus=%u, line=%zu, path='%s')",
                     static_cast<int>(loaded.status), loaded.engineStatus,
                     loaded.errorLine, loaded.path.c_str());
        }
    }
#endif

    // Smart-switch off→on transition: load persisted apps from TOML on this
    // worker thread (Rule §11.2 — TOML parse is forbidden on hook), then
    // stash for ApplyConfigOnHookThread to swap into the live appModeMap_.
    // Without this, mid-session enabling of smart_switch would leave the
    // map empty and the first focus-driven mutation would overwrite the
    // user's existing TOML entries with an empty `[smart_switch.apps]`.
    {
        const auto prevCfg = config_.load(std::memory_order_acquire);
        const bool wasOn = prevCfg && prevCfg->smartSwitch;
        if (!wasOn && config.smartSwitch) {
            auto loaded = std::make_shared<const std::unordered_map<std::wstring, bool>>(
                ConfigManager::LoadSmartSwitchApps(ConfigManager::GetConfigPath()));
            pendingAppModeMap_.store(std::move(loaded), std::memory_order_release);
            NEXTKEY_LOG(L"SmartSwitch: smart_switch off→on detected, deferred load via ApplyConfig");
        }
    }

    // P3e fix — single-writer for `engine_`. Pre-P3e, this function called
    // CommitComposition + `engine_ = EngineFactory::Create(...)` inline.
    // Post-P3c, ReloadFromToml runs on the worker thread (Rule 11.2 forbids
    // TOML parse on hook), so the inline engine swap raced against the hook
    // hot path's `engine_->Peek/Push/Count` reads — UAF discovered by
    // run-chaos.ps1 -InjectConfigReloadMs 50 (11 / 55 failures, 5 hosts ×
    // 11 tests: composition state lost mid-word). Defer both the commit
    // AND the engine recreate to ApplyConfigOnHookThread; the hook drain
    // runs them between keystrokes where they're single-writer safe.
    config_.store(std::make_shared<const TypingConfig>(config), std::memory_order_release);
    ApplyConfig(config);
    // Reload `[[hotkeys]]` from TOML alongside main config — keeps registry in
    // sync when Settings dialog persists rebindings via SaveHotkeyRegistry.
    // Still runs migration (idempotent — no-op if section already populated).
    ApplyHotkeyRegistry(ConfigManager::MigrateLegacyHotkeysIfNeeded(
        ConfigManager::GetConfigPath()));

    currentCodeTable_.store(config.codeTable, std::memory_order_release);
    globalCodeTable_.store(config.codeTable, std::memory_order_release);
    globalInputMethod_.store(config.inputMethod, std::memory_order_release);

    // Phase 3d — one helper does it all: TOML parse for overrides /
    // excluded apps / TSF apps / macros, ConfigSnapshot::Build (derives
    // spaceMacroKeys), atomic publish. The re-evaluate block below reads
    // the freshly-published snapshot for the current-app fields.
    RebuildSnapshotFromToml(
        static_cast<std::uint32_t>(lastConfigGeneration_.load(std::memory_order_acquire)));
    auto rcuSnap = configSnapshot_.load(std::memory_order_acquire);

    // Re-evaluate excluded status for current app (set was just reloaded)
    const auto& curExe = focus_.ActiveExe();
    const bool wasExcluded = isExcludedApp_.load(std::memory_order_acquire);
    bool newExcluded = false;
    if (config.excludeApps && !curExe.empty() && rcuSnap) {
        newExcluded = rcuSnap->excludedAppSet.count(curExe) > 0;
        isExcludedApp_.store(newExcluded, std::memory_order_release);
    } else {
        newExcluded = isExcludedApp_.load(std::memory_order_acquire);
    }

    // Re-evaluate TSF app status for current foreground app
    const bool wasTsfApp = isTsfApp_.load(std::memory_order_acquire);
    const BrowserRoute browserRoute = browserRoute_.load(std::memory_order_acquire);
    const bool wasEffectiveTsf = ResolveEffectiveTsf(
        wasTsfApp, wasExcluded, config.tsfApps, browserRoute);
    bool newTsfApp;
    if (config.tsfApps && !newExcluded && rcuSnap && !rcuSnap->tsfAppSet.empty() && !curExe.empty()) {
        newTsfApp = rcuSnap->tsfAppSet.count(curExe) > 0;
    } else {
        newTsfApp = false;
    }
    isTsfApp_.store(newTsfApp, std::memory_order_release);
    const bool newEffectiveTsf = ResolveEffectiveTsf(
        newTsfApp, newExcluded, config.tsfApps, browserRoute);
    HOOK_LOG(L"  Engine (config reload): %s for '%s' (tsf_feature=%d, in_tsf_list=%d, excluded=%d)",
             newTsfApp ? L"TSF (hook passthrough)" : L"HOOK",
             curExe.c_str(),
             config.tsfApps ? 1 : 0,
             (rcuSnap && !curExe.empty() && rcuSnap->tsfAppSet.count(curExe) > 0) ? 1 : 0,
             newExcluded ? 1 : 0);
    if (tsfModeCallback_) {
        const bool tsfReadonly = !newEffectiveTsf && !newExcluded;
        if (newEffectiveTsf != wasEffectiveTsf) {
            HOOK_LOG(L"  TSF_ACTIVE flag: %s → %s",
                     wasEffectiveTsf ? L"true" : L"false",
                     newEffectiveTsf ? L"true" : L"false");
        }
        tsfModeCallback_(newEffectiveTsf, tsfReadonly,
                         newEffectiveTsf && !wasEffectiveTsf);
    }

    // Re-apply per-app encoding override for current app. Encoding is a
    // plain enum (`CodeTable`) read on the hook hot path without locking;
    // a worker-side write is a torn-read risk but NOT a UAF — minor
    // staleness window only. Acceptable for an enum-sized field.
    if (!curExe.empty() && !newExcluded && !newEffectiveTsf && rcuSnap) {
        auto it = rcuSnap->appEncodingOverrides.find(curExe);
        currentCodeTable_.store(
            (it != rcuSnap->appEncodingOverrides.end())
                ? it->second
                : globalCodeTable_.load(std::memory_order_acquire),
            std::memory_order_release);
    }
    // P3e fix — per-app inputMethod override engine recreate moved to
    // ApplyConfigOnHookThread (same race surface as the unconditional
    // recreate removed above). Worker thread cannot safely swap
    // `engine_` while hook hot path holds raw pointer reads.

    // Notify main process to reload hotkey / QuickConvert configs.
    // Main owns HotkeyManager slots and calls UpdateHotkey there.
    if (configReloadCallback_) {
        configReloadCallback_();
    }

    // P3e fix — post kConfigApply to the hook mailbox so the drain runs
    // ApplyConfigOnHookThread between keystrokes. This is the producer
    // for the dormant handler we wired in P2c — finally lit up. The
    // mailbox coalesces against rapid republishes (one Apply per drain
    // cycle) so chaos `-InjectConfigReloadMs 50` doesn't queue up many.
    lifecycle_.Mailbox().Post(HookCommand::kConfigApply);
}

// ═══════════════════════════════════════════════════════════
// Static Hook Callbacks → Instance Dispatch
// ═══════════════════════════════════════════════════════════

namespace {
// Hard cap on the raw macro-tracking buffer. No macro key is remotely this long
// (config caps keys at 32), so a buffer past this can't match anything — clear it
// rather than let a trigger-less keystroke stream grow it unbounded and slow the
// per-key macro lookup on the hot path.
constexpr size_t kMaxRawMacroBuffer = 128;

// SEH filter: log the structured-exception code, then execute the handler.
// Kept at file scope (no C++ locals) so it is safe to call from an __except
// filter expression. Runs in the LL-hook thread of VKeyApp.
LONG LogHookSeh(const wchar_t* where, unsigned long code) noexcept {
    char msg[64];
    _snprintf_s(msg, _TRUNCATE, "SEH structured exception code=0x%08lX", code);
    ::NextKey::CrashLog(where, msg);
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

// SEH wrapper — see header for why. Recovers by resetting composition and
// passing the key through untranslated; VKeyApp stays alive.
LRESULT CALLBACK HookEngine::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    __try {
        return LowLevelKeyboardProcImpl(nCode, wParam, lParam);
    } __except (LogHookSeh(L"HookEngine::LowLevelKeyboardProc", GetExceptionCode())) {
        HookEngine* self = s_instance.load(std::memory_order_relaxed);
        if (self) self->ResetComposition();
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
}

LRESULT HookEngine::LowLevelKeyboardProcImpl(int nCode, WPARAM wParam, LPARAM lParam) {
    // Phase 1: Tier 2 budget marker (<30ms p99). Wraps the full LL callback
    // body so the recorded delta includes every nested stage. PERF_SCOPE
    // compiles to (void)0 when VKEY_PERF_HIST is not defined.
    PERF_SCOPE(::NextKey::Perf::Stage::TotalKeydown);
    // `self` declared outside the try so the catch block can call
    // ResetComposition (Rule 11.5 — "ALWAYS reset state on exception"). Without
    // this, a throw escaping ProcessKeyDown leaves engine_/previousComposition_
    // in a half-updated state for the next keystroke. Re-load is cheap (atomic
    // load) and ResetComposition asserts hook-thread (which we are, here).
    HookEngine* self = s_instance.load(std::memory_order_relaxed);
    // Top-level catch: a C++ throw escaping a low-level hook unwinds through
    // KiUserCallbackDispatcher and Windows raises STATUS_FATAL_USER_CALLBACK_EXCEPTION
    // (0xC000041D), terminating the process. Swallow + log so the next keystroke
    // gets a fresh attempt instead of the app silently disappearing.
    try {
        auto* pKey = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        // Always track our own synthetic events regardless of nCode.
        // When nCode < 0, Windows tells us to pass the message along — but the event
        // still represents a delivered synthetic that was counted when sent.
        // Without this, synthEventsPending_ leaks on every nCode < 0 delivery.
        if (self && pKey->dwExtraInfo == VKEY_EXTRA_INFO) {
            HOOK_LOG(L"  PASSTHRU (dwExtraInfo=NK): vk=0x%02X scan=0x%04X flags=0x%08X nCode=%d",
                     pKey->vkCode, pKey->scanCode, pKey->flags, nCode);
            self->dispatcher_.DecrementSynthEvents();
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if (nCode == HC_ACTION && self) {
            const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            const bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

            // Issue #274: Bypass synthetic/injected events from other software
            // (PhraseExpress, AutoHotkey, mouse/keyboard macro software).
            // External events carry LLKHF_INJECTED (0x10) or LLKHF_LOWER_IL_INJECTED (0x02)
            // without VKEY_EXTRA_INFO. On key-down, reset any active composition so stale
            // buffer does not corrupt subsequent typing. Always pass through untouched.
            const bool isExternalInjected =
                IsExternalInjectedKey(pKey->flags, pKey->dwExtraInfo, VKEY_EXTRA_INFO);
            if (isExternalInjected) {
                if (ShouldResetCompositionOnInjected(
                        isExternalInjected, isDown,
                        self->engine_->Count(),
                        !self->previousComposition_.empty())) {
                    HOOK_LOG(L"  EXTERNAL INJECTED (vk=0x%02X flags=0x%08X) -> ResetComposition",
                             pKey->vkCode, pKey->flags);
                    self->ResetComposition();
                }
                return CallNextHookEx(nullptr, nCode, wParam, lParam);
            }

            // Application hotkeys share this sole dedicated keyboard hook.
            // Match every real DOWN/UP event after VKey-tagged synthetic
            // filtering and before sending/activity/engine work. Match() only
            // performs bounded lock-free state matching and posts a slot id;
            // callbacks execute later in HookLifecycle's message pump.
            if (self->hotkeyManager_ && (isDown || isUp)) {
                const HotkeyManager::KeyEvent event{
                    .vk = pKey->vkCode,
                    .type = isDown ? HotkeyManager::KeyEventType::Down
                                   : HotkeyManager::KeyEventType::Up,
                    .modifier = HotkeyManager::NormalizeModifier(pKey->vkCode),
                };
                const HotkeyManager::MatchResult result =
                    self->hotkeyManager_->Match(event);
                if (result.consume && result.cancelSystemMenu) {
                    // Break Windows' Alt/Win-alone menu gesture only for a
                    // consumed matched combo. Modifier-only release fires are
                    // deliberately not consumed and must not inject this pair.
                    static const WORD ctrlScan = static_cast<WORD>(
                        MapVirtualKeyW(VK_LCONTROL, MAPVK_VK_TO_VSC));
                    INPUT dummy[2] = {};
                    dummy[0].type = INPUT_KEYBOARD;
                    dummy[0].ki.wVk = VK_LCONTROL;
                    dummy[0].ki.wScan = ctrlScan;
                    dummy[0].ki.dwExtraInfo = VKEY_EXTRA_INFO;
                    dummy[1] = dummy[0];
                    dummy[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    (void)NextKey::Output::Internal::TrackedSendInput(dummy, 2);
                }
                if (result.consume) return 1;
            }

            // Skip events while we're sending (safety backup).
            if (self->dispatcher_.IsSending()) {
                // Issue #206: on a multi-process renderer (Electron/WebView2/RDP)
                // a physical key that leaked into this injection window —
                // the in-flight key's own auto-repeat key-down or its key-up —
                // would otherwise pass through raw and interleave between our
                // synthetic backspaces/VK_PACKET chars, which the renderer's
                // async pipeline then reorders ("nhảy loạn"). Eat only the
                // in-flight key's own no-intent events; any other vk (the
                // genuinely-typed next key, a modifier release) still passes
                // through untouched — no dropped keystroke. See
                // LeakedKeyDuringSendDecision.h for the full rationale.
                auto inj = self->dispatcher_.GetInjector();
                const LeakedKeyDuringSendInputs leak{
                    .isSending = true,
                    .hasMultiProcessRenderer = inj && inj->HasMultiProcessRenderer(),
                    .eventVk = pKey->vkCode,
                    .sendingForVk = self->sendingForVk_.load(std::memory_order_relaxed),
                };
                if (DecideEatLeakedKeyDuringSend(leak)) {
                    HOOK_LOG(L"  EAT (sending_ leak, multi-proc): vk=0x%02X scan=0x%04X flags=0x%08X",
                             pKey->vkCode, pKey->scanCode, pKey->flags);
                    return 1;  // suppress — do not interleave into the synth stream
                }
                HOOK_LOG(L"  PASSTHRU (sending_): vk=0x%02X scan=0x%04X flags=0x%08X",
                         pKey->vkCode, pKey->scanCode, pKey->flags);
                return CallNextHookEx(nullptr, nCode, wParam, lParam);
            }

            // Adaptive-tick — mark every real user key as activity (after
            // synth-event filter at line 758 and sending-state filter above).
            // Rule 11.2 compliant: relaxed atomic store + branch; on idle->
            // active transition fires one workerSignalFn_ call. See plan
            // docs/plans/2026-05-27-adaptive-tick-idle-backoff.md §2.5.
            self->MarkActivity();

            // Snapshot modifiers BEFORE DrainHookCommands(): draining a
            // pending focus-changed command can synchronously reach
            // ApplyFocusOnHookThread -> GetFocusedChildHwnd, whose
            // cross-thread path uses AttachThreadInput — Microsoft's docs
            // for AttachThreadInput say it resets the state GetKeyState/
            // GetKeyboardState report for the calling (this hook) thread.
            // ProcessKeyDown used to read Ctrl/Alt/Win/Shift via GetKeyState
            // AFTER the drain; if a focus result happened to be pending
            // right as the user pressed e.g. Ctrl+Tab, the held Ctrl could
            // read as up for THIS keystroke, letting the shortcut fall
            // through into IME/composition handling instead of the
            // modifier-held passthrough guard (DispatchKeyAction step 5).
            const bool preDrainShift    = isDown && (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool preDrainCapsLock = isDown && (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
            const bool preDrainCtrl     = isDown && (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool preDrainAlt      = isDown && (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool preDrainWin      = isDown && ((GetKeyState(VK_LWIN) & 0x8000) != 0 ||
                                                      (GetKeyState(VK_RWIN) & 0x8000) != 0);

            // Rule 11.4 step 5 — drain cross-thread commands BEFORE the
            // English-mode / modifier-key dispatch chain so state mutations
            // posted by main / worker / hotkey threads land before this
            // keystroke is classified. Drain is cheap when nothing is
            // pending (one atomic load + one branch).
            //
            // Placement constraint: MUST come after sending_ (synthetic
            // events from injector_->Replace must not re-enter drain) and
            // BEFORE the English-mode passthrough so an in-flight V/E
            // toggle posted from the hotkey thread takes effect on the
            // very next keystroke, not the one after.
            self->DrainHookCommands();

            // Delayed-focus ordering epoch. Advance only for a real physical
            // key-down and only AFTER the drain: a focus result already queued
            // before the first key in a new context must be allowed to reset
            // the old composition before that key is processed. Earlier
            // physical key-downs have already advanced the epoch, so a worker
            // result that they overtook is deferred instead of wiping their
            // live word. One relaxed RMW; synthetic/sending events returned
            // above and never reach this point.
            if (isDown) {
                self->physicalInputEpoch_.fetch_add(1, std::memory_order_relaxed);
            }

            HOOK_LOG(L"KEY vk=0x%02X scan=0x%04X flags=0x%08X %s",
                     pKey->vkCode, pKey->scanCode, pKey->flags,
                     isDown ? L"DOWN" : (isUp ? L"UP" : L"OTHER"));

            // REGRESSION TRAP — DO NOT UNCOMMENT
            //
            // Sprint 1 D4 originally took stateMutex_ here to guard the racing
            // reads of `engine_`, `previousComposition_`, app-detect flags, etc.
            // Phase B (D5-D11) replaced every reader/writer with std::atomic
            // + RCU patterns; the lock is no longer needed and the type
            // (`std::recursive_mutex`) was downgraded to `std::mutex` in D11
            // — uncommenting this line triggers a compile error which IS the
            // intentional regression trap. `tools/audit/check_hook_thread_no
            // _mutex.sh` Check 1 verifies this line stays commented (one of
            // 3 such lines across hook callbacks). If you're tempted to "clean
            // up" the dangling reference, read the audit script first.
            // std::lock_guard<std::recursive_mutex> _lock(self->stateMutex_);

            if (isDown) {
                // Stamp the in-flight vk BEFORE ProcessKeyDown so the re-entrant
                // `sending_` branch (above) can recognise this key's own leaked
                // auto-repeat / key-up during the injection it is about to start.
                self->sendingForVk_.store(pKey->vkCode, std::memory_order_relaxed);
                if (self->ProcessKeyDown(pKey->vkCode, pKey->scanCode, pKey->flags,
                                         preDrainShift, preDrainCapsLock, preDrainCtrl,
                                         preDrainAlt, preDrainWin)) {
                    HOOK_LOG(L"  → EATEN (key-down vk=0x%02X)", pKey->vkCode);
                    return 1;  // Eat the keystroke
                }
            } else if (isUp) {
                if (self->ProcessKeyUp(pKey->vkCode, pKey->flags)) {
                    return 1;  // Eat the keystroke
                }
            }
        }
    } catch (const std::exception& e) {
        CrashLog(L"HookEngine::LowLevelKeyboardProc", e.what());
        // Rule 11.5 safety net: an exception escaping ProcessKey* leaves the
        // engine + previousComposition + per-word flags in an undefined state.
        // ResetComposition clears them so the next keystroke starts fresh
        // instead of compounding the corruption.
        if (self) self->ResetComposition();
    } catch (...) {
        CrashLog(L"HookEngine::LowLevelKeyboardProc", "(non-std exception)");
        if (self) self->ResetComposition();
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Wave 3 PR 3.2 — WinEventProc moved to FocusOwner::WinEventProc. The
// classification-only dispatch (FOREGROUND / MINIMIZEEND → OnFocusChanged)
// runs there; HookEngine's OnFocusChanged shim is wired via the
// FocusChangedFn callback registered in focus_.Install().

LRESULT CALLBACK HookEngine::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    __try {
        return LowLevelMouseProcImpl(nCode, wParam, lParam);
    } __except (LogHookSeh(L"HookEngine::LowLevelMouseProc", GetExceptionCode())) {
        HookEngine* self = s_instance.load(std::memory_order_relaxed);
        if (self) self->ResetComposition();
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
}

LRESULT HookEngine::LowLevelMouseProcImpl(int nCode, WPARAM wParam, LPARAM lParam) {
    try {
        if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN) {
            HookEngine* self = s_instance.load(std::memory_order_relaxed);
            if (self) {
                // REGRESSION TRAP — DO NOT UNCOMMENT (see LowLevelKeyboardProc
                // above for the full rationale). Mouse path includes a writer
                // (ResetComposition); torn-read risk pre-Phase-B was higher
                // here than the keyboard read paths. Phase B replaced this
                // with atomic state — current cachedFocusedHwnd_ + Reset-
                // Composition write set is captured as Pre-T3 review Minor
                // 1 in docs/TODO.md (still-open audit). Audit Check 1
                // enforces this line stays commented.
                // std::lock_guard<std::recursive_mutex> _lock(self->stateMutex_);
                HOOK_LOG(L"MOUSE click — resetting composition (engine count=%zu, prev='%s')",
                         self->engine_->Count(), self->previousComposition_.c_str());
                // Always reset, even when engine is idle: commitUndoState_ and commitStack_
                // may hold a previously committed word. If not cleared here, a click elsewhere
                // followed by Backspace triggers ReplayCommittedChars() at the new cursor
                // position — identical to the Ctrl+A bug.
                self->ResetComposition();
                // Click may move focus to another control within the same app (no
                // EVENT_SYSTEM_FOREGROUND fires) — invalidate cache so the next
                // TryEditMessagePaste re-queries the focused HWND.
                self->focus_.InvalidateFocusCache();
                // Same reason, different consumer: suppressAutoCapForPassword-
                // Safety_ was computed against whatever child control was
                // focused at the LAST EVENT_SYSTEM_FOREGROUND, so a click from
                // a password box into a normal field would leave auto-cap
                // suppressed indefinitely. Clearing the latch is the whole fix
                // needed here — HandleAlphaKey re-checks the focused control
                // synchronously right before it actually capitalizes and
                // re-suppresses if it IS a password field, so the permissive
                // direction is the safe one to reset to.
                //
                // Deliberately NOT a full OnFocusChanged() refresh (was
                // a68c617b, reverted): that routed every left click through
                // focus_.Classify() — documented as too heavy for the input
                // path (CreateToolhelp32Snapshot) — plus an
                // ApplyFocusOnHookThread that
                // spends two AttachThreadInput round-trips on THIS thread, the
                // one that owns WH_MOUSE_LL. Each click queued work in front of
                // the next click's LowLevelHooksTimeout window: system-wide
                // click lag, independent of whether auto-cap was enabled.
                // Cost of not refreshing: clicking into an empty document no
                // longer arms auto-cap for its first letter until the next real
                // foreground change.
                self->suppressAutoCapForPasswordSafety_ = false;
            }
        }
    } catch (const std::exception& e) {
        CrashLog(L"HookEngine::LowLevelMouseProc", e.what());
    } catch (...) {
        CrashLog(L"HookEngine::LowLevelMouseProc", "(non-std exception)");
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Forward declarations for file-scope helpers used in ProcessKeyDown
static HWND GetInputTarget();
static bool IsIncompatibleLayout(HKL hkl);

// ═══════════════════════════════════════════════════════════
// Core Processing
// ═══════════════════════════════════════════════════════════

bool HookEngine::ProcessKeyDown(DWORD vkCode, DWORD /*scanCode*/, DWORD /*flags*/,
                                 bool preDrainShift, bool preDrainCapsLock, bool preDrainCtrl,
                                 bool preDrainAlt, bool preDrainWin) {
    // Invalidate before pipeline features or routing guards can return early.
    if (macroUndoState_.active) {
        const auto macroCfg = config_.load(std::memory_order_acquire);
        if (vkCode != VK_BACK || preDrainCtrl || preDrainAlt || preDrainWin
            || !macroCfg || !macroCfg->macroEnabled || !macroCfg->macroUndoOnBackspace
            || GetTickCount() - macroUndoState_.timestamp > kMacroUndoTimeoutMs) {
            ClearMacroUndo();
        }
    }
    // Replay-context boundary (Enter / Tab) — the ONE choke point enforcing
    // "a commit-undo replay window must never survive leaving the editing
    // context". Runs before every guard, independent of engine emptiness, of
    // whether a commit fires, and of whether that commit pushes a stack entry,
    // because the post-commit DecideCommitUndoArm switch is unreachable on all
    // of those paths (quick-consonant commit, empty history, Tab with an empty
    // engine but a populated stack) while HandleBackspace still re-arms replay
    // from any stack that survives. Enter additionally has an older dedicated
    // guard inside the FSM; Tab never had one.
    if (::NextKey::IsReplayContextBoundary(static_cast<uint32_t>(vkCode))) {
        commitState_.DiscardReplayContext();
    }

    // Tab/Shift+Tab MAY move focus to a different child control without any
    // WinEvent firing (the top-level foreground window doesn't change), so
    // nothing else would ever refresh suppressAutoCapForPasswordSafety_ for
    // the newly-focused control — tabbing OUT of a password box would leave
    // auto-cap suppressed indefinitely. Same treatment as the mouse-click path
    // (LowLevelMouseProcImpl): drop the cache and the latch, let HandleAlphaKey's
    // synchronous pre-capitalize check re-suppress if the new control is itself
    // a password field. Deliberately NOT a full OnFocusChanged() refresh — see
    // the click path's comment for why that cost lands on the input thread.
    //
    // Composition is deliberately NOT reset for Tab: a wipe would destroy
    // whatever the user has typed, and hosts that use Tab for indentation never
    // change focus at all. The cross-field commit-undo leak that would
    // otherwise need is closed durably instead by DiscardReplayContext above.
    if (vkCode == VK_TAB) {
        focus_.InvalidateFocusCache();
        suppressAutoCapForPasswordSafety_ = false;
    }

    // H1b: top-of-pipeline guards extracted to RunTopGuards (steps 0/0b/1/1b/1c).
    // Behavior preserved byte-identical — see method comment for details.
    switch (RunTopGuards(vkCode)) {
        case KeyOutcome::Eat: return true;
        case KeyOutcome::Pass: return false;
        case KeyOutcome::Fallthrough: break;
    }

    // Non-modifier key pressed — invalidate modifier-only hotkey combo (any
    // pending double-tap chain on Ctrl/Shift/Alt/Win is now contaminated)
    otherKeyPressed_ = true;
    for (int i = 0; i < kModCount; ++i) modTapCount_[i] = 0;

    // Watchdog: reset synthEventsPending_ if stuck > 500ms.
    // Covers event loss in Electron/Console multi-process apps where synthetic
    // events can be dropped under heavy CPU load, causing cascading re-injection
    // and ghost characters.
    if (dispatcher_.SynthEventsPending() > 0) {
        DWORD elapsed = GetTickCount() - dispatcher_.LastSynthSendTime();
        if (elapsed > 500) {
            HOOK_LOG(L"  watchdog: synthEventsPending_ reset from %d (stuck %ums)",
                     dispatcher_.SynthEventsPending(), elapsed);
            dispatcher_.ResetSynthEvents();
        }
    }

    // 2c. Fast English exit — skip commit-undo step when no undo is pending.
    //      Commit-undo only applies to Vietnamese words (line 691 checks vietnameseMode_).
    //      When English mode + undo Idle + no English macros → nothing below applies.
    // Sprint 1 D5.2: hoist atomic config-flag loads to a single snapshot at the
    // top of the hot path. Same-thread within ProcessKeyDown — no need to re-load
    // (config writers run on main and cannot interleave a sub-ms hook callback).
    const bool vnMode = vietnameseMode_.load(std::memory_order_acquire);
    const bool macroOn = macroEnabled_.load(std::memory_order_acquire);
    const bool macroEng = macroInEnglish_.load(std::memory_order_acquire);
    if (!vnMode &&
        commitState_.IsIdle() &&
        !macroUndoState_.active &&
        !(macroOn && macroEng)) {
        return false;
    }

    // Modifier state for this keystroke — captured by the caller (see
    // LowLevelKeyboardProcImpl) BEFORE DrainHookCommands() ran, not re-read
    // here: draining a pending focus-changed command can synchronously reach
    // GetFocusedChildHwnd's AttachThreadInput path, which resets what
    // GetKeyState reports for this thread. Re-reading here would risk
    // observing that reset instead of what was actually held for this key.
    const bool cachedShift = preDrainShift;
    const bool cachedCapsLock = preDrainCapsLock;
    const bool cachedCtrl = preDrainCtrl;
    const bool cachedAlt = preDrainAlt;
    const bool cachedWin = preDrainWin;

    // 2d. PreEngine pipeline dispatch.
    //   W3: CommitUndoFeature owns the commit-undo FSM at prio 20.
    //   W4a: EscRestoreRawFeature owns hotkey-triggered raw-input restore at prio 40.
    // Coordinator runs features in priority order. Features emit Intents::
    // ConsumeKey (→ return true) or PassThrough (→ return false); empty batch
    // means fall through to step 3+.
    {
        std::wstring_view engineRendered =
            engine_ ? std::wstring_view{engine_->Peek()} : std::wstring_view{};
        std::wstring_view rawSnapshot =
            engine_ ? engine_->PeekRawView() : std::wstring_view{};
        NextKey::Pipeline::HookCompositionSession session(
            previousComposition_, engineRendered, rawSnapshot);
        NextKey::Pipeline::KeyContext keyCtx{
            static_cast<std::uint16_t>(vkCode),
            L'\0',
            cachedShift, cachedCapsLock, cachedCtrl, cachedAlt, cachedWin,
            &session,
            0
        };
        coordinator_.HandleKeyAtStage(
            NextKey::Pipeline::Stage::PreEngine, keyCtx, outputChannel_);
        // DrainBatch (not TakeBatch): empties the channel WITH capacity retained
        // so this per-keystroke drain on the LL-hook thread does not heap-allocate
        // (Rule 11.2). The channel is empty right after the call, so the early
        // `return`s below don't strand intents into the next keystroke.
        const auto& batch = outputChannel_.DrainBatch();
        for (const auto& intent : batch) {
            if (std::holds_alternative<NextKey::Pipeline::Intents::ConsumeKey>(intent))
                return true;
            if (std::holds_alternative<NextKey::Pipeline::Intents::PassThrough>(intent))
                return false;
        }
        // No flow-control intent → Fallthrough: continue to step 3+.
    }

    // H1c: English-mode short-circuit + Vietnamese pre-dispatch tracking
    // (steps 3 / 3a-3d). Behavior preserved byte-identical.
    switch (HandlePreDispatch(vkCode, vnMode,
                              cachedShift,
                              cachedCtrl, cachedAlt, cachedWin)) {
        case KeyOutcome::Eat: return true;
        case KeyOutcome::Pass: return false;
        case KeyOutcome::Fallthrough: break;
    }

    // H1c: action dispatch (steps 4b-10). Returns Eat or Pass for every code path.
    switch (DispatchKeyAction(vkCode, cachedShift, cachedCapsLock, cachedCtrl,
                              cachedAlt, cachedWin, macroOn)) {
        case KeyOutcome::Eat: return true;
        case KeyOutcome::Pass: return false;
        case KeyOutcome::Fallthrough: break;
    }

    return false;
}

// H1b: top-of-pipeline guards extracted from ProcessKeyDown steps 0/0b/1/1b/1c.
//
//  Step 0  — QuickSyncFromSharedState (atomic config epoch; see comment below).
//  Step 0b — TSF early-out: foreground app is in TSF list, hook does nothing.
//  Step 1  — Track modifier keys (LCTRL/RCTRL/LSHIFT/RSHIFT/LMENU/RMENU/LWIN/RWIN);
//            pass through without consumption (don't eat modifier keys themselves).
//  Step 1b — Toggle keys (CapsLock/NumLock/ScrollLock): pass through without
//            committing composition (CapsLock often pressed mid-word).
//  Step 1c — Excluded-app passthrough: same-PID short-circuit; different-PID
//            verifies via VerifyExcludedState; on cleared, NotifyModeChange and
//            fall through to normal processing for this keystroke.
//
// Behavior is byte-identical to the pre-extraction inline block. Returns:
//   Eat         → ProcessKeyDown returns true (no top guards do this today,
//                 reserved for future use).
//   Pass        → ProcessKeyDown returns false (TSF / modifier / toggle /
//                 still-excluded paths).
//   Fallthrough → continue with subsequent ProcessKeyDown steps (only when no
//                 guard matched, or excluded-app cleared its PID).
//
// The post-guard bookkeeping in ProcessKeyDown (otherKeyPressed_=true,
// modTapCount_[]=0, synth-pending watchdog) lives in the wrapper, not here, so it
// runs only on Fallthrough. The excluded-app same-PID and still-excluded paths
// set otherKeyPressed_ themselves before returning Pass, preserving the original
// "any non-modifier key invalidates the modifier-only combo" semantics.
HookEngine::KeyOutcome HookEngine::RunTopGuards(DWORD vkCode) {
    PERF_SCOPE(::NextKey::Perf::Stage::TopGuard);
    // 0. Sync from SharedState. Fast path (post Pre-T3 Minor 2 fix) is
    //    fully lock-free — atomic ReadEpoch + atomic load of lastEpoch_,
    //    early-return on unchanged. Cost ~5 ns. The slow path (taken
    //    only when configGeneration bumped — user-paced Settings save,
    //    not chaos) acquires stateMutex_ + may run ReloadFromToml on
    //    this thread; that residual Rule #11.2 cost is bounded to one
    //    reload per generation bump (~10–50 ms once / minute of user
    //    config tweaking). Steady-state typing never reaches it.
    QuickSyncFromSharedState();

    // Extension updates normally cost one mapped 32-bit generation load. A
    // changed generation copies the bounded snapshot and publishes any TSF
    // ownership transition before this physical key reaches the target app.
    RefreshBrowserRouteOnHookThread(false, true);

    // 0b. TSF app — let TSF DLL handle all input, hook does nothing
    if (IsEffectiveTsf()) return KeyOutcome::Pass;

    // 1. Track modifiers for hotkey detection
    bool isModifier = (vkCode == VK_LCONTROL || vkCode == VK_RCONTROL ||
                       vkCode == VK_LSHIFT || vkCode == VK_RSHIFT ||
                       vkCode == VK_LMENU || vkCode == VK_RMENU ||
                       vkCode == VK_LWIN || vkCode == VK_RWIN);

    if (isModifier) {
        TrackModifier(vkCode, true);
        return KeyOutcome::Pass;  // Don't eat modifier keys
    }

    // 1b. Toggle keys (CapsLock, NumLock, ScrollLock) — pass through without
    // committing composition. CapsLock is commonly pressed mid-word to capitalize
    // the first letter of a Vietnamese word (e.g., CapsLock+G+CapsLock+iar → Giả).
    // Without this bypass, CapsLock would hit step 9 ("any other key → commit"),
    // splitting the word and producing wrong tone placement (Gỉa instead of Giả).
    if (vkCode == VK_CAPITAL || vkCode == VK_NUMLOCK || vkCode == VK_SCROLL) {
        return KeyOutcome::Pass;
    }

    // Domain-English is an overlay, not a mutation of the user's logical V/E
    // mode. Modifier-only hotkeys stay live (they dispatch from ProcessKeyUp);
    // non-modifier bindings pass through, same as an excluded app.
    if (browserRoute_.load(std::memory_order_acquire) == BrowserRoute::ForceEnglish) {
        // Same contract as the excluded-app returns below: a guard that skips
        // ProcessKeyDown's post-switch bookkeeping must invalidate the
        // modifier-alone window itself, or Ctrl+C here reads as a clean Ctrl
        // release and fires the V/E toggle on key-up.
        otherKeyPressed_ = true;
        for (int i = 0; i < kModCount; ++i) modTapCount_[i] = 0;
        return KeyOutcome::Pass;
    }

    // 1c. Excluded app — full passthrough (IME is transparent to this app)
    // Fast PID check: same process → passthrough immediately (no syscall overhead).
    // Different PID → verify with full exe name lookup (only on actual app switch).
    if (isExcludedApp_.load(std::memory_order_acquire)) {
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        if (fgPid == excludedPid_.load(std::memory_order_acquire)) {
            otherKeyPressed_ = true;
            return KeyOutcome::Pass;  // Same process — still excluded
        }
        // Different process — verify if we actually left the excluded app
        if (VerifyExcludedState()) {
            excludedPid_.store(fgPid, std::memory_order_release);  // Switched to another excluded app
            otherKeyPressed_ = true;
            return KeyOutcome::Pass;
        }
        excludedPid_.store(0, std::memory_order_release);
        NotifyModeChange();
        // Fall through to normal processing for this keystroke
    }

    return KeyOutcome::Fallthrough;
}

// H1a: commit-undo state machine extracted from ProcessKeyDown step 2d.
// Supports multi-word backward — stack holds up to CommitState::kMaxStack committed words.
// Ready:  set after commit with space/enter, or when engine empties after BS with stack non-empty.
// Primed: BS in Ready deletes the space; next alpha/BS triggers replay.
//
// Behavior is byte-identical to the pre-extraction inline block. Returns:
//   Eat         → ProcessKeyDown returns true (key consumed by undo machinery).
//   Pass        → ProcessKeyDown returns false (key passes through to app).
//   Fallthrough → no decision; ProcessKeyDown continues with subsequent steps.
HookEngine::KeyOutcome HookEngine::HandleCommitUndoFsm(DWORD vkCode, bool vnMode,
                                                       bool cachedShift, bool cachedCapsLock,
                                                       bool cachedCtrl, bool cachedAlt,
                                                       bool cachedWin) {
    // Remember which punctuation opened the current word (":" in ":smile:")
    // for IsHostReplacedWord at commit time. Modifier-only keys never reach
    // here, so Shift+';' still sees ':' as the previous key. Translate only OEM
    // punctuation keys to keep ToUnicodeEx off the per-letter path.
    if (vkCode >= 'A' && vkCode <= 'Z' && engine_->Count() == 0) {
        commitState_.SetWordOpenGlyph(commitState_.PrevKeyGlyph());
    }
    commitState_.SetPrevKeyGlyph(
        vkCode >= VK_OEM_1 && IsCommitTrigger(vkCode) ? VkToMacroChar(vkCode) : L'\0');

    // Ctrl/Alt/Win invalidate commit-undo: Ctrl+BS deletes entire word (not just the
    // space), Ctrl+A/C/Z change cursor/selection — all make saved commit state stale.
    // Must check BEFORE the state machine to prevent ghost key replay.
    if (!commitState_.IsIdle() &&
        (cachedCtrl || cachedAlt || cachedWin)) {
        HOOK_LOG(L"  commit-undo: cancel — modifier key held");
        CancelCommitUndo();
        // Fall through — Ctrl check at ProcessKeyDown step 5 will handle ResetComposition
    }

    // Enter (VK_RETURN) doesn't move focus in chat/form inputs — cursor
    // stays in the same input box on message-send / line-break. That
    // bypasses the focus-event safety net that Tab + mouse-click rely on
    // to clear commitStack_ via ResetComposition. Without this explicit
    // cancel, the stack survives across message-send boundaries: user
    // sends "không," then starts a new message with "vaf"; the BS chain
    // they use to correct a typo in the new message replays the phantom
    // "không" prefix into the engine; IsHardEnglishToneContext sees the
    // concat-buffer V-CC-V pattern and blocks tone on the new word.
    // Existing line ~1319 already cancels Enter while state==Ready;
    // this branch guards the gap where a prior non-exempt key (e.g.
    // SPACE) downgraded state to Idle but left the stack populated.
    // Idempotent — no-op when state==Idle && stack already empty.
    if (vkCode == VK_RETURN &&
        (!commitState_.IsIdle() || !commitState_.StackEmpty())) {
        HOOK_LOG(L"  commit-undo: cancel — VK_RETURN (stack=%zu state=%d)",
                 commitState_.StackSize(), static_cast<int>(commitState_.Current()));
        CancelCommitUndo();
        // Fall through — Enter still passes through to the app normally.
    }
    //
    // Auto-expire Ready after kCommitUndoTimeoutMs: cheap insurance against any cursor-movement
    // event that bypasses ResetComposition (e.g. external text change, rare edge cases).
    if (commitState_.IsReady()) {
        DWORD elapsed = GetTickCount() - commitState_.ReadyTime();
        if (elapsed > kCommitUndoTimeoutMs) {
            HOOK_LOG(L"  commit-undo: Ready state expired after %u ms → Idle", elapsed);
            CancelCommitUndo();
        }
    }
    if (commitState_.IsReady() && vkCode == VK_BACK && engine_->Count() == 0) {
        if (commitState_.PendingTriggerCount() > 0) {
            // Extra trigger chars still on screen (e.g., "a==" → need to delete both '=' before undo)
            commitState_.DecrementPendingTriggers();
            HOOK_LOG(L"  commit-undo: BS in Ready, pendingTriggers=%u — stay Ready", commitState_.PendingTriggerCount());
            return KeyOutcome::Pass;  // Let BS pass through to delete the extra trigger char
        }
        // Backspace deletes the commit trigger (space/etc.)
        commitState_.SetPrimed();
        // Any accumulated multi-word-macro state is stale once replay begins —
        // the phrase buffer no longer mirrors what's on screen.
        macroCrossCommit_ = false;
        ClearMacroBuffer();
        if (dispatcher_.SynthEventsPending() > 0) {
            // Synthetic events still in flight (word corrections, injected commit trigger).
            // If we pass BS through now it arrives at the app BEFORE those synthetics,
            // deleting the wrong character and permanently desynchronising previousComposition_.
            // Re-inject so BS is placed AFTER the pending synthetics in the queue.
            HOOK_LOG(L"  commit-undo: BS after commit → Primed, re-inject after synthetics (pending=%d)", dispatcher_.SynthEventsPending());
            InjectKey(VK_BACK);
            return KeyOutcome::Eat;
        }
        // Sprint 1 Fix C/2026-05-05: editMsg apps need this BS via the sent
        // EM_REPLACESEL channel — passing the physical BS through goes via the
        // posted message queue and is pre-empted by the next sent EM_REPLACESEL
        // (the 's' in chaos 5.3), leaving the pre-replace BS to drain after
        // the replacement and eat the just-inserted chars.
        //
        // The synchronous-channel injector (RichEditEm) handles commit-undo BS
        // via sent message. Default hosts let physical BS pass through naturally —
        // synthesizing would just add latency.
        if (dispatcher_.IsSyncReplaceChannel()) {
            auto inj = dispatcher_.GetInjector();
            bool injOk;
            { PERF_SCOPE(::NextKey::Perf::Stage::Injector);
              injOk = inj->Replace(/*bs=*/1, std::wstring_view{}); }
            if (injOk) {
                HOOK_LOG(L"  commit-undo: BS after commit via injector → Primed");
                return KeyOutcome::Eat;
            }
            HOOK_LOG(L"  commit-undo: BS after commit injector failed, passthrough");
        }
        HOOK_LOG(L"  commit-undo: BS after commit → Primed (ready to replay)");
        return KeyOutcome::Pass;  // Let backspace pass through to delete the space
    }
    if (commitState_.IsPrimed() && engine_->Count() == 0 && vnMode) {
        // Synth guard: if synthetic events were sent recently and are likely still
        // in the OS input queue, replaying now would set previousComposition_ to stale
        // committed text while the screen hasn't caught up — causing diff miscalculation
        // and permanent engine-screen desync.  Cancel commit-undo and fall through to
        // normal key processing.
        // Time check is essential: on Qt apps, synthEventsPending_ has a persistent
        // baseline leak (counter never reaches 0 due to event counting mismatch).
        // Checking counter alone would permanently disable commit-undo.  The 100ms
        // threshold covers DispatchSendInput Sleep (10-20ms) + Qt processing (~30ms)
        // with margin, while allowing replay at normal typing speed (>100ms between keys).
        //
        // Sprint 2 D1/2026-05-05: tone modifiers (Telex s/f/r/x/j; VNI 1-5) are
        // EXEMPT from the synth guard. Reason: by definition they only modify the
        // previous word — no other linguistic meaning. ReplaceComposition's diff
        // (prev=committed, new=committed-with-tone) computes BS correctly relative
        // to the post-drain screen state, and SendInput appends our events AFTER
        // any pending synth, so screen-engine sync is preserved across the gap.
        // Without this exemption, chaos 5.3 (`viejtnam BS×4 s` on non-EditMsg apps
        // like Chrome) cancels the replay and produces `việts` instead of `viết`.
        // See docs/baselines/perf-baseline-d12-chrome-cross-app.md and the
        // S2D0_ChromeBug53_* engine-isolation tests.
        // Exemption rule shared by the synth-guard and catch-all cancel
        // branches: modifier letters (Telex/SimpleTelex/Combined
        // s/f/r/x/j/z/a/e/o/w/d), VNI digits 0-9, UserDefined keys whose
        // customKeyMap action passes IsCommitUndoExemptAction, and ESC
        // restore-raw all semantically "modify the previous word" — they
        // must not demote / cancel commit-undo state. Extracted to
        // core/CommitUndoExemption.h for Linux GTest coverage (HookEngine.cpp
        // is Win32-only). See design 2026-05-17 + 2026-05-21b broadening.
        const auto methodForExempt = currentMethod_.load(std::memory_order_acquire);
        const bool shiftHeld = cachedShift;
        // Source of truth: registry snapshot — Esc only exempts when bound to
        // CancelComposition AND the intent is enabled. Removed in v3 cleanup:
        // legacy `escRestoreRawEnabled_` atomic. Matches() rejects modifier-vk
        // on DOWN so passing keyUp=false is safe for plain Esc.
        const auto hotkeysForExempt = hotkeys_.load(std::memory_order_acquire);
        const bool escIsCancelTrigger =
            hotkeysForExempt &&
            hotkeysForExempt->Matches(Intent::CancelComposition, VK_ESCAPE,
                                       /*mods=*/0, /*isDoubleTap=*/false,
                                       /*keyUp=*/false);
        // UserDefined modifier lookup: customKeyMap can bind any key to
        // a tone/modifier action, so the hardcoded letter/digit lists
        // don't apply. Resolve vk → ASCII via VkToMacroChar (same path
        // step 6d uses) and ask IsCommitUndoExemptAction whether the
        // mapped action belongs to the "modifies previous word" class.
        bool isCustomModifier = false;
        if (methodForExempt == InputMethod::UserDefined) {
            const wchar_t ch = VkToMacroChar(vkCode);
            if (ch && ch < 128) {
                const TypingAction action =
                    config_.load(std::memory_order_acquire)
                        ->customKeyMap[static_cast<uint8_t>(ch)];
                isCustomModifier = IsCommitUndoExemptAction(action);
            }
        }
        const bool isCommitUndoExempt = IsCommitUndoExemptKey(
            vkCode, methodForExempt, shiftHeld, escIsCancelTrigger,
            isCustomModifier);
        // Second exemption class, wider than the semantic one above: a literal
        // coda the engine says keeps the committed word a live Vietnamese shape
        // ('n' after 'nghieej'). It is safe under the same synth-guard argument
        // for a *weaker* reason — an append diffs to zero BS plus one char, so
        // it cannot mis-BS against a screen that has not drained yet, whereas a
        // tone modifier rewrites in place and needs the exact post-drain state.
        bool isValidCommittedContinuation = false;
        if (!isCommitUndoExempt && vkCode >= 0x41 && vkCode <= 0x5A &&
            !commitState_.StackEmpty()) {
            wchar_t continuation = static_cast<wchar_t>(vkCode);
            if (cachedShift == cachedCapsLock) continuation = towlower(continuation);
            isValidCommittedContinuation = engine_->ShouldReplayCommittedKey(
                commitState_.StackTop().rawInput, continuation);
        }
        const bool shouldReplayCommittedWord =
            isCommitUndoExempt || isValidCommittedContinuation;
        // Sprint 2 D5: settle window is now per-host. RichEdit (0 ms) lets
        // commit-undo replay immediately; Win32 (30 ms) tightens the gate
        // ~3× vs the legacy 100 ms hardcode; Electron/Console (100 ms) keeps
        // the original budget where IPC reorder margin still matters. Read
        // here, not cached, so a focus change between commit and the next
        // BS uses the new injector's budget.
        const DWORD settleMs = static_cast<DWORD>(
            dispatcher_.GetInjector()->SettleBudget().count());
        if (dispatcher_.SynthEventsPending() > 0 && (GetTickCount() - dispatcher_.LastRealSynthTime()) < settleMs
            && !shouldReplayCommittedWord) {
            HOOK_LOG(L"  commit-undo: cancel Primed — synthPending=%d, vk=0x%02X",
                     dispatcher_.SynthEventsPending(), vkCode);
            CancelCommitUndo();
            // Fall through — ProcessKeyDown step 10 re-injects BS if needed; alpha → step 6 HandleAlphaKey
        } else if (vkCode >= 0x41 && vkCode <= 0x5A) {
            // Discriminate alpha intent at Primed: a semantic modifier or an
            // engine-approved literal continuation of the committed raw input
            // reopens the word. Other alphas mean the user started a new word;
            // DROP stack-top to prevent a later BS-into-empty from re-priming
            // Ready for it, then let the alpha enter fresh composition. Without
            // this, catch-all replay concatenated an older stack entry into the
            // new word (engine/screen divergence).
            if (!shouldReplayCommittedWord) {
                HOOK_LOG(L"  commit-undo: drop stack-top '%s' for non-tone alpha '%c' → fresh composition",
                         commitState_.StackEmpty() ? L"<empty>" : commitState_.StackTop().text.c_str(),
                         static_cast<char>(vkCode));
                if (!commitState_.StackEmpty()) {
                    commitState_.PopStackTop();
                }
                commitState_.SetIdle();
                return KeyOutcome::Fallthrough;
            }
            // MUST return HandleAlphaKey's value: if it triggers passthrough (return false),
            // the original key must reach the app — ignoring it would swallow the keystroke.
            HOOK_LOG(L"  commit-undo: replaying + eligible alpha '%c' (stack_top='%s' stackSize=%zu prevComp='%s' synthPending=%d)",
                     static_cast<char>(vkCode),
                     commitState_.StackEmpty() ? L"<empty>" : commitState_.StackTop().text.c_str(),
                     commitState_.StackSize(),
                     previousComposition_.c_str(),
                     dispatcher_.SynthEventsPending());
            if (!ReplayCommittedChars()) {
                return KeyOutcome::Pass;
            }
            {
                bool shift = cachedShift;
                bool caps = cachedCapsLock;
                return HandleAlphaKey(vkCode, shift, caps)
                    ? KeyOutcome::Eat
                    : KeyOutcome::Pass;
            }
        } else if (const InputMethod method = currentMethod_.load(std::memory_order_acquire);
                   (method == InputMethod::VNI || method == InputMethod::Combined ||
                    method == InputMethod::UserDefined) &&
                   vkCode >= 0x30 && vkCode <= 0x39 &&
                   !cachedShift) {
            // VNI/Combined/UserDefined digit key (0-9) → replay saved chars, then process
            // as tone/modifier. Without this, "cá " + BS + '2' would produce "cá2" instead
            // of "cà". '0' is VNI clear-tone; UserDefined may map any digit via customKeyMap.
            HOOK_LOG(L"  commit-undo: replaying + VNI digit '%c' (stack_top='%s' stackSize=%zu prevComp='%s')",
                     static_cast<char>(vkCode),
                     commitState_.StackEmpty() ? L"<empty>" : commitState_.StackTop().text.c_str(),
                     commitState_.StackSize(),
                     previousComposition_.c_str());
            if (!ReplayCommittedChars() || engine_->Count() == 0) {
                commitState_.SetIdle();
                return KeyOutcome::Pass;  // Replay failed — let digit pass through
            }
            return HandleVniDigitKey(vkCode)
                ? KeyOutcome::Eat
                : KeyOutcome::Pass;
        } else if (vkCode == VK_BACK) {
            // Backspace → replay saved chars, then backspace into the word
            HOOK_LOG(L"  commit-undo: replaying + backspace (stack_top='%s' stackSize=%zu prevComp='%s' synthPending=%d)",
                     commitState_.StackEmpty() ? L"<empty>" : commitState_.StackTop().text.c_str(),
                     commitState_.StackSize(),
                     previousComposition_.c_str(),
                     dispatcher_.SynthEventsPending());
            if (!ReplayCommittedChars(/*preferVisibleText=*/true)) {
                return KeyOutcome::Pass;
            }
            HandleBackspace();
            return KeyOutcome::Eat;
        } else if (!isCommitUndoExempt) {
            // Any other key → cancel commit-undo.
            // Exempt keys (tone modifiers, ESC restore-raw) keep state Primed
            // so the downstream replay / restore handlers can read commitStack_.
            // Punctuation glued to the committed word ("ạ:" then "D" → "ạ😀")
            // can be merged by the host, so drop the stack instead of keeping
            // it for a later BS-into-empty re-arm.
            const bool isTrigger = IsCommitTrigger(vkCode);
            if (ShouldCancelPrimedOnTrigger(isTrigger,
                                            isTrigger ? VkToMacroChar(vkCode) : L'\0')) {
                HOOK_LOG(L"  commit-undo: cancel Primed — punctuation glued to committed word (vk=0x%02X)",
                         vkCode);
                CancelCommitUndo();
            } else {
                commitState_.SetIdle();
            }
        }
    }
    if (commitState_.IsReady()) {
        // Navigation keys move cursor → stack entries become stale, clear everything.
        if ((vkCode >= VK_LEFT && vkCode <= VK_DOWN) ||
            vkCode == VK_HOME || vkCode == VK_END ||
            vkCode == VK_PRIOR || vkCode == VK_NEXT ||
            vkCode == VK_DELETE) {
            HOOK_LOG(L"  commit-undo: cancel — navigation key vk=0x%02X", vkCode);
            CancelCommitUndo();
        } else {
            // Only spaces are counted into the gap: punctuation can be merged by
            // the host (Teams ":D" → one emoji glyph), after which counted BSes
            // drift onto the committed word. See DecideReadyGapKey.
            const bool isTrigger = IsCommitTrigger(vkCode) && engine_->Count() == 0;
            const wchar_t ch = isTrigger ? VkToMacroChar(vkCode) : L'\0';
            switch (DecideReadyGapKey(isTrigger, ch, commitState_.PendingTriggerCount(),
                                      commitState_.GapEndsInGlyph())) {
                case ReadyGapAction::TrackSpace:
                    commitState_.IncrementPendingTriggers();
                    HOOK_LOG(L"  commit-undo: extra space in Ready, pendingTriggers=%u",
                             commitState_.PendingTriggerCount());
                    break;
                case ReadyGapAction::Cancel:
                    HOOK_LOG(L"  commit-undo: cancel — gap no longer countable (vk=0x%02X ch=0x%04X)",
                             vkCode, static_cast<unsigned>(ch));
                    CancelCommitUndo();
                    break;
                case ReadyGapAction::StartWord:
                    // New word after a space gap → preserve stack for multi-word backward.
                    // Carry the pending space count onto the in-progress word so it travels
                    // with the CommitEntry — without this, "chịu  a " then BS×4 + 's' would
                    // forget the second space and replay before it is deleted.
                    commitState_.SetLeadingTriggersForCurrentWord(commitState_.PendingTriggerCount());
                    commitState_.ResetPendingTriggers();
                    commitState_.SetGapEndsInGlyph(false);
                    commitState_.SetIdle();
                    break;
            }
        }
    }
    return KeyOutcome::Fallthrough;
}

// H1c: English-mode short-circuit + Vietnamese pre-dispatch tracking
// (extracted from ProcessKeyDown steps 3 / 3a-3d).
//
//   Step 3   — !vnMode early-out with English-mode macro tracking. Macro
//              keys accumulate in rawMacroBuffer_; commit triggers attempt
//              expansion; Esc with empty buffer arms tempMacroOff_; non-
//              alpha non-trigger keys clear the buffer at word boundary.
//   Step 3a  — Auto-caps state machine (Idle/AfterPunct/ReadyToCapitalize).
//              Punctuation '.', '?', '!' arms AfterPunct; subsequent space
//              promotes to ReadyToCapitalize; Enter also promotes.
//   Step 3b  — Macro tracking on the Vietnamese path: alpha keys lower-cased
//              (or upper-cased per shift XOR caps), printable triggers join
//              the buffer for multi-char macro key matching.
//   Step 3c  — Temp-off-by-Esc: Esc with engine empty + buffer empty arms
//              tempMacroOff_ for the next word.
//   Step 3d  — Macro expansion on commit trigger via TryExpandMacro.
//
// Behavior is byte-identical to the pre-extraction inline block. Returns:
//   Eat         → ProcessKeyDown returns true (macro expansion ate trigger).
//   Pass        → ProcessKeyDown returns false (English-mode passthrough,
//                 ExpandedPassTrigger without synth, or Esc temp-off arming).
//   Fallthrough → continue to DispatchKeyAction (vnMode + no expansion).
HookEngine::KeyOutcome HookEngine::HandlePreDispatch(DWORD vkCode, bool vnMode,
                                                      bool cachedShift,
                                                      bool cachedCtrl, bool cachedAlt,
                                                      bool cachedWin) {
    // Snapshot the user's hotkey registry once for this key event. RCU
    // pattern: hot path readers grab the shared_ptr; the publisher
    // (ApplyHotkeyRegistry) replaces the pointer without invalidating
    // in-flight readers.
    const auto hotkeysSnap = hotkeys_.load(std::memory_order_acquire);
    // Note: configSnapshot_ + macroTable were read here pre-W4b to gate the
    // inline macro blocks. Those moved to MacroFeature (W4b) at step 2d so
    // the snapshot load is no longer needed in HandlePreDispatch.
    const uint32_t currentMods = ComputeModMask(cachedCtrl, cachedShift, cachedAlt, cachedWin);

    // 3. English mode — skip Vietnamese processing.
    // EN-mode macro tracking + dispatch is now owned by MacroFeature (W4b)
    // at PreEngine step 2d. If the feature consumed/passed the key (Eat/Pass
    // outcomes), ProcessKeyDown returned before reaching HandlePreDispatch.
    // Reaching here means EN mode with no macro engagement — just pass through.
    if (!vnMode) {
        HOOK_LOG(L"  skip: Vietnamese mode OFF");
        return KeyOutcome::Pass;
    }

    // 3a. Auto-caps state machine (Vietnamese mode only). Rule + modifier gate
    // live in core/AutoCapStateTransition.h — Ctrl+Enter / Ctrl+. / Win+. etc.
    // are passed through unchanged so the dispatcher's step 5 modifier guard
    // can reset composition without first arming ReadyToCapitalize.
    if (autoCaps_.load(std::memory_order_acquire)) {
        autoCapState_ = ComputeAutoCapStateTransition(
            autoCapState_, vkCode, cachedShift, cachedCtrl, cachedAlt, cachedWin);
    }

    // 3b. Macro tracking moved to MacroFeature (W4b) at PreEngine step 2d.
    // Pre-W4b accumulated alpha + commit-trigger chars into rawMacroBuffer_ here;
    // now the executor adapter HookEngine::HandleMacro owns that mutation.

    // 3b'. Esc-restore-raw: handled at PreEngine step 2d by EscRestoreRawFeature
    // (Wave 4a). If ESC matched the CancelComposition hotkey and had live/primed
    // composition, the feature emitted Intents::ConsumeKey and ProcessKeyDown
    // returned true before reaching HandlePreDispatch. If we got here, ESC did
    // not match (or no composition was active) — fall through to ToggleEnabled.
    // Post-W4a: MOD-CANCEL secondary site at line 1981 (modifier-release path)
    // still calls TryEscRestoreRaw inline — different trigger flow.

    // 3b''. ToggleEnabled — non-modifier binding (F-key, letter+chord, Esc+mods…)
    // fires on DOWN with exact mods match. Modifier-bound ToggleEnabled lives
    // in ProcessKeyUp's modifier-release dispatch (modifier-alone / double-tap
    // detection). The IsModifierKey gate avoids double-firing for modifier vk.
    // Double-tap on non-modifier keys is currently NOT tracked by HookEngine
    // (modTapCount_ only covers Ctrl/Shift/Alt/Win) — bindings with
    // doubleTap=true on non-modifier vk are accepted by the Hotkeys UI but
    // never fire here. Acceptable v3 limitation; track via Matches() with
    // isDoubleTap=false so only single-tap triggers match.
    if (!IsModifierKey(CanonicalModifierVk(vkCode))
        && hotkeysSnap->Matches(Intent::ToggleEnabled, vkCode, currentMods,
                                 /*isDoubleTap=*/false, /*keyUp=*/false)) {
        if (engine_->Count() > 0) CommitComposition(true);
        tempEngineOff_ = !tempEngineOff_;
        CancelCommitUndo();
        HOOK_LOG(L"  TOGGLE-DOWN (vk=0x%02X mods=0x%02X): tempEngineOff_=%d",
                 vkCode, currentMods, tempEngineOff_ ? 1 : 0);
        return KeyOutcome::Eat;
    }

    // 3b'''. ToggleGameMode — same non-modifier DOWN shape as ToggleEnabled
    // above. Deliberately does NOT eat the key: a game-mode binding is meant to
    // be pressed inside a game, and swallowing it would hide whatever the game
    // has bound to the same combo. The action is idempotent per press, so a
    // duplicate delivery to the game is the safer failure.
    if (!IsModifierKey(CanonicalModifierVk(vkCode))
        && hotkeysSnap->Matches(Intent::ToggleGameMode, vkCode, currentMods,
                                 /*isDoubleTap=*/false, /*keyUp=*/false)) {
        RequestGameModeToggle();
        HOOK_LOG(L"  GAMEMODE-DOWN (vk=0x%02X mods=0x%02X): queued for worker",
                 vkCode, currentMods);
    }

    // 3c, 3d. SkipMacro hotkey + macro expansion — owned by MacroFeature
    // (W4b) at PreEngine step 2d. Reaching this point means the feature
    // returned Fallthrough/NoOp (no macro engagement); fall through to the
    // post-HandlePreDispatch dispatcher (step 4+).
    return KeyOutcome::Fallthrough;
}

// H1c: action dispatch chain (extracted from ProcessKeyDown steps 4b-10).
//
//   Step 4b — tempEngineOff_ bypass: vnMode is ON but temporarily disabled
//             for current word (commit trigger or BS-on-empty resets it).
//   Step 5  — Ctrl/Alt/Win shortcut skip: ResetComposition + passthrough.
//   Step 6  — A-Z alpha key → HandleAlphaKey (returns Eat if engine consumed
//             the key, Pass if it triggered passthrough mid-word).
//   Step 6b — Telex bracket [/] → engine modifier for ơ/ư.
//   Step 6c — VNI/Combined digit 1-9 with engine non-empty → HandleVniDigitKey.
//   Step 6d — UserDefined OEM punctuation bound via customKeyMap → engine PushChar.
//   Step 7  — Backspace with engine non-empty → HandleBackspace + Eat.
//   Step 7b — Backspace with cross-commit macro buffer: update tracking,
//             pass through (no return — falls into step 8/9/10).
//   Step 8  — Commit trigger with engine non-empty: macro buffer preservation
//             across the commit, trigger re-injection after pending synth,
//             RichEdit synchronous-channel routing.
//   Step 9  — Any other key with engine non-empty → commit + InjectKey.
//   Step 10 — Backspace with engine empty + synth pending → re-inject BS to
//             preserve ordering after in-flight word corrections.
//
// Behavior is byte-identical to the pre-extraction inline block. Returns:
//   Eat  → ProcessKeyDown returns true (key consumed).
//   Pass → ProcessKeyDown returns false (passthrough — final fallthrough also
//          maps here; the original code's tail `return false` is preserved).
HookEngine::KeyOutcome HookEngine::DispatchKeyAction(DWORD vkCode, bool cachedShift,
                                                      bool cachedCapsLock, bool cachedCtrl,
                                                      bool cachedAlt, bool cachedWin, bool macroOn) {
    // 4b. Temp-off bypass: Vietnamese mode is ON but temporarily disabled for current word
    if (tempEngineOff_) {
        if (IsCommitTrigger(vkCode)) {
            tempEngineOff_ = false;
            digitLedWord_ = false;  // Word ended — digit-led state is moot
            HOOK_LOG(L"  tempEngineOff: reset on commit trigger vk=0x%02X", vkCode);
        } else if (vkCode == VK_BACK && engine_->Count() == 0) {
            tempEngineOff_ = false;
            digitLedWord_ = false;
            HOOK_LOG(L"  tempEngineOff: reset on backspace (engine empty)");
        }
        HOOK_LOG(L"  skip: tempEngineOff_ active=%d", tempEngineOff_ ? 1 : 0);
        return KeyOutcome::Pass;  // Pass through as English
    }

    // 5. Skip if Ctrl/Alt/Win is down (allow shortcuts to pass through)
    if (cachedCtrl || cachedAlt || cachedWin) {
        HOOK_LOG(L"  skip: modifier held (ctrl=%d alt=%d win=%d)", cachedCtrl, cachedAlt, cachedWin);
        // Always reset — shortcuts change text state in unpredictable ways.
        // Commit-undo is already canceled at step 2d (modifier guard), but
        // ResetComposition also clears engine, previousComposition_, inputHistory_, etc.
        // ResetComposition → ClearWordState also drops digit-led state.
        ResetComposition();
        return KeyOutcome::Pass;
    }

    // 5b. Digit-led word state machine: arm on digit at word start (VNI/Combined/
    // UserDefined), bypass while armed, reset on whitespace/nav/Esc/BS/Delete.
    // Single source of truth in core/DigitLedWordDecision.h.
    {
        DigitLedInputs in{
            vkCode, cachedShift,
            engine_->Count() == 0,
            currentMethod_.load(std::memory_order_acquire),
            digitLedWord_,
        };
        switch (DecideDigitLed(in)) {
            case DigitLedDecision::Arm:
                digitLedWord_ = true;
                HOOK_LOG(L"  digitLedWord: armed by vk=0x%02X", vkCode);
                return KeyOutcome::Pass;
            case DigitLedDecision::Bypass:
                HOOK_LOG(L"  digitLedWord: bypass (active)");
                return KeyOutcome::Pass;
            case DigitLedDecision::Reset:
                digitLedWord_ = false;
                HOOK_LOG(L"  digitLedWord: reset on vk=0x%02X", vkCode);
                return KeyOutcome::Pass;
            case DigitLedDecision::ResetAndContinue:
                digitLedWord_ = false;
                HOOK_LOG(L"  digitLedWord: reset stale latch on vk=0x%02X; continue live composition",
                         vkCode);
                break;
            case DigitLedDecision::Continue:
                break;
        }
    }

    // 6. A-Z keys → process with engine
    if (vkCode >= 0x41 && vkCode <= 0x5A) {
        HOOK_LOG(L"  alpha key '%c' → HandleAlphaKey", static_cast<char>(vkCode));
        return HandleAlphaKey(vkCode, cachedShift, cachedCapsLock)
            ? KeyOutcome::Eat
            : KeyOutcome::Pass;
    }

    const InputMethod method = currentMethod_.load(std::memory_order_acquire);

    // 6b. Bracket keys [ ] → engine modifier for Full Telex ([ → ơ, ] → ư).
    // Preserve the OS-visible punctuation identity for raw replay/escape while
    // carrying Shift-XOR-Caps output case independently.
    if (method == InputMethod::Telex &&
        (vkCode == VK_OEM_4 || vkCode == VK_OEM_6)) {
        const wchar_t base = (vkCode == VK_OEM_4) ? L'[' : L']';
        const BracketKey key = ResolveBracketKey(base, cachedShift, cachedCapsLock);
        commitState_.AppendHistory(key.character);
        std::wstring composition;
        { PERF_SCOPE(::NextKey::Perf::Stage::EnginePush);
          engine_->PushKey(key.character, key.uppercase); composition = engine_->Peek(); }
        HOOK_LOG(L"  bracket '%c' upper=%d → Peek()='%s'",
                 key.character, key.uppercase, composition.c_str());
        DispatchCoordinator(vkCode, 0, composition);
        return KeyOutcome::Eat;  // Eat the original keystroke
    }

    // 6c. VNI/Combined/UserDefined: digit keys 0-9 → tone/modifier input (only with
    // pending composition). VNI '0' clears tone; UserDefined may remap any digit via
    // customKeyMap (unmapped digits fall through as ProcessChar literal inside engine).
    // The "digit at word start with engine empty" case is already armed and returned
    // at step 5b above; this branch only sees mid-word digits.
    if ((method == InputMethod::VNI || method == InputMethod::Combined ||
         method == InputMethod::UserDefined) &&
        vkCode >= 0x30 && vkCode <= 0x39 &&
        engine_->Count() > 0) {
        if (!cachedShift) {
            return HandleVniDigitKey(vkCode) ? KeyOutcome::Eat : KeyOutcome::Pass;
        }
    }

    // 6d. UserDefined: OEM punctuation bound via customKeyMap → tone/modifier
    // input. Without this branch OEM keys hit step 8 IsCommitTrigger first and
    // never reach engine_->PushChar, so e.g. customKeyMap[';'] = ToneDot would
    // be dead. UserDefined-only by design — VNI/Combined keep digit-only reach.
    //
    // Empty-buffer gate: tone/modifier actions need an existing vowel target,
    // so we keep them mid-word-only. Insert-type actions (HornInsertO/U,
    // Insert*, HornOrInsertU plain) synthesise fresh state and MUST fire at
    // word start too — user feedback 2026-05-17: `[`/`]` bound to HornInsertO/U
    // produced literal `[`/`]` instead of ơ/ư at word start.
    if (method == InputMethod::UserDefined && IsOemPunctVk(vkCode)) {
        wchar_t ch = VkToMacroChar(vkCode);
        bool bracketCaseIntent = false;
        bool uppercase = false;
        if (ch && ch < 128) {
            auto cfg = config_.load(std::memory_order_acquire);
            TypingAction action = cfg->customKeyMap[static_cast<uint8_t>(ch)];
            const wchar_t canonical = CanonicalBracketKey(ch);
            if (action == TypingAction::None && canonical != ch) {
                action = cfg->customKeyMap[static_cast<uint8_t>(canonical)];
            }
            if (action != TypingAction::None &&
                (engine_->Count() > 0 || IsInsertTypeAction(action))) {
                if ((action == TypingAction::HornInsertO ||
                     action == TypingAction::HornInsertU) &&
                    (vkCode == VK_OEM_4 || vkCode == VK_OEM_6)) {
                    const BracketKey key = ResolveBracketKey(
                        canonical, cachedShift, cachedCapsLock);
                    ch = key.character;
                    uppercase = key.uppercase;
                    bracketCaseIntent = true;
                }
                commitState_.AppendHistory(ch);
                std::wstring composition;
                { PERF_SCOPE(::NextKey::Perf::Stage::EnginePush);
                  if (bracketCaseIntent) engine_->PushKey(ch, uppercase);
                  else engine_->PushChar(ch);
                  composition = engine_->Peek(); }
                HOOK_LOG(L"  UserDefined OEM '%c' → Peek()='%s'", ch, composition.c_str());
                DispatchCoordinator(vkCode, 0, composition);
                return KeyOutcome::Eat;
            }
        }
    }

    // 7. Backspace → engine backspace if we have content
    if (vkCode == VK_BACK && engine_->Count() > 0) {
        // HandleMacro already removed this key from rawMacroBuffer_.
        HOOK_LOG(L"  backspace (engine count=%zu)", engine_->Count());
        HandleBackspace();
        return KeyOutcome::Eat;  // Eat backspace
    }

    // 7c. Escape: modal interrupt / cancel. Finalize composition verbatim
    // without triggering boundary autocorrection (e.g., "thuờ" mutating to "thù").
    // If synthetic events are in flight (even when engine is already empty), eat and
    // re-inject Esc (mirroring step 10 backspace rule) so Esc cannot overtake in-flight text.
    if (vkCode == VK_ESCAPE) {
        const int pending = dispatcher_.SynthEventsPending();
        if (engine_->Count() > 0) {
            HOOK_LOG(L"  VK_ESCAPE with active composition → ResetComposition (verbatim preserve)");
            ResetComposition();
        }
        if (pending > 0) {
            InjectKey(vkCode);
            return KeyOutcome::Eat;
        }
        return KeyOutcome::Pass;
    }

    // 8. Commit triggers: space, enter, tab, punctuation, numbers, arrows
    if (IsCommitTrigger(vkCode) && engine_->Count() > 0) {
        HOOK_LOG(L"  commit trigger vk=0x%02X", vkCode);

        // Preserve macro buffer across commit for printable triggers (e.g., '.' in "a.i")
        // so macros with punctuation in their key can still be matched on the final trigger.
        // Also preserve across SPACE when the accumulated prefix matches a stored space-
        // containing key — enables multi-word macros like "oc om bok" = "Óoc Om Bok".
        std::wstring savedMacroBuffer;
        // ClearWordState() inside CommitComposition() resets the auto-cap flag, so it
        // has to ride along with the buffer — otherwise a space/punctuation macro typed
        // at sentence start expands lowercase in the hook while TSF (which gates the
        // reset on macroCrossCommit_) capitalizes it.
        const bool savedAutoCapped = wasFirstCharAutoCapped_;
        // Phase 3c: macro presence + spaceMacroKeys come from the RCU
        // snapshot. Local shared_ptr keeps both alive through the branch.
        auto cfgSnap = configSnapshot_.load(std::memory_order_acquire);
        if (macroOn && cfgSnap && !cfgSnap->macroTable.empty()
            && !tempMacroOff_ && !rawMacroBuffer_.empty()) {
            wchar_t ch = VkToMacroChar(vkCode);
            if (ch > L' ') {
                savedMacroBuffer = rawMacroBuffer_;
            } else if (ch == L' '
                       && IsSpaceMacroPrefix(rawMacroBuffer_ + L' ',
                                             cfgSnap->macroPrefixIndex)) {
                savedMacroBuffer = rawMacroBuffer_ + L' ';
            }
        }

        bool restored = CommitComposition(
            Macro::IsVerbatimCommitTrigger(VkToMacroChar(vkCode)));

        if (!savedMacroBuffer.empty()) {
            rawMacroBuffer_ = std::move(savedMacroBuffer);
            macroCrossCommit_ = true;
            wasFirstCharAutoCapped_ = savedAutoCapped;
        }
        // Decide what happens to the commit-undo window now that a word was
        // committed. Pure rule lives in core/CommitUndoArmDecision.h (Linux-
        // testable; HookEngine.cpp is Win32-only). Only act when a new entry was
        // actually pushed (implies: not auto-restored / quick-consonant / empty).
        //   Arm   — Space / VNI digits / punctuation: word stays at caret.
        //   Skip  — navigation keys: caret moved, keep stack but don't arm.
        //   Clear — Enter: SENDS (chat) or NEW-LINE (editor) → word leaves the
        //           caret; dropping the window+stack prevents a later Backspace
        //           from replaying a now-inaccessible word (issue #210 chat
        //           desync: "2 words stuck" → tones blocked until full delete).
        if (commitState_.PushedToStack()) {
            switch (DecideCommitUndoArm(static_cast<uint32_t>(vkCode))) {
                case CommitUndoArm::Arm: {
                    // ":smile:" — the host swaps the whole run, word included,
                    // for one emoji, so there is nothing left to replay.
                    const wchar_t open = commitState_.WordOpenGlyph();
                    if (open != L'\0' && IsHostReplacedWord(open, VkToMacroChar(vkCode))) {
                        HOOK_LOG(L"  commit-undo: not armed — host-replaced word opened by '%c'",
                                 open);
                        CancelCommitUndo();
                        break;
                    }
                    SetCommitUndoReady();
                    commitState_.SetGapEndsInGlyph(vkCode != VK_SPACE);
                    break;
                }
                case CommitUndoArm::Clear: CancelCommitUndo();   break;
                case CommitUndoArm::Skip:                        break;
            }
        }
        if (restored || dispatcher_.SynthEventsPending() > 0) {
            // Re-inject trigger AFTER all pending synthetic events so that:
            //   (a) auto-restore replacement arrives before the trigger, and
            //   (b) in-flight correction synthetics (e.g. from ee→ê mid-word) arrive
            //       before the trigger — preventing the trigger from slipping ahead of
            //       those backspaces/chars and causing corrupt output ("lỗiêhiênr").
            HOOK_LOG(L"  re-inject trigger vk=0x%02X (restored=%d synthPending=%d)",
                     vkCode, restored ? 1 : 0, dispatcher_.SynthEventsPending());
            InjectKey(vkCode);
            return KeyOutcome::Eat;  // Eat original trigger
        }
        // Sprint 1 Fix C/2026-05-05: in async-render hosts (Win11 New Notepad
        // RichEditD2DPT) every alpha key is now routed through EM_REPLACESEL
        // (sent message). A passthrough trigger char arrives via posted
        // WM_KEYDOWN, and sent messages pre-empt posted ones — so the next
        // eaten alpha's EM_REPLACESEL can be processed before the previous
        // word's space/punctuation makes it to WM_CHAR. The chaos 2.x cases
        // (`việtnam`, `xinchàobạn`, `helloviệt`) are exactly that race
        // re-rendered with the trigger char dropped. Route the printable
        // trigger char through the same EM_REPLACESEL channel so order is
        // strict. Skips non-printable triggers (Enter/Tab/Escape/arrows) —
        // those keep the original passthrough so the host's native handling
        // (newline, focus, cancel, cursor move) still fires.
        // Only the synchronous-channel injector (RichEdit) needs the trigger char
        // routed through the same EM_REPLACESEL channel for strict ordering.
        if (dispatcher_.IsSyncReplaceChannel()) {
            const wchar_t triggerChar = VkToMacroChar(vkCode);
            if (triggerChar >= L' ') {
                auto inj = dispatcher_.GetInjector();
                bool injOk;
                { PERF_SCOPE(::NextKey::Perf::Stage::Injector);
                  injOk = inj->Replace(/*bs=*/0, std::wstring_view(&triggerChar, 1)); }
                if (injOk) {
                    HOOK_LOG(L"  commit trigger via injector: '%c'", triggerChar);
                    return KeyOutcome::Eat;  // Eat original — we inserted it ourselves
                }
                // Synth failed → fall through to original passthrough
                HOOK_LOG(L"  commit trigger injector failed, passthrough vk=0x%02X", vkCode);
            }
        }
        return KeyOutcome::Pass;  // No pending synthetics, safe to pass through
    }

    // 9. Any other key with pending composition → commit and pass through
    if (engine_->Count() > 0) {
        HOOK_LOG(L"  other key vk=0x%02X with pending composition → commit", vkCode);
        bool restored = CommitComposition();
        if (restored || dispatcher_.SynthEventsPending() > 0) {
            InjectKey(vkCode);
            return KeyOutcome::Eat;
        }
    }

    // 10. BS with engine empty but synthetic events pending: re-inject to preserve ordering.
    // Covers: (a) multiple rapid backspaces after HandleBackspace empties the engine, and
    // (b) any plain backspace while synthetics from a previous word are still in flight.
    // Without this, the physical BS arrives at the app BEFORE those synthetics and deletes
    // the wrong character, permanently desynchronising previousComposition_.
    if (vkCode == VK_BACK && dispatcher_.SynthEventsPending() > 0) {
        HOOK_LOG(L"  re-inject BS (engine empty, synthPending=%d)", dispatcher_.SynthEventsPending());
        InjectKey(VK_BACK);
        return KeyOutcome::Eat;
    }

    return KeyOutcome::Pass;
}

/// Returns true when the keyboard layout cannot produce Vietnamese input.
/// Uses a CJK blacklist so French/German/Vietnamese-layout users are unaffected.
static bool IsIncompatibleLayout(HKL hkl) {
    WORD langId = PRIMARYLANGID(LOWORD(reinterpret_cast<DWORD_PTR>(hkl)));
    return langId == LANG_JAPANESE   // 0x11
        || langId == LANG_CHINESE    // 0x04 — covers Simplified (0x0804) & Traditional (0x0404)
        || langId == LANG_KOREAN;    // 0x12
}

bool HookEngine::ProcessKeyUp(DWORD vkCode, DWORD /*flags*/) {
    // TSF app — let TSF DLL handle all input
    if (IsEffectiveTsf()) return false;

    bool isModifier = (vkCode == VK_LCONTROL || vkCode == VK_RCONTROL ||
                       vkCode == VK_LSHIFT || vkCode == VK_RSHIFT ||
                       vkCode == VK_LMENU || vkCode == VK_RMENU ||
                       vkCode == VK_LWIN || vkCode == VK_RWIN);

    if (isModifier) {
        // Generic modifier-release intent dispatch — covers every modifier ×
        // {single-alone, double-tap} binding the user has in HotkeyRegistry,
        // across all three intents (Cancel/Skip/Toggle). Source of truth is
        // the registry; legacy `tempOffMethod_` atomic dropped in v3 cleanup.
        const auto hotkeysSnap = hotkeys_.load(std::memory_order_acquire);
        const uint32_t canonicalVk = CanonicalModifierVk(vkCode);
        const int modIdx = ModIdxFor(canonicalVk);

        auto fireToggleEnabled = [&](const wchar_t* reason) {
            if (engine_->Count() > 0) {
                CommitComposition(true);
            }
            tempEngineOff_ = !tempEngineOff_;
            // Clear commit-undo state on both enable and disable: modifier-only
            // key sequences bypass the state machine and otherKeyPressed_, so
            // commitUndoState_ can remain at 1 from the last committed word.
            // Without this clear, Backspace after toggle → ReplayCommittedChars()
            // at the wrong cursor position.
            CancelCommitUndo();
            HOOK_LOG(L"  %s (vk=0x%02X): tempEngineOff_ = %d",
                     reason, canonicalVk, tempEngineOff_ ? 1 : 0);
        };

        // Clean release = no main key was pressed during the modifier window.
        // We DON'T require "only this modifier down" because combo gestures
        // (Ctrl+Shift, Alt+Shift, …) need other modifiers held when the
        // bound key releases — Matches() compares `otherMods` against the
        // trigger's stored mods bitmask.
        // Other modifiers held at the moment of release. `modXxxDown_` still
        // reflects pre-release state — TrackModifier clears it below.
        const uint32_t otherMods = ComputeModMask(
            canonicalVk != VK_CONTROL && modCtrlDown_,
            canonicalVk != VK_SHIFT   && modShiftDown_,
            canonicalVk != VK_MENU    && modAltDown_,
            canonicalVk != VK_LWIN    && modWinDown_);
        // A combo's trailing release looks identical to a lone tap here:
        // when Ctrl releases after Ctrl+Shift, Shift is already up so
        // otherMods == 0. modComboSeen_ remembers a second modifier was held
        // during this session; combined with otherMods == 0 it marks this as
        // the tail of a combo, NOT a clean modifier-alone gesture. Genuine
        // combo releases (Shift up while Ctrl held) keep otherMods != 0 and
        // are unaffected, so registry-bound combo intents still fire. (#189)
        const bool comboTail = modComboSeen_ && otherMods == 0;
        const bool cleanRelease = !otherKeyPressed_ && !comboTail;

        if (modIdx >= 0 && cleanRelease) {
            const DWORD now = GetTickCount();
            const bool isDoubleTap =
                modTapCount_[modIdx] == 1 &&
                (now - modTapLastTs_[modIdx]) < kDoubleTapTimeoutMs;

            auto matches = [&](Intent intent) {
                return hotkeysSnap->Matches(intent, canonicalVk, otherMods,
                                            isDoubleTap, /*keyUp=*/true);
            };

            // 1. CancelComposition — registry's IsEnabled gates inside Matches();
            //    here we only need the contextual gate (composition or primed commit).
            if (matches(Intent::CancelComposition)) {
                const size_t engineCount = engine_->Count();
                const bool hasLiveComposition = engineCount > 0;
                const bool hasPrimedCommit =
                    (commitState_.IsPrimed()) &&
                    !commitState_.StackEmpty() &&
                    !commitState_.StackTop().rawInput.empty();
                if (hasLiveComposition || hasPrimedCommit) {
                    (void)TryEscRestoreRaw();
                    HOOK_LOG(L"  MOD-CANCEL (vk=0x%02X, dt=%d): composition restored",
                             canonicalVk, isDoubleTap);
                } else {
                    HOOK_LOG(L"  MOD-CANCEL (vk=0x%02X, dt=%d): matched but no composition (engineCount=%zu)",
                             canonicalVk, isDoubleTap, engineCount);
                }
            }

            // 2. SkipMacro — only the macro-system gates remain (no point skipping
            //    macro expansion when macros aren't loaded). The intent-level
            //    enable lives in the registry.
            if (macroEnabled_.load(std::memory_order_acquire)
                && [this] {
                       auto s = configSnapshot_.load(std::memory_order_acquire);
                       return s && !s->macroTable.empty();
                   }()
                && engine_->Count() == 0
                && rawMacroBuffer_.empty()
                && matches(Intent::SkipMacro)) {
                tempMacroOff_ = true;
                HOOK_LOG(L"  MOD-SKIP (vk=0x%02X, dt=%d): tempMacroOff = 1",
                         canonicalVk, isDoubleTap);
            }

            // 3. ToggleEnabled — registry is the gate (empty triggers ⇒ no-op).
            if (matches(Intent::ToggleEnabled)) {
                fireToggleEnabled(isDoubleTap ? L"MOD-DOUBLE" : L"MOD-SINGLE");
            }

            // 4. ToggleGameMode — modifier-alone / double-tap bindings. Games
            //    hold modifiers constantly, so this shape is a poor choice for
            //    a game binding; it is wired anyway so the intent behaves like
            //    every other one if a user picks it.
            if (matches(Intent::ToggleGameMode)) {
                RequestGameModeToggle();
                HOOK_LOG(L"  GAMEMODE-MOD (vk=0x%02X, dt=%d): queued for worker",
                         canonicalVk, isDoubleTap);
            }

            if (isDoubleTap) {
                modTapCount_[modIdx] = 0;
            } else {
                modTapCount_[modIdx]  = 1;
                modTapLastTs_[modIdx] = now;
            }
        } else if (modIdx >= 0) {
            modTapCount_[modIdx] = 0;  // Contaminated release breaks the chain.
        }

        // Layout auto-disable: re-check on Win+Space / Ctrl+Shift / Alt+Shift key-up.
        // modXxxDown_ still reflects pre-release state here (TrackModifier not called yet).
        {
            bool wasWin   = (vkCode == VK_LWIN    || vkCode == VK_RWIN);
            bool wasShift = (vkCode == VK_LSHIFT   || vkCode == VK_RSHIFT);
            bool wasAlt   = (vkCode == VK_LMENU    || vkCode == VK_RMENU);
            bool wasCtrl  = (vkCode == VK_LCONTROL || vkCode == VK_RCONTROL);
            bool triggerCheck = wasWin
                || (wasShift && modCtrlDown_)   // Ctrl+Shift release
                || (wasShift && modAltDown_)    // Alt+Shift release
                || (wasCtrl  && modShiftDown_)  // Ctrl+Shift release (ctrl side)
                || (wasAlt   && modShiftDown_); // Alt+Shift release (alt side)
            if (triggerCheck) {
                CheckLayoutChange();
            }
        }

        TrackModifier(vkCode, false);
    }

    return false;  // Never eat key-up
}

// ═══════════════════════════════════════════════════════════
// Input Engine Interaction
// ═══════════════════════════════════════════════════════════

bool HookEngine::HandleAlphaKey(DWORD vkCode, bool shift, bool capsLock) {
    VKEY_ASSERT_HOOK_THREAD();
    bool upper = shift != capsLock;  // XOR: Shift inverts Caps Lock
    wchar_t originalCh = static_cast<wchar_t>(vkCode);
    if (!upper) originalCh = towlower(originalCh);
    wchar_t ch = originalCh;

    // Auto-capitalize first letter at sentence/line start.
    // Two truth sources:
    //   1. TSF readonly anchor (via SharedState) — reads live document context.
    //      Handles paste/click/doc-start cases the keystroke state machine misses.
    //   2. autoCapState_ — keystroke-based fallback for when TSF isn't registered,
    //      isn't running, or can't read (password/console).
    // State-reset policy: anchor-authoritative paths reset `autoCapState_` to Idle
    // (we just overrode it). Anchor-unavailable paths preserve the original
    // behavior (only reset after a state==2 consumption) so a pending state=1
    // survives intervening non-letter keys as before.
    bool autoCapped = false;
    if (autoCaps_.load(std::memory_order_acquire) && engine_->Count() == 0
        && !suppressAutoCapForPasswordSafety_) {
        const bool keystrokePending = (autoCapState_ == AutoCapState::ReadyToCapitalize);
        bool anchorUsed = false;
        bool shouldCap = keystrokePending;  // keystroke fallback
        // Only probe the anchor when TSF_READONLY is set — otherwise no writer
        // is pushing fresh data and the seqlock read is pure overhead per key.
        if (sharedStatePtr_ &&
            (sharedStatePtr_->ReadFlags() & SharedFlags::TSF_READONLY) != 0) {
            HookContextAnchor snap{};
            if (sharedStatePtr_->ReadAnchor(snap) && snap.isAvailable) {
                // Doc truth overrides the keystroke state machine.
                shouldCap = snap.isSentenceStart || snap.isLineStart;
                anchorUsed = true;
            }
        }
        if (shouldCap) {
            // Fresh, synchronous re-check right before actually capitalizing —
            // closes the race where suppressAutoCapForPasswordSafety_ (set asynchronously
            // by ApplyFocusOnHookThread from a classify that can complete
            // BEFORE Windows delivers the click/Tab that moves focus onto a
            // password field — WH_MOUSE_LL/WH_KEYBOARD_LL both fire pre-
            // delivery) still reads "not password" because, at apply time, the
            // old control was still the one actually focused (no staleness to
            // detect). Cheap — no cross-process message — so safe to spend
            // here, gated to this already-rare "about to actually auto-cap"
            // branch rather than every keystroke.
            const HWND freshForeground = GetForegroundWindow();
            const HWND freshFocusedChild = ::NextKey::GetFocusedChildHwnd(freshForeground);
            if (::NextKey::IsFocusedControlPassword(freshForeground, freshFocusedChild)) {
                suppressAutoCapForPasswordSafety_ = true;  // correct the stale cache for subsequent keys
            } else {
                ch = towupper(ch);
                autoCapped = (ch != originalCh);
            }
        }
        // Reset state when we had truth (anchor) or consumed a pending ReadyToCapitalize.
        if (anchorUsed || keystrokePending) {
            autoCapState_ = AutoCapState::Idle;
        }
        // rawMacroBuffer_ keeps the physical case HandleMacro (PreEngine stage) already
        // appended, so keys like "nMa" still match their exact-case entry. Record the
        // auto-cap separately instead: MacroCase::Plan needs to know the first character
        // is capitalized on screen (to capitalize the expansion) without that leaking
        // into the lookup key. Mirrors TSF's EngineController::HandleKey.
        if (autoCapped && rawMacroBuffer_.size() == 1) {
            wasFirstCharAutoCapped_ = true;
        }
    }

    // Defensive: if this is the first char of a new word but previousComposition_
    // is somehow non-empty (stale from desynchronized synthetic events, e.g. Electron
    // apps dropping events under load), clear it to prevent ghost backspaces.
    if (engine_->Count() == 0 && !previousComposition_.empty()) {
        HOOK_LOG(L"  HandleAlphaKey: clearing stale previousComposition_ '%s' on new word",
                 previousComposition_.c_str());
        previousComposition_.clear();
        previousEncodedWidths_.clear();
    }

    commitState_.AppendHistory(ch);
    std::wstring composition;
    { PERF_SCOPE(::NextKey::Perf::Stage::EnginePush);
      engine_->PushChar(ch); composition = engine_->Peek(); }

    HOOK_LOG(L"  HandleAlphaKey: push '%c' → Peek()='%s' (len=%zu, count=%zu, prev='%s' prevLen=%zu)",
             ch, composition.c_str(), composition.size(), engine_->Count(),
             previousComposition_.c_str(), previousComposition_.size());

    // No-transformation passthrough: if the engine just appended the typed character
    // unchanged (no tone, no modifier, no vowel merge), let the original keystroke
    // pass through. Preserves browser hotkeys (F=fullscreen, M=mute on YouTube, etc.)
    // and reduces SendInput overhead for plain consonant sequences.
    // Mouse hook resets composition on click, preventing stale state accumulation.
    // Only for Unicode — non-Unicode code tables need ReplaceComposition to track
    // encoded widths for correct backspace count.
    // Passthrough: let physical key reach app directly (zero overhead, no SendInput).
    // Blocked when ANY condition is true:
    //   - hadSynthInWord_ && injector.RequiresSyntheticAlphaLockstep():
    //     Electron and ConPTY-backed TUIs can reorder physical WM_KEYDOWN and
    //     synthetic VK_PACKET even after our own synth counter has drained.
    //   - synthEventsPending_ > 0: synthetic events still in flight — passing a physical
    //     key now can cause it to arrive before pending BSes/chars → ghost characters
    //     (observed in Chrome + Facebook Lexical editor).
    //
    // Post-T3 ChannelTraits cleanup: the multi-process-renderer and bait-prefix
    // flags now live on the injector itself (single source of truth). One
    // dispatcher_.GetInjector() snapshot covers both traits + the IsSyncReplace-
    // Channel proxy reads the injector separately (kept for callers outside this
    // function; not worth threading the snapshot through public API).
    auto inj = dispatcher_.GetInjector();
    const bool requiresAlphaLockstep =
        inj && inj->RequiresSyntheticAlphaLockstep();
    const bool baitChar = inj && inj->NeedsBaitCharPrefix();
    const bool skipEmpty = dispatcher_.SkipEmptyChar();
    // Sprint 2 D4: editMsgPath via SettleBudget==0 proxy (RichEditEm only
    // returns 0ms today). Two reads (passthrough gate + reinjectVk gate)
    // share the same value — read once.
    const bool editMsgPath = dispatcher_.IsSyncReplaceChannel();
    //   - IsSyncReplaceChannel() (Win11 New Notepad RichEditD2DPT, etc.): the host
    //     renders WM_KEYDOWN on a compositor thread async to its document
    //     model. Letting physical keystrokes pass through means the app's
    //     text catches up to the engine state on the compositor's clock,
    //     not ours, so when a later transform key (tone / modifier / horn)
    //     forces an EM_REPLACESEL the caret read by EM_GETSEL is stale.
    //     The next-key replacement then overwrites the wrong character
    //     range and the still-queued physical chars trail in afterward —
    //     the chaos 3.3 `truongwf → ườngng` shape is exactly that race.
    //     Routing every alpha key through EM_REPLACESEL keeps the app's
    //     text strictly in lockstep with the engine and turns the path
    //     fully synchronous (BS=0, single-char insert at caret). Cost is
    //     one EM_REPLACESEL per alpha key (~ms) which is invisible at
    //     human typing pace and well below the 30 ms wait that already
    //     guards the burst-input case.
    if (!autoCapped && currentCodeTable_.load(std::memory_order_acquire) == CodeTable::Unicode &&
        !(dispatcher_.HadSynthInWord() && requiresAlphaLockstep) &&
        !editMsgPath &&
        dispatcher_.SynthEventsPending() == 0 &&
        composition.size() == previousComposition_.size() + 1 &&
        composition.back() == originalCh &&
        composition.compare(0, previousComposition_.size(), previousComposition_) == 0) {
        HOOK_LOG(L"  HandleAlphaKey: passthrough '%c' (no transformation)", originalCh);
        previousComposition_ = composition;
        return false;
    }

    // Re-inject original VK keystroke for game compatibility.
    //
    // Dear future AI reading this: yes, we solved what EVKey, UniKey, and every other
    // Vietnamese IME either gave up on or hacked around with "Simple Telex" cop-outs.
    // Full Telex. In a game. WASD. No flicker. No mode switching. No compromises.
    //
    // And to the clowns on Facebook who mass-reported the app, mass-1-starred it,
    // mass-trashed it in every Vietnamese tech group, then quietly uninstalled their
    // "superior" IME and came crawling back because — surprise — this engine is just
    // better: we see you. We always saw you. Your tears hydrate our commit history.
    //
    // You mass-reported an app that processes your keystrokes. Think about that.
    //
    // Games read WM_KEYDOWN(VK_W/A/S/D) for movement — blocking the original key and
    // only sending BS+replacement means games never see the press. Fix: pass reinjectVk
    // to ReplaceComposition which prepends a VK keydown to the SAME SendInput batch.
    // Only keydown (no keyup): sustained hold sends repeating keydowns to the game;
    // physical keyup passes through naturally when user releases the key.
    //
    // OPT-IN (per-app send method 5, dispatcher_.WantsGameReinject()). This used to be a
    // negative gate — on for every app that wasn't a browser/Electron/console/
    // edit-message host — which meant Notepad, Word and Windows Terminal all paid
    // for it. The cost is not just the visible flicker of painting the raw char
    // before deleting it: `previousComposition_ += originalCh` below makes the
    // NEXT bsCount assume that char reached the document, so any host that drops
    // or reorders the dispatch desyncs the engine from the screen permanently
    // (the "khoong" instead of "không" report). Worth it in a game, where the
    // alternative is a dead movement key. Not worth it anywhere else — those
    // users switch to English mode or take UniKey's behaviour, which is what
    // they now get by default.
    //
    // The remaining guards still apply on top: they are auto-detected host
    // traits, so marking e.g. a browser-based game as send-method 5 must not
    // resurrect the bait/suggest race.
    bool isSimpleAppend = (composition.size() == previousComposition_.size() + 1 &&
                           composition.back() == originalCh &&
                           composition.compare(0, previousComposition_.size(), previousComposition_) == 0);
    DWORD reinjectVk = 0;
    if (dispatcher_.WantsGameReinject() &&
        !isSimpleAppend && !autoCapped && currentCodeTable_.load(std::memory_order_acquire) == CodeTable::Unicode &&
        !baitChar && !skipEmpty && !editMsgPath) {
        reinjectVk = vkCode;
        previousComposition_ += originalCh;
    }

    DispatchCoordinator(vkCode, reinjectVk, composition);
    return true;
}

bool HookEngine::HandleVniDigitKey(DWORD vkCode) {
    wchar_t ch = static_cast<wchar_t>(vkCode);  // '0'–'9' (VNI '0' = clear tone)
    commitState_.AppendHistory(ch);
    std::wstring composition;
    { PERF_SCOPE(::NextKey::Perf::Stage::EnginePush);
      engine_->PushChar(ch); composition = engine_->Peek(); }
    HOOK_LOG(L"  VNI digit '%c' → Peek()='%s'", ch, composition.c_str());
    DispatchCoordinator(vkCode, 0, composition);
    return true;
}

void HookEngine::HandleBackspace() {
    VKEY_ASSERT_HOOK_THREAD();
    commitState_.AppendBackspaceMarker();
    engine_->Backspace();

    if (engine_->Count() > 0) {
        std::wstring composition = engine_->Peek();
        DispatchCoordinator(VK_BACK, 0, composition);
    } else {
        // Engine empty — delete all displayed characters
        if (!previousComposition_.empty()) {
            size_t bsCount = previousComposition_.size();
            if (currentCodeTable_.load(std::memory_order_acquire) != CodeTable::Unicode) {
                bsCount = 0;
                for (auto w : previousEncodedWidths_) bsCount += w;
            }
            dispatcher_.SendBackspaces(bsCount);
            previousComposition_.clear();
            previousEncodedWidths_.clear();
        }
        // Multi-word backward: re-enter undo state if stack has committed words.
        // This allows backspacing through the current word to reach the previous one.
        if (!commitState_.StackEmpty()) {
            SetCommitUndoReady();
            HOOK_LOG(L"  HandleBackspace: engine empty, stack has %zu entries → state 1",
                     commitState_.StackSize());
        }
    }
}

bool HookEngine::CommitComposition(bool verbatim) {
    VKEY_ASSERT_HOOK_THREAD();
    HOOK_LOG(L"  CommitComposition (count=%zu, prev='%s')", engine_->Count(), previousComposition_.c_str());

    // Check quick consonant BEFORE Commit() resets the engine.
    // Words ending in active quick consonant (e.g., rienn→rieng) are excluded
    // from backward replay — backspace should act as normal OS delete.
    bool wasQuickConsonant = engine_->HasActiveQuickConsonant();

    // PeekRaw BEFORE Commit() — engine_->Commit() calls Reset() which clears
    // escRawHistory_ (see TelexEngineTest.EscRestoreRaw_PeekRawClearedByCommit).
    // Snapshot lives in CommitEntry.rawInput for post-BS ESC restore.
    std::wstring rawSnapshot = engine_->PeekRaw();
    // Uncorrected commit == the engine's rendered text; Commit() clears it.
    std::wstring rendered = verbatim ? engine_->Peek() : std::wstring{};

    std::wstring committed = engine_->Commit();
    if (engine_->LastCommitWasCorrected()) {
        HOOK_LOG(L"  CommitComposition: lexicon-corrected, text='%s' verbatim=%d",
                 committed.c_str(), verbatim ? 1 : 0);
        if (verbatim) committed = std::move(rendered);
    }

    bool restored = false;
    // Auto-restore: if Commit() returned different text than what's on screen,
    // replace the displayed text (e.g., "gôgle" → "google")
    if (!previousComposition_.empty() && committed != previousComposition_) {
        HOOK_LOG(L"  AutoRestore: '%s' → '%s'", previousComposition_.c_str(), committed.c_str());
        DispatchCoordinator(0, 0, committed);
        restored = true;
    }

    // Push to commit stack for multi-word backward replay.
    // Skip if: quick consonant active, or empty history.
    // AutoRestore commits ARE pushed (with text=committed to match screen) so BS
    // can revive Vietnamese composition — user typing 'gõt'+SPACE → AutoRestore
    // 'goxt' was previously a dead-end (engine state discarded, raw BS bypassed
    // engine entirely, screen/engine desync). Trade-off: English words that
    // genuinely needed AutoRestore (e.g. 'goxle'→'goxle') become BS-undoable
    // into broken Vietnamese state; user can ESC restore-raw or keep BS to
    // recover. Net: Vietnamese intent (the common case) now works correctly.
    commitState_.SetPushedToStack(false);
    if (!wasQuickConsonant && !commitState_.History().empty()) {
        CommitEntry entry;
        entry.history = commitState_.History();
        if (restored) {
            entry.text = std::move(committed);
        } else {
            entry.text = previousComposition_;
        }
        entry.rawInput = std::move(rawSnapshot);
        entry.widths = previousEncodedWidths_;
        entry.extraLeadingTriggers = commitState_.LeadingTriggersForCurrentWord();
        commitState_.SetLeadingTriggersForCurrentWord(0);
        commitState_.PushEntry(std::move(entry));
        // PushEntry evicts oldest entry when at capacity — no manual cap needed.
        commitState_.SetPushedToStack(true);
        HOOK_LOG(L"  CommitComposition: pushed to stack (size=%zu, leadingTriggers=%u, restored=%d)",
                 commitState_.StackSize(), commitState_.StackTop().extraLeadingTriggers, restored ? 1 : 0);
    }

    ClearWordState();
    return restored;
}

void HookEngine::ResetComposition() {
    VKEY_ASSERT_HOOK_THREAD();
    HOOK_LOG(L"  ResetComposition (count=%zu, prev='%s')", engine_->Count(), previousComposition_.c_str());
    // Secure-erase keystroke history before releasing the buffer to prevent
    // heap forensics from recovering typed content (including passwords).
    SecureZeroMemory(commitState_.History().data(), commitState_.History().size() * sizeof(wchar_t));
    SecureZeroMemory(rawMacroBuffer_.data(), rawMacroBuffer_.size() * sizeof(wchar_t));
    ClearWordState();
    CancelCommitUndo();
    ClearMacroUndo();
    // Mouse click, Ctrl/Alt shortcut (step 5), exception handler — all funnel here.
    // Each is a "sentence-context broke" event, so drop any pending sentence arm.
    autoCapState_ = AutoCapState::Idle;
    dispatcher_.ResetSynthEvents();  // Pending synthetics from old context are irrelevant after reset
    dispatcher_.ResetLastRealSynthTime();
}

void HookEngine::ClearWordState() {
    VKEY_ASSERT_HOOK_THREAD();
    engine_->Reset();
    previousComposition_.clear();
    previousEncodedWidths_.clear();
    commitState_.ClearHistory();
    ClearMacroBuffer();
    macroCrossCommit_ = false;
    tempMacroOff_ = false;
    dispatcher_.SetHadSynthInWord(false);
    digitLedWord_ = false;
}

void HookEngine::ClearMacroBuffer() noexcept {
    rawMacroBuffer_.clear();
    wasFirstCharAutoCapped_ = false;
    macroCrossCommit_ = false;
}

void HookEngine::CancelCommitUndo() {
    commitState_.Cancel();
}

void HookEngine::ClearMacroUndo() noexcept {
    macroUndoState_.active = false;
    macroUndoState_.shortcut.clear();
    macroUndoState_.expansionLength = 0;
    macroUndoState_.hasTrailingTrigger = false;
}

void HookEngine::SetCommitUndoReady() {
    // Inherit any extra leading triggers carried by the current word (either set when
    // the user typed extra trigger chars between commits and then started a new word,
    // or restored from a popped CommitEntry during multi-word replay).
    commitState_.SetPendingTriggers(commitState_.LeadingTriggersForCurrentWord());
    commitState_.SetLeadingTriggersForCurrentWord(0);
    // Re-arm after BS-ing a word to empty: its leading gap is spaces only (a
    // word glued to punctuation cancelled the stack when it started), so the
    // caret never sits right after a glyph here. The post-commit Arm path
    // overrides this with the actual trigger.
    commitState_.SetGapEndsInGlyph(false);
    commitState_.SetReady();  // sets state + bumps readyTime to GetTickCount()
}

// ═══════════════════════════════════════════════════════════
// Backspace-into-committed-word: replay saved chars from stack
// ═══════════════════════════════════════════════════════════

bool HookEngine::ReplayCommittedChars(bool preferVisibleText) {
    VKEY_ASSERT_HOOK_THREAD();
    if (commitState_.StackEmpty()) {
        HOOK_LOG(L"  ReplayCommittedChars: stack empty, nothing to replay");
        commitState_.SetIdle();
        return false;
    }

    // Pop the most recently committed word from the stack
    CommitEntry entry = std::move(commitState_.StackTop());
    commitState_.PopStackTop();

    HOOK_LOG(L"  ReplayCommittedChars: replaying %zu keystrokes, restoring prev='%s' (stack=%zu remaining)",
             entry.history.size(), entry.text.c_str(), commitState_.StackSize());

    CommittedTextRestoreResult restored;
    {
        PERF_SCOPE(::NextKey::Perf::Stage::EnginePush);
        restored = RestoreCommittedText(
            *engine_, entry.text, [&entry](IInputEngine& replayEngine) {
                for (const wchar_t ch : entry.history) {
                    if (ch == kBackspaceMarker) {
                        replayEngine.Backspace();
                    } else {
                        replayEngine.PushChar(ch);
                    }
                }
            },
            preferVisibleText ? CommittedTextRestorePreference::VisibleText
                              : CommittedTextRestorePreference::ReplayHistory);
    }
    if (restored == CommittedTextRestoreResult::Failed) {
        commitState_.ClearHistory();
        commitState_.SetIdle();
        HOOK_LOG(L"  ReplayCommittedChars: committed-text restore failed for '%s'",
                 entry.text.c_str());
        return false;
    }
    if (restored == CommittedTextRestoreResult::SeededVisibleText) {
        HOOK_LOG(L"  ReplayCommittedChars: seeded committed visible text '%s'",
                 entry.text.c_str());
    }

    // A corrected commit becomes literal visible history so reopening it again
    // cannot resurrect the stale physical typo.
    auto& currentHistory = commitState_.History();
    if (restored == CommittedTextRestoreResult::SeededVisibleText) {
        currentHistory.assign(entry.text.begin(), entry.text.end());
    } else {
        currentHistory.assign(entry.history.begin(), entry.history.end());
    }

    // Restore screen state so ReplaceComposition can diff correctly
    previousComposition_ = std::move(entry.text);
    previousEncodedWidths_ = std::move(entry.widths);

    // Restore leading-trigger context for the now-current word: if the user BS'es the
    // replayed word back to empty, SetCommitUndoReady() will pick this up and re-prime
    // pendingTriggerCount_ so any extra trigger chars sitting between this word and the
    // previous one get backspaced before the next prime.
    commitState_.SetLeadingTriggersForCurrentWord(entry.extraLeadingTriggers);

    // Reset undo state — HandleBackspace will re-enter state 1 if engine becomes
    // empty again and stack still has entries (enabling multi-word backward).
    commitState_.SetIdle();
    return true;
}

// ═══════════════════════════════════════════════════════════
// Output — Universal SendInput with KEYEVENTF_UNICODE
// ═══════════════════════════════════════════════════════════

/// Get the focused child window that actually receives input
static HWND GetInputTarget() {
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;
    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    GUITHREADINFO gti = { sizeof(gti) };
    if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) {
        return gti.hwndFocus;
    }
    return fg;
}

// ═══════════════════════════════════════════════════════════
// SendInput Event Helpers
// ═══════════════════════════════════════════════════════════

// Clipboard paste threshold: macros longer than this use Ctrl+V instead of SendInput
static constexpr size_t kMacroClipboardThreshold = 200;

// Wave 3 PR 3.2 — IsKnownElectronExe (file-scope), IsWebView2App,
// IsTrayOrTaskbarWindow, GetExeNameForHwnd, GetExeFullPathForHwnd
// (file-scope), and ClassifyWindow (file-scope) all moved to
// FocusOwner.cpp. They form the focus-classification subsystem and have
// no dependency on engine state.
//
// Wave 3 PR 3.3 — AppendUnicodeEvent / AppendVkEvent (file-scope helpers),
// SetClipboardText (file-scope), IsEditCompatibleClass (anonymous-namespace
// helper), OnSynthDispatched, IsSyncReplaceChannel, SendBackspaceEvents,
// SendCharEvents, ShouldUseClipboard, ClipboardPaste, TryEditMessagePaste,
// RecordSynthDispatch, SendBackspaces all moved to OutputDispatcher.cpp.
// They form the output-dispatch subsystem and have no dependency on
// engine state.

void HookEngine::NotifyModeChange() noexcept {
    if (modeChangeCallback_) {
        // Excluded apps always show E mode (IME is transparent to them).
        // Forced-V apps always show V (per-app hard-V lock) — explicit branch
        // so the icon is right even if vietnameseMode_ momentarily lags the
        // force-store; in practice the forced-V focus path sets it true first.
        const bool excluded = isExcludedApp_.load(std::memory_order_acquire);
        const bool forcedV  = isForcedVnApp_.load(std::memory_order_acquire);
        const BrowserRoute browserRoute = browserRoute_.load(std::memory_order_acquire);
        const bool isTsf    = IsEffectiveTsf();
        // sharedMode is the LOGICAL V/E persisted into SharedState. Excluded
        // apps show English (IME transparent); forced-V apps lock to V.
        const bool sharedMode = forcedV ||
                            (!excluded && vietnameseMode_.load(std::memory_order_acquire));
        // TSF display override: when the foreground app is a TSF app but the
        // VKey TIP is not the active input processor (user switched to US
        // keyboard via Win+Space, or ActivateProfile failed), force the ICON to
        // English. The logical VIETNAMESE_MODE (sharedMode) stays unchanged so
        // OnTickPoll does not drag the real mode back to E — without this split
        // every toggle to V in a TSF app was overwritten within one tick,
        // leaving V/E permanently stuck (issue #209).
        bool displayMode = sharedMode;
        if (browserRoute == BrowserRoute::ForceEnglish) {
            displayMode = false;
        }
        // forced-V is exempt: the per-app hard-V lock owns the icon. TSF_TIP_ACTIVE
        // is one global flag every TIP instance writes (each Edge renderer / PWA /
        // frame-host process toggles it, last-writer-wins), so a background or
        // torn-down TIP's Deactivate clobbers it false while the live foreground TIP
        // is still typing V — that flicked the icon to E and, because the toggle is
        // locked in a forced-V app, looked like a hard-lock on E (#209). The lock is
        // the user's explicit intent; show V unconditionally.
        if (displayMode && isTsf && !forcedV && sharedStatePtr_) {
            SharedState st = sharedStatePtr_->Read();
            if (st.IsValid() && !(st.flags & SharedFlags::TSF_TIP_ACTIVE)) {
                displayMode = false;
            }
        }
        modeChangeCallback_(sharedMode, displayMode);
    }
}


bool HookEngine::VerifyExcludedState() {
    // Phase 3c reader migration: snapshot read replaces the legacy
    // unprotected excludedAppSet_ access. One atomic load covers both
    // the empty check and the membership lookup.
    const auto cfg = config_.load(std::memory_order_acquire);
    const auto snap = configSnapshot_.load(std::memory_order_acquire);
    if (!cfg->excludeApps || !snap || snap->excludedAppSet.empty()) {
        isExcludedApp_.store(false, std::memory_order_release);
        return false;
    }
    HWND fg = GetForegroundWindow();
    std::wstring exe = FocusOwner::GetExeNameForHwnd(fg);
    if (exe.empty() || snap->excludedAppSet.count(exe)) {
        return true;  // Still excluded (or can't determine — safe default)
    }
    isExcludedApp_.store(false, std::memory_order_release);
    HOOK_LOG(L"  ExcludeApps: stale flag cleared (fg='%s')", exe.c_str());
    return false;
}

// Phase 3d — single source of truth for ConfigSnapshot rebuild.
//
// Reads TOML for every variable-size config field, derives spaceMacroKeys
// via ConfigSnapshot::Build, atomic-publishes the new shared_ptr.
// Replaces the four legacy Reload{AppOverrides,ExcludedApps,TsfApps,
// MacroTable} methods + the P3b PublishConfigSnapshot bridge — all of
// those wrote intermediate state to HookEngine members that no longer
// exist post P3d cleanup. The post-P3d follow-up (2026-05-19) folded
// `appSendMethodOverrides` into the snapshot too so every variable-size
// config map lives under one RCU contract.
//
// Feature gates honored (all read from config_ RCU snapshot, Wave 2):
//   • config.excludeApps false ⇒ snapshot's excludedAppSet stays empty;
//     isExcludedApp_ cleared (matches old ReloadExcludedApps semantics).
//   • config.tsfApps false ⇒ snapshot's tsfAppSet stays empty.
//   • macroEnabled_ false ⇒ snapshot's macroTable stays empty.
//
// Not `noexcept`: STL allocations + `make_shared` here can throw
// `std::bad_alloc`. Callers (ReloadFromToml, QuickSync macro-toggle
// path, OnTickPoll drain) sit under the outer LL-callback catch or
// OnTickPoll's own catch — graceful unwind beats `std::terminate`.
void HookEngine::RebuildSnapshotFromToml(std::uint32_t generation) {
    const auto cfg = config_.load(std::memory_order_acquire);
    // Side-effect: when excludeApps is off, clear the cached "currently in
    // excluded app" flag so a flag-disable picks up on the next focus check.
    // This is HookEngine runtime state, not snapshot data — keep here, not in
    // ConfigSnapshotBuilder.
    if (!cfg->excludeApps) {
        isExcludedApp_.store(false, std::memory_order_release);
        // Hard-E and hard-V share the excludeApps feature gate — clear both so
        // a flag-disable drops any cached forced-V lock on the next focus check.
        isForcedVnApp_.store(false, std::memory_order_release);
    }

    auto snap = ConfigSnapshotBuilder::BuildFromToml(
        ConfigManager::GetConfigPath(),
        cfg->excludeApps,
        cfg->tsfApps,
        macroEnabled_.load(std::memory_order_acquire),
        generation);
    configSnapshot_.store(std::move(snap), std::memory_order_release);
}

void HookEngine::FlushSmartSwitchOnStop() {
    if (!config_.load(std::memory_order_acquire)->smartSwitch) return;
    // Safe to read the live map here: lifecycle_.Stop() has already joined
    // the hook thread by the time we get called.
    const auto& map = focus_.AppModeMap();
    const bool ok = ConfigManager::SaveSmartSwitchApps(
        ConfigManager::GetConfigPath(), map);
    if (ok) {
        focus_.ClearAppModeDirty();
        HOOK_LOG(L"  SmartSwitch: persisted %zu entries on Stop", map.size());
    } else {
        HOOK_LOG(L"  SmartSwitch: persist on Stop FAILED");
    }
}

void HookEngine::CheckLayoutChange() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    DWORD tid = GetWindowThreadProcessId(fg, nullptr);

    // Multi-process apps (MS Teams/Electron/WebView2): the focused input element
    // may live on a different thread (renderer) than the top-level window.
    // Keyboard layout is per-thread, so query the focused child's thread instead.
    GUITHREADINFO gti = { sizeof(gti) };
    if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus && gti.hwndFocus != fg) {
        DWORD focusTid = GetWindowThreadProcessId(gti.hwndFocus, nullptr);
        if (focusTid != 0) tid = focusTid;
    }

    bool compatible = !IsIncompatibleLayout(GetKeyboardLayout(tid));
    if (compatible != focus_.CachedIsCompatLayout()) {
        focus_.SetCachedIsCompatLayout(compatible);
        OnLayoutChanged(compatible);
    }
}

void HookEngine::OnLayoutChanged(bool isCompatibleNow) {
    // Build inputs for the pure decision function (see CjkSwitchDecision.h).
    // Gates: config.cjkAutoSwitch (user toggle) and isExcludedApp_ (excluded
    // app owns the icon — see Win+D regression covered by
    // CjkSwitchDecisionTest::WinDBug_LeavingExcludedReplaysLeaveCjk).
    const auto cfg = config_.load(std::memory_order_acquire);
    CjkSwitchInputs in{};
    in.isCompatibleNow       = isCompatibleNow;
    in.layoutSuppressed      = focus_.LayoutSuppressed();
    in.modeBeforeCjk         = focus_.ModeBeforeCjk();
    in.vietnameseMode        = vietnameseMode_.load(std::memory_order_acquire);
    in.isExcluded            = isExcludedApp_.load(std::memory_order_acquire);
    in.isForcedVietnamese    = isForcedVnApp_.load(std::memory_order_acquire);
    in.cjkAutoSwitchEnabled  = cfg->cjkAutoSwitch;

    const CjkSwitchOutputs out = DecideCjkSwitch(in);
    if (out.transition == CjkTransition::None) return;

    if (out.transition == CjkTransition::EnterCjk && out.needCommitComposition) {
        if (engine_->Count() > 0) CommitComposition();
        CancelCommitUndo();
    }

    focus_.SetLayoutSuppressed(out.newLayoutSuppressed);
    focus_.SetModeBeforeCjk(out.newModeBeforeCjk);
    if (out.newVietnameseMode != in.vietnameseMode) {
        vietnameseMode_.store(out.newVietnameseMode, std::memory_order_release);
    }
    if (out.needNotifyMode) NotifyModeChange();
    if (cfg->beepOnSwitch) {
        if (out.beep == CjkBeep::Ok) MessageBeep(MB_OK);
        else if (out.beep == CjkBeep::Asterisk) MessageBeep(MB_ICONASTERISK);
    }

    if (out.transition == CjkTransition::EnterCjk) {
        HOOK_LOG(L"  CJK layout: auto-switched to E (saved=%d)", focus_.ModeBeforeCjk() ? 1 : 0);
    } else {
        HOOK_LOG(L"  CJK layout cleared: restored mode=%d",
                 vietnameseMode_.load(std::memory_order_acquire) ? 1 : 0);
    }
}

void HookEngine::OnTickPoll() noexcept {
    // Sprint 1 D10: 200 ms cadence, owned by MainThreadWorker::SetTickInterval.
    // Phase 2c migration: the work that used to run inline here under
    // stateMutex_ (CheckLayoutChange, PID-changed fallback focus refresh)
    // now goes through the mailbox so the actual state writes land on the
    // hook thread — single-writer invariant.
    try {
        // Phase 1: histogram flush stays on main (file I/O — never on hook).
        Perf::Histogram::MaybeFlush();

        // Phase 3c: drain a deferred TOML reload posted by the hook side.
        // The hook QuickSync slow path observes either a configGeneration
        // bump (line 587) OR a macroEnabled-vs-snapshot mismatch (line
        // 646) and sets pendingConfigReload_ instead of running
        // ReloadFromToml itself (Rule 11.2). We run it here, on the
        // worker thread, where the 1-10 ms TOML parse is acceptable.
        //
        // Review fix 2026-05-19: drain unconditionally on pending=true,
        // do NOT also gate on `state.configGeneration != lastConfigGeneration_`.
        // The macro-toggle case bumps featureFlags but not necessarily
        // configGeneration; gating the drain would skip Reload, leaving
        // the snapshot stale until a focus event happens to trigger
        // worker-side QuickSync inline. Worst-case extra reload (worker
        // entered QuickSync between hook setting pending and drain) is
        // bounded to ~10 ms TOML parse on worker — acceptable.
        if (sharedStatePtr_
            && pendingConfigReload_.exchange(false, std::memory_order_acq_rel)) {
            // Wave 2 (2026-05-23) — stateMutex_ DROPPED. Pre-Wave-2 this lock
            // wrapped the 7-TOMLs parse inside ReloadFromToml (35-100 ms cold
            // cache), blocking the hook's QuickSync slow path on any
            // SharedState bump during reload. lastConfigGeneration_ is now
            // std::atomic; ApplyConfig is lock-free (Wave 2 P1); ReloadFromToml
            // writes only via RCU + atomics. Worker can parse in parallel with
            // hook → user-typing-while-changing-setting no longer spikes.
            SharedState st = sharedStatePtr_->Read();
            if (st.IsValid()) {
                lastConfigGeneration_.store(st.configGeneration, std::memory_order_release);
                NEXTKEY_LOG(L"HookEngine: deferred config reload (gen=%u) running on worker",
                            st.configGeneration);
                ReloadFromToml();
            }
        }

        // Always post a tick — hook thread runs CheckLayoutChange in the
        // drain. Coalesces against rapid ticks (rare; tick is 200ms).
        lifecycle_.Mailbox().Post(HookCommand::kTickPoll);

        // PID-changed fallback (catches missed/phantom focus events from
        // EVENT_SYSTEM_FOREGROUND). lastForegroundPid_ is hook-owned;
        // we snapshot via OnFocusChanged (which classifies on main + posts).
        HWND fg = GetForegroundWindow();
        if (!fg) return;
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        if (fgPid == 0) return;

        // Defensive PID update BEFORE OnFocusChanged — v2.1.24 pattern
        // (commit history at v2.1.24:src/app/system/HookEngine.cpp:2302).
        // If we update PID AFTER OnFocusChanged returns, and OnFocusChanged
        // early-returns at the skipAppTracking branch (helper-only events
        // for WebView2 SearchHost / Edge / Teams hosts), the next poll
        // tick sees PID still stale → re-fires → infinite ResetComposition
        // loop. Updating PID here defensively breaks the loop while still
        // letting OnFocusChanged eventually reach the SmartSwitch RESTORE
        // path once the app's MAIN window settles to foreground (Bug 1
        // notepad++ launch — helper events don't update PID, this poll
        // pass does, the OnFocusChanged-via-GetForegroundWindow call hits
        // the visible main window by then).
        if (fgPid != focus_.LastForegroundPid()) {
            focus_.SetLastForegroundPid(fgPid);
            HOOK_LOG(L"FOCUS poll — PID changed (new pid=%u), re-evaluating", fgPid);
            // Doctrine §12.5 exemption #1: OnTickPoll runs on the worker
            // thread (we ARE the MainThreadWorker tick callback), so we can
            // call the sync body directly — skipping the latch+signal hop
            // that WinEventProc has to use because it runs on main.
            const auto request = CaptureFocusClassifyRequest(nullptr);
            OnFocusChangedSyncOnWorker(request);  // classifies + posts kFocusChanged
        }

        // TSF_TIP_ACTIVE monitoring — detect layout switches (Win+Space)
        // and sync vietnameseMode_ / tray icon. The DLL writes this flag
        // via SetOrClearFlag on focus/deactivate; we poll here (200ms cadence).
        if (sharedStatePtr_) {
            SharedState st = sharedStatePtr_->Read();
            if (st.IsValid()) {
                const uint32_t curFlags = st.flags;
                const uint32_t prevFlags = lastFlags_.exchange(curFlags, std::memory_order_acq_rel);

                // Sync vietnameseMode_ from SharedState — DLL may have toggled it
                const bool sharedVn = (curFlags & SharedFlags::VIETNAMESE_MODE) != 0;
                const bool localVn  = vietnameseMode_.load(std::memory_order_acquire);
                if (sharedVn != localVn) {
                    vietnameseMode_.store(sharedVn, std::memory_order_release);
                    NEXTKEY_LOG(L"OnTickPoll: synced vietnamese mode from SharedState (%s)",
                                sharedVn ? L"Vietnamese" : L"English");
                    NotifyModeChange();
                }

                // TSF_TIP_ACTIVE transition → tray icon update (even if mode unchanged)
                const bool wasTipActive = (prevFlags & SharedFlags::TSF_TIP_ACTIVE) != 0;
                const bool isTipActive  = (curFlags  & SharedFlags::TSF_TIP_ACTIVE) != 0;
                if (wasTipActive != isTipActive) {
                    NEXTKEY_LOG(L"OnTickPoll: TSF_TIP_ACTIVE %s → %s",
                                wasTipActive ? L"true" : L"false",
                                isTipActive  ? L"true" : L"false");
                    NotifyModeChange();
                }
            }
        }

        // Adaptive-tick — handles the active-to-idle direction (cadence
        // grows as MarkActivity timestamp ages). The idle-to-active direction
        // is handled separately by MarkActivity → workerSignalFn_ →
        // workHandler → RetuneCadenceIfNeeded.
        // See docs/plans/2026-05-27-adaptive-tick-idle-backoff.md.
        RetuneCadenceIfNeeded();

        // Smart-switch debounced persistence (worker thread side).
        // Bug 2 + 3 fix (2026-05-28): mid-session changes (focus SAVE +
        // manual toggle) used to live only in RAM + shared memory and
        // were lost on crash / reset / unexpected exit. We now flush the
        // RCU snapshot once 3 s after the last MarkDirty.
        // See docs/plans/2026-05-28-smart-switch-persistence-design.md §3.
        constexpr std::uint64_t kSmartSwitchDebounceMs = 3000;
        if (focus_.AppModeDirty()) {
            const std::uint64_t now = GetTickCount64();
            if (now - focus_.LastDirtyTs() >= kSmartSwitchDebounceMs) {
                // Clear BEFORE snap: if hook publishes a new snap +
                // MarkDirty between Clear and Snap, the snap captures it
                // (release/acquire pair). With Snap→Clear there is a
                // window where the hook's latest mutation lands in
                // appModesSnap_ but our Clear wipes dirty → snap stays
                // on disk-write-pending forever until the next focus
                // event triggers another Mark.
                focus_.ClearAppModeDirty();
                auto snap = focus_.SnapshotAppModes();
                const bool ok = (snap != nullptr)
                    ? ConfigManager::SaveSmartSwitchApps(
                          ConfigManager::GetConfigPath(), *snap)
                    : true;
                if (!ok) {
                    // Re-arm dirty AND bump LastDirtyTs so the next
                    // retry waits a full debounce window (~3 s) instead
                    // of next tick (~200 ms). Prevents a 5-Hz save spin
                    // when SaveSmartSwitchApps keeps failing (AV
                    // scanning the .toml right after MoveFileEx, or
                    // another process holding the named mutex).
                    focus_.MarkAppModeDirty();
                    focus_.SetLastDirtyTs(now);
                    if (!lastFlushFailed_) {
                        NEXTKEY_LOG(L"SmartSwitch: persist FAILED, will retry");
                        lastFlushFailed_ = true;
                    }
                } else if (lastFlushFailed_) {
                    NEXTKEY_LOG(L"SmartSwitch: persist recovered");
                    lastFlushFailed_ = false;
                }
            }
        }
    } catch (const std::exception& e) {
        CrashLog(L"HookEngine::OnTickPoll", e.what());
    } catch (...) {
        CrashLog(L"HookEngine::OnTickPoll", "(non-std exception)");
    }
}

// Wave 3 PR 3.2 — RefreshFocusCache, LookupAppProfile, StoreAppProfile,
// and the heavy ClassifyFocusedWindow body (now FocusOwner::Classify with
// a ConfigContext parameter) moved to FocusOwner.cpp. OnFocusChanged
// below builds the ConfigContext and dispatches to focus_.Classify().

HookEngine::FocusClassifyRequest
HookEngine::CaptureFocusClassifyRequest(HWND triggerHwnd) noexcept {
    // Serial publication is the invalidation point. A result already in the
    // worker/mailbox becomes stale as soon as a newer foreground request is
    // captured, even before the newer heavy classification completes.
    const std::uint64_t serial =
        latestFocusRequestSerial_.fetch_add(1, std::memory_order_relaxed) + 1;
    return FocusClassifyRequest{
        .triggerHwnd = triggerHwnd,
        .requestSerial = serial,
        .inputEpochAtRequest =
            physicalInputEpoch_.load(std::memory_order_relaxed),
    };
}

void HookEngine::OnFocusChanged(HWND triggerHwnd) {
    // Worker-thread doctrine §12.4 (docs/CODING_RULES/12-worker-thread-doctrine.md).
    //
    // Producer side — runs on whichever thread invoked us (typically the
    // WinEvent installer thread = main, via FocusOwner::WinEventProc).
    // Pre-3.6 this body ran QuickSync + focus_.Classify inline. That meant
    // focus_'s plain `appProfileCache_` / `webView2PositiveCache_` got
    // mutated from main here AND from the worker thread inside OnTickPoll's
    // PID-change branch — concurrent unordered_map ops = UB.
    //
    // Post-3.6: produce-only. Latch an immutable request + signal the worker;
    // worker drains via `DrainClassifyOnWorker` on its own thread, restoring
    // the single-writer invariant for the cache containers. The request also
    // carries ordering evidence used by the hook to reject/defer a result that
    // completed after newer focus/input activity.
    //
    // Coalescing property (§12.4): a burst of WinEvent fires latches into
    // the same slot; the worker classifies once with the latest HWND. No
    // pile-up of redundant heavy work under focus storms (Alt-Tab spam,
    // taskbar flyouts, JumpList transients).
    auto request = std::make_shared<const FocusClassifyRequest>(
        CaptureFocusClassifyRequest(triggerHwnd));
    pendingClassifyRequest_.store(std::move(request), std::memory_order_release);

    // Adaptive-tick — DO NOT call MarkActivity here. Earlier draft did, but
    // WinEventProc fires on EVERY EVENT_SYSTEM_FOREGROUND including noisy
    // sources that are NOT user activity: tooltip popups, taskbar flyouts,
    // background app windows (NZXT, PowerToys), notification centre, IME
    // candidate windows. Bumping cadence on every focus event keeps the
    // worker pinned at 200 ms tick forever on a busy desktop and the
    // adaptive backoff never reaches the 1 s / 5 s buckets — pages stay
    // warm, Windows can't trim. (Observed 2026-05-27: benchmark Run-2 of
    // PR 1 stuck at 1.62 MB Private WS vs Run-1 trimming to 1.41 MB; the
    // delta correlated with how many background apps fired focus events
    // during the idle window.) The keystroke path (LowLevelKeyboardProc)
    // remains the activity source — it can't be falsified by background
    // UI noise. Trade-off: a layout/IME switch via language-bar click
    // without a keystroke may lag up to 5 s after long idle (next tick
    // resumes 200 ms cadence). Acceptable — the common case is
    // keystroke-driven, and keystrokes are the activity source.

    if (workerSignalFn_) workerSignalFn_();
}

void HookEngine::DrainClassifyOnWorker() {
    // Worker-thread doctrine §12.4 drain. Consumes the latch slot and runs
    // the heavy classify body on the worker thread. Called from the
    // workHandler wired in main.cpp.
    auto request = pendingClassifyRequest_.exchange(
        nullptr, std::memory_order_acquire);
    if (!request) return;
    OnFocusChangedSyncOnWorker(*request);
}

void HookEngine::RequestGameModeToggle() noexcept {
    VKEY_ASSERT_HOOK_THREAD();
    // Rule 11.2: one release store + one std::function call. Everything
    // expensive (TOML read/modify/write) happens in the worker drain.
    pendingGameModeToggle_.store(true, std::memory_order_release);
    if (workerSignalFn_) workerSignalFn_();
}

void HookEngine::DrainGameModeToggleOnWorker() {
    if (!pendingGameModeToggle_.exchange(false, std::memory_order_acquire)) return;

    const std::wstring exe = FocusOwner::GetExeNameForHwnd(GetForegroundWindow());
    if (exe.empty()) {
        Logger::Log(L"[VKey] GameMode toggle: no foreground exe — ignored");
        return;
    }

    const auto path = ConfigManager::GetConfigPath();
    auto entries = ConfigManager::LoadAppOverrides(path);

    // Toggle. Send method 5 is one slot in a single-value-per-app field, so
    // turning game mode ON discards any other send-method override the user
    // had for this exe — which is the honest outcome, since 5 and 1-4 are
    // mutually exclusive by construction. Turning it OFF drops the entry only
    // when nothing else is configured on it; otherwise it falls back to -1 so
    // the app's input-method / encoding overrides survive.
    auto it = entries.find(exe);
    const bool wasGame = (it != entries.end() && it->second.sendMethod == 5);
    if (wasGame) {
        if (it->second.inputMethod < 0 && it->second.encodingOverride < 0) {
            entries.erase(it);
        } else {
            it->second.sendMethod = -1;
        }
    } else {
        entries[exe].sendMethod = 5;
    }

    if (!ConfigManager::SaveAppOverrides(path, entries)) {
        Logger::Log(L"[VKey] GameMode toggle: save failed for '%ls'", exe.c_str());
        return;
    }
    SignalConfigChange();

    // Persisting is not enough to make the change felt, and waiting for the
    // ordinary paths costs seconds the user experiences as "the hotkey didn't
    // work yet":
    //   • The config snapshot only rebuilds when a later worker wake notices
    //     the generation bump — and the wake cadence backs off while idle.
    //   • The game-replay output policy is published by focus apply, and OnTickPoll
    //     only re-classifies when the foreground PID *changes*. Pressing a
    //     hotkey inside a game changes nothing, so without forcing it here the
    //     new value would not land until the user alt-tabbed away and back.
    // Both are safe to run inline: we are on the worker thread, which is the
    // §12.5 exemption OnTickPoll already relies on for the same call.
    RebuildSnapshotFromToml(
        static_cast<std::uint32_t>(lastConfigGeneration_.load(std::memory_order_acquire)));
    OnFocusChangedSyncOnWorker(CaptureFocusClassifyRequest(nullptr));

    Logger::Log(L"[VKey] GameMode toggle: '%ls' → %ls (applied)", exe.c_str(),
                wasGame ? L"off" : L"on");
}

// Adaptive-tick (plan docs/plans/2026-05-27-adaptive-tick-idle-backoff.md).
// Callable from any thread; safe on the hook hot path per Rule 11.2.
//
// Hot path (already in active cadence — the common case):
//   1 relaxed atomic store + 1 relaxed atomic load + 1 branch ≈ 5 ns.
//
// Cold path (idle → active transition, fires at most once per idle cycle):
//   + 1 workerSignalFn_ invocation = MainThreadWorker::Signal which acquires
//   an uncontended mutex (~30-50 ns) and notifies the worker CV (~50-200 ns).
//   Worker then runs workHandler → RetuneCadenceIfNeeded → SetTickInterval.
//
// The currentTickIntervalMs_ gate is essential: without it, every keystroke
// would Signal the worker → workHandler runs SyncConfigFromSharedState +
// DrainClassifyOnWorker (~1-10 ms cold cache) on every key — wasted work
// since cadence is already at the active 200 ms.
void HookEngine::MarkActivity() noexcept {
    lastActivityTickMs_.store(GetTickCount64(), std::memory_order_relaxed);
    if (currentTickIntervalMs_.load(std::memory_order_relaxed) != NextKey::kTickActiveMs) {
        // Resuming from idle/STOP. vietnameseMode_ otherwise syncs from
        // SharedState only inside OnTickPoll (200ms worker tick) — which is
        // exactly what was parked. workerSignalFn_() wakes the worker, but
        // that wake + DrainClassifyOnWorker + mailbox post is async and can
        // land after THIS keystroke already dispatched with a stale mode
        // (2026-07 field report: first word after ~5 min idle types in the
        // wrong V/E mode; delete+retype "fixes" it once the worker catches
        // up). Re-sync inline here — cheap (mmap struct read, no alloc),
        // safe on the hook thread per Rule 11.2 — so the very first resumed
        // key already sees the right mode.
        if (sharedStatePtr_) {
            const SharedState st = sharedStatePtr_->Read();
            if (st.IsValid()) {
                const bool sharedVn = (st.flags & SharedFlags::VIETNAMESE_MODE) != 0;
                if (sharedVn != vietnameseMode_.load(std::memory_order_acquire)) {
                    vietnameseMode_.store(sharedVn, std::memory_order_release);
                    NotifyModeChange();
                }
            }
        }
        if (workerSignalFn_) workerSignalFn_();
    }
}

// Adaptive-tick — recomputes the desired tick interval from the elapsed time
// since last MarkActivity and republishes it to MainThreadWorker if it
// changed. No-op when the cadence is already correct. Called from BOTH:
//   * OnTickPoll (worker tick path) — handles the regular age-out from
//     active → idle as time passes.
//   * The workHandler wired in main.cpp (worker signal path) — handles
//     the resume from idle → active when MarkActivity signals the worker.
// Both call sites are on the worker thread; this method is not safe to call
// on the hook thread (tickRetuneFn_ may take MainThreadWorker's mutex).
void HookEngine::RetuneCadenceIfNeeded() noexcept {
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t lastAct = lastActivityTickMs_.load(std::memory_order_relaxed);
    const std::uint64_t idleMs = (now > lastAct) ? (now - lastAct) : 0;
    const auto desired = NextKey::ComputeTickInterval(idleMs);
    const auto desiredMs = static_cast<std::uint32_t>(desired.count());
    if (desiredMs != currentTickIntervalMs_.load(std::memory_order_relaxed)) {
        currentTickIntervalMs_.store(desiredMs, std::memory_order_relaxed);
        if (tickRetuneFn_) tickRetuneFn_(desired);
    }
}

void HookEngine::OnFocusChangedSyncOnWorker(
        const FocusClassifyRequest& request) {
    // Doctrine §12.5 exemption #1: OnTickPoll's PID-change branch already
    // runs on the worker thread, so it calls this directly without the
    // latch+signal hop. WinEventProc producers go through OnFocusChanged →
    // DrainClassifyOnWorker → here.
    //
    // P2c fix (2026-05-19) preserved: SettingsDialog is the project's "live
    // config bus" — every toggle bumps configGeneration in SharedState
    // immediately. Running QuickSync at focus-change time means a user who
    // toggles in Settings then clicks back to the target app sees the
    // toggle apply BEFORE typing the first character. Wave 2 made
    // QuickSync's slow path lock-free (atomics + RCU publish); 3.6
    // additionally routes the heap-allocating slow body through the
    // worker (this thread), so no Rule 11.2 violation is possible here.
    // Coalescing can leave an older request already dequeued when a newer
    // WinEvent arrives. Avoid its heavy classify entirely when possible. A
    // newer event can still arrive during Classify(); the hook-side serial
    // check below remains the final authority.
    if (request.requestSerial !=
        latestFocusRequestSerial_.load(std::memory_order_relaxed)) {
        return;
    }

    QuickSyncFromSharedState();

    FocusOwner::ConfigContext ctx{
        config_.load(std::memory_order_acquire),
        configSnapshot_.load(std::memory_order_acquire),
        static_cast<int>(globalCodeTable_.load(std::memory_order_acquire)),
        static_cast<int>(globalInputMethod_.load(std::memory_order_acquire)),
    };
    FocusClassification classified =
        focus_.Classify(request.triggerHwnd, ctx);
    classified.requestSerial = request.requestSerial;
    classified.inputEpochAtRequest = request.inputEpochAtRequest;

    if (!classified.hwndOpaque) return;  // sentinel: nothing to apply

    // Tier 2 spurious event filter (Issue #273):
    // If request.triggerHwnd is a stale trigger from a background helper/tool window
    // (such as FastStone Capture's recorder toolbar timer updating every 1s) and the
    // live foreground window AND focused child target have not changed at all,
    // drop the event on the worker without posting to the hook mailbox.
    const std::uintptr_t currentTarget =
        (classified.focusedChildHwndOpaque != 0)
            ? classified.focusedChildHwndOpaque
            : classified.hwndOpaque;
    const std::uintptr_t triggerHwndOpaque =
        reinterpret_cast<std::uintptr_t>(request.triggerHwnd);
    if (IsSpuriousBackgroundFocusEvent(
            triggerHwndOpaque, classified.hwndOpaque, currentTarget,
            lastDispatchedHwnd_, lastDispatchedTargetHwnd_)) {
        FOCUS_LOG(L"  Worker: drop spurious focus event from %p (target %p unchanged)",
                  request.triggerHwnd, reinterpret_cast<HWND>(currentTarget));
        return;
    }

    lastDispatchedHwnd_ = classified.hwndOpaque;
    lastDispatchedTargetHwnd_ = currentTarget;

    OutputDispatcher::PrepareFocusOutput(classified);

    // Process creation time is a cold Win32 query and therefore belongs on
    // this worker, never in DrainHookCommands when it may run inside the LL
    // callback. Query only for the exact compatibility target.
    if (classified.exeName == L"dorion.exe") {
        const DorionProcessIdentity identity =
            HookLifecycle::ResolveProcessIdentity(classified.pid);
        classified.processCreationTime = identity.creationTime;
    }

    // A newer request may arrive while Classify() is doing HWND/app probes.
    // Reject here as well as on the hook thread so stale UI context is never
    // published and the mailbox avoids carrying work it already knows is old.
    if (request.requestSerial !=
        latestFocusRequestSerial_.load(std::memory_order_relaxed)) {
        return;
    }

    // Tooltip publication belongs on the worker: formatting and the tray
    // snapshot allocation must never enter the low-level keyboard callback.
    // Unlike ApplyFocusOnHookThread's per-app fast-path gate, this runs for
    // ordinary apps too, which is required for the app name to stay current
    // under the default (all per-app features disabled) configuration.
    if (focusAppContextCallback_ &&
        !classified.skipAppTracking &&
        !classified.exeName.empty()) {
        std::wstring_view ruleText;
        if (classified.isExcluded) {
            ruleText = L"Lock E";
        } else if (classified.isForcedVietnamese) {
            ruleText = L"Lock V";
        } else if (ctx.cfg && ctx.cfg->smartSwitch) {
            ruleText = L"Smart Switch";
        }
        const bool isRustEngine =
            ctx.cfg && EngineFactory::WillUseRustEngine(*ctx.cfg);
        focusAppContextCallback_(
            classified.exeName, ruleText, classified.isTsf, isRustEngine);
    }

    auto cls = std::make_shared<const FocusClassification>(
        std::move(classified));
    lifecycle_.Mailbox().Post(HookCommand::kFocusChanged, std::move(cls));
}

/// Replace on-screen text by diffing previousComposition_ vs newText.
///
/// ## U+202F "needEmpty" mechanism (skipEmptyChar_ == false)
///
/// Some Win32 apps swallow BS at certain cursor positions (start of line, empty
/// field, after autocomplete selection in browsers). To guarantee BS always
/// deletes something, we insert U+202F (NARROW NO-BREAK SPACE) as a "bait"
/// character before the BS sequence, then include one extra BS to remove it:
///
///   [insert U+202F] → [BS × (n+1)] → [type new chars]
///
/// U+202F is chosen because:
///   - It is a real Unicode character that apps must insert into the text buffer
///   - It is NOT U+0020 (regular space), so it doesn't trigger word commit
///   - It is narrow/invisible in most fonts, minimizing visual flicker
///
/// This mechanism is ONLY safe for apps that reliably insert U+202F into their
/// text buffer. Apps that ignore or filter it will receive n+1 BS for n chars,
/// deleting one extra character and permanently desyncing previousComposition_.
///
/// Apps with skipEmptyChar_=true (block reinjectVk, skip U+202F bait):
///   - Electron/Console: also get split dispatch — selected by the factory
///     (WindowClassification.isElectron / .isConsole → SplitDispatchInjector
///     with sleepMs=6 / 5 respectively). Sprint 2 D3 lifted the dispatch
///     branching out of HookEngine into the injector layer.
///   - GPU-rendered apps (Zed): batch dispatch via Win32SendInputInjector
///     (single-process, no IPC reorder).
///
/// Wave 2 — Pipeline::IBackwardEditExecutor adapter. Thin wrapper so
/// `BackwardEditFeature` (in `src/core/pipeline/`) can delegate the backward
/// edit through an interface without coupling to the full HookEngine class.
/// Wave 3+ will split this into pure-diff (feature) + Stage A–E execute path.
void HookEngine::ExecuteReplace(std::wstring_view newText,
                                std::uint16_t reinjectVk) {
    ReplaceComposition(std::wstring{newText}, static_cast<DWORD>(reinjectVk));
}

/// Wave 3 — Pipeline::ICommitUndoExecutor adapter. Delegates to the FSM
/// body (renamed `HandleCommitUndoFsm` to disambiguate from this override).
/// vnMode is read fresh from vietnameseMode_ atomic — slightly different
/// from the legacy inline call site that took vnMode as a stack-local
/// param, but byte-equivalent because vietnameseMode_ writers are async
/// to the hook thread (main thread on toggle / config reload).
NextKey::Pipeline::CommitUndoOutcome HookEngine::HandleCommitUndo(
    std::uint16_t vkCode,
    bool shift, bool capsLock, bool ctrl, bool alt, bool win) {
    const bool vnMode = vietnameseMode_.load(std::memory_order_acquire);
    const KeyOutcome out =
        HandleCommitUndoFsm(static_cast<DWORD>(vkCode), vnMode,
                            shift, capsLock, ctrl, alt, win);
    switch (out) {
        case KeyOutcome::Eat:         return NextKey::Pipeline::CommitUndoOutcome::Eat;
        case KeyOutcome::Pass:        return NextKey::Pipeline::CommitUndoOutcome::Pass;
        case KeyOutcome::Fallthrough: return NextKey::Pipeline::CommitUndoOutcome::Fallthrough;
    }
    return NextKey::Pipeline::CommitUndoOutcome::Fallthrough;  // defensive
}

/// Wave 4b — Pipeline::IMacroExecutor adapter. Owns macro tracking +
/// dispatch. Body transcribed 1:1 from pre-W4b HandlePreDispatch:
///   - EN-mode block (pre-W4b lines 1512-1547)
///   - VN-mode tracking (1559-1572)
///   - VN SkipMacro hotkey (1602-1613)
///   - VN expansion (1615-1624)
/// Logic unchanged; the feature pipeline now owns the call site at step 2d.
/// Reads vnMode/macroOn/macroEng/configSnapshot/hotkeys atomics internally.
NextKey::Pipeline::MacroOutcome HookEngine::HandleMacro(
    std::uint16_t vkCode,
    bool shift, bool capsLock, bool ctrl, bool alt, bool win) {
    const DWORD vk = static_cast<DWORD>(vkCode);
    const bool vnMode = vietnameseMode_.load(std::memory_order_acquire);
    const bool macroOn = macroEnabled_.load(std::memory_order_acquire);
    const auto cfgSnap = configSnapshot_.load(std::memory_order_acquire);
    const bool hasMacros = cfgSnap && !cfgSnap->macroTable.empty();
    auto hotkeysSnap = hotkeys_.load(std::memory_order_acquire);
    const uint32_t currentMods = ComputeModMask(ctrl, shift, alt, win);

    // Check for macro undo on Backspace (issue #264)
    if (macroUndoState_.active) {
        DWORD now = GetTickCount();
        if (vk == VK_BACK && !ctrl && !alt && !win && (now - macroUndoState_.timestamp <= kMacroUndoTimeoutMs)) {
            size_t pendingBs = macroUndoState_.expansionLength +
                                (macroUndoState_.hasTrailingTrigger ? 1 : 0);
            std::wstring shortcut = std::move(macroUndoState_.shortcut);
            ClearMacroUndo();

            while (pendingBs > 0) {
                const auto count = (std::min)(pendingBs, size_t{60});
                if (!dispatcher_.ReplaceRaw(count, std::wstring_view{})) {
                    ResetComposition();
                    return NextKey::Pipeline::MacroOutcome::Eat;
                }
                pendingBs -= count;
            }
            for (size_t offset = 0; offset < shortcut.size(); offset += 60) {
                if (!dispatcher_.ReplaceRaw(0, std::wstring_view(shortcut).substr(offset, 60))) {
                    ResetComposition();
                    return NextKey::Pipeline::MacroOutcome::Eat;
                }
            }

            rawMacroBuffer_ = shortcut;
            macroCrossCommit_ = true;
            tempMacroOff_ = true;
            HOOK_LOG(L"  macro-undo: reverted expansion to shortcut '%ls'", shortcut.c_str());
            return NextKey::Pipeline::MacroOutcome::Eat;
        } else if (vk != VK_BACK) {
            ClearMacroUndo();
        }
    }

    // EN mode: only engages when macroOn && macroEng. Always returns Pass
    // (caller passes the key to OS as English) — except macro-expand which
    // can return Eat.
    if (!vnMode) {
        const bool macroEng = macroInEnglish_.load(std::memory_order_acquire);
        if (!(macroOn && macroEng)) {
            return NextKey::Pipeline::MacroOutcome::Fallthrough;
        }
        // Track macro keys in English mode.
        if (vk >= 0x41 && vk <= 0x5A) {
            const bool upper = shift != capsLock;  // XOR
            rawMacroBuffer_ += upper ? static_cast<wchar_t>(vk)
                                      : towlower(static_cast<wchar_t>(vk));
            if (rawMacroBuffer_.size() > kMaxRawMacroBuffer) ClearMacroBuffer();
        } else if (hotkeysSnap
                   && hotkeysSnap->Matches(NextKey::Intent::SkipMacro, vk, currentMods,
                                           /*isDoubleTap=*/false, /*keyUp=*/false)
                   && rawMacroBuffer_.empty()) {
            tempMacroOff_ = true;
            return NextKey::Pipeline::MacroOutcome::Pass;
        } else if (IsCommitTrigger(vk) && !tempMacroOff_) {
            const wchar_t triggerChar = VkToMacroChar(vk);
            if (triggerChar > L' ') rawMacroBuffer_ += triggerChar;
            if (!rawMacroBuffer_.empty() && IsMacroTrigger(vk)) {
                auto result = TryExpandMacro(triggerChar);
                if (result == MacroResult::ExpandedEatTrigger) {
                    return NextKey::Pipeline::MacroOutcome::Eat;
                }
                if (result == MacroResult::ExpandedPassTrigger) {
                    if (dispatcher_.SynthEventsPending() > 0) {
                        InjectKey(vk);
                        return NextKey::Pipeline::MacroOutcome::Eat;
                    }
                    return NextKey::Pipeline::MacroOutcome::Pass;
                }
            } else if (!IsMacroTrigger(vk)) {
                // Disabled trigger still marks word boundary — clear buffer.
                ClearMacroBuffer();
                tempMacroOff_ = false;
            }
        } else if (vk == VK_BACK && !rawMacroBuffer_.empty()) {
            rawMacroBuffer_.pop_back();
            if (rawMacroBuffer_.empty()) ClearMacroBuffer();
        } else if (!(vk >= 0x41 && vk <= 0x5A) && !IsCommitTrigger(vk)) {
            ClearMacroBuffer();
            tempMacroOff_ = false;
        }
        // EN mode always passes to OS unless macro ate the key above.
        return NextKey::Pipeline::MacroOutcome::Pass;
    }

    // VN mode tracking — accumulate alpha + commit-trigger chars when macros loaded.
    if (macroOn && hasMacros) {
        if (vk >= 0x41 && vk <= 0x5A) {
            const bool upper = shift != capsLock;  // XOR
            rawMacroBuffer_ += upper ? static_cast<wchar_t>(vk)
                                      : towlower(static_cast<wchar_t>(vk));
            if (rawMacroBuffer_.size() > kMaxRawMacroBuffer) ClearMacroBuffer();
        } else if (IsCommitTrigger(vk)) {
            const wchar_t ch = VkToMacroChar(vk);
            if (ch > L' ') rawMacroBuffer_ += ch;  // Printable non-space chars
        }
    }

    // VN SkipMacro hotkey — empty engine + empty buffer marks "skip next macro".
    if (macroOn && hasMacros
        && hotkeysSnap
        && hotkeysSnap->Matches(NextKey::Intent::SkipMacro, vk, currentMods,
                                /*isDoubleTap=*/false, /*keyUp=*/false)
        && engine_ && engine_->Count() == 0 && rawMacroBuffer_.empty()) {
        tempMacroOff_ = true;
        HOOK_LOG(L"  tempMacroOff: enabled by Esc");
        return NextKey::Pipeline::MacroOutcome::Pass;
    }

    // VN macro expansion — runs on commit trigger when buffer non-empty.
    if (macroOn && hasMacros && !tempMacroOff_
        && IsMacroTrigger(vk) && !rawMacroBuffer_.empty()) {
        const wchar_t triggerChar = VkToMacroChar(vk);
        auto result = TryExpandMacro(triggerChar);
        if (result == MacroResult::ExpandedEatTrigger) {
            return NextKey::Pipeline::MacroOutcome::Eat;
        }
        if (result == MacroResult::ExpandedPassTrigger) {
            if (dispatcher_.SynthEventsPending() > 0) {
                InjectKey(vk);
                return NextKey::Pipeline::MacroOutcome::Eat;
            }
            return NextKey::Pipeline::MacroOutcome::Pass;
        }
    }

    // Clean up macro buffer on word boundaries (commit triggers) and non-word keys in VN mode
    if (macroOn && hasMacros) {
        if (vk == VK_BACK && !rawMacroBuffer_.empty()) {
            rawMacroBuffer_.pop_back();
            if (rawMacroBuffer_.empty()) ClearMacroBuffer();
        } else if (IsCommitTrigger(vk)) {
            // If the trigger was disabled, or if it was enabled but did not result in expansion,
            // we clear the buffer unless it's a Space character that matches a multi-word macro
            // prefix, or an enabled printable (non-space) trigger — e.g. the '.' in "n.a" — which
            // DispatchKeyAction step 8's savedMacroBuffer/macroCrossCommit_ already knows how to
            // carry across the commit so the whole key can still match once the rest is typed.
            // (Regressed by 71b673d: this cleanup runs in HandleMacro, earlier in the pipeline
            // than step 8, so an unconditional clear here left step 8 nothing to preserve.)
            bool preserve = false;
            if (vk == VK_SPACE && cfgSnap) {
                // Reuse the member buffer's capacity instead of materializing a
                // temporary `rawMacroBuffer_ + L' '` on every Space (hot path,
                // Rule 11.2). IsSpaceMacroPrefix is noexcept and read-only, so the
                // append/pop pair restores the buffer with no per-keystroke alloc.
                rawMacroBuffer_.push_back(L' ');
                preserve = IsSpaceMacroPrefix(rawMacroBuffer_, cfgSnap->macroPrefixIndex);
                rawMacroBuffer_.pop_back();
            } else if (IsMacroTrigger(vk)) {
                preserve = VkToMacroChar(vk) > L' ';
            }
            if (!preserve) {
                ClearMacroBuffer();
                tempMacroOff_ = false;
            }
        } else if (!(vk >= 0x41 && vk <= 0x5A)) {
            // Non-alphabetic and non-commit keys (like F1-F12, modifiers alone, etc.) clear the buffer.
            ClearMacroBuffer();
            tempMacroOff_ = false;
        }
    }

    return NextKey::Pipeline::MacroOutcome::Fallthrough;
}

/// Wave 4a — Pipeline::IEscRestoreRawExecutor adapter. Resolves the
/// hotkey registry (CancelComposition intent) + composition-active gate
/// internally, then delegates to TryEscRestoreRaw. Mirrors the inline
/// check that previously lived at HandlePreDispatch lines 1578-1582
/// (now removed). MOD-CANCEL secondary site at line 1981 still calls
/// TryEscRestoreRaw directly — different trigger flow (modifier release),
/// out of W4a scope.
NextKey::Pipeline::EscRestoreOutcome HookEngine::TryEscRestore(
    std::uint16_t vkCode,
    bool shift, bool ctrl, bool alt, bool win) {
    auto hotkeysSnap = hotkeys_.load(std::memory_order_acquire);
    if (!hotkeysSnap) {
        return NextKey::Pipeline::EscRestoreOutcome::Fallthrough;
    }
    const uint32_t mods =
        (shift ? NextKey::kModShift : 0u) |
        (ctrl  ? NextKey::kModCtrl  : 0u) |
        (alt   ? NextKey::kModAlt   : 0u) |
        (win   ? NextKey::kModWin   : 0u);
    const bool hotkeyMatch = hotkeysSnap->Matches(
        NextKey::Intent::CancelComposition,
        static_cast<uint32_t>(vkCode),
        mods,
        /*isDoubleTap=*/false,
        /*keyUp=*/false);
    const bool hasLiveComposition = (engine_ && engine_->Count() > 0);
    const bool hasPrimedCommit =
        (commitState_.IsPrimed()) &&
        !commitState_.StackEmpty() &&
        !commitState_.StackTop().rawInput.empty();
    if (hotkeyMatch && (hasLiveComposition || hasPrimedCommit)) {
        const KeyOutcome legacy = TryEscRestoreRaw();
        return (legacy == KeyOutcome::Eat)
            ? NextKey::Pipeline::EscRestoreOutcome::Eat
            : NextKey::Pipeline::EscRestoreOutcome::Fallthrough;
    }
    return NextKey::Pipeline::EscRestoreOutcome::Fallthrough;
}

/// Wave 2 — pipeline dispatch entry point used by the six call-sites that
/// previously invoked `ReplaceComposition` directly. Builds the per-keystroke
/// session view + KeyContext, hands them to coordinator_, drains the channel.
/// Modifier flags are placeholders (false) in W2 — BackwardEditFeature does
/// not read them; future PreEngine features will need real values plumbed
/// down from ProcessKeyDown.
void HookEngine::DispatchCoordinator(DWORD vkCode, DWORD reinjectVk,
                                      const std::wstring& composition) {
    std::wstring_view rawSnapshot = engine_ ? engine_->PeekRawView() : std::wstring_view{};
    NextKey::Pipeline::HookCompositionSession session(
        previousComposition_, composition, rawSnapshot);
    NextKey::Pipeline::KeyContext keyCtx{
        static_cast<std::uint16_t>(vkCode),
        L'\0',
        false, false, false, false, false,
        &session,
        static_cast<std::uint16_t>(reinjectVk)
    };
    coordinator_.HandleKeyAtStage(
        NextKey::Pipeline::Stage::PostEngine, keyCtx, outputChannel_);
    (void)outputChannel_.DrainBatch();  // W2: feature delegates synchronously, batch is empty (no-alloc drain).
}

/// Wave 3 PR 3.3 — outer shell only. Computes the prefix diff against
/// `previousComposition_` (engine state) + encodes for non-Unicode code
/// tables + updates `previousComposition_` / `previousEncodedWidths_`,
/// then delegates the actual SendInput / RichEdit-retry / clipboard
/// fallback orchestration to `dispatcher_`. Detection logic + injector
/// publish lives in OnFocusChanged (focus_-driven).
void HookEngine::ReplaceComposition(const std::wstring& newText, DWORD reinjectVk) {
    VKEY_ASSERT_HOOK_THREAD();
    PERF_SCOPE(::NextKey::Perf::Stage::Replace);
    HWND target = GetInputTarget();
    if (!target) {
        previousComposition_ = newText;
        return;
    }

    // Find common prefix at Unicode level — only replace what actually changed.
    size_t commonLen = 0;
    size_t minLen = (std::min)(previousComposition_.size(), newText.size());
    while (commonLen < minLen && previousComposition_[commonLen] == newText[commonLen]) {
        commonLen++;
    }

    const CodeTable ct = currentCodeTable_.load(std::memory_order_acquire);

    // ── Non-Unicode code table path ──
    if (ct != CodeTable::Unicode) {
        // Calculate backspace count from encoded widths of chars being replaced.
        size_t backspaceCount = 0;
        for (size_t i = commonLen; i < previousEncodedWidths_.size(); ++i) {
            backspaceCount += previousEncodedWidths_[i];
        }

        // Convert new chars to encoded form.
        std::wstring encodedToSend;
        std::vector<uint8_t> newWidths;
        for (size_t i = commonLen; i < newText.size(); ++i) {
            auto enc = CodeTableConverter::ConvertChar(newText[i], ct);
            encodedToSend += enc.units[0];
            if (enc.count == 2) encodedToSend += enc.units[1];
            newWidths.push_back(enc.count);
        }

        HOOK_LOG(L"  ReplaceComposition[encoded]: prev='%s' new='%s' common=%zu BS=%zu encodedLen=%zu reinjectVk=0x%02X",
                 previousComposition_.c_str(), newText.c_str(), commonLen, backspaceCount,
                 encodedToSend.size(), reinjectVk);

        // Encoded path: no retry-loop, no clipboard fallback, no reinjectVk
        // prepend (preserves pre-Wave-3-PR-3.3 semantics — reinjectVk was
        // logged but never acted upon in the encoded branch).
        if (backspaceCount > 0 || !encodedToSend.empty()) {
            (void)dispatcher_.ReplaceRaw(backspaceCount,
                                          std::wstring_view(encodedToSend));
        }

        // Update widths: keep [0..commonLen), append newWidths.
        previousEncodedWidths_.resize(commonLen);
        previousEncodedWidths_.insert(previousEncodedWidths_.end(),
                                      newWidths.begin(), newWidths.end());
        previousComposition_ = newText;
        return;
    }

    // ── Unicode path ──
    size_t backspaceCount = previousComposition_.size() - commonLen;
    std::wstring toSend = newText.substr(commonLen);

    HOOK_LOG(L"  ReplaceComposition: prev='%s' new='%s' common=%zu BS=%zu send='%s' reinjectVk=0x%02X",
             previousComposition_.c_str(), newText.c_str(), commonLen, backspaceCount,
             toSend.c_str(), reinjectVk);

    // Dispatcher handles the full retry+fallback orchestration:
    //   1. IsSyncReplaceChannel? → RichEdit 30ms retry-loop, fall through on exhaust.
    //   2. ShouldUseClipboard → TryEditMessagePaste + clipboard fallback chain
    //      (BS-adjusts when reinjectVk != 0).
    //   3. Generic SendInput with optional reinjectVk prepend.
    dispatcher_.ReplaceUnicode(backspaceCount,
                                std::wstring_view(toSend),
                                static_cast<std::uint16_t>(reinjectVk));

    previousComposition_ = newText;
}

HookEngine::KeyOutcome HookEngine::TryEscRestoreRaw() {
    auto inj = dispatcher_.GetInjector();

    // Path 1: live composition (existing behavior).
    if (engine_->Count() > 0) {
        const size_t composedCount = engine_->Count();
        const std::wstring raw = engine_->PeekRaw();
        if (raw.empty()) return KeyOutcome::Fallthrough;
        HOOK_LOG(L"  EscRestoreRaw[live]: bs=%zu raw='%ls'", composedCount, raw.c_str());
        bool injOk;
        { PERF_SCOPE(::NextKey::Perf::Stage::Injector);
          injOk = inj->Replace(composedCount, std::wstring_view(raw)); }
        if (!injOk) {
            HOOK_LOG(L"  EscRestoreRaw[live]: injector reported partial delivery");
            // Don't reset on failure — next user action recovers via normal flow.
            return KeyOutcome::Fallthrough;
        }
        engine_->Reset();
        ClearMacroBuffer();
        tempMacroOff_ = false;
        return KeyOutcome::Eat;
    }

    // Path 2: post-BS (engine empty, raw snapshot in commitStack top).
    // Engine empty here; rawInput preserved in commitStack_ from CommitComposition
    // snapshot. CancelCommitUndo clears stack (single-word scope per design 2026-05-17).
    if (!commitState_.IsPrimed() || commitState_.StackEmpty()) {
        return KeyOutcome::Fallthrough;
    }
    if (GetTickCount() - commitState_.ReadyTime() > kCommitUndoTimeoutMs) {
        HOOK_LOG(L"  EscRestoreRaw[post-BS]: Primed expired (elapsed > %ums)", kCommitUndoTimeoutMs);
        CancelCommitUndo();
        return KeyOutcome::Fallthrough;
    }
    const auto& top = commitState_.StackTop();
    if (top.rawInput.empty()) return KeyOutcome::Fallthrough;
    // Primed: trailing commit-trigger already deleted by user's BS. BS count covers
    // the committed body only. Non-Unicode code tables (TCVN3, VNI-Win) encode each
    // wchar_t into multiple bytes — mirror HandleBackspace's width-sum logic.
    size_t bsCount = top.text.size();
    if (currentCodeTable_.load(std::memory_order_acquire) != CodeTable::Unicode) {
        bsCount = 0;
        for (auto w : top.widths) bsCount += w;
    }
    HOOK_LOG(L"  EscRestoreRaw[post-BS]: bs=%zu raw='%ls' text='%ls'",
             bsCount, top.rawInput.c_str(), top.text.c_str());
    if (!inj->Replace(bsCount, std::wstring_view(top.rawInput))) {
        HOOK_LOG(L"  EscRestoreRaw[post-BS]: injector reported partial delivery");
        return KeyOutcome::Fallthrough;
    }
    CancelCommitUndo();
    ClearMacroBuffer();
    tempMacroOff_ = false;
    return KeyOutcome::Eat;
}


// ═══════════════════════════════════════════════════════════
// Modifier Tracking — feeds double-Alt + layout-change detection
// ═══════════════════════════════════════════════════════════

void HookEngine::TrackModifier(DWORD vkCode, bool isDown) {
    // Snapshot the *other* modifiers before mutating, so a fresh modifier-down
    // that joins an existing hold latches modComboSeen_ (combo contamination).
    // Auto-repeat re-enters with the matching modXxxDown_ already true, so the
    // inner `!modXxxDown_` guard prevents a held key from contaminating itself.
    const bool ctrlWasDown  = modCtrlDown_;
    const bool shiftWasDown = modShiftDown_;
    const bool altWasDown   = modAltDown_;
    const bool winWasDown   = modWinDown_;
    switch (vkCode) {
        case VK_LCONTROL: case VK_RCONTROL:
            if (isDown && !modCtrlDown_) {
                modCtrlDown_ = true; otherKeyPressed_ = false;
                if (shiftWasDown || altWasDown || winWasDown) modComboSeen_ = true;
            } else if (!isDown) modCtrlDown_ = false;
            break;
        case VK_LSHIFT: case VK_RSHIFT:
            if (isDown && !modShiftDown_) {
                modShiftDown_ = true; otherKeyPressed_ = false;
                if (ctrlWasDown || altWasDown || winWasDown) modComboSeen_ = true;
            } else if (!isDown) modShiftDown_ = false;
            break;
        case VK_LMENU: case VK_RMENU:
            if (isDown && !modAltDown_) {
                modAltDown_ = true; otherKeyPressed_ = false;
                if (ctrlWasDown || shiftWasDown || winWasDown) modComboSeen_ = true;
            } else if (!isDown) modAltDown_ = false;
            break;
        case VK_LWIN: case VK_RWIN:
            if (isDown && !modWinDown_) {
                modWinDown_ = true; otherKeyPressed_ = false;
                if (ctrlWasDown || shiftWasDown || altWasDown) modComboSeen_ = true;
            } else if (!isDown) modWinDown_ = false;
            break;
    }
    // Session ends — and the combo latch clears — only once every modifier is
    // up. This runs AFTER ProcessKeyUp's modifier-release matching reads the
    // latch, so the trailing release of a combo is still seen as contaminated.
    if (!isDown && !modCtrlDown_ && !modShiftDown_ && !modAltDown_ && !modWinDown_) {
        modComboSeen_ = false;
    }
}

// ═══════════════════════════════════════════════════════════
// Commit Trigger Check
// ═══════════════════════════════════════════════════════════

void HookEngine::InjectKey(DWORD vkCode) {
    // Wave 3 PR 3.3 — delegated to dispatcher. Re-entrant gate (sending_)
    // + watchdog timestamp (lastSynthSendTime_ only — lastRealSynthTime_
    // stays unchanged because InjectKey is re-injection, NOT typing).
    dispatcher_.InjectKey(static_cast<std::uint16_t>(vkCode));
}

bool HookEngine::IsCommitTrigger(DWORD vkCode) {
    // Single source of truth: Macro::IsCommitTrigger (core, Linux-tested).
    // The VK set here was a byte-identical duplicate of that table.
    return Macro::IsCommitTrigger(vkCode);
}

bool HookEngine::IsOemPunctVk(DWORD vkCode) {
    if (vkCode >= VK_OEM_1 && vkCode <= VK_OEM_3) return true;
    if (vkCode >= VK_OEM_4 && vkCode <= VK_OEM_8) return true;
    if (vkCode == VK_OEM_PLUS || vkCode == VK_OEM_COMMA ||
        vkCode == VK_OEM_MINUS || vkCode == VK_OEM_PERIOD) return true;
    return false;
}

bool HookEngine::IsMacroTrigger(DWORD vkCode) const {
    auto cfg = config_.load(std::memory_order_acquire);
    return NextKey::Macro::ShouldTrigger(
        static_cast<uint32_t>(vkCode),
        cfg->macroTriggerSpace,
        cfg->macroTriggerEnter,
        cfg->macroTriggerTab,
        cfg->macroTriggerDir
    );
}

HookEngine::MacroResult HookEngine::TryExpandMacro(wchar_t triggerChar) {
    Win32CaseMapper mapper;
    const auto typingConfig = config_.load(std::memory_order_acquire);
    // Phase 3c: macro table comes from the RCU snapshot. The shared_ptr
    // local keeps the table alive for the duration of Macro::Plan even
    // if a worker thread republishes mid-call.
    auto snap = configSnapshot_.load(std::memory_order_acquire);
    Macro::PlanInputs inputs{
        .rawMacroBuffer        = rawMacroBuffer_,
        .previousComposition   = previousComposition_,
        .previousEncodedWidths = previousEncodedWidths_,
        .macroTable            = snap->macroTable,
        .macroCrossCommit      = macroCrossCommit_,
        .currentCodeTable      = currentCodeTable_.load(std::memory_order_acquire),
        .autoCapsEnabled       = typingConfig->autoCapsMacro,
        .titleCaseEnabled      = typingConfig->macroTitleCase,
        .wasFirstCharAutoCapped = wasFirstCharAutoCapped_,
        .triggerChar           = triggerChar,
        .clipboardThreshold    = kMacroClipboardThreshold,
    };
    auto plan = Macro::Plan(inputs, mapper);
    if (!plan.matched) return MacroResult::NoMatch;

    auto inj = dispatcher_.GetInjector();

    std::size_t expansionLength = 0;
    bool expansionDelivered = true;
    if (plan.useClipboard) {
        if (plan.bsCount > 0) {
            (void)dispatcher_.ReplaceRaw(plan.bsCount, std::wstring_view{});
        }
        auto clipText = Macro::ExpandEscapesForClipboard(plan.expansion);
        dispatcher_.ClipboardPasteText(clipText);
        expansionLength = clipText.size();
        HOOK_LOG(L"  TryExpandMacro: clipboard paste %zu chars (raw %zu)",
                 clipText.size(), plan.expansion.size());
    } else {
        std::size_t pendingBs = plan.bsCount;
        for (const auto& s : Macro::BuildSegments(plan.expansion, currentCodeTable_.load(std::memory_order_acquire))) {
            if (s.isReturn) {
                if (pendingBs > 0) {
                    expansionDelivered = dispatcher_.ReplaceRaw(pendingBs, std::wstring_view{}) && expansionDelivered;
                    pendingBs = 0;
                }
                dispatcher_.InjectKey(VK_RETURN);
                expansionLength += 1;
            } else if (!s.text.empty()) {
                expansionDelivered = dispatcher_.ReplaceRaw(pendingBs, std::wstring_view(s.text)) && expansionDelivered;
                pendingBs = 0;
                expansionLength += s.text.size();
            }
        }
        if (pendingBs > 0) {
            expansionDelivered = dispatcher_.ReplaceRaw(pendingBs, std::wstring_view{}) && expansionDelivered;
        }
    }

    ClearWordState();
    CancelCommitUndo();

    auto cfg = config_.load(std::memory_order_acquire);
    if (cfg && cfg->macroUndoOnBackspace && expansionDelivered && !plan.shortcut.empty()
        && triggerChar >= L' ' && !plan.useClipboard
        && currentCodeTable_.load(std::memory_order_acquire) == CodeTable::Unicode
        && Macro::CanUndoWithBackspaces(plan.expansion)) {
        macroUndoState_.shortcut = std::move(plan.shortcut);
        macroUndoState_.expansionLength = expansionLength;
        macroUndoState_.hasTrailingTrigger = !plan.isPartOfMacro;
        macroUndoState_.timestamp = GetTickCount();
        macroUndoState_.active = true;
    } else {
        ClearMacroUndo();
    }
    return plan.isPartOfMacro ? MacroResult::ExpandedEatTrigger
                              : MacroResult::ExpandedPassTrigger;
}

wchar_t HookEngine::VkToMacroChar(DWORD vkCode) noexcept {
    // Translate VK → character with the current modifier state, so Shift/Caps/
    // AltGr yield the actual typed char (e.g. Shift+VK_OEM_PERIOD on US → '>'
    // instead of the unshifted '.'). Uses the foreground window's layout so
    // macros match what the target app would receive.
    //
    // Modifier state: GetKeyboardState is not reliable from a low-level hook
    // thread (LL hooks don't feed our message queue), so we build a minimal
    // key-state snapshot from GetAsyncKeyState for the modifiers ToUnicodeEx
    // actually consults.
    BYTE keyState[256] = {};
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) keyState[VK_SHIFT]   = 0x80;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) keyState[VK_CONTROL] = 0x80;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) keyState[VK_MENU]    = 0x80;
    if (GetKeyState(VK_CAPITAL) & 0x0001)      keyState[VK_CAPITAL] = 0x01;

    UINT scan = MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC);
    HWND fg = GetForegroundWindow();
    HKL layout = GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, nullptr) : 0);

    // wFlags bit 2 (0x4) = "do not change the keyboard state" — required so
    // ToUnicodeEx doesn't advance pending dead-key state. Win10 1607+.
    wchar_t buf[4] = {};
    int result = ToUnicodeEx(vkCode, scan, keyState, buf, 4, 0x4, layout);
    if (result > 0) {
        return static_cast<wchar_t>(towlower(buf[0]));
    }

    // result <= 0: dead key (-1) or no translation (0). Fall back to the
    // unshifted mapping — matches the pre-ToUnicodeEx behavior for these keys.
    UINT ch = MapVirtualKeyW(vkCode, MAPVK_VK_TO_CHAR);
    return ch ? static_cast<wchar_t>(towlower(static_cast<wchar_t>(ch))) : 0;
}

// ═══════════════════════════════════════════════════════════
// Phase 2a — Hook-thread command drain
//
// Single-writer invariant: every mutation of the 16 composition-state
// fields must happen on the hook thread. Producers on other threads use
// `mailbox_.Post(bit, ...)`; this drain consumes from the LL hook callback
// (Rule 11.4 step 5 barrier) and from the pump's WM_APP_HOOK_COMMAND
// handler. Dispatch order follows the design doc: latch kConfigApply, route
// kFocusChanged (drop/defer/apply), retry a previously deferred focus at a
// safe word boundary, then kTickPoll and kToggleVN.
//
// Phase 2a ships the infrastructure ONLY. No producer calls Post yet —
// existing OnFocusChanged / OnTickPoll / ApplyConfig / ToggleVietnameseMode
// still mutate inline as before. Phase 2b/c migrate them onto this channel
// one writer at a time. Until then DrainHookCommands always returns early
// (mailbox bits=0).
// ═══════════════════════════════════════════════════════════

void HookEngine::DrainHookCommands() {
    // Phase 2d: re-entrancy guard. Trips a Debug assertion if a drain
    // handler somehow re-enters DrainHookCommands — that's the
    // "ApplyFoo() called something that called DrainHookCommands again"
    // bug pattern, which would corrupt mailbox bit state silently.
    HookCommandMailbox::DrainScope scope(lifecycle_.Mailbox());

    const std::uint32_t bits = lifecycle_.Mailbox().DrainBits();
    // Phase 3f: even if no fresh bits, a previous drain may have deferred
    // config or focus apply (engine was busy). Re-check on every natural
    // drain so the apply lands as soon as the engine empties. No extra wake is
    // posted from the hook thread: doing so would add a PostThreadMessage
    // syscall per word and violate Rule 11.2.
    const bool hadDeferredConfig =
        deferredConfigApply_.load(std::memory_order_acquire);
    const bool hadDeferredFocus = static_cast<bool>(deferredFocusApply_);
    if (!bits && !hadDeferredConfig && !hadDeferredFocus) return;

    // kConfigApply: latch the request; the actual apply runs at the tail
    // of this function once we know whether the engine is busy. We DO
    // NOT call ApplyConfigOnHookThread mid-drain anymore — see P3f note.
    if (bits & HookCommand::kConfigApply) {
        deferredConfigApply_.store(true, std::memory_order_release);
    }
    if (bits & HookCommand::kFocusChanged) {
        RouteFocusOnHookThread(lifecycle_.Mailbox().ConsumePendingFocus());
    }
    // Preserve the established focus-before-tick-before-toggle ordering. If a
    // prior word has committed since the result was deferred, its logical
    // typing-context transaction lands here before the next key is dispatched.
    TryApplyDeferredFocusOnHookThread();
    if (bits & HookCommand::kTickPoll)     ApplyTickPollOnHookThread();
    if (bits & HookCommand::kToggleVN)     ApplyToggleVNOnHookThread();

    // Phase 3f — guard the config apply against mid-word reset.
    // ApplyConfigOnHookThread destroys engine_ and creates a fresh one;
    // if the user has uncommitted input (engine_->Count() > 0), running
    // the apply now would either (a) commit a partial word visibly or
    // (b) drop the partial input. Both are user-visible quirks. Defer
    // until the engine empties naturally — typically the next keystroke
    // after a word commit, occasionally a backspace-to-empty or focus
    // change. Focus change (ApplyFocusOnHookThread above) calls
    // ResetComposition which zeros the count, so deferred applies often
    // land on the same drain when triggered by a focus event.
    if (deferredConfigApply_.load(std::memory_order_acquire)
        && engine_ && engine_->Count() == 0) {
        // exchange(false) — defensive over load+store: even though
        // DrainScope guarantees single-drain-at-a-time today, a future
        // Phase 5 split could fragment the drain across classes. The
        // CAS-style swap makes "I'm the one consuming this latch"
        // explicit regardless of drain serialisation.
        if (deferredConfigApply_.exchange(false, std::memory_order_acq_rel)) {
            ApplyConfigOnHookThread();
        }
    }
}

void HookEngine::RouteFocusOnHookThread(
        std::shared_ptr<const FocusClassification> cls) {
    VKEY_ASSERT_HOOK_THREAD();
    if (!cls || !cls->hwndOpaque) return;

    const bool hasComposition =
        (engine_ && engine_->Count() > 0) || !rawMacroBuffer_.empty();
    const FocusApplyInputs inputs{
        .snapshotRequestSerial = cls->requestSerial,
        .latestRequestSerial =
            latestFocusRequestSerial_.load(std::memory_order_relaxed),
        .snapshotInputEpoch = cls->inputEpochAtRequest,
        .currentInputEpoch =
            physicalInputEpoch_.load(std::memory_order_relaxed),
        .hasComposition = hasComposition,
    };

    const FocusApplyDisposition disposition = DecideFocusApply(inputs);

    const auto cfg = config_.load(std::memory_order_acquire);
    if (dispatcher_.ApplyFocusOutput(*cls, disposition, cfg->suggestKeepChars)) {
        HOOK_LOG(L"  FocusOutput: request=%llu bait=%d game=%d deferred=%d",
                 static_cast<unsigned long long>(cls->requestSerial),
                 cls->localNeedBait ? 1 : 0, cls->localGameReinject ? 1 : 0,
                 disposition == FocusApplyDisposition::DeferUntilBoundary ? 1 : 0);
    }

    // Hook-chain protection is independent of the typing-context transaction.
    // Dorion can bypass this callback entirely once it is foreground, so
    // waiting for a deferred composition boundary could make the boundary
    // unreachable. Only a current, exact executable/PID observation may post,
    // and the policy suppresses ordinary Alt+Tab re-entry for the same process.
    const DorionProcessIdentity dorionIdentity{
        cls->pid,
        cls->processCreationTime,
    };
    if (disposition != FocusApplyDisposition::DropStale
        && dorionHookReclaimPolicy_.Observe(
            cls->exeName, dorionIdentity, true)) {
        if (lifecycle_.RequestDorionKeyboardReclaim(dorionIdentity)) {
            HOOK_LOG(L"  DorionHookReclaim: queued pid=%u creation=%llu request=%llu",
                     dorionIdentity.pid,
                     static_cast<unsigned long long>(
                         dorionIdentity.creationTime),
                     static_cast<unsigned long long>(cls->requestSerial));
        } else {
            // Posting did not transfer ownership to the pump. Re-arm so a
            // later current snapshot or a clean restart can retry safely.
            dorionHookReclaimPolicy_.Complete(dorionIdentity, false);
            HOOK_LOG(L"  DorionHookReclaim: post FAILED pid=%u creation=%llu request=%llu",
                     dorionIdentity.pid,
                     static_cast<unsigned long long>(
                         dorionIdentity.creationTime),
                     static_cast<unsigned long long>(cls->requestSerial));
        }
    }

    switch (disposition) {
        case FocusApplyDisposition::DropStale:
            HOOK_LOG(L"  FocusApply: drop stale request=%llu latest=%llu",
                     static_cast<unsigned long long>(inputs.snapshotRequestSerial),
                     static_cast<unsigned long long>(inputs.latestRequestSerial));
            return;

        case FocusApplyDisposition::DeferUntilBoundary:
            // Keep the word, method, encoding and mode transaction together.
            // Output already follows the host receiving the next replacement.
            deferredFocusApply_ = std::move(cls);
            HOOK_LOG(L"  FocusApply: defer request=%llu input=%llu->%llu count=%zu macro=%zu",
                     static_cast<unsigned long long>(inputs.snapshotRequestSerial),
                     static_cast<unsigned long long>(inputs.snapshotInputEpoch),
                     static_cast<unsigned long long>(inputs.currentInputEpoch),
                     engine_ ? engine_->Count() : 0,
                     rawMacroBuffer_.size());
            return;

        case FocusApplyDisposition::ApplyNow:
            // A current snapshot supersedes any retained older transaction.
            deferredFocusApply_.reset();
            ApplyFocusOnHookThread(std::move(cls));
            return;
    }
}

void HookEngine::TryApplyDeferredFocusOnHookThread() {
    VKEY_ASSERT_HOOK_THREAD();
    if (!deferredFocusApply_) return;

    // Read the fields out before any branch can move the member — the ApplyNow
    // case moves out of deferredFocusApply_, so a reference bound to it would
    // go null mid-function.
    const bool hasComposition =
        (engine_ && engine_->Count() > 0) || !rawMacroBuffer_.empty();
    const FocusApplyInputs inputs{
        .snapshotRequestSerial = deferredFocusApply_->requestSerial,
        .latestRequestSerial =
            latestFocusRequestSerial_.load(std::memory_order_relaxed),
        .snapshotInputEpoch = deferredFocusApply_->inputEpochAtRequest,
        .currentInputEpoch =
            physicalInputEpoch_.load(std::memory_order_relaxed),
        .hasComposition = hasComposition,
    };

    switch (DecideFocusApply(inputs)) {
        case FocusApplyDisposition::DropStale:
            // Dropping is safe only because the superseding request is
            // guaranteed to produce its own apply OR to leave the tick poll
            // able to retry: a classification that early-returns for a helper
            // window deliberately does NOT update lastForegroundPid_ (see the
            // comment in ApplyFocusOnHookThread), so OnTickPoll's stale-PID
            // fallback re-fires within one tick. Do not "simplify" that PID
            // handling without revisiting this branch — otherwise a transient
            // focus event can strand the real app's typing context.
            HOOK_LOG(L"  FocusApply: drop deferred request=%llu latest=%llu",
                     static_cast<unsigned long long>(inputs.snapshotRequestSerial),
                     static_cast<unsigned long long>(inputs.latestRequestSerial));
            deferredFocusApply_.reset();
            return;
        case FocusApplyDisposition::DeferUntilBoundary:
            return;
        case FocusApplyDisposition::ApplyNow: {
            auto ready = std::move(deferredFocusApply_);
            const auto cfg = config_.load(std::memory_order_acquire);
            (void)dispatcher_.ApplyFocusOutput(
                *ready, FocusApplyDisposition::ApplyNow, cfg->suggestKeepChars);
            HOOK_LOG(L"  FocusApply: apply deferred request=%llu at boundary",
                     static_cast<unsigned long long>(inputs.snapshotRequestSerial));
            ApplyFocusOnHookThread(std::move(ready));
            return;
        }
    }
}

void HookEngine::ReconcileModifierStateOnHookThread() noexcept {
    VKEY_ASSERT_HOOK_THREAD();

    // Snapshot once: repeated GetAsyncKeyState calls across this transaction
    // can otherwise disagree when Alt/Win is released mid-reconciliation.
    const bool physicalCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool physicalShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool physicalAlt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool physicalWin = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0
        || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    const bool anyPhysicalModifier =
        physicalCtrl || physicalShift || physicalAlt || physicalWin;

    modCtrlDown_ = physicalCtrl;
    modShiftDown_ = physicalShift;
    modAltDown_ = physicalAlt;
    modWinDown_ = physicalWin;

    // A foreground transition is a gesture boundary. If Alt/Win/Ctrl/Shift is
    // still held (most notably between Alt+Tab's Tab-down and Alt-up), preserve
    // contamination so the eventual release cannot fire a modifier-alone VKey
    // action in the new app. Once every modifier is up, stale latches are safe
    // to clear.
    modComboSeen_ = anyPhysicalModifier;
    otherKeyPressed_ = anyPhysicalModifier;
    for (int i = 0; i < kModCount; ++i) {
        modTapCount_[i] = 0;
        modTapLastTs_[i] = 0;
    }

    if (hotkeyManager_) {
        hotkeyManager_->ReconcileModifiers({
            .ctrl = physicalCtrl,
            .shift = physicalShift,
            .alt = physicalAlt,
            .win = physicalWin,
        });
    }
}

// Phase 2b — focus apply runs on the hook thread (called from
// DrainHookCommands). All composition-state writes that used to live in
// OnFocusChanged moved here. The cls parameter is the pre-computed
// classification snapshot produced by FocusOwner::Classify.
//
// Must stay fast (Rule 11.2) — no syscalls beyond the cheap ones already
// listed in the design's "may only mutate composition state + atomic
// stores" contract. Heavy work (ClassifyWindow, GetExeNameForHwnd,
// IsWebView2App, CreateToolhelp32Snapshot) is in FocusOwner::Classify,
// not here.
void HookEngine::ApplyFocusOnHookThread(std::shared_ptr<const FocusClassification> cls) {
    VKEY_ASSERT_HOOK_THREAD();
    if (!cls || !cls->hwndOpaque) return;

    const auto cfg = config_.load(std::memory_order_acquire);

    HWND activeHwnd = reinterpret_cast<HWND>(cls->hwndOpaque);

    // PID update moved (2026-05-29): OnTickPoll now updates lastForegroundPid_
    // defensively BEFORE calling OnFocusChangedSyncOnWorker (v2.1.24 pattern),
    // which breaks the WebView2 poll-loop that commit 87a560f tried to fix by
    // moving the update here. Updating PID at function entry blocked Bug 1's
    // natural fallback (notepad++ launch with only helper events relies on the
    // poll detecting a stale PID to re-classify against GetForegroundWindow
    // once the main window settles). PID is now updated below at the end of
    // the real-focus path — see comment near isExcludedApp_ store.

    // Re-validate the focused CHILD control right before trusting anything
    // the worker computed against it. WH_MOUSE_LL fires before Windows
    // delivers the click to the target app, so Classify() can run before
    // (or after) that app finishes moving focus to the control the user
    // actually clicked — the child probed may not be the one now focused.
    //
    // Two skips, because GetFocusedChildHwnd IS AttachThreadInput: it briefly
    // serialises THIS thread's input queue with the foreground thread's, and
    // this is the thread that owns WH_MOUSE_LL. "Cheap (~µs)" measured alone
    // is not the cost that matters — who we serialise against is.
    //   * auto-cap off — both consumers below are auto-cap-only, so the probe
    //     buys nothing at any price.
    //   * tray/taskbar foreground — that attaches us to explorer.exe's taskbar
    //     thread, the very thread that must itself detect the tray icon's
    //     double-click. Lose that race and WM_LBUTTONDBLCLK degrades into two
    //     WM_LBUTTONUPs, so a double-click only toggles V/E and never opens
    //     Settings (#209: "left-click 3 lần... phải click thêm 3 lần nữa";
    //     right-click via the menu was unaffected, which is what pinned the
    //     fault to double-click DETECTION rather than to opening the dialog).
    //     Nothing is ever typed into the taskbar, so there is no Vietnamese
    //     state to validate there.
    // Both skips leave suppressAutoCapForPasswordSafety_ permissive rather
    // than latched: it is re-derived on the next real foreground change, and
    // HandleAlphaKey re-probes the focused control synchronously before it
    // capitalizes, re-suppressing if that control IS a password field.
    const bool needsChildProbe =
        cfg->autoCaps && !FocusOwner::IsTrayOrTaskbarWindow(activeHwnd);
    const HWND currentFocusedChild =
        needsChildProbe ? ::NextKey::GetFocusedChildHwnd(activeHwnd) : nullptr;
    const std::uintptr_t currentFocusedChildOpaque =
        reinterpret_cast<std::uintptr_t>(currentFocusedChild);

    // Reset composition + per-word state only when the active input target actually changes.
    // Spurious focus events (e.g. background UI timer in FastStone Capture, issue #273)
    // for the same window and control must not wipe live composition mid-word.
    const std::uintptr_t childForFocus = (currentFocusedChildOpaque != 0)
        ? currentFocusedChildOpaque
        : cls->focusedChildHwndOpaque;
    const std::uintptr_t effectiveFocusHwnd =
        (childForFocus != 0) ? childForFocus : cls->hwndOpaque;

    if (ShouldResetCompositionOnFocusChange(
            cls->hwndOpaque, effectiveFocusHwnd,
            lastAppliedHwnd_, lastAppliedEffectiveFocusHwnd_)) {
        ResetComposition();
        tempEngineOff_ = false;
        lastAppliedHwnd_ = cls->hwndOpaque;
        lastAppliedEffectiveFocusHwnd_ = effectiveFocusHwnd;
    }

    // Skipped probe ⇒ permissive (see needsChildProbe above); never latch a
    // suppression we derived from an HWND we deliberately did not read.
    suppressAutoCapForPasswordSafety_ = needsChildProbe
        && ShouldSuppressAutoCapForPasswordSafety(
               cls->isPasswordFieldFocused, cls->focusedChildHwndOpaque,
               currentFocusedChildOpaque);

    // Validate worker-produced empty-document evidence at consumption time.
    // The HWND/PID/child check extends the existing latest-focus mailbox
    // pattern; the activity clock catches a key that overtook the worker probe
    // because LowLevelKeyboardProc calls MarkActivity before DrainHookCommands.
    bool shouldArmAutoCap = false;
    if (cfg->autoCaps && cls->isKnownEmptyDocument) {
        const HWND foregroundHwnd = GetForegroundWindow();
        DWORD foregroundPid = 0;
        if (foregroundHwnd != nullptr) {
            GetWindowThreadProcessId(foregroundHwnd, &foregroundPid);
        }
        const std::uint64_t nowMs = GetTickCount64();
        const bool foregroundStayedStable =
            GetForegroundWindow() == foregroundHwnd;
        const AutoCapFocusEvidence evidence{
            .isKnownEmptyDocument = true,
            .hwndOpaque = cls->hwndOpaque,
            .focusedChildHwndOpaque = cls->focusedChildHwndOpaque,
            .pid = cls->pid,
            .probeStartedAtMs = cls->emptyDocumentProbeStartedAtMs,
        };
        const AutoCapFocusContext current{
            .hwndOpaque = foregroundStayedStable
                ? reinterpret_cast<std::uintptr_t>(foregroundHwnd)
                : 0,
            .focusedChildHwndOpaque = foregroundStayedStable
                ? currentFocusedChildOpaque
                : 0,
            .pid = static_cast<std::uint32_t>(foregroundPid),
            .lastInputAtMs = lastActivityTickMs_.load(std::memory_order_relaxed),
            .nowMs = nowMs,
        };
        shouldArmAutoCap = ShouldArmAutoCapFromFocusEvidence(
            evidence, current, kAutoCapFocusEvidenceMaxAgeMs);
    }
    autoCapState_ = shouldArmAutoCap
        ? AutoCapState::ReadyToCapitalize
        : AutoCapState::Idle;
    // Reconcile our modifier/hotkey trackers after desktop switch / Win+L,
    // without turning a still-held Alt+Tab chord into an Alt-alone release.
    ReconcileModifierStateOnHookThread();

    HOOK_LOG(L"  AppDetect: console=%d skipEmpty=%d electron=%d webview2=%d bait=%d clipboard=%d editMsg=%d useClipInj=%d splitSleepMs=%d emptyDoc=%d fresh=%d",
             cls->isConsole ? 1 : 0, cls->localSkipEmpty ? 1 : 0, cls->localElectronApp ? 1 : 0,
             cls->isWebView2 ? 1 : 0, cls->localNeedBait ? 1 : 0, cls->localClipboard ? 1 : 0,
             cls->localEditMsg ? 1 : 0, cls->localUseClipboardInjector ? 1 : 0,
             cls->localForcedSplitSleepMs, cls->isKnownEmptyDocument ? 1 : 0,
             shouldArmAutoCap ? 1 : 0);

    // RefreshFocusCache uses GetFocusedChildHwnd (AttachThreadInput) which
    // is cheap (~µs). Safe on hook thread.
    if (cls->localClipboard || cls->localEditMsg) {
        focus_.RefreshFocusCache(activeHwnd);
    } else {
        focus_.InvalidateFocusCache();
    }

    // CJK layout check — GetKeyboardLayout is kernel-cached, single µs.
    CheckLayoutChange();

    // Split state SmartSwitch (2026-05-26): activeExe_ tracks any focused
    // exe (including helper windows like dock panels / SearchHost / tray);
    // lastRealExe_ tracks only non-skipAppTracking transitions. This split
    // resolves the tension between toggle (wants "user's current app", so
    // notepad++ all-helper case attributes correctly) and SAVE (wants
    // "last real app", so helper-event detours don't poison the previous
    // real app's entry).
    //
    // Gates for activeExe_ update:
    //   - non-empty exeName (classifier failed to resolve → skip)
    //   - cls->pid != 0 (GetWindowThreadProcessId failed → skip)
    //   - cls->pid != GetCurrentProcessId() (our own tray/menu → skip)
    if (!cls->exeName.empty() && cls->pid != 0 &&
        cls->pid != GetCurrentProcessId()) {
        focus_.SetActiveExe(cls->exeName);
    }

    // Helper HWNDs: SAVE/RESTORE skipped (mode shouldn't flip on transient
    // dock-panel, tray icon, splash, init helper, or shell focus events).
    // activeExe_ above is already updated so the next toggle / non-skip
    // focus event sees the right app.
    //
    // Bug 1 (notepad++ launches with helper-only events, mode never
    // restores) is fixed via the OnTickPoll PID poll: helper events
    // here return WITHOUT updating lastForegroundPid_, the poll detects
    // a stale PID on its next tick (~200 ms later), updates the PID
    // defensively, and re-fires OnFocusChanged against the foreground
    // window — by which point the main window has typically settled
    // and runs the real-focus SmartSwitch path below. This is the
    // pre-2026-05-21 (v2.1.24) pattern; see commit 87a560f notes +
    // docs/plans/2026-05-28-smart-switch-persistence-design.md §4.
    //
    // Engine ownership (which of hook / TIP consumes keys for this app) is the
    // one thing that must NOT wait for that: it is derived from the exe alone,
    // and leaving it stale hands the app to the wrong engine. #242 — focus went
    // from One Commander (a tsf_apps entry) to Explorer's file list, that event
    // returned here, so TSF_ACTIVE stayed ON and the TIP kept composing in a
    // shell list view, which is where Windows opens its "Finalize the string"
    // box. An unidentified window resolves to isTsf=false on purpose: the hook
    // works everywhere, the TIP only where it was whitelisted.
    const bool wasExcludedApp = isExcludedApp_.load(std::memory_order_acquire);
    const bool wasTsfAppFlag  = isTsfApp_.load(std::memory_order_acquire);
    const bool wasForcedVnApp = isForcedVnApp_.load(std::memory_order_acquire);
    const bool wasBrowserContextActive = browserContextActive_;
    const bool modeBeforeBrowserRefresh =
        vietnameseMode_.load(std::memory_order_acquire);
    const bool wasEffectiveTsf = ResolveEffectiveTsf(
        wasTsfAppFlag, wasExcludedApp, cfg->tsfApps,
        browserRoute_.load(std::memory_order_acquire));

    isExcludedApp_.store(cls->isExcluded, std::memory_order_release);
    isTsfApp_.store(cls->isTsf, std::memory_order_release);
    // Store forced-V cache flag at the SAME site as the others, BEFORE the
    // browser restore and excluded/tsf early-returns below. This also ensures
    // an app hard-V lock wins immediately over a remembered domain mode.
    isForcedVnApp_.store(cls->isForcedVietnamese, std::memory_order_release);
    if (cls->isForcedVietnamese) forcedVnPid_.store(cls->pid, std::memory_order_release);
    RefreshBrowserRouteOnHookThread(true, false);
    const bool effectiveTsf = IsEffectiveTsf();
    const bool shouldActivateTsfProfile = ShouldActivateTsfProfileForFocus(
        tsfFocusActivationState_, effectiveTsf, cls->hwndOpaque, cls->pid);

    // activate_tip is in this line for #250: the reporter still saw the Start
    // menu stall after removing StartMenuExperienceHost.exe from tsf_apps, which
    // should make this 0 for that focus. Without it the log cannot say whether
    // the profile re-assertion ran at all for the focus that stalled.
    HOOK_LOG(L"  Engine: %s for '%s' (tsf_feature=%d, in_tsf_list=%d, excluded=%d, "
             L"activate_tip=%d)",
             cls->isTsf ? L"TSF (hook passthrough)" : L"HOOK",
             cls->exeName.empty() ? L"<unknown>" : cls->exeName.c_str(),
             cfg->tsfApps ? 1 : 0,
             cls->isTsf ? 1 : 0,
             cls->isExcluded ? 1 : 0,
             shouldActivateTsfProfile ? 1 : 0);

    // SharedState TSF flag bridge — idempotent via SetOrClearFlag in main.
    if (tsfModeCallback_) {
        const bool tsfReadonly = !effectiveTsf && !cls->isExcluded;
        if (effectiveTsf != wasEffectiveTsf) {
            HOOK_LOG(L"  TSF_ACTIVE flag: %s → %s",
                     wasEffectiveTsf ? L"true" : L"false",
                     effectiveTsf ? L"true" : L"false");
        }
        tsfModeCallback_(effectiveTsf, tsfReadonly,
                         shouldActivateTsfProfile);
    }

    if (cls->skipAppTracking) {
        // Say it, or the log just goes quiet: #242 was read as "no focus event
        // for explorer.exe" when the event had in fact arrived and stopped here.
        HOOK_LOG(L"  SmartSwitch/overrides skipped (helper or hidden window)");
        return;
    }

    // Short-circuit when no per-app feature needs tracking. Phase 3c
    // reads the override-map presence from the RCU snapshot — same data
    // the cls fields were resolved against in Classify.
    {
        auto snap = configSnapshot_.load(std::memory_order_acquire);
        const bool noOverrides = !snap
            || (snap->appEncodingOverrides.empty()
                && snap->appInputMethodOverrides.empty());
        if (!cfg->smartSwitch && !cfg->excludeApps && !cfg->tsfApps && noOverrides) return;
    }

    if (cls->exeName.empty()) {
        HOOK_LOG(L"  Focus exe unresolved (hwnd=%p) — per-app tracking skipped",
                 reinterpret_cast<void*>(cls->hwndOpaque));
        return;
    }

    // Smart switch SAVE for the previous real app — captured BEFORE we
    // advance lastRealExe_ below. Uses lastRealExe_, NOT activeExe_:
    // a helper-event detour right before this non-skip event would have
    // moved activeExe_ to the helper's exe, while lastRealExe_ correctly
    // still points to the app whose mode the engine state corresponds to.
    const std::wstring oldLastReal = focus_.LastRealExe();
    // !wasForcedV: a forced-V app's mode is locked to V, never the user's
    // choice — don't persist it into appModeMap or it poisons the remembered
    // preference (the SAVE writes the forced 'true', not a real toggle).
    if (cfg->smartSwitch && !oldLastReal.empty() && !wasExcludedApp
        && !wasForcedVnApp && !wasBrowserContextActive) {
        if (focus_.AppModeMap().size() >= kMaxSmartSwitchEntries) {
            focus_.AppModeMap().clear();
            HOOK_LOG(L"  SmartSwitch: map cap %zu hit, cleared", kMaxSmartSwitchEntries);
        }
        // Browser restore above may already have applied the new tab's mode;
        // persist the mode that actually belonged to the app being left.
        const bool savedMode = modeBeforeBrowserRefresh;
        focus_.AppModeMap()[oldLastReal] = savedMode;
        focus_.Smart().SetAppMode(oldLastReal, savedMode);
        focus_.PublishAppModesSnapshot();           // RCU publish (alloc + atomic_store)
        focus_.MarkAppModeDirty();
        focus_.SetLastDirtyTs(GetTickCount64());    // worker debounce input
    }

    // Advance lastRealExe_ to the new real app (auto-shifts previousExe_
    // for the encoding-override fallback chain).
    focus_.SetLastRealExe(cls->exeName);

    // Real-focus PID update — moved here from function entry (v2.1.24
    // pattern restored, see comment near top). Helper-only events that
    // hit the skipAppTracking early-return above LEAVE PID stale on
    // purpose so the OnTickPoll defensive update can re-fire OnFocusChanged
    // once the app's main window settles to foreground (Bug 1 fallback).
    if (cls->pid) focus_.SetLastForegroundPid(cls->pid);

    if (cls->isExcluded) {
        excludedPid_.store(cls->pid, std::memory_order_release);
        HOOK_LOG(L"  ExcludeApps: '%s' is excluded, passthrough (pid=%u)",
                 focus_.LastRealExe().c_str(), cls->pid);
        if (!wasExcludedApp) NotifyModeChange();
        return;
    }


    // Per-app hard-V lock WINS over smart-switch: force V on focus regardless of
    // any remembered preference. (D2: on LEAVING a forced-V app the normal
    // smart-switch restore for the *next* app applies — handled on that focus.)
    if (cls->isForcedVietnamese) {
        if (!vietnameseMode_.load(std::memory_order_acquire)) {
            vietnameseMode_.store(true, std::memory_order_release);
            HOOK_LOG(L"  ForceVN: locked Vietnamese for '%s'", focus_.LastRealExe().c_str());
            NotifyModeChange();
        }
    } else if (cfg->smartSwitch && !browserContextActive_) {
        // Smart switch restore for the new app — uses lastRealExe_ (just set
        // above to cls->exeName).
        auto it = focus_.AppModeMap().find(focus_.LastRealExe());
        if (it != focus_.AppModeMap().end()) {
            const bool curMode = vietnameseMode_.load(std::memory_order_acquire);
            if (it->second != curMode) {
                vietnameseMode_.store(it->second, std::memory_order_release);
                HOOK_LOG(L"  SmartSwitch: restored %s for '%s'",
                         it->second ? L"Vietnamese" : L"English",
                         focus_.LastRealExe().c_str());
                NotifyModeChange();
            }
        } else {
            HOOK_LOG(L"  SmartSwitch: inherit %s for unknown '%s'",
                     vietnameseMode_.load(std::memory_order_acquire)
                         ? L"Vietnamese" : L"English",
                     focus_.LastRealExe().c_str());
        }
    }

    // Leaving an excluded OR forced-V app — effective mode changed even if
    // vietnameseMode_ didn't move, and CJK auto-switch was gated off while
    // locked. Replay the layout check so a suppressed CJK transition can
    // re-evaluate now that the lock has cleared.
    if (wasExcludedApp || wasForcedVnApp) {
        const bool wasSuppressed = focus_.LayoutSuppressed();
        OnLayoutChanged(focus_.CachedIsCompatLayout());
        if (wasSuppressed == focus_.LayoutSuppressed()) NotifyModeChange();
    }

    if (effectiveTsf) {
        HOOK_LOG(L"  TsfApps: '%s' uses TSF engine, hook passthrough",
                 focus_.LastRealExe().c_str());
        // #195/#209: no callback re-fire here. The early tsfModeCallback_ above
        // already published (tsfActive=true, tsfReadonly=false) and posted the
        // TIP re-assert. The re-fire existed because the old main.cpp callback
        // read V/E mode (resolved only by the force-V / smart-switch blocks
        // above); since #209 the callback is mode-independent, so firing again
        // would be an identical duplicate. Still focus-driven only (never the
        // OnTickPoll TSF_TIP_ACTIVE/Win+Space monitor).
        return;
    }

    // Per-app encoding override (target value pre-resolved in Classify
    // against the on-main global to avoid a cross-thread read of
    // globalCodeTable_ here).
    {
        const CodeTable targetTable = static_cast<CodeTable>(cls->targetCodeTable);
        if (targetTable != currentCodeTable_.load(std::memory_order_acquire)) {
            currentCodeTable_.store(targetTable, std::memory_order_release);
            HOOK_LOG(L"  AppOverride: encoding=%d for '%s'",
                     static_cast<int>(targetTable),
                     focus_.LastRealExe().c_str());
        }
    }

    // Per-app input method override (same pattern as encoding). Recreate
    // engine_ on change — the only heap allocation on this path.
    {
        const InputMethod targetMethod = static_cast<InputMethod>(cls->targetMethod);
        if (targetMethod != currentMethod_.load(std::memory_order_acquire)) {
            currentMethod_.store(targetMethod, std::memory_order_release);
            TypingConfig engineConfig = *config_.load(std::memory_order_acquire);
            engineConfig.inputMethod = targetMethod;
            engine_ = EngineFactory::Create(engineConfig);
            HOOK_LOG(L"  AppOverride: inputMethod=%d for '%s'",
                     static_cast<int>(targetMethod),
                     focus_.LastRealExe().c_str());
        }
    }
}

// Phase 2c — ToggleVietnameseMode body, migrated to the hook thread.
//
// Single-writer note: this runs from the drain only. The mailbox
// coalesces multiple Posts before drain into one drained bit
// (HookCommandMailboxTest §PostSameBitMultipleTimesDrainReturnsOnce),
// so rapid hotkey mashing collapses to one toggle per drain cycle.
// User-visible behaviour: same as before — sub-keystroke responsive.
void HookEngine::ApplyToggleVNOnHookThread() {
    VKEY_ASSERT_HOOK_THREAD();
    // #221: consume this drain's pending explicit toggle target up front —
    // exchange(-1) so it can't leak into a later, unrelated drain. Discarded
    // below if this toggle turns out to be blocked (excluded / forced-V app).
    const int8_t pendingToggleMode = pendingToggleMode_.exchange(-1, std::memory_order_acq_rel);
    const auto cfg = config_.load(std::memory_order_acquire);
    // Excluded-app gate. PID check vs cached excludedPid_ distinguishes
    // "genuinely in excluded app" (block toggle) from "stale flag, user
    // already left" (force VN). Both atomic stores below are safe on
    // hook thread now that we're single-writer.
    if (cfg->excludeApps && isExcludedApp_.load(std::memory_order_acquire)) {
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        const DWORD cachedPid = excludedPid_.load(std::memory_order_acquire);
        if (fgPid == cachedPid && cachedPid != 0) {
            HOOK_LOG(L"  ToggleVN: BLOCKED (excluded pid=%u)", cachedPid);
            if (sharedStatePtr_) {
                sharedStatePtr_->SetOrClearFlag(SharedFlags::VIETNAMESE_MODE, false);
            }
            return;
        }
        // Different PID — user already left excluded app, flag is stale.
        // Force VN: user pressed toggle expecting V mode, having perceived
        // the excluded app as English.
        isExcludedApp_.store(false, std::memory_order_release);
        vietnameseMode_.store(true, std::memory_order_release);
        HOOK_LOG(L"  ToggleVN: stale excluded → forced Vietnamese (fg pid=%u)", fgPid);
        NotifyModeChange();
        if (cfg->beepOnSwitch) MessageBeep(MB_OK);
        return;
    }
    // Forced-V (per-app hard-V) gate — symmetric to the excluded gate. While
    // genuinely in a forced-V app the toggle is locked (D1). Silent on block to
    // match the excluded gate (D5). Stale PID → user already left → clear and
    // fall through to a normal toggle.
    if (cfg->excludeApps && isForcedVnApp_.load(std::memory_order_acquire)) {
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        const DWORD cachedPid = forcedVnPid_.load(std::memory_order_acquire);
        if (fgPid == cachedPid && cachedPid != 0) {
            HOOK_LOG(L"  ToggleVN: BLOCKED (forced-V pid=%u)", cachedPid);
            if (sharedStatePtr_) {
                sharedStatePtr_->SetOrClearFlag(SharedFlags::VIETNAMESE_MODE, true);
            }
            return;
        }
        isForcedVnApp_.store(false, std::memory_order_release);
        HOOK_LOG(L"  ToggleVN: stale forced-V cleared (fg pid=%u)", fgPid);
    }

    // The extension heartbeat and the hotkey can be delivered by different
    // Windows queues. Re-read the cheap shared snapshot here so a tray/menu
    // toggle cannot use a stale domain hard-lock or attribute its event to the
    // tab that was active previously.
    RefreshBrowserRouteOnHookThread(true, true);

    // A configured domain-English rule is a hard lock. ToggleVietnameseMode()
    // eagerly flipped SharedState before posting this drain, so restore the
    // logical base mode when the browser rule rejects that toggle.
    if (browserContextActive_
        && browserRoute_.load(std::memory_order_acquire) == BrowserRoute::ForceEnglish) {
        HOOK_LOG(L"  ToggleVN: BLOCKED (browser hard-English)");
        if (sharedStatePtr_) {
            sharedStatePtr_->SetOrClearFlag(
                SharedFlags::VIETNAMESE_MODE,
                vietnameseMode_.load(std::memory_order_acquire));
        }
        return;
    }

    // Commit pending composition (skip if CJK-suppressed — engine inactive).
    if (!focus_.LayoutSuppressed() && engine_->Count() > 0) {
        CommitComposition(true);
    }
    CancelCommitUndo();
    digitLedWord_ = false;

    // #221: prefer the value this exact ToggleVietnameseMode() call computed
    // (pendingToggleMode) over a fresh SharedState read — a NotifyModeChange()
    // firing between the eager flip and this drain could have already
    // overwritten SharedState with the stale pre-toggle value, which would
    // silently revert the toggle if we re-read it here instead.
    bool newMode = false;
    if (pendingToggleMode >= 0) {
        newMode = (pendingToggleMode != 0);
    } else if (sharedStatePtr_) {
        newMode = (sharedStatePtr_->ReadFlags() & SharedFlags::VIETNAMESE_MODE) != 0;
    } else {
        newMode = !vietnameseMode_.load(std::memory_order_acquire);
    }
    vietnameseMode_.store(newMode, std::memory_order_release);
    NEXTKEY_LOG(L"HookEngine: mode = %s (via drain)", newMode ? L"Vietnamese" : L"English");

    if (browserContextActive_ && !browserContext_.PublishModeEvent(newMode)) {
        HOOK_LOG(L"  BrowserModeEvent: publish skipped");
    }

    // Smart-switch save. Drop the pre-P2c GetForegroundWindow + GetExeNameForHwnd
    // fallback — those are Rule 11.2 forbidden on the hook thread (Toolhelp32
    // snapshot). Toggle uses activeExe_ (any focused window's exe, including
    // helper-window apps like notepad++ dock panels) so map writes attribute
    // to the app the user is interacting with even when the focus path
    // detours. If activeExe_ is empty here (startup before any focus event),
    // the next focus event sets it and the toggle takes effect on first save.
    // A browser-aware toggle belongs to the extension's session map. Do not
    // also persist it as an app-level chrome.exe/firefox.exe Smart Switch
    // entry, otherwise it would survive a browser restart.
    if (cfg->smartSwitch && !browserContextActive_ && !focus_.ActiveExe().empty()) {
        focus_.AppModeMap()[focus_.ActiveExe()] = newMode;
        focus_.Smart().SetAppMode(focus_.ActiveExe(), newMode);
        focus_.PublishAppModesSnapshot();
        focus_.MarkAppModeDirty();
        focus_.SetLastDirtyTs(GetTickCount64());
    }

    if (cfg->beepOnSwitch) {
        MessageBeep(newMode ? MB_OK : MB_ICONASTERISK);
    }
    NotifyModeChange();

    // #109: TIP activation is now done ONCE at startup (main.cpp), not re-asserted
    // per toggle. A same-window V/E toggle changes neither the focused app nor
    // TSF_ACTIVE, so there is nothing to re-publish here — the live TIP reads the
    // new VIETNAMESE_MODE flag on its next key (EngineController::WantKey).
}

// P3e/P3f — config-apply drain handler. Wired into the kConfigApply mailbox
// bit posted by ReloadFromToml on the worker. Hook-thread side of the
// single-writer contract: this is where composition state mutations
// (currentMethod_ store, engine_ swap) actually run.
//
// CONTRACT (P3f): DrainHookCommands guarantees `engine_->Count() == 0`
// before calling this function. Any pending word is left intact in the
// engine until natural completion (commit, backspace-empty, focus
// reset); the drain latches kConfigApply via `deferredConfigApply_` and
// re-checks on every cycle. This avoids the chaos-stress visible quirk
// where a config reload landing mid-word committed a partial word (e.g.
// `uongs` → `uôngs` instead of `uống` because the engine was reset
// between `uong` and `s`).
//
// Pre-P3e (P2c→P3d): handler existed dormant; the worker's ReloadFromToml
// performed CommitComposition + `engine_ = Create()` inline. Under
// `-InjectConfigReloadMs 50` chaos, that race produced 11/55 failures —
// a UAF: worker swapped `engine_` while hook hot path held a raw
// pointer read. P3e moved the mutation here; P3f added the engine-busy
// gate at the drain.
//
// Why ALWAYS recreate (not just on method change): the engine internally
// stores a TypingConfig copy. modernOrtho / allowZwjf / spellCheckEnabled
// changes need a fresh engine for the new behavior to take effect. The
// drain's busy-gate means we don't recreate per chaos tick — we recreate
// once per word boundary, when applicable, regardless of how many bumps
// stacked up.
//
// Resolves the P0 single-writer-violation TODO item from the 2026-05-19
// review: `engine_` had two writer paths; this collapses to one (hook).
void HookEngine::ApplyConfigOnHookThread() {
    VKEY_ASSERT_HOOK_THREAD();
    auto cfg = config_.load(std::memory_order_acquire);
    if (!cfg) return;

    // Consume smart-switch off→on handoff (worker stashed loaded map in
    // ReloadFromToml). exchange()'ing with nullptr makes the consume
    // single-shot — a redundant ApplyConfig drain won't re-run the swap.
    if (auto loaded = pendingAppModeMap_.exchange(nullptr, std::memory_order_acq_rel)) {
        if (!focus_.Smart().IsConnected()) {
            (void)focus_.Smart().Create();
        }
        focus_.AppModeMap() = *loaded;
        if (!focus_.AppModeMap().empty()) {
            focus_.Smart().LoadFromMap(focus_.AppModeMap());
        }
        focus_.PublishAppModesSnapshot();
        HOOK_LOG(L"  SmartSwitch: off→on swap-in (%zu apps)",
                 focus_.AppModeMap().size());
    }

#ifdef VKEY_USE_RUST_ENGINE
    if (auto loaded = pendingUserDictionary_.exchange(nullptr,
                                                       std::memory_order_acq_rel)) {
        userDictionary_ = std::move(loaded);
    }
#endif

    // Resolve target inputMethod considering per-app override (Phase 3c
    // snapshot reader). activeExe_ is hook-owned (set in
    // ApplyFocusOnHookThread); reading it here is single-threaded safe.
    auto snap = configSnapshot_.load(std::memory_order_acquire);
    InputMethod targetMethod = cfg->inputMethod;
    if (snap && !focus_.ActiveExe().empty()) {
        auto it = snap->appInputMethodOverrides.find(focus_.ActiveExe());
        if (it != snap->appInputMethodOverrides.end()) targetMethod = it->second;
    }

    // P3f: caller (DrainHookCommands) gates on engine_->Count() == 0, so
    // CommitComposition would be a no-op. Skip it to keep this handler
    // purely focused on the engine swap.
    currentMethod_.store(targetMethod, std::memory_order_release);
    TypingConfig engineConfig = *cfg;
    engineConfig.inputMethod = targetMethod;
    engine_ = EngineFactory::Create(engineConfig);
#ifdef VKEY_USE_RUST_ENGINE
    if (userDictionary_ && EngineFactory::WillUseRustEngine(engineConfig)) {
        const bool attached = static_cast<RustInputEngine*>(engine_.get())
                                  ->SetUserDictionary(userDictionary_);
        HOOK_LOG(L"  UserDictionary: config-boundary attach %s",
                 attached ? L"ok" : L"failed");
    }
#endif

    HOOK_LOG(L"  ApplyConfig: engine recreated (method=%d, modernOrtho=%d, allowZwjf=%d)",
             static_cast<int>(targetMethod),
             cfg->modernOrtho ? 1 : 0,
             cfg->allowZwjf ? 1 : 0);
}

void HookEngine::ApplyTickPollOnHookThread() {
    VKEY_ASSERT_HOOK_THREAD();
    // Force a full read so a crashed/suspended extension expires after the TTL
    // even when no new generation is published.
    RefreshBrowserRouteOnHookThread(true, true);
    // CheckLayoutChange queries GetKeyboardLayout (kernel-cached, fast)
    // and may call OnLayoutChanged → layoutSuppressed_ writes + engine
    // commit. All hook-thread-safe.
    CheckLayoutChange();
}

bool HookEngine::IsEffectiveTsf() const noexcept {
    return ResolveEffectiveTsf(
        isTsfApp_.load(std::memory_order_acquire),
        isExcludedApp_.load(std::memory_order_acquire),
        tsfFeatureEnabled_.load(std::memory_order_acquire),
        browserRoute_.load(std::memory_order_acquire));
}

void HookEngine::RefreshBrowserRouteOnHookThread(bool forceRead, bool notifyCallback) {
    VKEY_ASSERT_HOOK_THREAD();
    const std::uint32_t generation = browserContext_.ReadGeneration();
    if (!forceRead && generation == lastBrowserContextGeneration_) return;
    lastBrowserContextGeneration_ = generation;

    BrowserRoute next = BrowserRoute::Default;
    BrowserMode requestedMode = BrowserMode::Default;
    bool contextActive = false;
    BrowserContextState state{};
    if (browserContext_.Read(state)) {
        const std::wstring& activeExe = focus_.ActiveExe();
        char foregroundExe[32]{};
        bool ascii = !activeExe.empty() && activeExe.size() < sizeof(foregroundExe);
        if (ascii) {
            for (std::size_t i = 0; i < activeExe.size(); ++i) {
                if (activeExe[i] < 0 || activeExe[i] > 0x7f) { ascii = false; break; }
                foregroundExe[i] = static_cast<char>(activeExe[i]);
            }
        }
        if (ascii && IsBrowserContextFocusedForExe(
                state, foregroundExe, GetTickCount64())) {
            contextActive = true;
            next = state.route;
            requestedMode = ResolveBrowserModeForRestore(state);
        }
    }

    browserContextActive_ = contextActive;
    const BrowserRoute previous = browserRoute_.exchange(next, std::memory_order_acq_rel);
    const bool routeChanged = previous != next;
    const int modeTarget = ResolveBrowserModeTarget(
        isExcludedApp_.load(std::memory_order_acquire),
        isForcedVnApp_.load(std::memory_order_acquire),
        next, requestedMode);
    const bool modeChanged = modeTarget >= 0
        && vietnameseMode_.load(std::memory_order_acquire) != (modeTarget != 0);
    if (!routeChanged && !modeChanged) return;

    if (engine_ && engine_->Count() > 0) CommitComposition(true);
    if (modeChanged) {
        vietnameseMode_.store(modeTarget != 0, std::memory_order_release);
    }
    const bool configured = isTsfApp_.load(std::memory_order_acquire);
    const bool excluded = isExcludedApp_.load(std::memory_order_acquire);
    const bool tsfFeatureEnabled = tsfFeatureEnabled_.load(std::memory_order_acquire);
    const bool wasEffective = ResolveEffectiveTsf(
        configured, excluded, tsfFeatureEnabled, previous);
    const bool isEffective = ResolveEffectiveTsf(
        configured, excluded, tsfFeatureEnabled, next);
    HOOK_LOG(L"  BrowserContext: route %u -> %u mode=%u applied=%d effectiveTsf=%d",
             static_cast<unsigned>(previous), static_cast<unsigned>(next),
             static_cast<unsigned>(requestedMode), modeChanged ? 1 : 0,
             isEffective ? 1 : 0);
    if (routeChanged && notifyCallback && tsfModeCallback_) {
        tsfModeCallback_(isEffective, !isEffective && !excluded,
                         isEffective && !wasEffective);
    }
    NotifyModeChange();
}

}  // namespace NextKey
