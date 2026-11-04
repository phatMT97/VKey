// src/app/output/Win32SendInputInjector.cpp
//
// D1 implementation. Batch SendInput. Stack-buffer std::array (no heap
// alloc on hot path). Core plans the host-specific suggestion prefix and
// its compensating Backspace.
//
// Spec: docs/plans/sprint-2-output-injector.md §2.2
#include "Win32SendInputInjector.h"

#include <array>

#include "Internal.h"

namespace NextKey::Output {

namespace {

// Stack-buffer upper bound. Worst-case Vietnamese composition replacement
// needs ~8 chars + 8 BS; 256 events (= 128 keystrokes × down/up) is 16×
// safety margin. Sized to fit a single SendInput batch comfortably.
constexpr std::size_t kMaxBatch = 256;

}  // namespace

bool Win32SendInputInjector::Replace(std::size_t bsCount,
                                     std::wstring_view text,
                                     unsigned short reinjectVk) noexcept {
    if (bsCount == 0 && text.empty() && reinjectVk == 0) return true;
    if (bsCount > kMaxBatch / 2 || text.size() > kMaxBatch / 2) return false;

    const std::size_t reinjectCount = reinjectVk != 0 ? 1u : 0u;
    const auto heldMods = Internal::QueryHeldModifiers(bsCount, text, reinjectCount != 0);
    const auto plan = Internal::PrepareSuggestionDismissal(
        suggestionDismissal_, bsCount, text,
        suggestKeepChars_.load(std::memory_order_acquire));
    const std::size_t required = reinjectCount + heldMods.count * 2
        + (plan.emitPrefix ? 2u : 0u) + plan.effectiveBsCount * 2 + text.size() * 2;
    if (required > kMaxBatch) return false;

    std::array<INPUT, kMaxBatch> buf{};
    std::size_t i = 0;
    // The game key must retain the user's modifiers; only the replacement
    // after it is modifier-neutral. All capacity checks precede dispatch.
    if (reinjectCount != 0) {
        buf[i++] = Internal::MakeKeyEvent(reinjectVk, false);
    }
    for (std::size_t k = 0; k < heldMods.count; ++k) {
        buf[i++] = Internal::MakeKeyEvent(heldMods.vks[k], true);
    }
    Internal::AppendSuggestionPrefix(buf.data(), i, plan);
    for (std::size_t k = 0; k < plan.effectiveBsCount; ++k) {
        Internal::AppendKeyPair(buf.data(), i, VK_BACK);
    }
    for (WCHAR ch : text) Internal::AppendUnicodePair(buf.data(), i, ch);
    const auto restoreStart = i;
    for (std::size_t k = 0; k < heldMods.count; ++k) {
        buf[i++] = Internal::MakeKeyEvent(heldMods.vks[k], false);
    }

    UINT sent = 0;
    const bool delivered = Internal::TrackedSendInput(
        buf.data(), static_cast<UINT>(i), &sent);
    if (!delivered) {
        Internal::RecoverAfterPartialSend({buf.data(), i}, sent, heldMods,
                                           reinjectCount, restoreStart);
    }
    return delivered;
}

void Win32SendInputInjector::SendKey(unsigned short vkCode) noexcept {
    INPUT events[2] = {
        Internal::MakeKeyEvent(static_cast<WORD>(vkCode), /*keyup=*/false),
        Internal::MakeKeyEvent(static_cast<WORD>(vkCode), /*keyup=*/true),
    };
    // Fire-and-forget; partial-send detection logged by TrackedSendInput
    // but not actionable for SendKey (re-inject single key has no clean
    // fallback — the original use case at HookEngine line ~905 is
    // re-inject after synth-pending, not a primary delivery).
    (void)Internal::TrackedSendInput(events, 2);
}

}  // namespace NextKey::Output
