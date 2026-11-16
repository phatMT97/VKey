// VKey - Passive HotkeyManager tests
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "system/HotkeyManager.h"

#include <vector>

namespace NextKey {
namespace {

constexpr std::uint32_t kVkA = 0x41;
constexpr std::uint32_t kVkJ = 0x4A;
constexpr std::uint32_t kVkK = 0x4B;
constexpr std::uint32_t kVkLControl = 0xA2;
constexpr std::uint32_t kVkRControl = 0xA3;
constexpr std::uint32_t kVkLShift = 0xA0;
constexpr std::uint32_t kVkRShift = 0xA1;
constexpr std::uint32_t kVkLMenu = 0xA4;
constexpr std::uint32_t kVkRMenu = 0xA5;
constexpr std::uint32_t kVkLWin = 0x5B;
constexpr std::uint32_t kVkRWin = 0x5C;

struct DeferredFires {
    std::vector<HotkeyManager::SlotId> slots;

    static void Push(void* context, HotkeyManager::SlotId slot) noexcept {
        static_cast<DeferredFires*>(context)->slots.push_back(slot);
    }
};

struct PassThroughState {
    bool value = false;

    static bool Read(const void* context) noexcept {
        return static_cast<const PassThroughState*>(context)->value;
    }
};

struct PhysicalKeyState {
    std::uint32_t vk = 0;
    bool down = false;

    static bool IsDown(const void* context, std::uint32_t candidate) noexcept {
        const auto& self = *static_cast<const PhysicalKeyState*>(context);
        return candidate == self.vk && self.down;
    }
};

HotkeyManager::KeyEvent Down(std::uint32_t vk,
                              HotkeyManager::ModifierKey modifier = HotkeyManager::ModifierKey::None) {
    return {vk, HotkeyManager::KeyEventType::Down, modifier};
}

HotkeyManager::KeyEvent Up(std::uint32_t vk,
                            HotkeyManager::ModifierKey modifier = HotkeyManager::ModifierKey::None) {
    return {vk, HotkeyManager::KeyEventType::Up, modifier};
}

HotkeyManager MakeManager(DeferredFires& fires) {
    return HotkeyManager(&DeferredFires::Push, &fires);
}

HotkeyConfig ModifierCombo(HotkeyManager::ModifierKey modifier) {
    HotkeyConfig config{.vk = kVkJ};
    switch (modifier) {
    case HotkeyManager::ModifierKey::Control: config.ctrl = true; break;
    case HotkeyManager::ModifierKey::Shift: config.shift = true; break;
    case HotkeyManager::ModifierKey::Alt: config.alt = true; break;
    case HotkeyManager::ModifierKey::Win: config.win = true; break;
    case HotkeyManager::ModifierKey::None: break;
    }
    return config;
}

TEST(HotkeyManagerTest, StrictComboQueuesAndConsumesPairedEvents) {
    DeferredFires fires;
    int callbacks = 0;
    auto manager = MakeManager(fires);
    const auto slot = manager.AddHotkey({.ctrl = true, .vk = kVkJ}, [&] { ++callbacks; });
    manager.FinalizeBindings();

    EXPECT_FALSE(manager.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control)).consume);
    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_EQ(fires.slots, std::vector<HotkeyManager::SlotId>{slot});
    EXPECT_EQ(callbacks, 0);
    EXPECT_TRUE(manager.Match(Up(kVkJ)).consume);
    EXPECT_FALSE(manager.Match(Up(kVkLControl, HotkeyManager::ModifierKey::Control)).consume);

    manager.Dispatch(slot);
    EXPECT_EQ(callbacks, 1);
}

TEST(HotkeyManagerTest, AutoRepeatIsConsumedWithoutAnotherDeferredFire) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    manager.AddHotkey({.vk = kVkJ}, [] {});
    manager.FinalizeBindings();

    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_EQ(fires.slots.size(), 1u);
    EXPECT_TRUE(manager.Match(Up(kVkJ)).consume);
}

TEST(HotkeyManagerTest, PassThroughPassesPairedEventsAndSuppressesRepeat) {
    DeferredFires fires;
    PassThroughState passThrough{true};
    auto manager = MakeManager(fires);
    manager.AddHotkey({.vk = kVkJ}, [] {}, {&PassThroughState::Read, &passThrough});
    manager.FinalizeBindings();

    EXPECT_FALSE(manager.Match(Down(kVkJ)).consume);
    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_FALSE(manager.Match(Up(kVkJ)).consume);
    EXPECT_TRUE(fires.slots.empty());
}

TEST(HotkeyManagerTest, ExtraModifiersPreventComboMatch) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    manager.AddHotkey({.ctrl = true, .vk = kVkJ}, [] {});
    manager.FinalizeBindings();

    manager.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control));
    manager.Match(Down(kVkLShift, HotkeyManager::ModifierKey::Shift));
    EXPECT_FALSE(manager.Match(Down(kVkJ)).consume);
    EXPECT_TRUE(fires.slots.empty());
}

