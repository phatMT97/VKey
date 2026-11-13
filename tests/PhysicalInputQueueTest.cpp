// Tests for PhysicalInputQueue (src/core/PhysicalInputQueue.h)
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "core/PhysicalInputQueue.h"

using NextKey::PhysicalInputQueue;
using NextKey::QueuedPhysicalKey;

namespace {

QueuedPhysicalKey MakeKey(uint32_t vk, bool isDown, bool shift = false, bool ctrl = false) {
    return QueuedPhysicalKey{
        .vkCode = vk,
        .scanCode = vk,
        .flags = 0,
        .isDown = isDown,
        .lshift = shift,
        .rshift = false,
        .capsLock = false,
        .lctrl = ctrl,
        .rctrl = false,
        .lalt = false,
        .ralt = false,
        .lwin = false,
        .rwin = false,
    };
}

}  // namespace

TEST(PhysicalInputQueueTest, EmptyQueueInitialState) {
    PhysicalInputQueue queue;
    EXPECT_TRUE(queue.Empty());
    EXPECT_EQ(queue.Size(), 0u);
    EXPECT_FALSE(queue.HasOverflowed());
    EXPECT_EQ(queue.Peek(), nullptr);

    QueuedPhysicalKey key{};
    EXPECT_FALSE(queue.Pop(key));
}

TEST(PhysicalInputQueueTest, FIFOOrderPreservation) {
    PhysicalInputQueue queue;

    const auto k1 = MakeKey('A', /*isDown=*/true, /*shift=*/true);
    const auto k2 = MakeKey('A', /*isDown=*/false, /*shift=*/false);
    const auto k3 = MakeKey('B', /*isDown=*/true, /*shift=*/false);
    const auto k4 = MakeKey('B', /*isDown=*/false, /*shift=*/false);

    EXPECT_TRUE(queue.Enqueue(k1));
    EXPECT_TRUE(queue.Enqueue(k2));
    EXPECT_TRUE(queue.Enqueue(k3));
    EXPECT_TRUE(queue.Enqueue(k4));

    EXPECT_EQ(queue.Size(), 4u);
    EXPECT_FALSE(queue.HasOverflowed());

    QueuedPhysicalKey popped{};
    ASSERT_TRUE(queue.Pop(popped));
    EXPECT_EQ(popped, k1);

    ASSERT_TRUE(queue.Pop(popped));
    EXPECT_EQ(popped, k2);

    ASSERT_TRUE(queue.Pop(popped));
    EXPECT_EQ(popped, k3);

    ASSERT_TRUE(queue.Pop(popped));
    EXPECT_EQ(popped, k4);

    EXPECT_TRUE(queue.Empty());
    EXPECT_EQ(queue.Size(), 0u);
}

TEST(PhysicalInputQueueTest, NormalCapacityUnderThreshold) {
    PhysicalInputQueue queue;

    for (uint32_t i = 0; i < PhysicalInputQueue::kNormalCapacity; ++i) {
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/true)));
    }

    EXPECT_EQ(queue.Size(), PhysicalInputQueue::kNormalCapacity);
    EXPECT_FALSE(queue.HasOverflowed());

    for (uint32_t i = 0; i < PhysicalInputQueue::kNormalCapacity; ++i) {
        QueuedPhysicalKey key{};
        ASSERT_TRUE(queue.Pop(key));
        EXPECT_EQ(key.vkCode, i);
    }
    EXPECT_TRUE(queue.Empty());
}

TEST(PhysicalInputQueueTest, OverflowSetsFlagAndPreservesAllEvents) {
    PhysicalInputQueue queue;

    constexpr size_t kBurstCount = 80;  // > 64 normal capacity
    for (uint32_t i = 0; i < kBurstCount; ++i) {
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/(i % 2 == 0))));
    }

    // Flag must be set to inform HookEngine/Drain that an abnormal flood occurred
    EXPECT_TRUE(queue.HasOverflowed());

    // ALL events must be preserved (zero dropped keystrokes!)
    EXPECT_EQ(queue.Size(), kBurstCount);

    for (uint32_t i = 0; i < kBurstCount; ++i) {
        QueuedPhysicalKey key{};
        ASSERT_TRUE(queue.Pop(key));
        EXPECT_EQ(key.vkCode, i);
        EXPECT_EQ(key.isDown, (i % 2 == 0));
    }
    EXPECT_TRUE(queue.Empty());
}

TEST(PhysicalInputQueueTest, DownUpBalanceUnderCeilingPressure) {
    PhysicalInputQueue queue;

    // Enqueue 300 down events + 300 up events = 600 total events
    // This exceeds kHardCeiling (512).
    constexpr uint32_t kPairs = 300;
    for (uint32_t i = 0; i < kPairs; ++i) {
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/true)));
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/false)));
    }

    EXPECT_TRUE(queue.HasOverflowed());
    EXPECT_LE(queue.Size(), PhysicalInputQueue::kHardCeiling);

    // Count how many down and up events survived eviction
    size_t downCount = 0;
    size_t upCount = 0;

    QueuedPhysicalKey key{};
    while (queue.Pop(key)) {
        if (key.isDown) {
            ++downCount;
        } else {
            ++upCount;
        }
    }

    // Crucial guarantee: EVERY SINGLE RELEASE (key-up) is strictly preserved!
    // No host application can ever be left with a stuck key because all 300 releases survived!
    EXPECT_EQ(upCount, kPairs);
    EXPECT_GT(downCount, 0u);
}

TEST(PhysicalInputQueueTest, ClearAndReuse) {
    PhysicalInputQueue queue;

    for (uint32_t i = 0; i < 70; ++i) {
        queue.Enqueue(MakeKey(i, /*isDown=*/true));
    }
    EXPECT_TRUE(queue.HasOverflowed());

    queue.Clear();
    EXPECT_TRUE(queue.Empty());
    EXPECT_EQ(queue.Size(), 0u);
    EXPECT_FALSE(queue.HasOverflowed());

    EXPECT_TRUE(queue.Enqueue(MakeKey('Z', /*isDown=*/true)));
    EXPECT_EQ(queue.Size(), 1u);
    EXPECT_FALSE(queue.HasOverflowed());

    QueuedPhysicalKey key{};
    ASSERT_TRUE(queue.Pop(key));
    EXPECT_EQ(key.vkCode, static_cast<uint32_t>('Z'));
    EXPECT_TRUE(queue.Empty());
}
