// VKey - Passive Hotkey Manager
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/config/TypingConfig.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace NextKey {

/// Process-local hotkey matcher. HookEngine owns platform hook installation and
/// feeds normalized events here; this class neither knows nor owns a hook.
class HotkeyManager {
public:
    using Callback = std::function<void()>;
    using SlotId = std::size_t;

    enum class KeyEventType : std::uint8_t { Down, Up };

    /// Modifier identity normalized by the event source. Left and right
    /// physical modifier keys map to the same identity.
    enum class ModifierKey : std::uint8_t { None, Control, Shift, Alt, Win };

    struct KeyEvent {
        std::uint32_t vk = 0;
        KeyEventType type = KeyEventType::Down;
        ModifierKey modifier = ModifierKey::None;
    };

    struct MatchResult {
        bool consume = false;
        bool cancelSystemMenu = false;
    };

    struct PhysicalModifierState {
        bool ctrl = false;
        bool shift = false;
        bool alt = false;
        bool win = false;
    };

    /// The hook-side sink records an id for later pump-time Dispatch(). Its
    /// noexcept signature keeps callback execution and exception handling out
    /// of the matcher path.
    using DeferredFireSink = void (*)(void* context, SlotId slot) noexcept;
    using PassThroughPredicate = bool (*)(const void* context) noexcept;
    using PhysicalKeyDownPredicate =
        bool (*)(const void* context, std::uint32_t vk) noexcept;

    struct PassThrough {
        PassThroughPredicate predicate;
        const void* context;

        [[nodiscard]] bool AllowsEvent() const noexcept {
            return predicate && predicate(context);
        }
    };

    explicit HotkeyManager(DeferredFireSink deferredFireSink = nullptr,
                           void* deferredFireContext = nullptr) noexcept;
    ~HotkeyManager() = default;

    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;
    HotkeyManager(HotkeyManager&&) = delete;
    HotkeyManager& operator=(HotkeyManager&&) = delete;

    /// Wires the hook owner's deferred-fire target during startup. This is
    /// intentionally a function pointer/context pair: matching remains
    /// no-throw and platform-neutral, and no callback executes here.
    /// Must precede FinalizeBindings().
    void SetDeferredFireSink(DeferredFireSink sink, void* context) noexcept;

    /// Registers immutable callback metadata. Registration is startup-only;
    /// FinalizeBindings() freezes the slot topology for hook-thread matching.
    SlotId AddHotkey(const HotkeyConfig& config,
                     Callback callback,
                     PassThrough passThrough = {nullptr, nullptr});

    /// Allocates hook-thread state once and publishes the initial packed
    /// configurations. It must run before the matcher receives events.
    void FinalizeBindings();

    /// Atomically publishes a new binding for an existing immutable slot.
    /// An already-latched DOWN remains associated with its slot until UP.
    void UpdateHotkey(SlotId slot, const HotkeyConfig& config) noexcept;

    /// Matches one normalized event on the hook thread. It only performs
    /// bounded scans, atomic config loads, and hook-thread-owned state writes.
    MatchResult Match(const KeyEvent& event) noexcept;

    /// Invokes a callback from the hook thread's message pump, never from
    /// Match().
    void Dispatch(SlotId slot);

    /// Replaces the cached modifier state with the caller's physical snapshot;
    /// does not query platform APIs. Any modifier still physically held is
    /// marked contaminated because a focus change is a gesture boundary; this
    /// prevents its later release from firing a modifier-alone binding while
    /// still allowing a following ordinary-key combo to match.
    void ReconcileModifiers(const PhysicalModifierState& physical) noexcept;

    /// Contaminates in-flight modifier chords when an external interaction
    /// (such as a mouse click or gesture boundary) occurs while modifiers are held,
    /// preventing modifier-only hotkeys from firing on subsequent key-up.
    void ContaminateChord() noexcept;

    /// Repair non-modifier DOWN/UP latches after the keyboard hook's brief
    /// replacement gap. A latch is cleared only when the supplied physical
    /// snapshot says its key is already up; held keys retain normal repeat/UP
    /// pairing. The caller supplies the platform query to keep this portable.
    void ReconcileLatchedKeysAfterHookReplacement(
        PhysicalKeyDownPredicate isKeyDown,
        const void* context) noexcept;

    /// Helper for adapters that consume the conventional virtual-key values.
    /// It is deliberately numeric and portable: no Windows header is needed.
    [[nodiscard]] static ModifierKey NormalizeModifier(std::uint32_t vk) noexcept;

private:
    struct SlotBinding {
        Callback callback;
        PassThrough passThrough;
    };

    struct SlotState {
        bool comboKeyDown = false;
        bool comboPassedThrough = false;
        std::uint32_t latchedVk = 0;
    };

    static constexpr std::uint32_t kCtrlBit = 0x01u;
    static constexpr std::uint32_t kShiftBit = 0x02u;
    static constexpr std::uint32_t kAltBit = 0x04u;
    static constexpr std::uint32_t kWinBit = 0x08u;
    static constexpr std::uint32_t kVkMask = 0x0FFFFFFFu;

    [[nodiscard]] static std::uint32_t PackConfig(const HotkeyConfig& config) noexcept;
    [[nodiscard]] static HotkeyConfig UnpackConfig(std::uint32_t packed) noexcept;
    [[nodiscard]] bool ModifiersMatch(const HotkeyConfig& config) const noexcept;
    [[nodiscard]] bool HasModifiers() const noexcept;
    void UpdateModifierState(ModifierKey modifier, KeyEventType type) noexcept;
    void QueueDeferredFire(SlotId slot) const noexcept;

    std::vector<SlotBinding> slots_;
    std::vector<std::uint32_t> pendingConfigs_;
    std::unique_ptr<std::atomic<std::uint32_t>[]> liveConfigs_;
    std::unique_ptr<SlotState[]> slotState_;
    SlotId slotCount_ = 0;
    bool finalized_ = false;

    DeferredFireSink deferredFireSink_ = nullptr;
    void* deferredFireContext_ = nullptr;

    // Exclusively hook-thread-owned after FinalizeBindings().
    bool modCtrlDown_ = false;
    bool modShiftDown_ = false;
    bool modAltDown_ = false;
    bool modWinDown_ = false;
    bool otherKeyPressed_ = false;
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "HotkeyManager live configs require lock-free uint32_t atomics");

}  // namespace NextKey