TEST(HotkeyManagerTest, ModifierOnlyBindingFiresOnCleanReleaseButNotAfterAnotherKey) {
    DeferredFires cleanFires;
    int cleanCallbacks = 0;
    auto clean = MakeManager(cleanFires);
    const auto cleanSlot = clean.AddHotkey({.ctrl = true}, [&] { ++cleanCallbacks; });
    clean.FinalizeBindings();

    clean.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control));
    EXPECT_FALSE(clean.Match(Up(kVkLControl, HotkeyManager::ModifierKey::Control)).consume);
    ASSERT_EQ(cleanFires.slots, std::vector<HotkeyManager::SlotId>{cleanSlot});
    EXPECT_EQ(cleanCallbacks, 0);
    clean.Dispatch(cleanSlot);
    EXPECT_EQ(cleanCallbacks, 1);

    DeferredFires contaminatedFires;
    auto contaminated = MakeManager(contaminatedFires);
    contaminated.AddHotkey({.ctrl = true}, [] {});
    contaminated.FinalizeBindings();
    contaminated.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control));
    contaminated.Match(Down(kVkA));
    contaminated.Match(Up(kVkA));
    contaminated.Match(Up(kVkLControl, HotkeyManager::ModifierKey::Control));
    EXPECT_TRUE(contaminatedFires.slots.empty());
}

TEST(HotkeyManagerTest, ExternalChordContaminationPreventsModifierOnlyTrigger) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    manager.AddHotkey({.ctrl = true, .shift = true}, [] {});
    manager.FinalizeBindings();

    manager.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control));
    manager.Match(Down(kVkLShift, HotkeyManager::ModifierKey::Shift));

    // Mouse click or external interaction occurs while modifiers held
    manager.ContaminateChord();

    manager.Match(Up(kVkLShift, HotkeyManager::ModifierKey::Shift));
    manager.Match(Up(kVkLControl, HotkeyManager::ModifierKey::Control));

    EXPECT_TRUE(fires.slots.empty());
}

TEST(HotkeyManagerTest, ExternalChordContaminationResetsAfterModifiersReleased) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    const auto slot = manager.AddHotkey({.ctrl = true, .shift = true}, [] {});
    manager.FinalizeBindings();

    // 1. Contaminated chord does not fire
    manager.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control));
    manager.Match(Down(kVkLShift, HotkeyManager::ModifierKey::Shift));
    manager.ContaminateChord();
    manager.Match(Up(kVkLShift, HotkeyManager::ModifierKey::Shift));
    manager.Match(Up(kVkLControl, HotkeyManager::ModifierKey::Control));
    EXPECT_TRUE(fires.slots.empty());

    // 2. Subsequent clean chord fires normally
    manager.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control));
    manager.Match(Down(kVkLShift, HotkeyManager::ModifierKey::Shift));
    manager.Match(Up(kVkLShift, HotkeyManager::ModifierKey::Shift));
    manager.Match(Up(kVkLControl, HotkeyManager::ModifierKey::Control));
    EXPECT_EQ(fires.slots, std::vector<HotkeyManager::SlotId>{slot});
}

TEST(HotkeyManagerTest, FocusReconcilePreservesHeldModifierChordContamination) {
    constexpr std::uint32_t kVkTab = 0x09;

    DeferredFires fires;
    auto manager = MakeManager(fires);
    manager.AddHotkey({.alt = true}, [] {});
    manager.FinalizeBindings();

    manager.Match(Down(kVkLMenu, HotkeyManager::ModifierKey::Alt));
    manager.Match(Down(kVkTab));

    // Focus may change after Alt+Tab while Alt is still physically held.
    // Reconciliation must not turn that contaminated chord into a clean
    // modifier-alone gesture before the eventual Alt-up arrives.
    manager.ReconcileModifiers({.alt = true});
    manager.Match(Up(kVkTab));
    manager.Match(Up(kVkLMenu, HotkeyManager::ModifierKey::Alt));

    EXPECT_TRUE(fires.slots.empty());
}

TEST(HotkeyManagerTest, FocusReconcileContaminatesOtherwiseCleanHeldModifier) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    manager.AddHotkey({.win = true}, [] {});
    manager.FinalizeBindings();

    manager.Match(Down(kVkLWin, HotkeyManager::ModifierKey::Win));

    // Changing foreground is itself a gesture boundary. A Win release in the
    // new app must not fire a Win-alone VKey binding even when no ordinary key
    // event was observed before the focus transition.
    manager.ReconcileModifiers({.win = true});
    manager.Match(Up(kVkLWin, HotkeyManager::ModifierKey::Win));

    EXPECT_TRUE(fires.slots.empty());
}

