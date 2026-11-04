// src/app/output/SplitDispatchInjector.cpp
//
// D3 implementation. Split SendInput with Sleep between BS batch and
// char batch. Covers Electron (Discord/Slack/VSCode, sleepMs=6) and
// Console (CMD/PowerShell, sleepMs=5). Distinct from Win32 batched
// path because these renderers drop the second half of a single big
// batch under load — splitting + brief Sleep gives the message loop a
// chance to drain before the second wave.
//
// Spec: docs/plans/sprint-2-output-injector.md §2.4
#include "SplitDispatchInjector.h"

#include <array>

#include "Internal.h"

namespace NextKey::Output {

namespace {

constexpr std::size_t kMaxBatch = 256;

}  // namespace

bool SplitDispatchInjector::Replace(std::size_t bsCount,
                                    std::wstring_view text,
                                    unsigned short reinjectVk) noexcept {
    if (bsCount == 0 && text.empty() && reinjectVk == 0) return true;
    // Both batches must fit before any destructive input is dispatched.
    if (bsCount > kMaxBatch / 2 || text.size() > kMaxBatch / 2) return false;
    const std::size_t reinjectCount = reinjectVk != 0 ? 1u : 0u;
    const auto heldMods = Internal::QueryHeldModifiers(bsCount, L"", reinjectCount != 0);
    const auto plan = Internal::PrepareSuggestionDismissal(
        suggestionDismissal_, bsCount, text,
        suggestKeepChars_.load(std::memory_order_acquire));
    const std::size_t required = reinjectCount + heldMods.count * 2
        + (plan.emitPrefix ? 2u : 0u) + plan.effectiveBsCount * 2;
    if (required > kMaxBatch) return false;

    if (required > 0) {
        std::array<INPUT, kMaxBatch> bsBuf{};
        std::size_t bi = 0;
        if (reinjectCount != 0) {
            bsBuf[bi++] = Internal::MakeKeyEvent(reinjectVk, false);
        }
        for (std::size_t k = 0; k < heldMods.count; ++k) {
            bsBuf[bi++] = Internal::MakeKeyEvent(heldMods.vks[k], true);
        }
        Internal::AppendSuggestionPrefix(bsBuf.data(), bi, plan);
        for (std::size_t k = 0; k < plan.effectiveBsCount; ++k) {
            Internal::AppendKeyPair(bsBuf.data(), bi, VK_BACK);
        }
        const auto restoreStart = bi;
        for (std::size_t k = 0; k < heldMods.count; ++k) {
            bsBuf[bi++] = Internal::MakeKeyEvent(heldMods.vks[k], false);
        }
        UINT sent = 0;
        if (!Internal::TrackedSendInput(bsBuf.data(), static_cast<UINT>(bi), &sent)) {
            Internal::RecoverAfterPartialSend({bsBuf.data(), bi}, sent, heldMods,
                                               reinjectCount, restoreStart);
            return false;
        }
    }

    // Batch 2: chars (only if text non-empty). Sleep only when both
    // batches present — pure-BS or pure-text needs no inter-batch gap.
    // A lone re-inject key-down is not a deletion, so it must not buy the
    // gap on its own: that would add sleepMs_ to a path that never had it.
    if (!text.empty()) {
        if (bsCount > 0) {
            Internal::g_sleep(static_cast<DWORD>(sleepMs_));
        }
        std::array<INPUT, kMaxBatch + 16> charBuf{};
        std::size_t ci = 0;
        // Re-query after the sleep: physical modifiers may have changed.
        const auto charMods = Internal::QueryHeldModifiers(0, text);
        for (std::size_t k = 0; k < charMods.count; ++k) {
            charBuf[ci++] = Internal::MakeKeyEvent(charMods.vks[k], true);
        }
        for (WCHAR ch : text) {
            Internal::AppendUnicodePair(charBuf.data(), ci, ch);
        }
        const auto restoreStart = ci;
        for (std::size_t k = 0; k < charMods.count; ++k) {
            charBuf[ci++] = Internal::MakeKeyEvent(charMods.vks[k], false);
        }
        if (ci == 0) return true;
        UINT sent = 0;
        if (!Internal::TrackedSendInput(charBuf.data(), static_cast<UINT>(ci), &sent)) {
            Internal::RecoverAfterPartialSend({charBuf.data(), ci}, sent, charMods, 0, restoreStart);
            return false;
        }
    }
    return true;
}

void SplitDispatchInjector::SendKey(unsigned short vkCode) noexcept {
    INPUT events[2] = {
        Internal::MakeKeyEvent(static_cast<WORD>(vkCode), /*keyup=*/false),
        Internal::MakeKeyEvent(static_cast<WORD>(vkCode), /*keyup=*/true),
    };
    (void)Internal::TrackedSendInput(events, 2);
}

}  // namespace NextKey::Output
