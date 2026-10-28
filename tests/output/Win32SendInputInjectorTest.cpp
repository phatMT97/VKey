// tests/output/Win32SendInputInjectorTest.cpp
//
// Unit tests for Win32SendInputInjector. Mock SendInput via test seam
// (no GUI required); inspect captured INPUT events.
//
// Spec: docs/plans/sprint-2-output-injector.md §5.1, plan Task 1 step 2.
#include "InjectorTestBase.h"
#include "app/output/Win32SendInputInjector.h"

namespace NextKey::Output::Test {

class Win32SendInputInjectorTest : public InjectorTestBase {};

TEST_F(Win32SendInputInjectorTest, ReplaceEmptyTextSendsBackspacesOnly) {
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_TRUE(inj.Replace(3, L""));
    ASSERT_EQ(capturedInputs.size(), 6u);  // 3 BS × (down + up)
    for (size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(capturedInputs[i].ki.wVk, VK_BACK)
            << "event[" << i << "].wVk should be VK_BACK";
    }
    EXPECT_EQ(capturedInputs[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);  // first = down
    EXPECT_NE(capturedInputs[1].ki.dwFlags & KEYEVENTF_KEYUP, 0u);  // second = up
}

TEST_F(Win32SendInputInjectorTest, ReplaceWithCharsSendsBatch) {
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_TRUE(inj.Replace(2, L"vi"));
    // Expected: 2 BS down/up (4 events) + 2 chars down/up (4 events) = 8 events
    ASSERT_EQ(capturedInputs.size(), 8u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[2].ki.wVk, VK_BACK);
    // Char events use UNICODE flag + wScan
    EXPECT_NE(capturedInputs[4].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[4].ki.wScan, L'v');
    EXPECT_NE(capturedInputs[6].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[6].ki.wScan, L'i');
}

TEST_F(Win32SendInputInjectorTest, BaitCharFiresOnPureBackspaceByDefault) {
    // Default (suggestKeepChars=false): pure-BS still emits bait so a
    // Chromium suggestion popup can't swallow the BS as a dismiss-only
    // gesture — engine state stays in sync at the cost of a brief flicker.
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/true);
    EXPECT_TRUE(inj.Replace(2, L""));
    // Expected: bait (down+up=2) + (2+1)=3 BS × down+up = 8 events
    // (1 extra BS deletes the bait).
    ASSERT_EQ(capturedInputs.size(), 8u);
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[0].ki.wScan, 0x202F);
    for (size_t i = 2; i < 8; ++i) {
        EXPECT_EQ(capturedInputs[i].ki.wVk, VK_BACK)
            << "event[" << i << "].wVk should be VK_BACK";
    }
}

TEST_F(Win32SendInputInjectorTest, BaitCharSkippedOnPureBackspaceWhenSuggestKeepCharsOn) {
    // Opt-in setting: BS only dismisses the popup, preserves typed chars
    // ("face" + BS → "face"). Trade-off documented on the toggle.
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/true);
    inj.SetSuggestKeepChars(true);
    EXPECT_TRUE(inj.Replace(2, L""));
    // Expected: 2 BS × (down + up) = 4 events. No bait, no extra BS.
    ASSERT_EQ(capturedInputs.size(), 4u);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(capturedInputs[i].ki.wVk, VK_BACK)
            << "event[" << i << "].wVk should be VK_BACK";
    }
}

TEST_F(Win32SendInputInjectorTest, BaitCharFiresOnReplaceWithText) {
    // Partial-replace (BS + chars) keeps the bait — a tone/modifier
    // transform replaces the trailing chars and must dismiss any
    // pending Chromium suggest first so subsequent chars land into
    // the field, not into a stale suggestion frame.
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/true);
    EXPECT_TRUE(inj.Replace(1, L"x"));
    // Expected: bait (down+up = 2) + 2 BS down/up (= 4: 1 orig + 1 extra
    // to delete the bait) + 1 char down/up (= 2) = 8 events.
    ASSERT_EQ(capturedInputs.size(), 8u);
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[0].ki.wScan, 0x202F);
    EXPECT_EQ(capturedInputs[2].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[4].ki.wVk, VK_BACK);
    EXPECT_NE(capturedInputs[6].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[6].ki.wScan, L'x');
}

