// VKey - Physical input queue for deferring and replaying keystrokes
// arriving while output injection is in flight.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
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

/// Thread-confined to the hook thread. Zero allocations on common path (< 64 keys).
/// Preserves FIFO ordering of all keystrokes during active send.
/// Strictly protects key-up (release) events from eviction to avoid stuck keys at host.
class PhysicalInputQueue {
public:
    static constexpr size_t kNormalCapacity = 64;
    static constexpr size_t kHardCeiling = 512;

    PhysicalInputQueue() {
        buffer_.reserve(kNormalCapacity);
    }

    /// Enqueue a physical key event arriving while output injection is in-flight.
    /// Returns true on success.
    bool Enqueue(const QueuedPhysicalKey& key) noexcept {
        if (Size() >= kNormalCapacity) {
            overflowed_ = true;
        }

        if (Size() >= kHardCeiling) {
            overflowed_ = true;
            // Emergency ceiling reached: evict the oldest key-down to bound memory.
            // Releases (key-up) MUST NOT be evicted so host applications never experience stuck keys.
            bool evicted = false;
            for (size_t i = headIndex_; i < buffer_.size(); ++i) {
                if (buffer_[i].isDown) {
                    buffer_.erase(buffer_.begin() + static_cast<ptrdiff_t>(i));
                    evicted = true;
                    break;
                }
            }
            if (!evicted && headIndex_ < buffer_.size()) {
                // Extraordinary edge case where queue contains only releases at hard ceiling:
                // drop oldest release as last resort to preserve process stability.
                buffer_.erase(buffer_.begin() + static_cast<ptrdiff_t>(headIndex_));
            }
        }

        buffer_.push_back(key);
        return true;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return headIndex_ >= buffer_.size();
    }

    [[nodiscard]] size_t Size() const noexcept {
        return buffer_.size() - headIndex_;
    }

    [[nodiscard]] bool HasOverflowed() const noexcept {
        return overflowed_;
    }

    void ClearOverflow() noexcept {
        overflowed_ = false;
    }

    [[nodiscard]] bool Pop(QueuedPhysicalKey& outKey) noexcept {
        if (Empty()) {
            return false;
        }
        outKey = buffer_[headIndex_++];
        if (headIndex_ == buffer_.size()) {
            buffer_.clear();
            headIndex_ = 0;
        }
        return true;
    }

    [[nodiscard]] const QueuedPhysicalKey* Peek() const noexcept {
        if (Empty()) {
            return nullptr;
        }
        return &buffer_[headIndex_];
    }

    void Clear() noexcept {
        buffer_.clear();
        headIndex_ = 0;
        overflowed_ = false;
    }

private:
    std::vector<QueuedPhysicalKey> buffer_;
    size_t headIndex_ = 0;
    bool overflowed_ = false;
};

}  // namespace NextKey
