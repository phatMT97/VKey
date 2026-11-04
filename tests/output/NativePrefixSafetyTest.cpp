// VKey - native suggestion prefix safety across both SendInput channels
// SPDX-License-Identifier: GPL-3.0-only
#include "InjectorTestBase.h"
#include "app/output/SplitDispatchInjector.h"
#include "app/output/Win32SendInputInjector.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <numeric>

namespace NextKey::Output::Test {

class NativePrefixSafetyTest : public InjectorTestBase,
                              public ::testing::WithParamInterface<bool> {
protected:
    std::unique_ptr<IOutputInjector> MakeInjector() {
        if (GetParam()) {
            return std::make_unique<SplitDispatchInjector>(
                6, SuggestionDismissalPolicy::NativePrintablePrefix);
        }
        return std::make_unique<Win32SendInputInjector>(
            SuggestionDismissalPolicy::NativePrintablePrefix);
    }

    static inline UINT firstSendCount = 0;
    static inline int translationLength = 0;
    static inline std::vector<std::vector<INPUT>> batches;
    static inline std::vector<UINT> deliveredCounts;

    void LimitFirstSend(UINT count) {
        firstSendCount = count;
        batches.clear();
        deliveredCounts.clear();
        Internal::g_sendInput = [](UINT size, LPINPUT events, int) -> UINT {
            batches.emplace_back(events, events + size);
            capturedInputs.insert(capturedInputs.end(), events, events + size);
            const auto sent = batches.size() == 1 ? (std::min)(size, firstSendCount) : size;
            deliveredCounts.push_back(sent);
            return sent;
        };
    }
};

TEST_P(NativePrefixSafetyTest, NativePrefixReleasesAllModifierSidesBeforeKeyDown) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        switch (vk) {
            case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
            case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
            case VK_MENU: case VK_LMENU: case VK_RMENU:
            case VK_LWIN: case VK_RWIN: return static_cast<SHORT>(-32768);
            default: return 0;
        }
    };
    auto injector = MakeInjector();
    auto& inj = *injector;
    inj.SetSuggestKeepChars(true);  // Excel must still emit its native prefix.
    ASSERT_TRUE(inj.Replace(1, L""));

    constexpr WORD modifiers[] = {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                                  VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN};
    ASSERT_EQ(capturedInputs.size(), 22u);  // 8 up + A pair + 2 BS pairs + 8 down
    for (std::size_t k = 0; k < 8; ++k) {
        EXPECT_EQ(capturedInputs[k].ki.wVk, modifiers[k]);
        EXPECT_NE(capturedInputs[k].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
        EXPECT_EQ(capturedInputs[14 + k].ki.wVk, modifiers[k]);
        EXPECT_EQ(capturedInputs[14 + k].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    }
    for (std::size_t k = 8; k < 14; ++k) {
        EXPECT_EQ(capturedInputs[k].ki.wVk, k < 10 ? kNativePrintablePrefixVk : VK_BACK);
        EXPECT_EQ(capturedInputs[k].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
        EXPECT_EQ(capturedInputs[k].ki.dwFlags & KEYEVENTF_KEYUP,
                  k % 2 == 0 ? 0u : KEYEVENTF_KEYUP);
    }
    for (const auto& input : capturedInputs) {
        EXPECT_EQ(input.ki.dwExtraInfo, Internal::kVKeyExtraInfo);
    }
}

TEST_P(NativePrefixSafetyTest, NativePrefixDeletesPrefixAndOldTextBeforeReplacement) {
    auto injector = MakeInjector();
    auto& inj = *injector;
    ASSERT_TRUE(inj.Replace(1, L"đ"));
    ASSERT_EQ(capturedInputs.size(), 8u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, kNativePrintablePrefixVk);
    EXPECT_EQ(capturedInputs[1].ki.wVk, kNativePrintablePrefixVk);
    EXPECT_EQ(capturedInputs[0].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_EQ(capturedInputs[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_NE(capturedInputs[1].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    for (std::size_t k = 2; k < 6; ++k) EXPECT_EQ(capturedInputs[k].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[6].ki.wScan, L'đ');
    EXPECT_NE(capturedInputs[6].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
}

TEST_P(NativePrefixSafetyTest, NativePrefixNeverFiresWithoutBackspaces) {
    auto injector = MakeInjector();
    auto& inj = *injector;
    ASSERT_TRUE(inj.Replace(0, L""));
    EXPECT_TRUE(capturedInputs.empty());
    ASSERT_TRUE(inj.Replace(0, L"đ"));
    ASSERT_EQ(capturedInputs.size(), 2u);
    EXPECT_EQ(capturedInputs[0].ki.wScan, L'đ');
    EXPECT_NE(capturedInputs[0].ki.dwFlags & KEYEVENTF_UNICODE, 0u);
    EXPECT_TRUE(sleepDelays.empty());
}

TEST_P(NativePrefixSafetyTest, PartialNativePrefixBatchRestoresReleasedControl) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return (vk == VK_CONTROL || vk == VK_LCONTROL) ? static_cast<SHORT>(-32768) : 0;
    };
    sendInputReturnOverride = 1;  // only Ctrl-up delivered, then best-effort restore
    auto injector = MakeInjector();
    auto& inj = *injector;
    EXPECT_FALSE(inj.Replace(1, L""));
    ASSERT_EQ(capturedInputs.size(), 9u);  // 8-event batch + Ctrl-down recovery
    EXPECT_EQ(capturedInputs.back().ki.wVk, VK_LCONTROL);
    EXPECT_EQ(capturedInputs.back().ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_TRUE(sleepDelays.empty());
}

TEST_P(NativePrefixSafetyTest, PartialPrefixDownIsReleasedWithoutModifiers) {
    LimitFirstSend(1);
    auto injector = MakeInjector();
    EXPECT_FALSE(injector->Replace(1, L"đ"));
    ASSERT_EQ(batches.size(), 2u);
    ASSERT_EQ(batches.back().size(), 1u);
    EXPECT_EQ(batches.back()[0].ki.wVk, 'A');
    EXPECT_NE(batches.back()[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_EQ(std::accumulate(synthCounterDeltas.begin(), synthCounterDeltas.end(), 0), 2);
    EXPECT_TRUE(sleepDelays.empty());
}

TEST_P(NativePrefixSafetyTest, PartialPrefixIsReleasedBeforeRestoringModifiers) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL)
            ? static_cast<SHORT>(-32768) : 0;
    };
    LimitFirstSend(3);  // Shift-up, Ctrl-up, A-down; no A-up.
    auto injector = MakeInjector();
    EXPECT_FALSE(injector->Replace(1, L"đ"));
    ASSERT_EQ(batches.size(), 2u);
    ASSERT_EQ(batches.back().size(), 3u);
    EXPECT_EQ(batches.back()[0].ki.wVk, 'A');
    EXPECT_NE(batches.back()[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_EQ(batches.back()[1].ki.wVk, VK_LSHIFT);
    EXPECT_EQ(batches.back()[2].ki.wVk, VK_LCONTROL);
    EXPECT_EQ(batches.back()[1].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_EQ(batches.back()[2].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_TRUE(sleepDelays.empty());
}

TEST_P(NativePrefixSafetyTest, HeldAUsesAnotherPrefixWithoutReleasingA) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return vk == 'A' ? static_cast<SHORT>(-32768) : 0;
    };
    auto injector = MakeInjector();
    ASSERT_TRUE(injector->Replace(1, L"đ"));
    ASSERT_EQ(capturedInputs.size(), 8u);
    EXPECT_EQ(capturedInputs.front().ki.wVk, 'B');
    for (const auto& event : capturedInputs) EXPECT_NE(event.ki.wVk, 'A');
}

TEST_P(NativePrefixSafetyTest, HeldAAndBUseC) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return (vk == 'A' || vk == 'B') ? static_cast<SHORT>(-32768) : 0;
    };
    auto injector = MakeInjector();
    ASSERT_TRUE(injector->Replace(1, L"đ"));
    ASSERT_EQ(capturedInputs.size(), 8u);
    EXPECT_EQ(capturedInputs.front().ki.wVk, 'C');
    for (const auto& event : capturedInputs) {
        EXPECT_NE(event.ki.wVk, 'A');
        EXPECT_NE(event.ki.wVk, 'B');
    }
}

TEST_P(NativePrefixSafetyTest, NoAvailablePrefixDoesNotAddCompensatingDelete) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return (vk >= 'A' && vk <= 'C') ? static_cast<SHORT>(-32768) : 0;
    };
    auto injector = MakeInjector();
    ASSERT_TRUE(injector->Replace(1, L"đ"));
    ASSERT_EQ(capturedInputs.size(), 4u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, VK_BACK);
    EXPECT_EQ(capturedInputs[2].ki.wScan, L'đ');
}

TEST_P(NativePrefixSafetyTest, PrefixAndModifiersRecoverAtEveryFirstBatchBoundary) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL)
            ? static_cast<SHORT>(-32768) : 0;
    };
    // Split's first batch has 10 events; Win32 also includes two Unicode events.
    const UINT firstBatchSize = GetParam() ? 10u : 12u;
    for (UINT limit = 0; limit < firstBatchSize; ++limit) {
        SCOPED_TRACE(limit);
        capturedInputs.clear();
        synthCounterDeltas.clear();
        sleepDelays.clear();
        LimitFirstSend(limit);
        auto injector = MakeInjector();
        EXPECT_FALSE(injector->Replace(1, L"đ"));
        bool aDown = false, backspaceDown = false, packetDown = false;
        bool shiftDown = true, ctrlDown = true;
        for (std::size_t batch = 0; batch < batches.size(); ++batch) {
            for (UINT index = 0; index < deliveredCounts[batch]; ++index) {
                const auto& key = batches[batch][index].ki;
                const bool down = (key.dwFlags & KEYEVENTF_KEYUP) == 0;
                if (key.wVk == 'A') aDown = down;
                if (key.wVk == VK_BACK) backspaceDown = down;
                if ((key.dwFlags & KEYEVENTF_UNICODE) != 0) packetDown = down;
                if (key.wVk == VK_LSHIFT) shiftDown = down;
                if (key.wVk == VK_LCONTROL) ctrlDown = down;
                EXPECT_EQ(key.dwExtraInfo, Internal::kVKeyExtraInfo);
            }
        }
        EXPECT_FALSE(aDown);
        EXPECT_FALSE(backspaceDown);
        EXPECT_FALSE(packetDown);
        EXPECT_TRUE(shiftDown);
        EXPECT_TRUE(ctrlDown);
        EXPECT_EQ(std::accumulate(synthCounterDeltas.begin(), synthCounterDeltas.end(), 0),
                  static_cast<int>(std::accumulate(deliveredCounts.begin(), deliveredCounts.end(), 0u)));
        EXPECT_TRUE(sleepDelays.empty());
    }
}