TEST_F(Win32SendInputInjectorTest, HeldShiftIsReleasedAroundBackspaceReplacement) {
    // Issue #225: Edge Excel Web interprets a synthetic Backspace while the
    // user holds Shift for Telex Shift+dd -> Đ as a destructive editor
    // command. The injector must make the replacement modifier-neutral and
    // restore the physical Shift state afterward.
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_RSHIFT ? static_cast<SHORT>(-32768) : 0;
    };

    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/true);
    EXPECT_TRUE(inj.Replace(1, L"Đ"));

    // Shift-up, bait, two Backspaces (bait + original D), Đ, Shift-down.
    ASSERT_EQ(capturedInputs.size(), 10u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_RSHIFT);
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_NE(capturedInputs[1].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[1].ki.wScan, 0x202F);
    EXPECT_EQ(capturedInputs[3].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[5].ki.wVk, VK_BACK);
    EXPECT_NE(capturedInputs[7].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[7].ki.wScan, L'Đ');
    EXPECT_EQ(capturedInputs[9].ki.wVk, VK_RSHIFT);
    EXPECT_EQ(capturedInputs[9].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    for (const auto& input : capturedInputs) {
        EXPECT_EQ(input.ki.dwExtraInfo, Internal::kVKeyExtraInfo);
    }
}

TEST_F(Win32SendInputInjectorTest, BothShiftKeysRetainTheirIdentity) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_LSHIFT || vk == VK_RSHIFT
            ? static_cast<SHORT>(-32768)
            : 0;
    };

    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_TRUE(inj.Replace(1, L""));

    ASSERT_EQ(capturedInputs.size(), 6u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_LSHIFT);
    EXPECT_EQ(capturedInputs[1].ki.wVk, VK_RSHIFT);
    EXPECT_EQ(capturedInputs[2].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[3].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[4].ki.wVk, VK_LSHIFT);
    EXPECT_EQ(capturedInputs[5].ki.wVk, VK_RSHIFT);
}

TEST_F(Win32SendInputInjectorTest, HeldControlIsReleasedAroundBackspaceReplacement) {
    // Issue #267: Ctrl held during Backspace turns synthetic Backspaces into
    // Ctrl+Backspace shortcuts, deleting lines/words in Sublime, Notepad++, Word.
    // The injector must release held Ctrl before sending Backspaces and restore it after.
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_LCONTROL ? static_cast<SHORT>(-32768) : 0;
    };

    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_TRUE(inj.Replace(1, L"a"));

    // LControl-up, Backspace (down+up), 'a' (down+up), LControl-down.
    ASSERT_EQ(capturedInputs.size(), 6u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_LCONTROL);
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_EQ(capturedInputs[1].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[2].ki.wVk, VK_BACK);
    EXPECT_NE(capturedInputs[3].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[3].ki.wScan, L'a');
    EXPECT_EQ(capturedInputs[5].ki.wVk, VK_LCONTROL);
    EXPECT_EQ(capturedInputs[5].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    for (const auto& input : capturedInputs) {
        EXPECT_EQ(input.ki.dwExtraInfo, Internal::kVKeyExtraInfo);
    }
}

TEST_F(Win32SendInputInjectorTest, BothControlKeysRetainTheirIdentity) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_LCONTROL || vk == VK_RCONTROL
            ? static_cast<SHORT>(-32768)
            : 0;
    };

    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_TRUE(inj.Replace(1, L""));

    ASSERT_EQ(capturedInputs.size(), 6u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_LCONTROL);
    EXPECT_EQ(capturedInputs[1].ki.wVk, VK_RCONTROL);
    EXPECT_EQ(capturedInputs[2].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[3].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[4].ki.wVk, VK_LCONTROL);
    EXPECT_EQ(capturedInputs[5].ki.wVk, VK_RCONTROL);
}

TEST_F(Win32SendInputInjectorTest, HeldControlAndShiftAreBothReleased) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_LSHIFT || vk == VK_LCONTROL
            ? static_cast<SHORT>(-32768)
            : 0;
    };

    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_TRUE(inj.Replace(1, L""));

    ASSERT_EQ(capturedInputs.size(), 6u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_LSHIFT);
    EXPECT_EQ(capturedInputs[1].ki.wVk, VK_LCONTROL);
    EXPECT_EQ(capturedInputs[2].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[3].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[4].ki.wVk, VK_LSHIFT);
    EXPECT_EQ(capturedInputs[5].ki.wVk, VK_LCONTROL);
}

TEST_F(Win32SendInputInjectorTest, HeldAltIsReleasedAroundBackspaceReplacement) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_LMENU ? static_cast<SHORT>(-32768) : 0;
    };

    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_TRUE(inj.Replace(1, L"a"));

    ASSERT_EQ(capturedInputs.size(), 6u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_LMENU);
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_EQ(capturedInputs[1].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[2].ki.wVk, VK_BACK);
    EXPECT_NE(capturedInputs[3].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[3].ki.wScan, L'a');
    EXPECT_EQ(capturedInputs[5].ki.wVk, VK_LMENU);
    EXPECT_EQ(capturedInputs[5].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
}

