// VKey - CommitUndoArmDecision unit tests
// SPDX-License-Identifier: GPL-3.0-only
//
// Locks in the issue #210 fix: Enter must CLEAR commit-undo (never arm), so a
// Backspace after Enter cannot replay a word that the host already sent (chat)
// or pushed to a previous line (editor). HookEngine.cpp is Win32-only; this
// covers the pure decision it now delegates to.

#include "core/CommitUndoArmDecision.h"
#include <gtest/gtest.h>
#include <queue>
#include <string>
#include <string_view>

using NextKey::CommitUndoArm;
using NextKey::DecideCommitUndoArm;
using NextKey::IsCaretNavigationKey;

namespace {

// Win32 VK codes (kept local so the test documents the exact inputs).
constexpr uint32_t VK_RETURN_ = 0x0D;
constexpr uint32_t VK_SPACE_  = 0x20;
constexpr uint32_t VK_BACK_   = 0x08;
constexpr uint32_t VK_TAB_    = 0x09;
constexpr uint32_t VK_ESCAPE_ = 0x1B;
constexpr uint32_t VK_LEFT_   = 0x25;
constexpr uint32_t VK_UP_     = 0x26;
constexpr uint32_t VK_RIGHT_  = 0x27;
constexpr uint32_t VK_DOWN_   = 0x28;
constexpr uint32_t VK_HOME_   = 0x24;
constexpr uint32_t VK_END_    = 0x23;
constexpr uint32_t VK_PRIOR_  = 0x21;
constexpr uint32_t VK_NEXT_   = 0x22;
constexpr uint32_t VK_INSERT_ = 0x2D;
constexpr uint32_t VK_DELETE_ = 0x2E;

// --- The fix: Enter clears, never arms -------------------------------------

TEST(CommitUndoArmDecisionTest, Enter_Clears_issue210) {
    // Regression: leaving Enter armed let a post-Enter Backspace replay the
    // previous message's word in chat apps → "2 words stuck" → tones blocked.
    EXPECT_EQ(DecideCommitUndoArm(VK_RETURN_), CommitUndoArm::Clear);
}

// --- Printable triggers arm (word stays at caret) --------------------------

TEST(CommitUndoArmDecisionTest, Space_Arms) {
    EXPECT_EQ(DecideCommitUndoArm(VK_SPACE_), CommitUndoArm::Arm);
}

TEST(CommitUndoArmDecisionTest, VniDigitsAndPunctuation_Arm) {
    for (uint32_t vk = '0'; vk <= '9'; ++vk)
        EXPECT_EQ(DecideCommitUndoArm(vk), CommitUndoArm::Arm) << "vk=" << vk;
    EXPECT_EQ(DecideCommitUndoArm(0xBC), CommitUndoArm::Arm);  // VK_OEM_COMMA ','
    EXPECT_EQ(DecideCommitUndoArm(0xBE), CommitUndoArm::Arm);  // VK_OEM_PERIOD '.'
}

// --- Navigation keys skip (caret moved, but keep stack) --------------------

TEST(CommitUndoArmDecisionTest, NavigationKeys_Skip) {
    for (uint32_t vk : {VK_LEFT_, VK_UP_, VK_RIGHT_, VK_DOWN_, VK_HOME_, VK_END_,
                        VK_PRIOR_, VK_NEXT_, VK_ESCAPE_, VK_INSERT_,
                        VK_DELETE_}) {
        EXPECT_EQ(DecideCommitUndoArm(vk), CommitUndoArm::Skip) << "vk=" << vk;
    }
}

// --- Replay-context boundary policy ----------------------------------------
// Single source of truth for "this key LEAVES the editing context, so a replay
// window must never survive it". Enforced at one choke point in ProcessKeyDown
// (CommitState::DiscardReplayContext) because the post-commit switch below is
// unreachable when the engine is empty, when the commit is quick-consonant, or
// when no stack entry is pushed — and HandleBackspace re-arms replay from any
// stack that survives.

TEST(CommitUndoArmDecisionTest, ReplayContextBoundary_IsEnterAndTabOnly) {
    EXPECT_TRUE(NextKey::IsReplayContextBoundary(VK_RETURN_));
    EXPECT_TRUE(NextKey::IsReplayContextBoundary(VK_TAB_));

    // Same-field navigation is NOT a boundary — multi-word backward replay
    // legitimately depends on the stack surviving these.
    for (uint32_t vk : {VK_LEFT_, VK_UP_, VK_RIGHT_, VK_DOWN_, VK_HOME_, VK_END_,
                        VK_PRIOR_, VK_NEXT_, VK_ESCAPE_, VK_INSERT_, VK_DELETE_,
                        VK_SPACE_}) {
        EXPECT_FALSE(NextKey::IsReplayContextBoundary(vk)) << "vk=" << vk;
    }
}

TEST(CommitUndoArmDecisionTest, BoundaryKeysAgreeWithArmDecision) {
    // The two must not drift: every boundary key must also map to Clear, so the
    // post-commit switch stays consistent defense-in-depth for the choke point.
    for (uint32_t vk = 0; vk < 0x100; ++vk) {
        if (NextKey::IsReplayContextBoundary(vk)) {
            EXPECT_EQ(DecideCommitUndoArm(vk), CommitUndoArm::Clear)
                << "boundary key must also Clear, vk=" << vk;
        }
    }
}

// --- Tab clears (unlike same-field navigation, Tab commonly moves focus to
// a DIFFERENT control) -------------------------------------------------------

TEST(CommitUndoArmDecisionTest, Tab_Clears) {
    // Regression: Skip previously kept commitStack_ alive across a Tab-driven
    // focus change, so backspacing-to-empty in the NEW field could re-arm
    // Ready from the OLD field's stack entry and replay its text there.
    EXPECT_EQ(DecideCommitUndoArm(VK_TAB_), CommitUndoArm::Clear);
}

// --- Enter is NOT misclassified as navigation ------------------------------

TEST(CommitUndoArmDecisionTest, Enter_IsNot_Skip) {
    // Enter sits numerically below the navigation block; make sure it routes to
    // Clear, not Skip (Skip keeps the stack → the bug would persist).
    EXPECT_NE(DecideCommitUndoArm(VK_RETURN_), CommitUndoArm::Skip);
    EXPECT_NE(DecideCommitUndoArm(VK_RETURN_), CommitUndoArm::Arm);
}

// --- Ready gap: only spaces are countable ----------------------------------
// Host apps merge punctuation runs (Teams ":D" → one emoji, Word "..." → "…");
// one BS then erases several counted chars and the replay lands on a word that
// is already gone ("k s ạ :D" → emoji → BS×3 → "d" rendered "k sajd").

using NextKey::DecidePrimedTriggerAction;
using NextKey::DecideReadyGapKey;
using NextKey::PrimedTriggerAction;
using NextKey::ReadyGapAction;
using NextKey::ShouldCancelPrimedOnTrigger;

TEST(CommitUndoArmDecisionTest, ReadyGap_ExtraSpace_IsTracked) {
    EXPECT_EQ(DecideReadyGapKey(true, L' ', 0, false), ReadyGapAction::TrackSpace);
    // Space after a punctuation trigger ("chịu, ") stays countable.
    EXPECT_EQ(DecideReadyGapKey(true, L' ', 0, true), ReadyGapAction::TrackSpace);
}

TEST(CommitUndoArmDecisionTest, ReadyGap_Punctuation_Cancels_TeamsEmoticon) {
    // "ạ " + ':' — the ':' may fuse with the next char into an emoji.
    EXPECT_EQ(DecideReadyGapKey(true, L':', 0, false), ReadyGapAction::Cancel);
    // "ạ:" + ')' / "ạ." + '.' / "ạ " + '8'.
    EXPECT_EQ(DecideReadyGapKey(true, L')', 0, true), ReadyGapAction::Cancel);
    EXPECT_EQ(DecideReadyGapKey(true, L'.', 0, true), ReadyGapAction::Cancel);
    EXPECT_EQ(DecideReadyGapKey(true, L'8', 1, false), ReadyGapAction::Cancel);
    // Previously counted as a pending trigger ("a==").
    EXPECT_EQ(DecideReadyGapKey(true, L'=', 0, true), ReadyGapAction::Cancel);
}

TEST(CommitUndoArmDecisionTest, ReadyGap_NonPrintableTrigger_Cancels) {
    EXPECT_EQ(DecideReadyGapKey(true, L'\0', 0, false), ReadyGapAction::Cancel);
    EXPECT_EQ(DecideReadyGapKey(true, L'\t', 2, false), ReadyGapAction::Cancel);
}

TEST(CommitUndoArmDecisionTest, ReadyGap_WordAfterSpace_StartsWord) {
    EXPECT_EQ(DecideReadyGapKey(false, L'\0', 0, false), ReadyGapAction::StartWord);
    // "chịu, " + 'a': the space separates the new word from the comma.
    EXPECT_EQ(DecideReadyGapKey(false, L'\0', 1, true), ReadyGapAction::StartWord);
}

TEST(CommitUndoArmDecisionTest, ReadyGap_WordGluedToPunctuation_Cancels) {
    // "ạ:" + 'D' → host may render "ạ😀".
    EXPECT_EQ(DecideReadyGapKey(false, L'\0', 0, true), ReadyGapAction::Cancel);
}

TEST(CommitUndoArmDecisionTest, Primed_GluedPunctuation_Cancels) {
    // "ạ " BS (space gone) + ':' → ':' now touches "ạ".
    EXPECT_EQ(DecidePrimedTriggerAction(true, L':'), PrimedTriggerAction::Cancel);
    EXPECT_EQ(DecidePrimedTriggerAction(true, L'2'), PrimedTriggerAction::Cancel);
    EXPECT_TRUE(ShouldCancelPrimedOnTrigger(true, L':'));
    EXPECT_TRUE(ShouldCancelPrimedOnTrigger(true, L'2'));
}

TEST(CommitUndoArmDecisionTest, Primed_Space_ReEntersReady) {
    // "đanh " BS (space gone, Primed) + Space → caret is in gap after word again.
    // Must re-enter Ready so a subsequent BS can prime the word again.
    EXPECT_EQ(DecidePrimedTriggerAction(true, L' '), PrimedTriggerAction::ReEnterReady);
    EXPECT_FALSE(ShouldCancelPrimedOnTrigger(true, L' '));
}

TEST(CommitUndoArmDecisionTest, Primed_NonPrintableOrNonTrigger_Idle) {
    // Non-printable triggers (Tab, Esc) or non-trigger keys transition to Idle.
    EXPECT_EQ(DecidePrimedTriggerAction(true, L'\0'), PrimedTriggerAction::Idle);
    EXPECT_EQ(DecidePrimedTriggerAction(false, L'\0'), PrimedTriggerAction::Idle);
    EXPECT_FALSE(ShouldCancelPrimedOnTrigger(true, L'\0'));
    EXPECT_FALSE(ShouldCancelPrimedOnTrigger(false, L'\0'));
}

// --- Host-replaced words: narrow, only shapes seen in a real host -----------

using NextKey::IsHostReplacedWord;

TEST(CommitUndoArmDecisionTest, HostReplacedWord_EmojiShortcode) {
    // ":smile:" in Teams/Slack/Discord → one emoji, word included.
    EXPECT_TRUE(IsHostReplacedWord(L':', L':'));
}

TEST(CommitUndoArmDecisionTest, HostReplacedWord_OtherPunctuation_StaysRevivable) {
    // Not observed as a host transform → keep backspace-revive.
    EXPECT_FALSE(IsHostReplacedWord(L'(', L')'));   // "(chào)"
    EXPECT_FALSE(IsHostReplacedWord(L'"', L'"'));   // "\"chào\""
    EXPECT_FALSE(IsHostReplacedWord(L':', L' '));   // ":chào "
    EXPECT_FALSE(IsHostReplacedWord(L'\0', L':'));  // "chào:"
    EXPECT_FALSE(IsHostReplacedWord(L'\0', L'\0'));
}

TEST(CommitUndoArmDecisionTest, CaretNavigationKeysClassification) {
    EXPECT_TRUE(IsCaretNavigationKey(VK_LEFT_));
    EXPECT_TRUE(IsCaretNavigationKey(VK_UP_));
    EXPECT_TRUE(IsCaretNavigationKey(VK_RIGHT_));
    EXPECT_TRUE(IsCaretNavigationKey(VK_DOWN_));
    EXPECT_TRUE(IsCaretNavigationKey(VK_HOME_));
    EXPECT_TRUE(IsCaretNavigationKey(VK_END_));
    EXPECT_TRUE(IsCaretNavigationKey(VK_PRIOR_));
    EXPECT_TRUE(IsCaretNavigationKey(VK_NEXT_));

    EXPECT_FALSE(IsCaretNavigationKey(VK_RETURN_));
    EXPECT_FALSE(IsCaretNavigationKey(VK_SPACE_));
    EXPECT_FALSE(IsCaretNavigationKey(VK_TAB_));
    EXPECT_FALSE(IsCaretNavigationKey(VK_ESCAPE_));
    EXPECT_FALSE(IsCaretNavigationKey('A'));
}

// ── Keystroke sequence simulation: Commit-Undo FSM + Host Queue ──────────────
//
// Models the race condition between synchronous replacement (EM_REPLACESEL in
// RichEdit/Win11 Notepad) and the host's asynchronous posted message queue
// (WM_KEYDOWN).
//
// Scenario: "abc " committed → Backspace → Space → Backspace.
//
// In HEAD:
//   - Backspace 1: in Ready → Primed; SyncReplace(1, "") deletes space → "abc".
//   - Space: in Primed → Idle; posted to queue.
//   - Backspace 2: in Idle → posted to queue.
//   - Queue drains: Space then Backspace cancel out → "abc" (correct text, but
//     FSM dropped to Idle so diacritics stopped working).
//
// In naive Primed → Ready fix (Fallthrough for Space):
//   - Backspace 1: in Ready → Primed; SyncReplace(1, "") deletes space → "abc".
//   - Space: in Primed → Ready; posted to queue (document still "abc"!).
//   - Backspace 2: in Ready → Primed; SyncReplace(1, "") executes synchronously
//     BEFORE the posted Space drains! Deletes 'c' → "ab".
//   - Queue drains: Space is processed → "ab " (REGRESSION: 'c' lost!).
//
// In the proper fix:
//   - Space re-entering Ready is sent via SyncReplace(0, " ") on sync channels,
//     eating the physical key. Document becomes "abc " immediately.
//   - Backspace 2: SyncReplace(1, "") deletes ' ' → "abc" (no 'c' lost!).

struct HostQueueSimulator {
    std::wstring document;
    std::queue<wchar_t> postedQueue;
    bool isSyncChannel = true;

