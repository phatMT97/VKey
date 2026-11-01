// VKey - Commit-undo arming decision (post commit-trigger)
// SPDX-License-Identifier: GPL-3.0-only
//
// Pure decision: after a commit trigger fires and a word was pushed onto the
// commit-undo stack, what should happen to the undo window?
//
//   Arm   - open the undo window so a Backspace right after can revive the
//           just-committed word. Printable triggers (Space, VNI digits 0-9,
//           punctuation) keep the word AT the caret, so revive is valid.
//   Skip  - leave commit-undo state untouched (don't arm; keep the stack).
//           Navigation keys (arrows / Home / End / PgUp / PgDn / Tab / Esc /
//           Delete / Insert) move the caret, so arming would replay text at
//           the wrong position — but an in-place Backspace chain may still be
//           valid, so the stack is preserved (pre-existing behavior).
//   Clear - drop the whole undo window + stack. Enter (VK_RETURN) SENDS the
//           message in chat apps (the word is gone) or breaks to a NEW LINE in
//           editors (the word sits on the prior line). Either way the committed
//           word leaves the caret, so a later Backspace must NOT replay it back
//           into the new line/message. Leaving Enter armed is exactly what
//           desynced chat apps (Messenger / Zalo): Backspace after Enter
//           re-injected the previous message's word, the hook then saw "two
//           words stuck together", which violates Vietnamese phonotactics →
//           English-bias latch → tones blocked until the user deleted the whole
//           word. See issue #210.
//
// Extracted from HookEngine.cpp for Linux GTest coverage: HookEngine.cpp is
// Win32-only and is not linked into the cross-platform VKeyTests target. Raw
// VK hex constants keep this header free of <windows.h>.

#pragma once

#include <cstdint>

