// VKey - Output Dispatcher (Wave 3 PR 3.3, 2026-05-24)
// SPDX-License-Identifier: GPL-3.0-only

#include "OutputDispatcher.h"

#include "FocusOwner.h"
#include "HookCommandMailbox.h"
#include "PerfHistogram.h"  // PERF_SCOPE
#include "core/Logger.h"
#include "output/Internal.h"
#include "output/OutputInjectorFactory.h"
#include "output/Win32SendInputInjector.h"

#include <algorithm>
#include <chrono>
#include <vector>

#define HOOK_LOG(fmt, ...) do {                                              \
    if (::NextKey::Logger::IsEnabled())                                      \
        ::NextKey::Logger::Log(L"[Hook] " fmt, ##__VA_ARGS__);               \
} while (0)

namespace NextKey {

// MUST match HookEngine::VKEY_EXTRA_INFO ("NK" magic). Defined here too
// so OutputDispatcher.cpp doesn't pull in HookEngine.h (avoid header
// cycle — HookEngine.h includes OutputDispatcher.h post-PR-3.3).
namespace { constexpr ULONG_PTR kVKeyExtraInfo = 0x4E4B; }

std::atomic<OutputDispatcher*> OutputDispatcher::s_instance{nullptr};

// ─────────────────────────────────────────────────────────────────────
// SendInput event helpers (file-scope statics — moved from HookEngine.cpp)
// ─────────────────────────────────────────────────────────────────────
namespace {

void AppendUnicodeEvent(std::vector<INPUT>& events, WORD wScan) {
    INPUT inDown = {};
    inDown.type = INPUT_KEYBOARD;
    inDown.ki.wScan = wScan;
    inDown.ki.dwFlags = KEYEVENTF_UNICODE;
    inDown.ki.dwExtraInfo = kVKeyExtraInfo;
    events.push_back(inDown);

    INPUT inUp = inDown;
    inUp.ki.dwFlags |= KEYEVENTF_KEYUP;
    events.push_back(inUp);
}

void AppendVkEvent(std::vector<INPUT>& events, WORD wVk, WORD wScan) {
    INPUT inDown = {};
    inDown.type = INPUT_KEYBOARD;
    inDown.ki.wVk = wVk;
    inDown.ki.wScan = wScan;
    inDown.ki.dwExtraInfo = kVKeyExtraInfo;
    events.push_back(inDown);

    INPUT inUp = inDown;
    inUp.ki.dwFlags |= KEYEVENTF_KEYUP;
    events.push_back(inUp);
}

// Write Unicode text to clipboard. Returns false on any failure.
// Sets ExcludeClipboardContentFromMonitorProcessing to keep Win+V clean.
bool SetClipboardText(const std::wstring& text) noexcept {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) { CloseClipboard(); return false; }
    auto* dest = static_cast<wchar_t*>(GlobalLock(hMem));
    if (!dest) { GlobalFree(hMem); CloseClipboard(); return false; }
    memcpy(dest, text.c_str(), bytes);
    GlobalUnlock(hMem);
    SetClipboardData(CF_UNICODETEXT, hMem);

    // Exclude from Windows Clipboard History (Win+V) and cloud sync.
    // Win10 1809+; harmless no-op on older builds.
    static UINT cfExclude = RegisterClipboardFormat(
        L"ExcludeClipboardContentFromMonitorProcessing");
    if (cfExclude) {
        HGLOBAL hExclude = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (hExclude) {
            auto* p = static_cast<DWORD*>(GlobalLock(hExclude));
            if (p) { *p = 0; GlobalUnlock(hExclude); }
            SetClipboardData(cfExclude, hExclude);
        }
    }

