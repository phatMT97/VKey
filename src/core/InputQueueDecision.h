// VKey - Pure decision: should a physical key arriving during synthetic send
// be queued, eaten, or passed through?
// SPDX-License-Identifier: GPL-3.0-only
//
// Extracted from HookEngine::LowLevelKeyboardProc so the logic can be unit-
// tested on Linux (HookEngine.cpp itself is Windows-only). Extends
// LeakedKeyDuringSendDecision.h with the defer-and-replay queue policy
// (Phase 3 of the 2026-11-11 ghost key elimination plan).

#pragma once

#include "core/LeakedKeyDuringSendDecision.h"

namespace NextKey {

enum class InputQueueAction {
    NormalProcess,  // not sending -> normal hook processing
    EatNoIntent,    // in-flight key's own repeat/up (DecideEatLeakedKeyDuringSend) -> eat/suppress
    PassThrough,    // bare modifier or WASD in game -> pass through immediately
    Enqueue,        // genuine different keystroke during send -> eat and enqueue for ordered replay
};

/// Pure: no I/O, no syscalls, safe from any thread.
[[nodiscard]] inline InputQueueAction DecideInputQueueActionDuringSend(
    bool isSending,
    unsigned int eventVk,
    unsigned int sendingForVk,
    bool hasMultiProcessRenderer,
    bool isGameMovement,
    bool isBareModifier,
    bool /*queueFull*/ = false) noexcept {
    if (!isSending) {
        return InputQueueAction::NormalProcess;
    }

    const LeakedKeyDuringSendInputs leak{
        .isSending = true,
        .hasMultiProcessRenderer = hasMultiProcessRenderer,
        .eventVk = eventVk,
        .sendingForVk = sendingForVk,
    };
    if (DecideEatLeakedKeyDuringSend(leak)) {
        return InputQueueAction::EatNoIntent;
    }

    if (isGameMovement || isBareModifier) {
        return InputQueueAction::PassThrough;
    }

    // Preserve chronological FIFO ordering: genuine keys are always enqueued.
    // If the queue is at capacity, HookEngine will drain pending keys first
    // rather than letting the new key pass through out of order (Issue 4).
    return InputQueueAction::Enqueue;
}

}  // namespace NextKey
