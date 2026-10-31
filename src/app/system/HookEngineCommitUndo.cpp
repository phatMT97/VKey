// VKey - Hook Engine Commit-Undo Subsystem
// SPDX-License-Identifier: GPL-3.0-only

#include "HookEngine.h"

#include "CommitState.h"
#include "OutputDispatcher.h"
#include "PerfHistogram.h"
#include "core/CommitUndoArmDecision.h"
#include "core/CommitUndoExemption.h"
#include "core/Logger.h"
#include "core/engine/CommittedTextRestore.h"
#include "core/pipeline/ICommitUndoExecutor.h"
#include "output/IOutputInjector.h"

#include <cassert>
#include <cwctype>

#define HOOK_LOG(fmt, ...) do {                                              \
    if (::NextKey::Logger::IsEnabled())                                      \
        ::NextKey::Logger::Log(L"[Hook] " fmt, ##__VA_ARGS__);               \
} while (0)

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

namespace NextKey {

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

void HookEngine::CancelCommitUndo() {
    commitState_.Cancel();
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
    return NextKey::Pipeline::CommitUndoOutcome::Fallthrough;
}

}  // namespace NextKey
