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
    EXPECT_EQ(queue.Size(), PhysicalInputQueue::kHardCeiling);
    EXPECT_EQ(queue.Capacity(), PhysicalInputQueue::kHardCeiling);

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
    EXPECT_EQ(upCount, kPairs);
    EXPECT_GT(downCount, 0u);
}

TEST(PhysicalInputQueueTest, ReleaseNeverEvictedEvenBeyondCeiling) {
    // Probe 1: Enqueue releases past ceiling (512, 513, 514, 520).
    // Not a single release may be dropped, and capacity must ONLY double when full!
    PhysicalInputQueue queue;

    // Up to 512 releases
    for (uint32_t i = 0; i < 512; ++i) {
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/false)));
    }
    EXPECT_EQ(queue.Size(), 512u);
    EXPECT_EQ(queue.Capacity(), 512u);

    // 513th release: buffer was full (512/512), doubles to 1024
    EXPECT_TRUE(queue.Enqueue(MakeKey(512, /*isDown=*/false)));
    EXPECT_EQ(queue.Size(), 513u);
    EXPECT_EQ(queue.Capacity(), 1024u);

    // 514th release: buffer has free slots (513/1024), capacity MUST NOT double!
    EXPECT_TRUE(queue.Enqueue(MakeKey(513, /*isDown=*/false)));
    EXPECT_EQ(queue.Size(), 514u);
    EXPECT_EQ(queue.Capacity(), 1024u);

    // 515..520 releases: capacity MUST remain 1024 (not 131,072!)
    for (uint32_t i = 514; i < 520; ++i) {
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/false)));
    }
    EXPECT_EQ(queue.Size(), 520u);
    EXPECT_EQ(queue.Capacity(), 1024u);

    // Up to 1024 releases: capacity MUST remain 1024
    for (uint32_t i = 520; i < 1024; ++i) {
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/false)));
    }
    EXPECT_EQ(queue.Size(), 1024u);
    EXPECT_EQ(queue.Capacity(), 1024u);

    // 1025th release: now genuinely full, doubles to 2048
    EXPECT_TRUE(queue.Enqueue(MakeKey(1024, /*isDown=*/false)));
    EXPECT_EQ(queue.Size(), 1025u);
    EXPECT_EQ(queue.Capacity(), 2048u);

    EXPECT_TRUE(queue.HasOverflowed());

    // Release 0 must still be at the head and every single release must survive in order
    for (uint32_t i = 0; i < 1025; ++i) {
        QueuedPhysicalKey key{};
        ASSERT_TRUE(queue.Pop(key));
        EXPECT_EQ(key.vkCode, i);
        EXPECT_FALSE(key.isDown);
    }
    EXPECT_TRUE(queue.Empty());
}

TEST(PhysicalInputQueueTest, AlternatingEnqueuePopZeroMemoryGrowth) {
    // Probe 2: 100,000 alternating enqueue/pop cycles with live entries = 2.
    // Capacity must remain stable (64), without prefix accumulation or memory leak!
    PhysicalInputQueue queue;

    // Initial 2 entries
    EXPECT_TRUE(queue.Enqueue(MakeKey('A', /*isDown=*/true)));
    EXPECT_TRUE(queue.Enqueue(MakeKey('A', /*isDown=*/false)));

    for (uint32_t i = 0; i < 100'000; ++i) {
        QueuedPhysicalKey popped{};
        ASSERT_TRUE(queue.Pop(popped));

        EXPECT_TRUE(queue.Enqueue(MakeKey(i % 256, /*isDown=*/(i % 2 == 0))));
    }

    EXPECT_EQ(queue.Size(), 2u);
    // Circular ring buffer reuses slots without growing capacity!
    EXPECT_EQ(queue.Capacity(), PhysicalInputQueue::kNormalCapacity);
    EXPECT_FALSE(queue.HasOverflowed());
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

TEST(PhysicalInputQueueTest, AllocationFailurePreservesReleasesWithoutAllocation) {
    // Probe reproducing user issue:
    // Queue contains 64 releases. Buffer is full.
    // Allocation failure occurs (simulated std::bad_alloc).
    // The 65th release (and subsequent releases) MUST NOT be rejected or dropped!
    PhysicalInputQueue queue;

    for (uint32_t i = 0; i < 64; ++i) {
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/false)));
    }
    EXPECT_EQ(queue.Size(), 64u);
    EXPECT_FALSE(queue.HasOverflowed());

    // Force allocation failure for testing
    queue.SetSimulateAllocationFailureForTesting(true);

    // Enqueue 10 more releases (VK 64 to 73).
    // Since these are key-up (release) events, they MUST be accepted via the zero-allocation emergency table!
    for (uint32_t i = 64; i < 74; ++i) {
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/false)));
    }

    // Key-down under total allocation failure with no key-downs to evict returns false
    EXPECT_FALSE(queue.Enqueue(MakeKey('A', /*isDown=*/true)));

    EXPECT_TRUE(queue.HasOverflowed());
    EXPECT_EQ(queue.Size(), 74u);

    // Pop all 74 events:
    // First 64 events from buffer (VK 0..63)
    for (uint32_t i = 0; i < 64; ++i) {
        QueuedPhysicalKey key{};
        ASSERT_TRUE(queue.Pop(key));
        EXPECT_EQ(key.vkCode, i);
        EXPECT_FALSE(key.isDown);
    }

    // Remaining 10 events from emergency release table (VK 64..73)
    for (uint32_t i = 64; i < 74; ++i) {
        QueuedPhysicalKey key{};
        ASSERT_TRUE(queue.Pop(key));
        EXPECT_FALSE(key.isDown);
    }

    EXPECT_TRUE(queue.Empty());
    EXPECT_EQ(queue.Size(), 0u);
}

TEST(PhysicalInputQueueTest, AllocationFailureEvictsDownToPreserveRelease) {
    // Queue contains 32 down and 32 up events (total 64, capacity 64)
    PhysicalInputQueue queue;

    for (uint32_t i = 0; i < 32; ++i) {
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/true)));
        EXPECT_TRUE(queue.Enqueue(MakeKey(i, /*isDown=*/false)));
    }
    EXPECT_EQ(queue.Size(), 64u);

    // Force allocation failure
    queue.SetSimulateAllocationFailureForTesting(true);

    // When buffer is full but has key-down events, Enqueue can evict a key-down without allocating!
    EXPECT_TRUE(queue.Enqueue(MakeKey(100, /*isDown=*/false)));

    size_t upCount = 0;
    QueuedPhysicalKey key{};
    while (queue.Pop(key)) {
        if (!key.isDown) {
            ++upCount;
        }
    }
    // All 32 original releases + the 33rd release survived!
    EXPECT_EQ(upCount, 33u);
}