    CloseClipboard();
    return true;
}

constexpr size_t kMaxClassName = 64;
constexpr UINT   kEditMsgTimeoutMs = 50;

// VB6 (ThunderRT6*) apps are the whole reason this path exists — check first.
// _wcsnicmp is case-insensitive so "RichEdit" matches "RICHEDIT60W" too.
bool IsEditCompatibleClass(const wchar_t* cls) noexcept {
    if (!cls || !*cls) return false;
    if (_wcsnicmp(cls, L"ThunderRT6TextBox", 17) == 0) return true;
    if (_wcsnicmp(cls, L"ThunderRT6RichText", 18) == 0) return true;
    if (_wcsicmp(cls, L"Edit") == 0) return true;
    if (_wcsnicmp(cls, L"RichEdit", 8) == 0) return true;
    return false;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Ctor / dtor / Install / Uninstall
// ─────────────────────────────────────────────────────────────────────
OutputDispatcher::OutputDispatcher(FocusOwner& focus)
    : focus_(focus) {
    // Seed default injector via factory so the field is never null —
    // first-keystroke hot path can dereference unconditionally even
    // before any focus event has fired. Mirror of HookEngine pre-PR-3.3
    // ctor pattern (HookEngine.h:392 RCU init comment).
    injector_.store(NextKey::Output::Create({}), std::memory_order_release);
}

OutputDispatcher::~OutputDispatcher() {
    Uninstall();
}

void OutputDispatcher::Install() noexcept {
    s_instance.store(this, std::memory_order_release);
    NextKey::Output::Internal::g_synthCounterCallback =
        &OutputDispatcher::OnSynthDispatched;
}

void OutputDispatcher::Uninstall() noexcept {
    // Clear callback BEFORE nulling s_instance — otherwise an in-flight
    // TrackedSendInput could dereference s_instance after we clear it.
    NextKey::Output::Internal::g_synthCounterCallback = nullptr;
    if (s_instance.load(std::memory_order_acquire) == this) {
        s_instance.store(nullptr, std::memory_order_release);
    }
}

void OutputDispatcher::OnSynthDispatched(int delta) noexcept {
    auto* self = s_instance.load(std::memory_order_relaxed);
    if (!self) return;
    self->synthEventsPending_.fetch_add(delta, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────
// Injector RCU
// ─────────────────────────────────────────────────────────────────────
void OutputDispatcher::PrepareFocusOutput(FocusClassification& cls) noexcept {
    Output::WindowClassification c{};
    c.isRichEditD2DPT = cls.localEditMsg;
    c.isElectron = cls.localElectronApp;
    c.isConsole = cls.isConsole;
    c.isChromium = cls.localNeedBait;
    c.useClipboard = cls.localUseClipboardInjector;
    c.forcedSplitSleepMs = cls.localForcedSplitSleepMs;
    c.forceEmReplaceSel = cls.localForceEmReplaceSel;
    cls.outputInjector = Output::Create(c);
}

bool OutputDispatcher::ApplyFocusOutput(const FocusClassification& cls,
                                       FocusApplyDisposition disposition,
                                       bool suggestKeepChars) noexcept {
    // A live word only delays its logical settings/reset. Its next edit is
    // already going to the new host, which must not inherit game replay or
    // miss the browser's autocomplete-dismiss prefix.
    if (disposition == FocusApplyDisposition::DropStale || !cls.outputInjector) {
        return false;
    }

    cls.outputInjector->SetSuggestKeepChars(suggestKeepChars);
    SetSkipEmptyChar(cls.localSkipEmpty);
    SetUseClipboardPaste(cls.localClipboard);
    wantsGameReinject_ = cls.localGameReinject;
    SetInjector(cls.outputInjector);
    focus_.InvalidateFocusCache();
    return true;
}

void OutputDispatcher::SetInjector(
        std::shared_ptr<NextKey::Output::IOutputInjector> inj) noexcept {
    injector_.store(std::move(inj), std::memory_order_release);
}

std::shared_ptr<NextKey::Output::IOutputInjector>
OutputDispatcher::GetInjector() const noexcept {
    return injector_.load(std::memory_order_acquire);
}

// Edit-msg sync channel: SettleBudget()==0 means the injector was created for
// an Edit-compatible host (RichEdit / ThunderRT6 / plain Edit).  We re-check
// the focused class because a WinUI3 InputSiteWindowClass (Notepad search bar)
// can share the same injector type but isn't EM_REPLACESEL-compatible.
// NOTE: if a future injector with SettleBudget()==0 is NOT edit-message-based,
// this gate must be updated (or the check moved into the injector itself via
// a virtual like IsMessageBasedReplace()).
bool OutputDispatcher::IsSyncReplaceChannel() const noexcept {
    auto inj = injector_.load(std::memory_order_acquire);
    if (!inj || inj->SettleBudget().count() != 0) return false;

    if (inj->IsMessageBasedReplace() && inj->IsForced()) {
        return true;
    }

    HWND focusedWindow = focus_.CachedFocusedHwnd();
    if (focusedWindow && IsWindow(focusedWindow)) {
        const wchar_t* focusedClass = focus_.CachedFocusedClass().c_str();
        return IsEditCompatibleClass(focusedClass);
    }
    return true;
}

bool OutputDispatcher::ShouldUseClipboard(CodeTable currentTable) const noexcept {
    if (currentTable != CodeTable::Unicode) return false;
    return useClipboardPaste_.load(std::memory_order_acquire);
}

// ─────────────────────────────────────────────────────────────────────
// Low-level SendInput primitives
// ─────────────────────────────────────────────────────────────────────
void OutputDispatcher::SendBackspaceEvents(size_t count) noexcept {
    if (count == 0) return;
    const auto heldMods = NextKey::Output::Internal::QueryHeldModifiers(count, L"");
    std::vector<INPUT> events;
    events.reserve(heldMods.count * 2 + count * 2);
    for (size_t k = 0; k < heldMods.count; ++k) {
        events.push_back(NextKey::Output::Internal::MakeModifierEvent(heldMods.vks[k], true));
    }
    WORD bsScan = static_cast<WORD>(MapVirtualKeyW(VK_BACK, MAPVK_VK_TO_VSC));
    for (size_t i = 0; i < count; ++i) {
        AppendVkEvent(events, VK_BACK, bsScan);
    }
    const auto restoreStart = events.size();
    for (size_t k = 0; k < heldMods.count; ++k) {
        events.push_back(NextKey::Output::Internal::MakeModifierEvent(heldMods.vks[k], false));
    }
    sending_.store(true, std::memory_order_release);
    UINT sent = 0;
    if (!NextKey::Output::Internal::TrackedSendInput(
            events.data(), static_cast<UINT>(events.size()), &sent)) {
        NextKey::Output::Internal::RestoreModifiersAfterPartialSend(heldMods, sent, 0, restoreStart);
    }
    sending_.store(false, std::memory_order_release);
    RecordSynthDispatch();
}

void OutputDispatcher::SendCharEvents(const std::wstring& text) noexcept {
    const auto heldMods = NextKey::Output::Internal::QueryHeldModifiers(0, text);
    std::vector<INPUT> events;
    events.reserve(text.size() * 2 + heldMods.count * 2);
    for (size_t k = 0; k < heldMods.count; ++k) {
        events.push_back(NextKey::Output::Internal::MakeModifierEvent(heldMods.vks[k], true));
    }
    for (wchar_t ch : text) {
        AppendUnicodeEvent(events, ch);
    }
    const auto restoreStart = events.size();
    for (size_t k = 0; k < heldMods.count; ++k) {
        events.push_back(NextKey::Output::Internal::MakeModifierEvent(heldMods.vks[k], false));
    }
    sending_.store(true, std::memory_order_release);
    UINT sent = 0;
    if (!NextKey::Output::Internal::TrackedSendInput(
            events.data(), static_cast<UINT>(events.size()), &sent)) {
        NextKey::Output::Internal::RestoreModifiersAfterPartialSend(heldMods, sent, 0, restoreStart);
    }
    sending_.store(false, std::memory_order_release);
    RecordSynthDispatch();
}

void OutputDispatcher::RecordSynthDispatch() noexcept {
    DWORD now = GetTickCount();
    lastSynthSendTime_ = now;
    lastRealSynthTime_ = now;
}

// ─────────────────────────────────────────────────────────────────────
// ClipboardPaste — modifier release + Ctrl+V simulation
// ─────────────────────────────────────────────────────────────────────
void OutputDispatcher::ClipboardPaste(const std::wstring& text) noexcept {
    if (text.empty()) return;

    if (!SetClipboardText(text)) {
        HOOK_LOG(L"  ClipboardPaste: clipboard failed, fallback to SendInput");
        SendCharEvents(text);
        return;
    }

    // Release held modifiers to prevent Ctrl+Shift+V / Ctrl+Alt+V.
    // Scenario: user triggers macro with '!' (Shift+1) — Shift still held.
    struct ModRelease { WORD vk; WORD scan; bool wasDown; };
    ModRelease mods[] = {
        { VK_SHIFT, static_cast<WORD>(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC)),
          (GetKeyState(VK_SHIFT) & 0x8000) != 0 },
        { VK_MENU,  static_cast<WORD>(MapVirtualKeyW(VK_MENU, MAPVK_VK_TO_VSC)),
          (GetKeyState(VK_MENU) & 0x8000) != 0 },
        { VK_LWIN,  static_cast<WORD>(MapVirtualKeyW(VK_LWIN, MAPVK_VK_TO_VSC)),
          (GetKeyState(VK_LWIN) & 0x8000) != 0 },
        { VK_RWIN,  static_cast<WORD>(MapVirtualKeyW(VK_RWIN, MAPVK_VK_TO_VSC)),
          (GetKeyState(VK_RWIN) & 0x8000) != 0 },
    };

    std::vector<INPUT> preEvents;
    std::vector<INPUT> postEvents;
    for (auto& m : mods) {
        if (m.wasDown) {
            INPUT up{};
            up.type = INPUT_KEYBOARD;
            up.ki.wVk = m.vk;
            up.ki.wScan = m.scan;
            up.ki.dwFlags = KEYEVENTF_KEYUP;
            up.ki.dwExtraInfo = kVKeyExtraInfo;
            preEvents.push_back(up);

            INPUT down{};
            down.type = INPUT_KEYBOARD;
            down.ki.wVk = m.vk;
            down.ki.wScan = m.scan;
            down.ki.dwExtraInfo = kVKeyExtraInfo;
            postEvents.push_back(down);
        }
    }

    // Simulate Ctrl+V — hook proc passes these through (kVKeyExtraInfo marker)
    WORD ctrlScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
    WORD vScan = static_cast<WORD>(MapVirtualKeyW('V', MAPVK_VK_TO_VSC));
    INPUT inputs[4] = {};
    for (auto& in : inputs) {
        in.type = INPUT_KEYBOARD;
        in.ki.dwExtraInfo = kVKeyExtraInfo;
    }
    inputs[0].ki.wVk = VK_CONTROL;  inputs[0].ki.wScan = ctrlScan;
    inputs[1].ki.wVk = 'V';         inputs[1].ki.wScan = vScan;
    inputs[2].ki.wVk = 'V';         inputs[2].ki.wScan = vScan;     inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].ki.wVk = VK_CONTROL;  inputs[3].ki.wScan = ctrlScan;  inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    sending_.store(true, std::memory_order_release);
    if (!preEvents.empty()) {
        (void)NextKey::Output::Internal::TrackedSendInput(
            preEvents.data(), static_cast<UINT>(preEvents.size()));
    }
    (void)NextKey::Output::Internal::TrackedSendInput(inputs, 4);
    if (!postEvents.empty()) {
        (void)NextKey::Output::Internal::TrackedSendInput(
            postEvents.data(), static_cast<UINT>(postEvents.size()));
    }
    sending_.store(false, std::memory_order_release);
    RecordSynthDispatch();

    HOOK_LOG(L"  ClipboardPaste: pasted %zu chars via Ctrl+V (mod-release: %zu)",
             text.size(), preEvents.size());
}

// ─────────────────────────────────────────────────────────────────────
// EM_REPLACESEL fast path — primary VB6/ANSI path
// ─────────────────────────────────────────────────────────────────────
//
// All SendMessage calls use SMTO_ABORTIFHUNG with a 50ms timeout — the
// keyboard hook must never block: a hung target app would otherwise
// freeze every keystroke system-wide.
bool OutputDispatcher::TryEditMessagePaste(
        const std::wstring& text, size_t backspaceCount) noexcept {
    if (text.empty() && backspaceCount == 0) return true;

    // Prefer cached focused HWND (populated in OnFocusChanged / invalidated on mouse click)
    // to avoid AttachThreadInput on every keystroke. Fall back to a fresh query on miss.
    HWND hwnd = focus_.CachedFocusedHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        focus_.RefreshFocusCache(GetForegroundWindow());
        hwnd = focus_.CachedFocusedHwnd();
        if (!hwnd) {
            HOOK_LOG(L"  EditMsgPaste: no focused child hwnd");
            return false;
        }
    }

    const wchar_t* cls = focus_.CachedFocusedClass().c_str();
    if (!IsEditCompatibleClass(cls)) {
        HOOK_LOG(L"  EditMsgPaste: incompatible class='%s'", cls);
        return false;
    }

    constexpr UINT kFlags = SMTO_ABORTIFHUNG | SMTO_NORMAL;
    DWORD_PTR dummy = 0;
    DWORD newStart = 0, selEnd = 0;

    // EM_GETSEL is a query — safe to call before we suppress redraw below.
    if (backspaceCount > 0) {
        DWORD selStart = 0;
        if (!SendMessageTimeoutW(hwnd, EM_GETSEL,
                                 reinterpret_cast<WPARAM>(&selStart),
                                 reinterpret_cast<LPARAM>(&selEnd),
                                 kFlags, kEditMsgTimeoutMs, &dummy)) {
            HOOK_LOG(L"  EditMsgPaste: EM_GETSEL timed out (class='%s')", cls);
            return false;
        }
        if (static_cast<DWORD>(backspaceCount) > selEnd) {
            HOOK_LOG(L"  EditMsgPaste: BS=%zu > caret=%u (class='%s')",
                     backspaceCount, selEnd, cls);
            return false;
        }
        newStart = selEnd - static_cast<DWORD>(backspaceCount);
    }

    // Suppress repaint between EM_SETSEL (highlights selection) and EM_REPLACESEL —
    // otherwise the selection renders as a blue flash before being replaced.
    // Re-enable + InvalidateRect at the end to paint the final text once.
    // erase=FALSE: text controls paint their own background in WM_PAINT — TRUE would
    // cause a brief background-color flash before the text redraws on top.
    // Only re-enable if suppression actually took effect; if the FALSE send timed out
    // the control never entered no-redraw state, so skip the (redundant) TRUE send.
    bool redrawSuppressed = false;
    if (backspaceCount > 0) {
        redrawSuppressed = SendMessageTimeoutW(hwnd, WM_SETREDRAW, FALSE, 0,
                                               kFlags, kEditMsgTimeoutMs, &dummy) != 0;
        if (!SendMessageTimeoutW(hwnd, EM_SETSEL,
                                 static_cast<WPARAM>(newStart),
                                 static_cast<LPARAM>(selEnd),
                                 kFlags, kEditMsgTimeoutMs, &dummy)) {
            HOOK_LOG(L"  EditMsgPaste: EM_SETSEL timed out (class='%s')", cls);
            if (redrawSuppressed) {
                SendMessageTimeoutW(hwnd, WM_SETREDRAW, TRUE, 0,
                                    kFlags, kEditMsgTimeoutMs, &dummy);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return false;
        }
    }

    // wParam=TRUE → operation goes on the undo stack (Ctrl+Z works).
    BOOL replaceOk = SendMessageTimeoutW(hwnd, EM_REPLACESEL,
                                         static_cast<WPARAM>(TRUE),
                                         reinterpret_cast<LPARAM>(text.c_str()),
                                         kFlags, kEditMsgTimeoutMs, &dummy) != 0;

    if (redrawSuppressed) {
        SendMessageTimeoutW(hwnd, WM_SETREDRAW, TRUE, 0,
                            kFlags, kEditMsgTimeoutMs, &dummy);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    if (!replaceOk) {
        HOOK_LOG(L"  EditMsgPaste: EM_REPLACESEL timed out (class='%s')", cls);
        return false;
    }

    HOOK_LOG(L"  EditMsgPaste: class='%s' sel=[%u,%u] BS=%zu text='%s' OK",
             cls, newStart, selEnd, backspaceCount, text.c_str());
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// Dispatch entry points
// ─────────────────────────────────────────────────────────────────────
bool OutputDispatcher::ReplaceRaw(size_t backspaceCount,
                                   std::wstring_view text) noexcept {
    if (backspaceCount == 0 && text.empty()) return true;
    auto inj = injector_.load(std::memory_order_acquire);
    if (!inj) return false;
    sending_.store(true, std::memory_order_release);
    bool injOk;
    {
        PERF_SCOPE(::NextKey::Perf::Stage::Injector);
        injOk = inj->Replace(backspaceCount, text);
    }
    sending_.store(false, std::memory_order_release);
    RecordSynthDispatch();
    return injOk;
}

void OutputDispatcher::InjectKey(std::uint16_t vkCode) noexcept {
    auto inj = injector_.load(std::memory_order_acquire);
    if (!inj) return;
    sending_.store(true, std::memory_order_release);
    inj->SendKey(vkCode);
    sending_.store(false, std::memory_order_release);
    // Watchdog timestamp only — InjectKey is re-injection, not "real"
    // typing. Caller relies on lastRealSynthTime_ staying unchanged
    // (settle budget compares against it).
    lastSynthSendTime_ = GetTickCount();
}

void OutputDispatcher::SendBackspaces(size_t count) noexcept {
    if (count == 0) return;
    // Sprint 2 D2 uniform path: delegate to the active injector. RichEdit
    // class is handled inside RichEditEmReplaceSelInjector; bait-char
    // prefix lives inside Win32SendInputInjector::Replace, gated by
    // needsBaitCharPrefix_.
    (void)ReplaceRaw(count, {});
}

void OutputDispatcher::ReplaceUnicode(size_t backspaceCount,
                                       std::wstring_view text,
                                       std::uint16_t reinjectVk) noexcept {
    // ── Async-render apps (Win11 New Notepad RichEditD2DPT) ──
    // WinUI 3 RichEditBox renders on the compositor thread async to input. SendInput
    // BS+replace arrives a frame too late → suppressed key flashes before replacement.
    // EM_REPLACESEL goes straight into the RichEdit child synchronously → atomic.
    //
    // Burst-input race (chaos 3.3 / 5.2 / 6.1, fixed C/2026-05-05): under sub-1ms
    // inter-key, physical WM_KEYDOWN messages stack up faster than the compositor
    // renders them. The EM_GETSEL caret read inside TryEditMessagePaste sees a stale
    // (low) position so the BS > caret guard refuses the replacement. Fix: when
    // injector reports failure, sleep briefly (hook thread holds back further
    // callbacks while sleeping — no further physical keys race in) then retry.
    // The retry budget is 30 ms. An in-flight synchronous Replace may overrun
    // that budget, so the loop also checks monotonic wall time after each
    // failed attempt and never starts another retry after a slow failure.
    // reinjectVk != 0 skips this branch entirely. HandleAlphaKey only sets it
    // after reading IsSyncReplaceChannel() itself, but that predicate is
    // re-evaluated here and can flip in between (the cached child HWND is
    // destroyed → the focused-window test fails → it now returns true), so
    // "reinject implies not-sync-channel" is not an invariant. Forwarding the
    // VK into the loop below is not the fix either: it retries Replace up to
    // 30 times and would re-inject the raw char on every pass. Falling through
    // to the generic branch lets the active injector resolve it exactly once —
    // RichEditEmReplaceSelInjector already compensates or forwards it.
    if (reinjectVk == 0 && IsSyncReplaceChannel()) {
        HWND focusedWindow = focus_.CachedFocusedHwnd();
        if (!focusedWindow || !IsWindow(focusedWindow)) {
            focus_.RefreshFocusCache(GetForegroundWindow());
            focusedWindow = focus_.CachedFocusedHwnd();
        }

        bool isEditCompatible = false;
        if (focusedWindow) {
            const wchar_t* focusedClass = focus_.CachedFocusedClass().c_str();
            auto activeInjector = injector_.load(std::memory_order_acquire);
            isEditCompatible = IsEditCompatibleClass(focusedClass) || (activeInjector && activeInjector->IsForced());
        }

        bool isReplaced = false;
        if (isEditCompatible) {
            constexpr int kAsyncRenderMaxWaitMs = 30;
            constexpr int kAsyncRenderStepMs    = 1;
            int elapsedWaitTimeMs = 0;
            const auto retryDeadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds{kAsyncRenderMaxWaitMs};
            auto activeInjector = injector_.load(std::memory_order_acquire);
            for (;;) {
                bool isInjectionSuccessful = false;
                {
                    PERF_SCOPE(::NextKey::Perf::Stage::Injector);
                    isInjectionSuccessful = activeInjector->Replace(backspaceCount, text);
                }
                if (isInjectionSuccessful) {
                    if (synthEventsPending_.load(std::memory_order_relaxed) > 0) {
                        hadSynthInWord_ = true;
                    }
                    if (elapsedWaitTimeMs > 0) {
                        HOOK_LOG(L"  ReplaceComposition[editMsg]: caught up after %dms wait",
                                 elapsedWaitTimeMs);
                    }
                    isReplaced = true;
                    break;
                }
                // Keep the existing attempt-count cap, and additionally count
                // time spent inside Replace toward the retry budget. Check only
                // after failure: a successful synchronous edit remains valid
                // even if the target took longer than the retry budget.
                if (elapsedWaitTimeMs >= kAsyncRenderMaxWaitMs
                    || std::chrono::steady_clock::now() >= retryDeadline) {
                    break;
                }
                Sleep(kAsyncRenderStepMs);
                elapsedWaitTimeMs += kAsyncRenderStepMs;
            }
        }

        if (isReplaced) {
            return;
        }

        // If not compatible or if the retry loop is exhausted, we immediately fall back to SendInput.
        if (isEditCompatible) {
            HOOK_LOG(L"  ReplaceComposition[editMsg]: retry exhausted (30ms) — fallback to SendInput BS=%zu",
                     backspaceCount);
        } else if (!focusedWindow) {
            HOOK_LOG(L"  ReplaceComposition[editMsg]: no valid focused window — immediate fallback to SendInput BS=%zu",
                     backspaceCount);
        } else {
            HOOK_LOG(L"  ReplaceComposition[editMsg]: class '%s' not compatible — immediate fallback to SendInput BS=%zu",
                     focus_.CachedFocusedClass().c_str(), backspaceCount);
        }

        // Settle budget intentionally omitted for this fallback path:
        // 1. The edit-msg retry loop above already waited up to 30ms, so the
        //    target app has had time to process pending messages.
        // 2. Adding a full SettleBudget sleep (30-100ms) here risks pushing
        //    total hook latency past the LowLevelHooksTimeout (default 300ms).
        // 3. Win32SendInputInjector's Replace() is a single synchronous
        //    SendInput call — the events enter the input queue atomically.
        sending_.store(true, std::memory_order_release);
        NextKey::Output::Win32SendInputInjector fallbackInjector(false);
        bool isFallbackSuccessful = fallbackInjector.Replace(backspaceCount, text);
        if (!isFallbackSuccessful) {
            HOOK_LOG(L"  ReplaceComposition[editMsg]: fallback injector reported partial delivery");
        }
        sending_.store(false, std::memory_order_release);
        RecordSynthDispatch();

        if (synthEventsPending_.load(std::memory_order_relaxed) > 0) {
            hadSynthInWord_ = true;
        }
        return;
    }

    // ── VB6 / ANSI-internal windows ──
    // ANSI windows can't handle KEYEVENTF_UNICODE (VK_PACKET) — Vietnamese chars become '?'.
    // Primary: EM_REPLACESEL directly into focused Edit/RichEdit/ThunderRT6 child
    // (no clipboard side-effect). Fallback: clipboard paste when target isn't
    // an Edit-compatible class.
    //
    // reinjectVk != 0 semantics: HandleAlphaKey appended originalCh to
    // previousComposition_ for game-compat tracking, but the physical key was
    // blocked and never reached the ANSI window. Subtract 1 BS to compensate.
    // Reinject VK itself is skipped — ANSI desktop apps (XYplorer, etc.) don't
    // need game-style VK re-injection.
    // ReplaceUnicode is invoked only on the Unicode path (HookEngine routes
    // encoded code-tables through ReplaceRaw), so the code-table gate inside
    // ShouldUseClipboard would always pass here — read the raw atomic
    // directly to skip the redundant check.
    if (useClipboardPaste_.load(std::memory_order_acquire)) {
        std::wstring sendBuf(text);
        size_t bsCount = backspaceCount;
        if (reinjectVk != 0 && bsCount > 0) bsCount--;

        if (TryEditMessagePaste(sendBuf, bsCount)) {
            if (synthEventsPending_.load(std::memory_order_relaxed) > 0) {
                hadSynthInWord_ = true;
            }
            return;
        }

        HOOK_LOG(L"  ReplaceComposition[clipboard]: fallback BS=%zu (raw=%zu reinject=0x%X) send='%s'",
                 bsCount, backspaceCount, reinjectVk, sendBuf.c_str());
        if (bsCount > 0) {
            SendBackspaceEvents(bsCount);
            Sleep(8);  // Let app process deletions before clipboard paste
        }
        if (!sendBuf.empty()) {
            ClipboardPaste(sendBuf);
        }
        if (synthEventsPending_.load(std::memory_order_relaxed) > 0) {
            hadSynthInWord_ = true;
        }
        return;
    }

    // ── Generic path (Win32/Electron/Console via active injector) ──
    // Sprint 2 D3: BS+chars dispatch through injector (bait-char prefix +
    // split-with-Sleep live inside the impl, gated on classification flags
    // wired in OnFocusChanged).
    //
    // reinjectVk handling: forwarded into the injector so the VK keydown
    // rides the SAME SendInput batch as the backspaces that delete it. It
    // used to be a separate TrackedSendInput call here, which broke the
    // atomicity HandleAlphaKey assumes when it pre-adds the raw char to
    // previousComposition_ — see IOutputInjector::Replace for the desync
    // that leaks out when only one of the two batches lands.
    HOOK_LOG(L"  ReplaceComposition[send]: BS=%zu toSend='%.*s' skipEmpty=%d synthPending=%d reinjectVk=0x%02X",
             backspaceCount, static_cast<int>(text.size()), text.data(),
             skipEmptyChar_.load(std::memory_order_acquire) ? 1 : 0,
             synthEventsPending_.load(std::memory_order_relaxed), reinjectVk);

    if (backspaceCount > 0 || !text.empty() || reinjectVk != 0) {
        sending_.store(true, std::memory_order_release);

        auto inj = injector_.load(std::memory_order_acquire);
        bool injOk;
        {
            PERF_SCOPE(::NextKey::Perf::Stage::Injector);
            injOk = inj->Replace(backspaceCount, text, reinjectVk);
        }
        if (!injOk) {
            HOOK_LOG(L"  ReplaceComposition[send]: injector reported partial delivery");
        }

        sending_.store(false, std::memory_order_release);
        RecordSynthDispatch();
    }

    if (synthEventsPending_.load(std::memory_order_relaxed) > 0) {
        hadSynthInWord_ = true;
    }
}

}  // namespace NextKey
