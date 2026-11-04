// src/app/output/SplitDispatchInjector.h
//
// Split SendInput with Sleep between BS batch and char batch. Covers
// Electron (Discord / Slack / VSCode, sleepMs=6) and Console (CMD /
// PowerShell, sleepMs=5) — ~10% of host classes.
//
// Why merge Electron + Console: only difference is 1 ms in baseMs. Two
// impls 99% identical = duplication; one impl with constructor
// sleepMs_ parameterizes cleanly.
//
// Spec: docs/plans/sprint-2-output-injector.md §2.4
#pragma once

#include "IOutputInjector.h"
#include "core/SuggestionDismissalDecision.h"

namespace NextKey::Output {

class SplitDispatchInjector final : public IOutputInjector {
public:
    explicit SplitDispatchInjector(
        int sleepMsBetweenBatches,
        SuggestionDismissalPolicy suggestionDismissal = SuggestionDismissalPolicy::None,
        bool hasMultiProcessRenderer = false,
        bool requiresSyntheticAlphaLockstep = false) noexcept
        : sleepMs_(sleepMsBetweenBatches),
          suggestionDismissal_(suggestionDismissal),
          hasMultiProcessRenderer_(hasMultiProcessRenderer),
          requiresSyntheticAlphaLockstep_(
              hasMultiProcessRenderer || requiresSyntheticAlphaLockstep) {}

    bool Replace(std::size_t bsCount, std::wstring_view text,
                 unsigned short reinjectVk = 0) noexcept override;
    void SendKey(unsigned short vkCode) noexcept override;

    // Covers split Sleep (5-6 ms) + Electron / Qt event-loop (~30-90 ms).
    // Empirical from current HookEngine kSynthSettleMs hardcode.
    std::chrono::milliseconds SettleBudget() const noexcept override {
        return std::chrono::milliseconds{100};
    }

    // Channel traits are dispatch-arm hints set by the factory based on
    // WindowClassification. Electron is both multi-process and lockstep;
    // console is single-process but still lockstep because ConPTY/TUI input
    // can reorder physical WM_KEYDOWN against synthetic VK_PACKET.
    bool NeedsBaitCharPrefix() const noexcept override {
        return suggestionDismissal_ == SuggestionDismissalPolicy::UnicodeBait;
    }
    bool HasMultiProcessRenderer() const noexcept override {
        return hasMultiProcessRenderer_;
    }
    bool RequiresSyntheticAlphaLockstep() const noexcept override {
        return requiresSyntheticAlphaLockstep_;
    }

private:
    int sleepMs_;
    // Preserve the host's Core policy even when a per-app override selects
    // split dispatch: WebView2 needs Unicode bait, Excel needs a native VK.
    SuggestionDismissalPolicy suggestionDismissal_;
    // True for Electron / Qt-on-Chromium hosts. Used only by leaked-key
    // suppression; console hosts remain false here.
    bool hasMultiProcessRenderer_;
    // Blocks physical/synthetic alpha mixing within one transformed word.
    bool requiresSyntheticAlphaLockstep_;
};

}  // namespace NextKey::Output