TEST_F(Win32SendInputInjectorTest, BaitCharSkippedWhenBsCountZero) {
    // Pure-typing (no deletions) on the Chromium variant: no bait,
    // no extra BS — just the chars. Autocomplete-dismiss isn't needed
    // when nothing is being removed.
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/true);
    EXPECT_TRUE(inj.Replace(0, L"y"));
    ASSERT_EQ(capturedInputs.size(), 2u);  // 1 char × down+up
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[0].ki.wScan, L'y');
}

TEST_F(Win32SendInputInjectorTest, ReplaceReturnsFalseOnPartialSend) {
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    sendInputReturnOverride = 2;  // simulate partial: 2 events delivered out of 6
    EXPECT_FALSE(inj.Replace(3, L""));
}

TEST_F(Win32SendInputInjectorTest, PartialSendAfterShiftUpAttemptsShiftRestore) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_LSHIFT ? static_cast<SHORT>(-32768) : 0;
    };
    sendInputReturnOverride = 1;  // only the leading Shift-up was delivered

    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_FALSE(inj.Replace(1, L"x"));

    // Initial six-event batch plus a one-event best-effort Shift-down recovery.
    ASSERT_EQ(capturedInputs.size(), 7u);
    EXPECT_EQ(capturedInputs.back().ki.wVk, VK_LSHIFT);
    EXPECT_EQ(capturedInputs.back().ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    ASSERT_EQ(synthCounterDeltas.size(), 3u);
    EXPECT_EQ(synthCounterDeltas[0], 6);
    EXPECT_EQ(synthCounterDeltas[1], -5);
    EXPECT_EQ(synthCounterDeltas[2], 1);
}