TEST_P(NativePrefixSafetyTest, CapacityFailureSendsNothingIncludingWrappedCounts) {
    for (auto count : {std::size_t{127}, (std::numeric_limits<std::size_t>::max)()}) {
        SCOPED_TRACE(count);
        capturedInputs.clear();
        auto injector = MakeInjector();
        EXPECT_FALSE(injector->Replace(count, L""));
        EXPECT_TRUE(capturedInputs.empty());
    }
}

TEST_P(NativePrefixSafetyTest, ExactCapacityStillDispatchesCompletePrefixAndDeletes) {
    auto injector = MakeInjector();
    ASSERT_TRUE(injector->Replace(126, L""));
    ASSERT_EQ(capturedInputs.size(), 256u);
    EXPECT_EQ(capturedInputs[0].ki.wVk, 'A');
    EXPECT_EQ(capturedInputs[254].ki.wVk, VK_BACK);
    EXPECT_NE(capturedInputs[255].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
}

TEST_P(NativePrefixSafetyTest, InFlightAIsAvoidedBeforeAsyncStateUpdates) {
    auto injector = MakeInjector();
    {
        const Internal::CurrentKeyDownScope outer{'A'};
        ASSERT_TRUE(injector->Replace(1, L"đ"));
        EXPECT_EQ(capturedInputs.front().ki.wVk, 'B');
        capturedInputs.clear();
        {
            const Internal::CurrentKeyDownScope nested{'B'};
            ASSERT_TRUE(injector->Replace(1, L"đ"));
            EXPECT_EQ(capturedInputs.front().ki.wVk, 'C');
        }
        capturedInputs.clear();
        ASSERT_TRUE(injector->Replace(1, L"đ"));
        EXPECT_EQ(capturedInputs.front().ki.wVk, 'B');
    }
    capturedInputs.clear();
    ASSERT_TRUE(injector->Replace(1, L"đ"));
    EXPECT_EQ(capturedInputs.front().ki.wVk, 'A');
}

TEST_P(NativePrefixSafetyTest, RepeatedKeyScopesDoNotReleaseTheOuterContext) {
    {
        const Internal::CurrentKeyDownScope outer{'A'};
        {
            const Internal::CurrentKeyDownScope repeated{'A'};
            EXPECT_TRUE(Internal::IsInFlightKeyDown('A'));
        }
        EXPECT_TRUE(Internal::IsInFlightKeyDown('A'));
    }
    EXPECT_FALSE(Internal::IsInFlightKeyDown('A'));
}

TEST_P(NativePrefixSafetyTest, InvalidLayoutsDoNotAddADeletion) {
    Internal::g_nativePrefixLayout = []() noexcept -> HKL { return nullptr; };
    auto injector = MakeInjector();
    ASSERT_TRUE(injector->Replace(1, L"đ"));
    ASSERT_EQ(capturedInputs.size(), 4u);
    EXPECT_EQ(capturedInputs.front().ki.wVk, VK_BACK);
}

TEST_P(NativePrefixSafetyTest, CjkLayoutDoesNotTreatAnAsciiMappingAsProofOfInsertion) {
    Internal::g_nativePrefixLayout = []() noexcept -> HKL {
        return reinterpret_cast<HKL>(static_cast<ULONG_PTR>(0x04110411));
    };
    Internal::g_toUnicodeEx = [](UINT, UINT, const BYTE*, LPWSTR, int, UINT, HKL) -> int {
        ADD_FAILURE() << "CJK layout must not enter printable-probe translation";
        return 1;
    };
    auto injector = MakeInjector();
    ASSERT_TRUE(injector->Replace(1, L"đ"));
    ASSERT_EQ(capturedInputs.size(), 4u);
    EXPECT_EQ(capturedInputs.front().ki.wVk, VK_BACK);
}

TEST_P(NativePrefixSafetyTest, DeadUnmappedAndMultiUnitTranslationsDoNotAddADeletion) {
    for (int length : {-1, 0, 2}) {
        SCOPED_TRACE(length);
        // Exercise the actual selector; the API seam supplies host translation failures.
        translationLength = length;
        Internal::g_toUnicodeEx = [](UINT, UINT, const BYTE*, LPWSTR text, int, UINT flags, HKL) -> int {
            EXPECT_EQ(flags & 0x4u, 0x4u);
            text[0] = L'a';
            return translationLength;
        };
        capturedInputs.clear();
        auto injector = MakeInjector();
        ASSERT_TRUE(injector->Replace(1, L"đ"));
        ASSERT_EQ(capturedInputs.size(), 4u);
        EXPECT_EQ(capturedInputs.front().ki.wVk, VK_BACK);
    }
}

TEST_P(NativePrefixSafetyTest, CapsLockDependentMultiUnitAUsesB) {
    Internal::g_toUnicodeEx = [](UINT vk, UINT, const BYTE* state, LPWSTR text,
                                 int, UINT flags, HKL) -> int {
        EXPECT_EQ(flags & 0x4u, 0x4u);
        text[0] = static_cast<wchar_t>(vk);
        return vk == 'A' && (state[VK_CAPITAL] & 1) != 0 ? 2 : 1;
    };
    auto injector = MakeInjector();
    ASSERT_TRUE(injector->Replace(1, L"đ"));
    ASSERT_EQ(capturedInputs.size(), 8u);
    EXPECT_EQ(capturedInputs.front().ki.wVk, 'B');
}

TEST_P(NativePrefixSafetyTest, ZeroDeleteAndUnicodePolicyNeverQueryNativeLayout) {
    Internal::g_nativePrefixLayout = []() noexcept -> HKL {
        ADD_FAILURE() << "Native prefix environment queried for an unrelated operation";
        return nullptr;
    };
    auto injector = MakeInjector();
    ASSERT_TRUE(injector->Replace(0, L""));
    EXPECT_TRUE(capturedInputs.empty());
    ASSERT_TRUE(injector->Replace(0, L"đ"));
    EXPECT_EQ(capturedInputs.size(), 2u);
    const auto plan = Internal::PrepareSuggestionDismissal(
        SuggestionDismissalPolicy::UnicodeBait, 1, L"đ", false);
    EXPECT_EQ(plan.prefixChar, kUnicodeBaitChar);
    EXPECT_EQ(plan.effectiveBsCount, 2u);
}

TEST_P(NativePrefixSafetyTest, PartialGameReinjectRetainsPhysicalReleaseOwnership) {
    for (UINT delivered : {1u, 2u}) {
        SCOPED_TRACE(delivered);
        LimitFirstSend(delivered);
        capturedInputs.clear();
        auto injector = MakeInjector();
        EXPECT_FALSE(injector->Replace(1, L"đ", 'E'));
        EXPECT_EQ(batches.size(), delivered == 1 ? 1u : 2u);
        for (const auto& event : capturedInputs) {
            if (event.ki.wVk == 'E') {
                EXPECT_EQ(event.ki.dwFlags & KEYEVENTF_KEYUP, 0u);
            }
        }
        if (delivered == 2) {
            ASSERT_EQ(batches.back().size(), 1u);
            EXPECT_EQ(batches.back()[0].ki.wVk, 'A');
            EXPECT_NE(batches.back()[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
        }
    }
}

TEST_P(NativePrefixSafetyTest, PartialUnicodePairIsReleasedBeforeRestoringControl) {
    Internal::g_getAsyncKeyState = [](int vk) -> SHORT {
        return (vk == VK_CONTROL || vk == VK_LCONTROL) ? static_cast<SHORT>(-32768) : 0;
    };
    LimitFirstSend(2);  // Ctrl-up and Unicode-down, without the matching Unicode-up.
    auto injector = MakeInjector();
    EXPECT_FALSE(injector->Replace(0, L"đ"));
    ASSERT_EQ(batches.size(), 2u);
    ASSERT_EQ(batches.back().size(), 2u);
    const auto& cleanup = batches.back()[0].ki;
    EXPECT_EQ(cleanup.wScan, L'đ');
    EXPECT_EQ(cleanup.dwFlags, DWORD{KEYEVENTF_UNICODE | KEYEVENTF_KEYUP});
    EXPECT_EQ(batches.back()[1].ki.wVk, VK_LCONTROL);
    EXPECT_EQ(batches.back()[1].ki.dwFlags & KEYEVENTF_KEYUP, 0u);
    EXPECT_EQ(std::accumulate(synthCounterDeltas.begin(), synthCounterDeltas.end(), 0), 4);
    EXPECT_TRUE(sleepDelays.empty());
}

INSTANTIATE_TEST_SUITE_P(Channels, NativePrefixSafetyTest, ::testing::Bool());

}  // namespace NextKey::Output::Test
