// src/app/output/OutputInjectorFactory.cpp
//
// Phase 2 of the two-phase focus pipeline. FocusOwner::Classify owns the
// heavy Win32 inspection and passes this factory a compact snapshot.
//
// Create() dispatches channel branches in priority order:
//   useClipboard         → ClipboardInjector              (per-app sendMethod=1)
//   forcedSplitSleepMs>0 → SplitDispatchInjector(compat)  (per-app sendMethod=2/3)
//   RichEditD2DPT        → RichEditEmReplaceSelInjector
//   Electron             → SplitDispatchInjector(6 ms, multi-process renderer)
//   Console              → SplitDispatchInjector(5 ms, single-process)
//   default              → Win32SendInputInjector(suggestionDismissal)
#include "OutputInjectorFactory.h"

#include "Win32SendInputInjector.h"
#include "RichEditEmReplaceSelInjector.h"
#include "SplitDispatchInjector.h"
#include "ClipboardInjector.h"

namespace NextKey::Output {

namespace {
// Tuned to match the previous HookEngine constants:
//   Electron: 6 ms (Discord/Slack/VSCode renderer drain).
//   Console : 5 ms (CMD/PowerShell readline ingest).
// The per-app compatibility split (sendMethod 2/3) carries its own sleep in
// WindowClassification::forcedSplitSleepMs — resolved at the decision site
// (FocusOwner), not here.
constexpr int kElectronSleepMs = 6;
constexpr int kConsoleSleepMs  = 5;
}  // namespace

std::shared_ptr<IOutputInjector> Create(
        const WindowClassification& c) noexcept {
    // Priority order:
    //   if c.forceEmReplaceSel    → RichEditEmReplaceSelInjector(true) [sendMethod=4]
    //   if c.useClipboard         → ClipboardInjector              [sendMethod=1]
    //   if c.forcedSplitSleepMs>0 → SplitDispatchInjector(compat)  [sendMethod=2/3]
    //   if c.isRichEditD2DPT      → RichEditEmReplaceSelInjector   [D2]
    //   if c.isElectron           → SplitDispatchInjector(6)       [D3]
    //   if c.isConsole            → SplitDispatchInjector(5)       [D3]
    //   default                   → Win32SendInputInjector(c.suggestionDismissal)

    if (c.forceEmReplaceSel) {
        return std::make_shared<RichEditEmReplaceSelInjector>(/*forced=*/true);
    }
    if (c.useClipboard) {
        return std::make_shared<ClipboardInjector>();
    }
    if (c.forcedSplitSleepMs > 0) {
        // Per-app "send method = compatibility split" (sendMethod 2/3). The
        // user's explicit choice wins over the auto-detected Electron/Console
        // sleeps below, but inherits their renderer traits so protections
        // aren't lost: bait follows c.suggestionDismissal (Firefox's URL-bar inline
        // autocomplete behaves like Chromium's), hasMultiProcessRenderer
        // follows c.isElectron, and console/Electron channels keep alpha
        // lockstep. The sleep (resolved in
        // FocusOwner) spans the renderer / remote-session round-trip the
        // split is there to outlast.
        return std::make_shared<SplitDispatchInjector>(
            c.forcedSplitSleepMs, /*suggestionDismissal=*/c.suggestionDismissal,
            /*hasMultiProcessRenderer=*/c.isElectron,
            /*requiresSyntheticAlphaLockstep=*/c.isElectron || c.isConsole);
    }
    if (c.isRichEditD2DPT) {
        return std::make_shared<RichEditEmReplaceSelInjector>();
    }
    if (c.isElectron) {
        // Electron-on-Chromium hosts (WebView2 / Tauri / Dorion) need the
        // bait prefix even though they go through the split channel.
        // hasMultiProcessRenderer=true blocks mid-word passthrough in
        // HookEngine — Electron's multi-process renderer reorders
        // physical WM_KEYDOWN against synthetic VK_PACKET.
        return std::make_shared<SplitDispatchInjector>(
            kElectronSleepMs, c.suggestionDismissal, /*hasMultiProcessRenderer=*/true);
    }
    if (c.isConsole) {
        // Console hosts are not multi-process renderers, but ConPTY-backed
        // TUIs can reorder physical WM_KEYDOWN against synthetic VK_PACKET.
        return std::make_shared<SplitDispatchInjector>(
            kConsoleSleepMs, c.suggestionDismissal,
            /*hasMultiProcessRenderer=*/false,
            /*requiresSyntheticAlphaLockstep=*/true);
    }
    return std::make_shared<Win32SendInputInjector>(c.suggestionDismissal);
}

}  // namespace NextKey::Output