namespace NextKey {

enum class CommitUndoArm : uint8_t {
    Arm,
    Skip,
    Clear,
};

/// Keys that LEAVE the editing context entirely, so the committed word is no
/// longer at the caret and replaying it later would inject text somewhere it
/// does not belong: Enter (word moves to the previous line, or is SENT in a
/// chat app) and Tab (focus can move to an entirely different control).
///
/// Single source of truth for that policy. It is enforced at ONE choke point —
/// ProcessKeyDown calls CommitState::DiscardReplayContext() for these keys
/// before any guard can eat or pass the key — rather than relying on the
/// post-commit DecideCommitUndoArm switch, which is reachable only when a
/// commit trigger fires with a non-empty engine AND a new stack entry was
/// actually pushed. Quick-consonant commits, empty-history commits and
/// "Tab pressed with an empty engine but a populated stack" all bypass that
/// switch, and HandleBackspace re-arms replay from any surviving stack.
///
/// Same-field navigation (arrows / Home / End / PgUp / PgDn) is deliberately
/// NOT a boundary: the caret stays in the field and multi-word backward replay
/// legitimately depends on the stack surviving those keys.
[[nodiscard]] constexpr bool IsReplayContextBoundary(uint32_t vkCode) noexcept {
    constexpr uint32_t kVkReturn = 0x0D;
    constexpr uint32_t kVkTab    = 0x09;
    return vkCode == kVkReturn || vkCode == kVkTab;
}

[[nodiscard]] constexpr bool IsCaretNavigationKey(uint32_t vkCode) noexcept {
    constexpr uint32_t kVkPrior  = 0x21;  // Page Up
    constexpr uint32_t kVkNext   = 0x22;  // Page Down
    constexpr uint32_t kVkEnd    = 0x23;
    constexpr uint32_t kVkHome   = 0x24;
    constexpr uint32_t kVkLeft   = 0x25;
    constexpr uint32_t kVkDown   = 0x28;  // VK_LEFT..VK_DOWN occupy 0x25..0x28
    return (vkCode >= kVkLeft && vkCode <= kVkDown) ||
           vkCode == kVkHome || vkCode == kVkEnd ||
           vkCode == kVkPrior || vkCode == kVkNext;
}

/// `vkCode` is the Win32 virtual-key of the commit trigger that just fired.
[[nodiscard]] constexpr CommitUndoArm DecideCommitUndoArm(uint32_t vkCode) noexcept {
    constexpr uint32_t kVkEscape = 0x1B;
    constexpr uint32_t kVkPrior  = 0x21;  // Page Up
    constexpr uint32_t kVkNext   = 0x22;  // Page Down
    constexpr uint32_t kVkEnd    = 0x23;
    constexpr uint32_t kVkHome   = 0x24;
    constexpr uint32_t kVkLeft   = 0x25;
    constexpr uint32_t kVkDown   = 0x28;  // VK_LEFT..VK_DOWN occupy 0x25..0x28
    constexpr uint32_t kVkInsert = 0x2D;
    constexpr uint32_t kVkDelete = 0x2E;

    // Enter (leaves the line / sends) and Tab (can leave the control) — the
    // committed word is no longer at the caret, so never keep a replay window.
    // Defense-in-depth only: correctness comes from the DiscardReplayContext
    // choke point in ProcessKeyDown, which also covers the paths that never
    // reach this switch. Shares IsReplayContextBoundary so the two can't drift.
    if (IsReplayContextBoundary(vkCode)) return CommitUndoArm::Clear;

    const bool isNavigation =
        (vkCode >= kVkLeft && vkCode <= kVkDown) ||
        vkCode == kVkHome || vkCode == kVkEnd ||
        vkCode == kVkPrior || vkCode == kVkNext ||
        vkCode == kVkEscape ||
        vkCode == kVkDelete || vkCode == kVkInsert;
    if (isNavigation) return CommitUndoArm::Skip;

    // Space, VNI digits, punctuation: the word stays at the caret → arm revive.
    return CommitUndoArm::Arm;
}

// ── Host text transforms vs. commit-undo ──────────────────────────────────
//
// The hook cannot read host text; commit-undo replays a committed word on the
// assumption that every counted keystroke is still one char on screen. Hosts
// that rewrite typed text break that assumption, and the replay then BS-diffs
// against text that is gone. Known transforms and the rule covering each:
//
//   Host / feature                         Typed        Becomes   Rule
//   Teams/Slack/Discord emoticons          ":D" ":)"    😀 🙂     DecideReadyGapKey
//   Teams "(y)", Word "(c)"                "(y)"        👍 ©      DecideReadyGapKey
//   Word/Outlook AutoCorrect               "..." "--"   … —       DecideReadyGapKey
//   Teams/Slack/Discord emoji shortcode    ":smile:"    😄        IsHostReplacedWord
//
// DecideReadyGapKey is generic (only spaces are countable in the gap). Word-
// level rules are deliberately narrow — only shapes observed in a real host —
// because each one forfeits backspace-revive for that word. When a new host
// transform is reported: reproduce it, add a row here, extend the narrowest
// rule that covers it, and add a test to CommitUndoArmDecisionTest.cpp.
//
// UniKey has the same blind spot (verified 2026-10-16 in Teams: "k s ạ :D",
// BS×3, "s" → "k sá"); it only looks fine with "d" because UniKey appends an
// invalid key without backspacing, whereas VKey restores raw ("k sajd").

/// What a key typed while commit-undo is Ready (engine empty, caret sitting in
/// the gap after a committed word) does to the replay window.
enum class ReadyGapAction : uint8_t {
    TrackSpace,  ///< Count one more gap char; BS must delete it before priming.
    Cancel,      ///< Gap is no longer countable → drop the window + stack.
    StartWord,   ///< A new word starts; keep the stack for multi-word backward.
};

/// The hook cannot read host text: the replay window only works while counted
/// keystrokes still match the screen one-for-one. Spaces do. Anything else in
/// the gap can be merged or substituted by the host — Teams/Slack/Discord turn
/// ":D", ":)", "(y)", "8)" into one emoji glyph, Word AutoCorrect turns "..."
/// into "…" — and a single Backspace then erases several counted chars. Every
/// later BS lands one char to the left of where the hook thinks it is, the
/// replay revives a word that is already gone, and its BS-diff eats the gap
/// ("k s ạ :D" → emoji → BS×3 → "d" rendered "k sajd").
///
/// `triggerChar` is the char the trigger key inserts (0 when not printable).
/// `gapEndsInGlyph` is true when the char left of the caret is the committed
/// word's own non-space trigger (e.g. ':' in "ạ:"): a word started right
/// there fuses with it ("ạ:D") just like an extra trigger would.
[[nodiscard]] constexpr ReadyGapAction DecideReadyGapKey(
    bool isCommitTrigger, wchar_t triggerChar, uint8_t pendingTriggers,
    bool gapEndsInGlyph) noexcept {
    if (isCommitTrigger) {
        // Non-printable triggers (Esc/Tab/Enter/arrows) leave the caret
        // context; printable non-space ones make the gap host-transformable.
        return triggerChar == L' ' ? ReadyGapAction::TrackSpace
                                   : ReadyGapAction::Cancel;
    }
    if (pendingTriggers == 0 && gapEndsInGlyph) return ReadyGapAction::Cancel;
    return ReadyGapAction::StartWord;
}

/// Primed (gap already backspaced, caret right after the committed word): a
/// printable non-space trigger now sits glued to that word — "ạ" + ":" + "D"
/// becomes "ạ😀" in Teams — so the stack can no longer be trusted. A space or
/// a non-printable key keeps the existing Idle-with-stack behaviour.
[[nodiscard]] constexpr bool ShouldCancelPrimedOnTrigger(
    bool isCommitTrigger, wchar_t triggerChar) noexcept {
    return isCommitTrigger && triggerChar > L' ';
}

/// True when a word the host replaces wholesale was just committed, so it must
/// not be armed for replay. `openGlyph` is the char typed right before the
/// word's first letter (0 if none / not punctuation); `closeTrigger` is the
/// char of the trigger that committed it. Narrow on purpose — see the table
/// above; currently only the emoji shortcode ":name:".
[[nodiscard]] constexpr bool IsHostReplacedWord(wchar_t openGlyph,
                                                wchar_t closeTrigger) noexcept {
    return openGlyph == L':' && closeTrigger == L':';
}

}  // namespace NextKey