TEST(HotkeyManagerTest, FocusReconcileRestoresPhysicallyHeldModifierSnapshot) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    const auto slot = manager.AddHotkey({.ctrl = true, .vk = kVkJ}, [] {});
    manager.FinalizeBindings();

    // The LL callback's pre-update async snapshot can temporarily erase the
    // just-observed modifier during an in-callback focus drain. A later
    // pump-boundary snapshot must restore the physical hold before the next
    // ordinary key is matched.
    manager.ReconcileModifiers({.ctrl = true});

    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_EQ(fires.slots, std::vector<HotkeyManager::SlotId>{slot});
    EXPECT_TRUE(manager.Match(Up(kVkJ)).consume);
}

TEST(HotkeyManagerTest, HookReplacementClearsLatchWhoseKeyUpWasMissed) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    const auto slot = manager.AddHotkey({.vk = kVkJ}, [] {});
    manager.FinalizeBindings();

    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    ASSERT_EQ(fires.slots, std::vector<HotkeyManager::SlotId>{slot});

    const PhysicalKeyState physical{.vk = kVkJ, .down = false};
    manager.ReconcileLatchedKeysAfterHookReplacement(
        &PhysicalKeyState::IsDown, &physical);

    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_EQ(fires.slots,
              (std::vector<HotkeyManager::SlotId>{slot, slot}));
}

TEST(HotkeyManagerTest, HookReplacementRetainsLatchWhileKeyIsPhysicallyHeld) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    const auto slot = manager.AddHotkey({.vk = kVkJ}, [] {});
    manager.FinalizeBindings();

    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    ASSERT_EQ(fires.slots, std::vector<HotkeyManager::SlotId>{slot});

    const PhysicalKeyState physical{.vk = kVkJ, .down = true};
    manager.ReconcileLatchedKeysAfterHookReplacement(
        &PhysicalKeyState::IsDown, &physical);

    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_EQ(fires.slots, std::vector<HotkeyManager::SlotId>{slot});
    EXPECT_TRUE(manager.Match(Up(kVkJ)).consume);
    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_EQ(fires.slots,
              (std::vector<HotkeyManager::SlotId>{slot, slot}));
}

TEST(HotkeyManagerTest, RebindPublishesNewComboWhilePreservingInflightLatch) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    const auto slot = manager.AddHotkey({.ctrl = true, .vk = kVkJ}, [] {});
    manager.FinalizeBindings();

    manager.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control));
    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    manager.UpdateHotkey(slot, {.ctrl = true, .vk = kVkK});
    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_TRUE(manager.Match(Up(kVkJ)).consume);
    EXPECT_TRUE(manager.Match(Down(kVkK)).consume);
    EXPECT_EQ(fires.slots, (std::vector<HotkeyManager::SlotId>{slot, slot}));
}

TEST(HotkeyManagerTest, ReboundKeyPassesUntilInflightOldKeyIsReleased) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    const auto slot = manager.AddHotkey({.ctrl = true, .vk = kVkJ}, [] {});
    manager.FinalizeBindings();

    manager.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control));
    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    manager.UpdateHotkey(slot, {.ctrl = true, .vk = kVkK});

    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_FALSE(manager.Match(Down(kVkK)).consume);
    EXPECT_FALSE(manager.Match(Up(kVkK)).consume);
    EXPECT_EQ(fires.slots, std::vector<HotkeyManager::SlotId>{slot});

    EXPECT_TRUE(manager.Match(Up(kVkJ)).consume);
    EXPECT_TRUE(manager.Match(Down(kVkK)).consume);
    EXPECT_TRUE(manager.Match(Up(kVkK)).consume);
    EXPECT_EQ(fires.slots, (std::vector<HotkeyManager::SlotId>{slot, slot}));
}

TEST(HotkeyManagerTest, InflightReboundSlotDoesNotBlockAnotherSlot) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    const auto reboundSlot = manager.AddHotkey({.ctrl = true, .vk = kVkJ}, [] {});
    const auto otherSlot = manager.AddHotkey({.ctrl = true, .vk = kVkK}, [] {});
    manager.FinalizeBindings();

    manager.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control));
    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    manager.UpdateHotkey(reboundSlot, {.ctrl = true, .vk = kVkK});

    EXPECT_TRUE(manager.Match(Down(kVkK)).consume);
    EXPECT_EQ(fires.slots,
              (std::vector<HotkeyManager::SlotId>{reboundSlot, otherSlot}));
    EXPECT_TRUE(manager.Match(Up(kVkK)).consume);
    EXPECT_TRUE(manager.Match(Up(kVkJ)).consume);
}