    void SyncReplace(size_t bs, std::wstring_view text) {
        if (bs > document.size()) bs = document.size();
        document.resize(document.size() - bs);
        document.append(text);
    }

    void PostKey(wchar_t ch) {
        postedQueue.push(ch);
    }

    void DrainOne() {
        if (postedQueue.empty()) return;
        wchar_t ch = postedQueue.front();
        postedQueue.pop();
        if (ch == L'\b') {
            if (!document.empty()) document.pop_back();
        } else {
            document.push_back(ch);
        }
    }

    void DrainAll() {
        while (!postedQueue.empty()) {
            DrainOne();
        }
    }
};

enum class SimulatedFsmState {
    Idle,
    Ready,
    Primed,
};

struct CommitUndoSimOutcome {
    enum class Outcome { Eat, Pass };
    Outcome outcome;
    SimulatedFsmState nextState;
};

// With the proper fix (Space in Primed uses SyncReplace when isSyncChannel == true):
CommitUndoSimOutcome ProcessKeyFixed(SimulatedFsmState state, uint32_t vk,
                                     HostQueueSimulator& host) {
    if (state == SimulatedFsmState::Ready && vk == VK_BACK_) {
        // BS in Ready with pendingTriggers == 0:
        if (host.isSyncChannel) {
            host.SyncReplace(1, L"");
            return {CommitUndoSimOutcome::Outcome::Eat, SimulatedFsmState::Primed};
        }
        host.PostKey(L'\b');
        return {CommitUndoSimOutcome::Outcome::Pass, SimulatedFsmState::Primed};
    }
    if (state == SimulatedFsmState::Primed && vk == VK_SPACE_) {
        const auto action = DecidePrimedTriggerAction(true, L' ');
        if (action == PrimedTriggerAction::ReEnterReady) {
            if (host.isSyncChannel) {
                host.SyncReplace(0, L" ");
                return {CommitUndoSimOutcome::Outcome::Eat, SimulatedFsmState::Ready};
            }
            host.PostKey(L' ');
            return {CommitUndoSimOutcome::Outcome::Pass, SimulatedFsmState::Ready};
        }
    }
    if (state == SimulatedFsmState::Idle && vk == VK_BACK_) {
        host.PostKey(L'\b');
        return {CommitUndoSimOutcome::Outcome::Pass, SimulatedFsmState::Idle};
    }
    return {CommitUndoSimOutcome::Outcome::Pass, state};
}

// With the naive buggy Fallthrough (Space in Primed always posts to host queue):
CommitUndoSimOutcome ProcessKeyBuggy(SimulatedFsmState state, uint32_t vk,
                                     HostQueueSimulator& host) {
    if (state == SimulatedFsmState::Ready && vk == VK_BACK_) {
        if (host.isSyncChannel) {
            host.SyncReplace(1, L"");
            return {CommitUndoSimOutcome::Outcome::Eat, SimulatedFsmState::Primed};
        }
        host.PostKey(L'\b');
        return {CommitUndoSimOutcome::Outcome::Pass, SimulatedFsmState::Primed};
    }
    if (state == SimulatedFsmState::Primed && vk == VK_SPACE_) {
        const auto action = DecidePrimedTriggerAction(true, L' ');
        if (action == PrimedTriggerAction::ReEnterReady) {
            // BUG: falls through without SyncReplace, passing physical key to host queue!
            host.PostKey(L' ');
            return {CommitUndoSimOutcome::Outcome::Pass, SimulatedFsmState::Ready};
        }
    }
    return {CommitUndoSimOutcome::Outcome::Pass, state};
}

TEST(CommitUndoArmDecisionTest, FastTyping_SpaceBackspaceSequence_FixedSyncChannelPreservesWord) {
    HostQueueSimulator host;
    host.document = L"abc ";
    host.isSyncChannel = true;
    auto state = SimulatedFsmState::Ready;

    // 1. Backspace: Ready → Primed (space deleted via sync channel)
    auto res = ProcessKeyFixed(state, VK_BACK_, host);
    state = res.nextState;
    EXPECT_EQ(res.outcome, CommitUndoSimOutcome::Outcome::Eat);
    EXPECT_EQ(state, SimulatedFsmState::Primed);
    EXPECT_EQ(host.document, L"abc");
    EXPECT_TRUE(host.postedQueue.empty());

    // 2. Space: Primed → Ready (space inserted via sync channel, physical key eaten)
    res = ProcessKeyFixed(state, VK_SPACE_, host);
    state = res.nextState;
    EXPECT_EQ(res.outcome, CommitUndoSimOutcome::Outcome::Eat);
    EXPECT_EQ(state, SimulatedFsmState::Ready);
    EXPECT_EQ(host.document, L"abc ");
    EXPECT_TRUE(host.postedQueue.empty());

    // 3. Backspace: Ready → Primed (space deleted via sync channel)
    res = ProcessKeyFixed(state, VK_BACK_, host);
    state = res.nextState;
    EXPECT_EQ(res.outcome, CommitUndoSimOutcome::Outcome::Eat);
    EXPECT_EQ(state, SimulatedFsmState::Primed);
    EXPECT_EQ(host.document, L"abc");
    EXPECT_TRUE(host.postedQueue.empty());

    // Draining queue has nothing left to process
    host.DrainAll();
    EXPECT_EQ(host.document, L"abc");
}

TEST(CommitUndoArmDecisionTest, FastTyping_SpaceBackspaceSequence_BuggyFallthroughReproducesCharLoss) {
    HostQueueSimulator host;
    host.document = L"abc ";
    host.isSyncChannel = true;
    auto state = SimulatedFsmState::Ready;

    // 1. Backspace: Ready → Primed
    auto res = ProcessKeyBuggy(state, VK_BACK_, host);
    state = res.nextState;
    EXPECT_EQ(state, SimulatedFsmState::Primed);
    EXPECT_EQ(host.document, L"abc");

    // 2. Space: Primed → Ready, but falls through to posted queue!
    res = ProcessKeyBuggy(state, VK_SPACE_, host);
    state = res.nextState;
    EXPECT_EQ(res.outcome, CommitUndoSimOutcome::Outcome::Pass);
    EXPECT_EQ(state, SimulatedFsmState::Ready);
    EXPECT_EQ(host.document, L"abc");  // Not yet inserted in document!
    EXPECT_EQ(host.postedQueue.size(), 1u);

    // 3. Rapid Backspace: Ready → Primed via sync channel before posted Space drains!
    res = ProcessKeyBuggy(state, VK_BACK_, host);
    state = res.nextState;
    EXPECT_EQ(res.outcome, CommitUndoSimOutcome::Outcome::Eat);
    EXPECT_EQ(state, SimulatedFsmState::Primed);
    EXPECT_EQ(host.document, L"ab");  // 'c' was deleted by mistake!

    // Host finally drains posted queue: Space is inserted!
    host.DrainAll();
    EXPECT_EQ(host.document, L"ab ");  // Character loss reproduction: "ab " instead of "abc"
}

TEST(CommitUndoArmDecisionTest, FastTyping_SpaceBackspaceSequence_AsyncChannelPreservesWord) {
    HostQueueSimulator host;
    host.document = L"abc ";
    host.isSyncChannel = false;
    auto state = SimulatedFsmState::Ready;

    // 1. Backspace: Ready → Primed (posted)
    auto res = ProcessKeyFixed(state, VK_BACK_, host);
    state = res.nextState;
    EXPECT_EQ(res.outcome, CommitUndoSimOutcome::Outcome::Pass);
    EXPECT_EQ(state, SimulatedFsmState::Primed);

    // 2. Space: Primed → Ready (posted)
    res = ProcessKeyFixed(state, VK_SPACE_, host);
    state = res.nextState;
    EXPECT_EQ(res.outcome, CommitUndoSimOutcome::Outcome::Pass);
    EXPECT_EQ(state, SimulatedFsmState::Ready);

    // 3. Backspace: Ready → Primed (posted)
    res = ProcessKeyFixed(state, VK_BACK_, host);
    state = res.nextState;
    EXPECT_EQ(res.outcome, CommitUndoSimOutcome::Outcome::Pass);
    EXPECT_EQ(state, SimulatedFsmState::Primed);

    // Draining queue processes BS, Space, BS in strict FIFO order
    host.DrainAll();
    EXPECT_EQ(host.document, L"abc");
}

TEST(CommitUndoArmDecisionTest, FastTyping_MultiCycleRepetition_StaysConsistent) {
    HostQueueSimulator host;
    host.document = L"abc ";
    host.isSyncChannel = true;
    auto state = SimulatedFsmState::Ready;

    for (int i = 0; i < 5; ++i) {
        // BS → Primed
        auto res = ProcessKeyFixed(state, VK_BACK_, host);
        state = res.nextState;
        EXPECT_EQ(state, SimulatedFsmState::Primed);
        EXPECT_EQ(host.document, L"abc");

        // Space → Ready
        res = ProcessKeyFixed(state, VK_SPACE_, host);
        state = res.nextState;
        EXPECT_EQ(state, SimulatedFsmState::Ready);
        EXPECT_EQ(host.document, L"abc ");
    }

    // Final BS to prime for diacritic modification
    auto res = ProcessKeyFixed(state, VK_BACK_, host);
    state = res.nextState;
    EXPECT_EQ(state, SimulatedFsmState::Primed);
    EXPECT_EQ(host.document, L"abc");
}

}  // namespace