TEST_F(Win32SendInputInjectorTest, SendKeyEmitsDownAndUpWithMarker) {
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    inj.SendKey(VK_BACK);
    ASSERT_EQ(capturedInputs.size(), 2u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[0].ki.dwExtraInfo, Internal::kVKeyExtraInfo);
    EXPECT_EQ(capturedInputs[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_NE(capturedInputs[1].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
}

TEST_F(Win32SendInputInjectorTest, ReplaceNotifiesSynthCounterByEventCount) {
    // D5: every Internal::TrackedSendInput call must report its event count
    // back to the registered callback so HookEngine's synthEventsPending_
    // stays balanced against the per-event decrements in
    // LowLevelKeyboardProc. Replace(2, "vi") = 2 BS down/up + 2 chars
    // down/up = 8 events delivered in a single batch — one positive delta.
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_TRUE(inj.Replace(2, L"vi"));
    ASSERT_EQ(synthCounterDeltas.size(), 1u);
    EXPECT_EQ(synthCounterDeltas[0], 8);
}

TEST_F(Win32SendInputInjectorTest, ReplaceWithBaitCountsBaitEventsToo) {
    // Bait prefix adds 1 char (down+up=2) and 1 extra BS (down+up=2) on
    // top of the caller's bsCount. The callback must reflect the actual
    // dispatched count so HookEngine doesn't undercount and release the
    // synth-guard early.
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/true);
    EXPECT_TRUE(inj.Replace(1, L"x"));
    // Expected: 8 events — bait char (2) + 2 BS×(down+up)=4 + 1 char×(down+up)=2.
    ASSERT_EQ(synthCounterDeltas.size(), 1u);
    EXPECT_EQ(synthCounterDeltas[0], 8);
}

TEST_F(Win32SendInputInjectorTest, PartialSendEmitsCompensatingNegativeDelta) {
    // Renderer-drop simulation: SendInput returns 2 of 6. Callback fires
    // twice — first +6 (pre-dispatch), then -4 (recovery so the counter
    // ends up at +2, matching the 2 events that will round-trip through
    // the LL hook decrement path).
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    sendInputReturnOverride = 2;  // 6 expected, only 2 delivered
    EXPECT_FALSE(inj.Replace(3, L""));
    ASSERT_EQ(synthCounterDeltas.size(), 2u);
    EXPECT_EQ(synthCounterDeltas[0], 6);
    EXPECT_EQ(synthCounterDeltas[1], -4);
}

// ── Game-compat re-inject atomicity ──────────────────────────────────
// HandleAlphaKey pre-adds the raw char to previousComposition_ before
// calling here, so bsCount already assumes the re-injected char is on
// screen. Splitting the VK and the backspaces across two SendInput calls
// lets a host land one and drop the other, leaving the app with the raw
// char while the engine believes it was replaced ("khoong" for "không").

TEST_F(Win32SendInputInjectorTest, ReinjectVkRidesTheSameSendInputBatch) {
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_TRUE(inj.Replace(2, L"ô", /*reinjectVk=*/'O'));

    // 1 re-inject down + 2 BS × (down+up) + 1 char × (down+up) = 7 events.
    ASSERT_EQ(capturedInputs.size(), 7u);
    // Exactly ONE dispatch. Two entries here would mean the re-inject went
    // out separately again — the regression this test exists to catch.
    ASSERT_EQ(synthCounterDeltas.size(), 1u);
    EXPECT_EQ(synthCounterDeltas[0], 7);

    EXPECT_EQ(capturedInputs[0].ki.wVk, 'O');
    EXPECT_EQ(capturedInputs[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_EQ(capturedInputs[1].ki.wVk, VK_BACK);
    EXPECT_NE(capturedInputs[5].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[5].ki.wScan, L'ô');
}

TEST_F(Win32SendInputInjectorTest, ReinjectVkEmitsNoKeyUp) {
    // Key-down only: a sustained hold keeps feeding downs to the game, and
    // the physical key-up passes through the hook when the user releases.
    Win32SendInputInjector inj(/*needsBaitCharPrefix=*/false);
    EXPECT_TRUE(inj.Replace(1, L"â", /*reinjectVk=*/'A'));
    for (const auto& ev : capturedInputs) {
        if (ev.ki.wVk == 'A') {
            EXPECT_EQ(ev.ki.dwFlags & KEYEVENTF_KEYUP, 0u)
                << "re-inject must never emit a key-up";
        }
    }
}

TEST_F(Win32SendInputInjectorTest, ReinjectVkKeepsHeldShiftRestoreIndexHonest) {
    // Only the modifier release landed; compensate even though the re-inject
    // that follows it was not delivered.
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_LSHIFT ? static_cast<SHORT>(0x8000) : SHORT{0};
    };
    sendInputReturnOverride = 1;

    Win32SendInputInjector inj(false);
    EXPECT_FALSE(inj.Replace(2, L"ô", 'O'));
    ASSERT_EQ(capturedInputs.size(), 10u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_LSHIFT);
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_EQ(capturedInputs[1].ki.wVk, 'O');
    EXPECT_EQ(capturedInputs.back().ki.wVk, VK_LSHIFT);
    EXPECT_EQ(capturedInputs.back().ki.dwFlags & KEYEVENTF_KEYUP, 0u);
}

TEST_F(Win32SendInputInjectorTest, TextOnlyReleasesRightAltAndRestoresIt) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_RMENU ? static_cast<SHORT>(-32768) : 0;
    };
    Win32SendInputInjector inj(false);
    ASSERT_TRUE(inj.Replace(0, L"x"));
    ASSERT_EQ(capturedInputs.size(), 4u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_RMENU);
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_EXTENDEDKEY, 0u);
    EXPECT_EQ(capturedInputs[3].ki.wVk, VK_RMENU);
    EXPECT_EQ(capturedInputs[3].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_NE(capturedInputs[3].ki.dwFlags & KEYEVENTF_EXTENDEDKEY, 0u);
}

TEST_F(Win32SendInputInjectorTest, PartialSendRestoresOnlyModifiersActuallyReleased) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_LCONTROL || vk == VK_RCONTROL ? static_cast<SHORT>(-32768) : 0;
    };
    sendInputReturnOverride = 1;
    Win32SendInputInjector inj(false);
    EXPECT_FALSE(inj.Replace(1, L"x"));
    ASSERT_EQ(capturedInputs.size(), 9u);
    EXPECT_EQ(capturedInputs.back().ki.wVk, VK_LCONTROL);
    EXPECT_EQ(capturedInputs.back().ki.dwFlags & KEYEVENTF_KEYUP, 0u);
}

TEST_F(Win32SendInputInjectorTest, ControlIsReleasedBeforeGameReinject) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == VK_LCONTROL ? static_cast<SHORT>(-32768) : 0;
    };
    Win32SendInputInjector inj(false);
    ASSERT_TRUE(inj.Replace(1, L"a", 'A'));
    ASSERT_GE(capturedInputs.size(), 3u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_LCONTROL);
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_EQ(capturedInputs[1].ki.wVk, 'A');
    EXPECT_EQ(capturedInputs[2].ki.wVk, VK_BACK);
}

}  // namespace NextKey::Output::Test
