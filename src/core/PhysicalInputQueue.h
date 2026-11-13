// VKey - Physical input queue for deferring and replaying keystrokes
// arriving while output injection is in flight.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

namespace NextKey {

struct QueuedPhysicalKey {
    uint32_t vkCode = 0;
    uint32_t scanCode = 0;
    uint32_t flags = 0;
    bool isDown = false;
    bool lshift = false;
    bool rshift = false;
    bool capsLock = false;
    bool lctrl = false;
    bool rctrl = false;
    bool lalt = false;
    bool ralt = false;
    bool lwin = false;
    bool rwin = false;

    bool operator==(const QueuedPhysicalKey& other) const = default;
};

/// Thread-confined to the hook thread.
/// Circular ring buffer with zero allocations on common path (< 64 keys).
/// Preserves FIFO ordering of all keystrokes during active send.
/// Strictly protects key-up (release) events from eviction to avoid stuck keys at host.
class PhysicalInputQueue {
public:
    static constexpr size_t kNormalCapacity = 64;
    static constexpr size_t kHardCeiling = 512;

    PhysicalInputQueue() {
        buffer_.resize(kNormalCapacity);
    }

    /// Enqueue a physical key event arriving while output injection is in-flight.
    /// Returns true on success, false on allocation failure.
    bool Enqueue(const QueuedPhysicalKey& key) {
        try {
            if (count_ >= kNormalCapacity) {
                overflowed_ = true;
            }

            if (count_ >= kHardCeiling) {
                overflowed_ = true;
                // Ceiling pressure: search for the oldest key-down to evict.
                // Key-up (release) events MUST NEVER be evicted so host applications
                // never experience stuck keys.
                bool evicted = false;
                for (size_t i = 0; i < count_; ++i) {
                    const size_t idx = (head_ + i) % buffer_.size();
                    if (buffer_[idx].isDown) {
                        // Shift remaining elements forward in the circular buffer
                        for (size_t j = i; j + 1 < count_; ++j) {
                            const size_t curr = (head_ + j) % buffer_.size();
                            const size_t next = (head_ + j + 1) % buffer_.size();
                            buffer_[curr] = buffer_[next];
                        }
                        --count_;
                        evicted = true;
                        break;
                    }
                }
                if (!evicted) {
                    // All events currently in the queue are releases.
                    // Never evict any release; grow the buffer to accommodate the new event.
                    Grow();
                }
            } else if (count_ == buffer_.size()) {
                Grow();
            }

            const size_t tail = (head_ + count_) % buffer_.size();
            buffer_[tail] = key;
            ++count_;
            return true;
        } catch (const std::bad_alloc&) {
            overflowed_ = true;
            return false;
        }
    }

    [[nodiscard]] bool Empty() const noexcept {
        return count_ == 0;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return count_;
    }

    [[nodiscard]] size_t Capacity() const noexcept {
        return buffer_.size();
    }

    [[nodiscard]] bool HasOverflowed() const noexcept {
        return overflowed_;
    }

    void ClearOverflow() noexcept {
        overflowed_ = false;
    }

    [[nodiscard]] bool Pop(QueuedPhysicalKey& outKey) noexcept {
        if (count_ == 0) {
            return false;
        }
        outKey = buffer_[head_];
        head_ = (head_ + 1) % buffer_.size();
        --count_;
        if (count_ == 0) {
            head_ = 0;
        }
        return true;
    }

    [[nodiscard]] const QueuedPhysicalKey* Peek() const noexcept {
        if (count_ == 0) {
            return nullptr;
        }
        return &buffer_[head_];
    }

    void Clear() noexcept {
        head_ = 0;
        count_ = 0;
        overflowed_ = false;
    }

private:
    void Grow() {
        const size_t newCap = buffer_.empty() ? kNormalCapacity : buffer_.size() * 2;
        std::vector<QueuedPhysicalKey> newBuf(newCap);
        for (size_t i = 0; i < count_; ++i) {
            newBuf[i] = buffer_[(head_ + i) % buffer_.size()];
        }
        buffer_ = std::move(newBuf);
        head_ = 0;
    }

    std::vector<QueuedPhysicalKey> buffer_;
    size_t head_ = 0;
    size_t count_ = 0;
    bool overflowed_ = false;
};

}  // namespace NextKey
