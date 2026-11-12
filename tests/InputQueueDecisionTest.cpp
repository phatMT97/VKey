// Tests for DecideInputQueueActionDuringSend (InputQueueDecision.h)
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "core/InputQueueDecision.h"

using NextKey::DecideInputQueueActionDuringSend;
using NextKey::InputQueueAction;

namespace {

constexpr unsigned int kVkS = 0x53;       // 'S' (in-flight key)
constexpr unsigned int kVkI = 0x49;       // 'I' (next typed letter)
constexpr unsigned int kVkBack = 0x08;    // VK_BACK
constexpr unsigned int kVkShift = 0x10;   // VK_SHIFT
constexpr unsigned int kVkW = 0x57;       // 'W' (WASD game key)

}  // namespace

TEST(InputQueueDecisionTest, NotSending_NormalProcess) {
    const auto action = DecideInputQueueActionDuringSend(
        /*isSending=*/false,
        /*eventVk=*/kVkI,
        /*sendingForVk=*/kVkS,
        /*hasMultiProcessRenderer=*/true,
        /*isGameMovement=*/false,
        /*isBareModifier=*/false,
        /*queueFull=*/false);
    EXPECT_EQ(action, InputQueueAction::NormalProcess);
}

TEST(InputQueueDecisionTest, Sending_SameVkRepeatOrUp_EatNoIntent) {
    const auto action = DecideInputQueueActionDuringSend(
        /*isSending=*/true,
        /*eventVk=*/kVkS,
        /*sendingForVk=*/kVkS,
        /*hasMultiProcessRenderer=*/true,
        /*isGameMovement=*/false,
        /*isBareModifier=*/false,
        /*queueFull=*/false);
    EXPECT_EQ(action, InputQueueAction::EatNoIntent);
}

TEST(InputQueueDecisionTest, Sending_DifferentVk_Enqueues) {
    // User types 'i' while 's' (or backspace) is still injecting -> must enqueue!
    const auto action = DecideInputQueueActionDuringSend(
        /*isSending=*/true,
        /*eventVk=*/kVkI,
        /*sendingForVk=*/kVkS,
        /*hasMultiProcessRenderer=*/true,
        /*isGameMovement=*/false,
        /*isBareModifier=*/false,
        /*queueFull=*/false);
    EXPECT_EQ(action, InputQueueAction::Enqueue);
}

TEST(InputQueueDecisionTest, Sending_Backspace_Enqueues) {
    // User presses backspace while injection is in-flight -> must enqueue!
    const auto action = DecideInputQueueActionDuringSend(
        /*isSending=*/true,
        /*eventVk=*/kVkBack,
        /*sendingForVk=*/kVkS,
        /*hasMultiProcessRenderer=*/true,
        /*isGameMovement=*/false,
        /*isBareModifier=*/false,
        /*queueFull=*/false);
    EXPECT_EQ(action, InputQueueAction::Enqueue);
}

TEST(InputQueueDecisionTest, Sending_BareModifier_PassesThrough) {
    // Shift/Ctrl/Alt/Win should never be eaten or queued -> immediate pass-through
    const auto action = DecideInputQueueActionDuringSend(
        /*isSending=*/true,
        /*eventVk=*/kVkShift,
        /*sendingForVk=*/kVkS,
        /*hasMultiProcessRenderer=*/true,
        /*isGameMovement=*/false,
        /*isBareModifier=*/true,
        /*queueFull=*/false);
    EXPECT_EQ(action, InputQueueAction::PassThrough);
}

TEST(InputQueueDecisionTest, Sending_GameMovementKey_PassesThrough) {
    // WASD movement when engine is empty in game mode -> immediate pass-through
    const auto action = DecideInputQueueActionDuringSend(
        /*isSending=*/true,
        /*eventVk=*/kVkW,
        /*sendingForVk=*/kVkS,
        /*hasMultiProcessRenderer=*/false,
        /*isGameMovement=*/true,
        /*isBareModifier=*/false,
        /*queueFull=*/false);
    EXPECT_EQ(action, InputQueueAction::PassThrough);
}

TEST(InputQueueDecisionTest, Sending_QueueFull_EnqueuesToPreserveOrder) {
    // When queue is full, genuine keys must still enqueue so HookEngine
    // drains existing keys in FIFO order rather than letting new key jump ahead (Issue 4)
    const auto action = DecideInputQueueActionDuringSend(
        /*isSending=*/true,
        /*eventVk=*/kVkI,
        /*sendingForVk=*/kVkS,
        /*hasMultiProcessRenderer=*/true,
        /*isGameMovement=*/false,
        /*isBareModifier=*/false,
        /*queueFull=*/true);
    EXPECT_EQ(action, InputQueueAction::Enqueue);
}
