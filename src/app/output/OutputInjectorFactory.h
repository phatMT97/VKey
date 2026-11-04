// src/app/output/OutputInjectorFactory.h
//
// Output injector factory (Rule #11.3).
//
// Two-phase focus detection — Phase 1 lives in FocusOwner, not here:
//   Phase 1 — `FocusOwner::Classify`
//             runs on the CALLER thread (main, via WinEventProc /
//             OnTickPoll). Heavy Win32 inspection happens there because
//             the same FocusClassification feeds non-dispatch concerns
//             (passthrough policy, retry-loop gating, per-app override
//             reads); duplicating it here would create two sources of
//             truth. The relevant subset is repacked into
//             `WindowClassification` below before calling `Create`.
//   Phase 2 — `Create(WindowClassification)` constructs the injector,
//             ~1 heap alloc. Caller atomic_store-publishes the result
//             to `HookEngine::injector_` (RCU).
//
// Spec: docs/plans/sprint-2-output-injector.md §2.6
// Phase 2b focus refactor: docs/plans/2026-05-19-architecture-review-design.md
#pragma once

#include "IOutputInjector.h"
#include "core/SuggestionDismissalDecision.h"
#include <memory>

namespace NextKey::Output {

// Phase 2 input — pure data, no shared writes (Rule #11.3).
// Filled in by the FocusOwner/HookEngine focus pipeline (see header comment).
struct WindowClassification {
    bool isRichEditD2DPT  = false;  // Win11 New Notepad
    bool isElectron       = false;  // Discord / Slack / VSCode etc
    bool isConsole        = false;  // Windows Terminal / CMD / PowerShell
    SuggestionDismissalPolicy suggestionDismissal = SuggestionDismissalPolicy::None;
    bool useClipboard     = false;  // User configured clipboard fallback
    // Per-app "send method" override = compatibility split dispatch
    // (AppOverrideEntry::sendMethod 2/3). 0 = not forced. When > 0, route
    // through SplitDispatchInjector with this inter-batch sleep (ms) instead
    // of the default Win32 batch path — drains the BS batch before the char
    // batch so a laggy renderer / remote-session round-trip can't eat the
    // trailing char. sendMethod 2 → ~6ms (Firefox-family / local Gecko),
    // sendMethod 3 → ~25ms (cloud / remote desktop). Resolved in FocusOwner;
    // see docs/plans/firefox-escape-hatch-spike/firefox-voz-sticking-chars-investigation.md for background.
    int forcedSplitSleepMs = 0;
    bool forceEmReplaceSel = false;
};

// Phase 2 — construct injector for a classification. Always returns a
// usable injector (default branch is Win32). Caller atomic_store-
// publishes the result to HookEngine::injector_.
[[nodiscard]] std::shared_ptr<IOutputInjector> Create(
    const WindowClassification& c) noexcept;

}  // namespace NextKey::Output