TEST(HotkeyManagerTest, InflightSlotIgnoresReboundModifierOnlyConfig) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    const auto reboundSlot = manager.AddHotkey({.vk = kVkJ}, [] {});
    const auto otherSlot = manager.AddHotkey({.alt = true}, [] {});
    manager.FinalizeBindings();

    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    manager.UpdateHotkey(reboundSlot, {.alt = true});

    manager.Match(Down(kVkLMenu, HotkeyManager::ModifierKey::Alt));
    manager.Match(Up(kVkLMenu, HotkeyManager::ModifierKey::Alt));
    EXPECT_EQ(fires.slots,
              (std::vector<HotkeyManager::SlotId>{reboundSlot, otherSlot}));
    EXPECT_TRUE(manager.Match(Up(kVkJ)).consume);
}

TEST(HotkeyManagerTest, CallbacksRunOnlyWhenExplicitlyDispatched) {
    DeferredFires fires;
    int callbacks = 0;
    auto manager = MakeManager(fires);
    const auto slot = manager.AddHotkey({.vk = kVkJ}, [&] { ++callbacks; });
    manager.FinalizeBindings();

    manager.Match(Down(kVkJ));
    EXPECT_EQ(callbacks, 0);
    manager.Dispatch(slot);
    EXPECT_EQ(callbacks, 1);
}

TEST(HotkeyManagerTest, DeferredSinkCanBeWiredBeforeBindingsAreFinalized) {
    DeferredFires fires;
    HotkeyManager manager;
    const auto slot = manager.AddHotkey({.vk = kVkJ}, [] {});

    manager.SetDeferredFireSink(&DeferredFires::Push, &fires);
    manager.FinalizeBindings();

    EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    EXPECT_EQ(fires.slots, std::vector<HotkeyManager::SlotId>{slot});
}

TEST(HotkeyManagerTest, MatchedAltOrWinCombosRequestSystemMenuCancellation) {
    DeferredFires fires;
    auto manager = MakeManager(fires);
    manager.AddHotkey({.alt = true, .vk = kVkJ}, [] {});
    manager.AddHotkey({.ctrl = true, .vk = kVkK}, [] {});
    manager.AddHotkey({.win = true, .vk = kVkA}, [] {});
    manager.FinalizeBindings();

    manager.Match(Down(kVkLMenu, HotkeyManager::ModifierKey::Alt));
    EXPECT_TRUE(manager.Match(Down(kVkJ)).cancelSystemMenu);
    manager.Match(Up(kVkJ));
    manager.Match(Up(kVkLMenu, HotkeyManager::ModifierKey::Alt));
    manager.Match(Down(kVkLControl, HotkeyManager::ModifierKey::Control));
    EXPECT_FALSE(manager.Match(Down(kVkK)).cancelSystemMenu);
    manager.Match(Up(kVkK));
    manager.Match(Up(kVkLControl, HotkeyManager::ModifierKey::Control));
    manager.Match(Down(kVkLWin, HotkeyManager::ModifierKey::Win));
    EXPECT_TRUE(manager.Match(Down(kVkA)).cancelSystemMenu);
}

struct ModifierPair {
    std::uint32_t leftVk;
    std::uint32_t rightVk;
    HotkeyManager::ModifierKey modifier;
};

TEST(HotkeyManagerTest, NormalizedLeftAndRightModifiersMatchIdentically) {
    constexpr ModifierPair pairs[] = {
        ModifierPair{kVkLControl, kVkRControl, HotkeyManager::ModifierKey::Control},
        ModifierPair{kVkLShift, kVkRShift, HotkeyManager::ModifierKey::Shift},
        ModifierPair{kVkLMenu, kVkRMenu, HotkeyManager::ModifierKey::Alt},
        ModifierPair{kVkLWin, kVkRWin, HotkeyManager::ModifierKey::Win},
    };

    for (const ModifierPair pair : pairs) {
        SCOPED_TRACE(static_cast<int>(pair.modifier));
        DeferredFires fires;
        auto manager = MakeManager(fires);
        manager.AddHotkey(ModifierCombo(pair.modifier), [] {});
        manager.FinalizeBindings();

        EXPECT_EQ(HotkeyManager::NormalizeModifier(pair.leftVk), pair.modifier);
        EXPECT_EQ(HotkeyManager::NormalizeModifier(pair.rightVk), pair.modifier);
        manager.Match(Down(pair.leftVk, HotkeyManager::NormalizeModifier(pair.leftVk)));
        EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
        manager.Match(Up(kVkJ));
        manager.Match(Up(pair.leftVk, HotkeyManager::NormalizeModifier(pair.leftVk)));
        manager.Match(Down(pair.rightVk, HotkeyManager::NormalizeModifier(pair.rightVk)));
        EXPECT_TRUE(manager.Match(Down(kVkJ)).consume);
    }
}

}  // namespace
}  // namespace NextKey
