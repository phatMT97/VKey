// VKey - Physical input queue for deferring and replaying keystrokes
// arriving while output injection is in flight.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <bitset>
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
        if (count_ + emergencyCount_ >= kNormalCapacity) {
            overflowed_ = true;
        }

        // Always migrate any pending emergency releases into any free slots in buffer_ first
        TryMigrateEmergencyToRing();

        // If emergency releases are pending, no newer key-down can be enqueued ahead of them
        if (emergencyCount_ > 0) {
            overflowed_ = true;
            if (key.isDown) {
                // If there is an older isDown in buffer_, evict it to allow an emergency release to migrate in
                for (size_t i = 0; i < count_; ++i) {
                    const size_t idx = (head_ + i) % buffer_.size();
                    if (buffer_[idx].isDown) {
                        if (i == 0) {
                            head_ = (head_ + 1) % buffer_.size();
                            --count_;
                        } else {
                            for (size_t j = i; j + 1 < count_; ++j) {
                                const size_t curr = (head_ + j) % buffer_.size();
                                const size_t next = (head_ + j + 1) % buffer_.size();
                                buffer_[curr] = buffer_[next];
                            }
                            --count_;
                        }
                        TryMigrateEmergencyToRing();
                        break;
                    }
                }
                // Newer key-down is rejected: cannot overtake pending emergency releases under OOM
                return false;
            }
            // Newer key-up (release) is appended to emergency FIFO behind earlier releases
            return PushEmergencyRelease(key);
        }

        if (count_ >= kHardCeiling) {
            overflowed_ = true;
            // Ceiling pressure: search for the oldest key-down to evict.
            // Key-up (release) events MUST NEVER be evicted so host applications
            // never experience stuck keys.
            for (size_t i = 0; i < count_; ++i) {
                const size_t idx = (head_ + i) % buffer_.size();
                if (buffer_[idx].isDown) {
                    if (i == 0) {
                        head_ = (head_ + 1) % buffer_.size();
                        --count_;
                    } else {
                        // Shift remaining elements forward in the circular buffer
                        for (size_t j = i; j + 1 < count_; ++j) {
                            const size_t curr = (head_ + j) % buffer_.size();
                            const size_t next = (head_ + j + 1) % buffer_.size();
                            buffer_[curr] = buffer_[next];
                        }
                        --count_;
                    }
                    break;
                }
            }
        }

        // Only grow when the buffer has no free slots left.
        if (count_ == buffer_.size()) {
            try {
                if (simulateAllocationFailure_) {
                    throw std::bad_alloc();
                }
                Grow();
            } catch (const std::bad_alloc&) {
                overflowed_ = true;
                // Buffer cannot grow due to memory pressure.
                // Emergency fallback: try to evict the oldest key-down to make room without allocation.
                bool evicted = false;
                for (size_t i = 0; i < count_; ++i) {
                    const size_t idx = (head_ + i) % buffer_.size();
                    if (buffer_[idx].isDown) {
                        if (i == 0) {
                            head_ = (head_ + 1) % buffer_.size();
                            --count_;
                        } else {
                            for (size_t j = i; j + 1 < count_; ++j) {
                                const size_t curr = (head_ + j) % buffer_.size();
                                const size_t next = (head_ + j + 1) % buffer_.size();
                                buffer_[curr] = buffer_[next];
                            }
                            --count_;
                        }
                        evicted = true;
                        break;
                    }
                }
                if (!evicted) {
                    // All slots in the buffer are releases and memory cannot be allocated.
                    if (!key.isDown) {
                        // Crucial guarantee: KEY-UP (RELEASE) EVENTS MUST NEVER BE LOST!
                        // Even under total memory exhaustion, store this release in the fixed-capacity
                        // zero-allocation emergency FIFO so it can be replayed in order.
                        return PushEmergencyRelease(key);
                    }
                    // Only key-down events can be rejected under catastrophic OOM
                    return false;
                }
            }
        }

        const size_t tail = (head_ + count_) % buffer_.size();
        buffer_[tail] = key;
        ++count_;
        return true;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return count_ == 0 && emergencyCount_ == 0;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return count_ + emergencyCount_;
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
            if (!PopEmergencyRelease(outKey)) {
                return false;
            }
            return true;
        }

        outKey = buffer_[head_];
        head_ = (head_ + 1) % buffer_.size();
        --count_;
        if (count_ == 0) {
            head_ = 0;
        }

        // A slot was freed in buffer_: migrate the oldest emergency release in FIFO order
        TryMigrateEmergencyToRing();
        return true;
    }

    [[nodiscard]] const QueuedPhysicalKey* Peek() const noexcept {
        if (count_ > 0) {
            return &buffer_[head_];
        }
        if (emergencyCount_ > 0) {
            return &emergencyBuf_[emergencyHead_];
        }
        return nullptr;
    }

    void Clear() noexcept {
        head_ = 0;
        count_ = 0;
        emergencyHead_ = 0;
        emergencyCount_ = 0;
        emergencyPending_.reset();
        overflowed_ = false;
    }

    void SetSimulateAllocationFailureForTesting(bool simulate) noexcept {
        simulateAllocationFailure_ = simulate;
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

    bool PushEmergencyRelease(const QueuedPhysicalKey& key) noexcept {
        const size_t vk = static_cast<size_t>(key.vkCode & 0xFF);
        if (!emergencyPending_.test(vk)) {
            if (emergencyCount_ >= kEmergencyCapacity) {
                return false;
            }
            emergencyPending_.set(vk);
            const size_t tail = (emergencyHead_ + emergencyCount_) % kEmergencyCapacity;
            emergencyBuf_[tail] = key;
            ++emergencyCount_;
        } else {
            for (size_t i = 0; i < emergencyCount_; ++i) {
                const size_t idx = (emergencyHead_ + i) % kEmergencyCapacity;
                if ((emergencyBuf_[idx].vkCode & 0xFF) == vk) {
                    emergencyBuf_[idx] = key;
                    break;
                }
            }
        }
        return true;
    }

    bool PopEmergencyRelease(QueuedPhysicalKey& outKey) noexcept {
        if (emergencyCount_ == 0) {
            return false;
        }
        outKey = emergencyBuf_[emergencyHead_];
        emergencyHead_ = (emergencyHead_ + 1) % kEmergencyCapacity;
        --emergencyCount_;
        emergencyPending_.reset(outKey.vkCode & 0xFF);
        return true;
    }

    void TryMigrateEmergencyToRing() noexcept {
        while (emergencyCount_ > 0 && count_ < buffer_.size()) {
            QueuedPhysicalKey emKey{};
            if (PopEmergencyRelease(emKey)) {
                const size_t tail = (head_ + count_) % buffer_.size();
                buffer_[tail] = emKey;
                ++count_;
            } else {
                break;
            }
        }
    }

    std::vector<QueuedPhysicalKey> buffer_;
    size_t head_ = 0;
    size_t count_ = 0;
    bool overflowed_ = false;
    bool simulateAllocationFailure_ = false;

    // Zero-allocation fallback FIFO for key-up (release) events under severe memory exhaustion
    static constexpr size_t kEmergencyCapacity = 256;
    std::array<QueuedPhysicalKey, kEmergencyCapacity> emergencyBuf_{};
    std::bitset<kEmergencyCapacity> emergencyPending_{};
    size_t emergencyHead_ = 0;
    size_t emergencyCount_ = 0;
};

}  // namespace NextKey
