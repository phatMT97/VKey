// VKey - delayed focus-classification apply decision
// SPDX-License-Identifier: GPL-3.0-only
//
// Focus classification is intentionally asynchronous: WinEvent publishes a
// request, the worker performs the heavy HWND/app probes, then the hook thread
// consumes the immutable result. Physical input can overtake that worker
// round-trip. Applying such a result in the middle of a live word destroys the
// engine-side composition while the host UI keeps the already-rendered text.
//
// Keep the ordering policy pure and Linux-testable. The caller owns all side
// effects (drop the snapshot, retain it in a hook-owned deferred slot, or apply
// the logical typing context). Current host output can refresh even while that
// context waits, so a browser does not inherit the previous app's transport.

#pragma once

#include <cstdint>

namespace NextKey {

enum class FocusApplyDisposition : std::uint8_t {
    DropStale,
    DeferUntilBoundary,
    ApplyNow,
};

struct FocusApplyInputs {
    std::uint64_t snapshotRequestSerial{0};
    std::uint64_t latestRequestSerial{0};
    std::uint64_t snapshotInputEpoch{0};
    std::uint64_t currentInputEpoch{0};
    bool hasComposition{false};
};

struct TsfFocusActivationState {
    std::uintptr_t lastTsfHwndOpaque{0};
    std::uint32_t lastTsfPid{0};
};

/// Pure, allocation-free and syscall-free. Equality is intentional:
/// request serials identify the latest focus event, while input epochs answer
/// whether any physical key-down began after that event.
[[nodiscard]] constexpr FocusApplyDisposition
DecideFocusApply(const FocusApplyInputs& in) noexcept {
    if (in.snapshotRequestSerial != in.latestRequestSerial) {
        return FocusApplyDisposition::DropStale;
    }
    if (in.hasComposition && in.snapshotInputEpoch != in.currentInputEpoch) {
        return FocusApplyDisposition::DeferUntilBoundary;
    }
    return FocusApplyDisposition::ApplyNow;
}

/// Re-assert the VKey TIP once per TSF focus target. Duplicate WinEvents for
/// one window must not repeat the expensive profile activation, while leaving
/// TSF mode rearms the same target for a later visit.
/// Keyed on (HWND, PID) because Windows recycles window handles — the focus
/// classification cache pairs them for the same reason.
[[nodiscard]] constexpr bool ShouldActivateTsfProfileForFocus(
        TsfFocusActivationState& state,
        bool isTsf,
        std::uintptr_t hwndOpaque,
        std::uint32_t pid) noexcept {
    if (!isTsf || hwndOpaque == 0) {
        state.lastTsfHwndOpaque = 0;
        state.lastTsfPid = 0;
        return false;
    }

    const bool shouldActivate =
        state.lastTsfHwndOpaque != hwndOpaque || state.lastTsfPid != pid;
    state.lastTsfHwndOpaque = hwndOpaque;
    state.lastTsfPid = pid;
    return shouldActivate;
}

/// Pure decision for Tier 1 (Issue #273): checks whether the active typing target actually changed.
/// Returns true if either the top-level window changed OR the effective focused
/// control changed. If neither changed (e.g. spurious background UI updates),
/// composition must NOT be reset.
[[nodiscard]] constexpr bool ShouldResetCompositionOnFocusChange(
        std::uintptr_t currentHwndOpaque,
        std::uintptr_t currentEffectiveFocusOpaque,
        std::uintptr_t lastAppliedHwndOpaque,
        std::uintptr_t lastAppliedEffectiveFocusOpaque) noexcept {
    return (currentHwndOpaque != lastAppliedHwndOpaque) ||
           (currentEffectiveFocusOpaque != lastAppliedEffectiveFocusOpaque);
}

/// Pure decision for Tier 2 (Issue #273): filters out redundant background WinEvents.
/// When an event trigger comes from a helper/tool window (triggerHwnd != activeHwnd)
/// but the foreground window and effective focused control have not changed,
/// the event is a spurious background update (e.g. FastStone Capture timer UI).
[[nodiscard]] constexpr bool IsSpuriousBackgroundFocusEvent(
        std::uintptr_t triggerHwndOpaque,
        std::uintptr_t classifiedHwndOpaque,
        std::uintptr_t currentTargetOpaque,
        std::uintptr_t lastDispatchedHwndOpaque,
        std::uintptr_t lastDispatchedTargetOpaque) noexcept {
    return (triggerHwndOpaque != 0) &&
           (triggerHwndOpaque != classifiedHwndOpaque) &&
           (classifiedHwndOpaque == lastDispatchedHwndOpaque) &&
           (currentTargetOpaque == lastDispatchedTargetOpaque);
}

}  // namespace NextKey
