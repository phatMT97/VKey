// src/app/output/Win32SendInputInjector.h
//
// Default catch-all injector. Batch SendInput. Covers Win32 plain Edit,
// Chrome omnibox, Notepad++, Chromium renderer textareas (Gmail / ChatGPT)
// — ~85% of host classes.
//
// Core-planned suggestion dismissal: Unicode bait for browsers/Outlook,
// modifier-neutral native printable prefix for Excel.
//
// Spec: docs/plans/sprint-2-output-injector.md §2.2
#pragma once

#include "IOutputInjector.h"
#include "core/SuggestionDismissalDecision.h"

namespace NextKey::Output {

class Win32SendInputInjector final : public IOutputInjector {
public:
    explicit Win32SendInputInjector(SuggestionDismissalPolicy suggestionDismissal) noexcept
        : suggestionDismissal_(suggestionDismissal) {}

    bool Replace(std::size_t bsCount, std::wstring_view text,
                 unsigned short reinjectVk = 0) noexcept override;
    void SendKey(unsigned short vkCode) noexcept override;

    // Batch SendInput drains within ~20-25 ms on real hosts under load.
    std::chrono::milliseconds SettleBudget() const noexcept override {
        return std::chrono::milliseconds{30};
    }

    // Channel trait — derived from the constructor-supplied policy.
    // HookEngine uses this to skip game-compat reinjectVk for Chromium
    // browsers (the bait char already handles suggest dismiss).
    bool NeedsBaitCharPrefix() const noexcept override {
        return suggestionDismissal_ == SuggestionDismissalPolicy::UnicodeBait;
    }
    // Multi-process and alpha-lockstep traits inherit false — Win32 batched
    // dispatch is for vanilla single-process renderers.

private:
    SuggestionDismissalPolicy suggestionDismissal_;
};

}  // namespace NextKey::Output
