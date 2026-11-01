// VKey - Hook Engine Focus Application Subsystem
// SPDX-License-Identifier: GPL-3.0-only

#include "HookEngine.h"

#include "CommitState.h"
#include "FocusOwner.h"
#include "HotkeyManager.h"
#include "OutputDispatcher.h"
#include "helpers/AppHelpers.h"
#include "core/AutoCapDecision.h"
#include "core/FocusApplyDecision.h"
#include "core/Logger.h"
#include "core/config/ConfigSnapshot.h"
#include "core/engine/EngineFactory.h"
#include "app/system/HookCommandMailbox.h"

#include <cassert>

#include "HookEngineInternal.h"

namespace NextKey {

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

void HookEngine::ReconcileModifierStateOnHookThread(bool gestureBoundary) noexcept {
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
    // Same-target focus noise (#273: capture tools firing FOREGROUND every
    // second) is not a gesture boundary; clearing here breaks double-tap.
    if (gestureBoundary) {
        modComboSeen_ = anyPhysicalModifier;
        otherKeyPressed_ = anyPhysicalModifier;
        for (int i = 0; i < kModCount; ++i) {
            modTapCount_[i] = 0;
            modTapLastTs_[i] = 0;
        }
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
    //
    // Known accepted limitation (maintainer decision 2026-11-01): a fast A->B->A
    // switch whose B request is dropped/merged leaves A seeing "unchanged", so
    // keys typed into B during that window stay in the composition; accepted as rare.
    const std::uintptr_t childForFocus = (currentFocusedChildOpaque != 0)
        ? currentFocusedChildOpaque
        : cls->focusedChildHwndOpaque;
    const std::uintptr_t effectiveFocusHwnd =
        (childForFocus != 0) ? childForFocus : cls->hwndOpaque;

    const bool targetChanged = ShouldResetCompositionOnFocusChange(
        cls->hwndOpaque, effectiveFocusHwnd,
        lastAppliedHwnd_, lastAppliedEffectiveFocusHwnd_);

    if (targetChanged) {
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
    if (targetChanged) {
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
    }
    // Reconcile our modifier/hotkey trackers after desktop switch / Win+L,
    // without turning a still-held Alt+Tab chord into an Alt-alone release.
    // Runs even for an unchanged target: Win+L returns to the same window.
    ReconcileModifierStateOnHookThread(targetChanged);

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

}  // namespace NextKey
