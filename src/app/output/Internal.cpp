// src/app/output/Internal.cpp
//
// Definitions for the test seams declared in Internal.h. Pointers init
// to real Win32 APIs at static-init time.
#include "Internal.h"

namespace NextKey::Output::Internal {

namespace {

HKL QueryNativePrefixLayout() noexcept {
    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr) return nullptr;
    const DWORD foregroundThread = ::GetWindowThreadProcessId(foreground, nullptr);
    if (foregroundThread == 0) return nullptr;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (::GetGUIThreadInfo(foregroundThread, &info) == FALSE || info.hwndFocus == nullptr) {
        return nullptr;
    }
    const DWORD focusedThread = ::GetWindowThreadProcessId(info.hwndFocus, nullptr);
    return focusedThread != 0 ? ::GetKeyboardLayout(focusedThread) : nullptr;
}

struct NativePrefixKey {
    WORD vk{};
    WORD scan{};
};

NativePrefixKey SelectNativePrefix() noexcept {
    HKL layout = nullptr;
    for (const WORD vk : {kNativePrintablePrefixVk, WORD{'B'}, WORD{'C'}}) {
        if (IsInFlightKeyDown(vk) || (g_getAsyncKeyState(vk) & 0x8000) != 0) continue;
        if (layout == nullptr) {
            layout = g_nativePrefixLayout();
            if (layout == nullptr) return {};
            // An IME can compose native letters even when ToUnicodeEx maps
            // them to ASCII. Do not assume one deletable unit on CJK layouts.
            const WORD language = PRIMARYLANGID(LOWORD(reinterpret_cast<ULONG_PTR>(layout)));
            if (language == LANG_JAPANESE || language == LANG_CHINESE || language == LANG_KOREAN) {
                return {};
            }
        }
        const auto scan = static_cast<WORD>(::MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, layout));
        if (scan == 0) continue;
        BYTE state[256]{};
        bool valid = true;
        for (const BYTE caps : {BYTE{0}, BYTE{1}}) {
            state[VK_CAPITAL] = caps;
            wchar_t text[4]{};
            // Flag 0x4 preserves the keyboard buffer/dead-key state (Win10+).
            const int length = g_toUnicodeEx(vk, scan, state, text, 4, 0x4, layout);
            if (length != 1 || (text[0] != vk && text[0] != vk + (L'a' - L'A'))) {
                valid = false;
                break;
            }
        }
        if (valid) return {vk, scan};
    }
    return {};
}

}  // namespace

SendInputFn           g_sendInput           = ::SendInput;
GetAsyncKeyStateFn    g_getAsyncKeyState    = ::GetAsyncKeyState;
SendMessageWFn        g_sendMessageW        = ::SendMessageW;
SendMessageTimeoutWFn g_sendMessageTimeoutW = ::SendMessageTimeoutW;
SleepFn               g_sleep               = ::Sleep;
SynthCounterFn        g_synthCounterCallback = nullptr;
SynthEventsDispatchedFn g_synthEventsDispatchedCallback = nullptr;
SynthEventsRollbackFn   g_synthEventsRollbackCallback   = nullptr;
NativePrefixLayoutFn g_nativePrefixLayout  = QueryNativePrefixLayout;
ToUnicodeExFn        g_toUnicodeEx          = ::ToUnicodeEx;
thread_local std::uint8_t g_inFlightPrefixKeys = 0;

SuggestionDismissalPlan PrepareSuggestionDismissal(
        SuggestionDismissalPolicy policy, std::size_t bsCount,
        std::wstring_view text, bool suggestKeepChars) noexcept {
    auto plan = PlanSuggestionDismissal(policy, bsCount, text, suggestKeepChars);
    if (plan.prefixVk == 0) return plan;
    const auto key = SelectNativePrefix();
    if (key.vk == 0) {
        return PlanSuggestionDismissal(SuggestionDismissalPolicy::None,
                                       bsCount, text, suggestKeepChars);
    }
    plan.prefixVk = key.vk;
    plan.prefixScan = key.scan;
    return plan;
}

bool TrackedSendInput(INPUT* events, UINT count, UINT* sentCount) noexcept {
    // Pre-increment the synth counter BEFORE SendInput. The Win32
    // callback fires synchronously during SendInput on the same thread —
    // if we incremented after, the LL hook proc could decrement the
    // counter for our just-dispatched events before we'd added them,
    // leaving synthEventsPending_ negative-leaning until watchdog reset.
    if (g_synthCounterCallback && count > 0) {
        g_synthCounterCallback(static_cast<int>(count));
    }
    if (g_synthEventsDispatchedCallback && count > 0) {
        g_synthEventsDispatchedCallback(events, count);
    }
    UINT sent = g_sendInput(count, events, sizeof(INPUT));
    if (sentCount != nullptr) *sentCount = sent;
    if (sent < count) {
        if (g_synthCounterCallback) {
            // Compensate: the (count - sent) events never reached the OS
            // queue, so they won't round-trip through the hook to balance
            // the pre-increment. Subtract them out to keep the counter true.
            g_synthCounterCallback(-static_cast<int>(count - sent));
        }
        if (g_synthEventsRollbackCallback) {
            // Issue 4: Rollback unsent events from the echo buffer so they
            // do not linger and falsely match external injected events.
            g_synthEventsRollbackCallback(events + sent, count - sent);
        }
    }
    return sent == count;
}

}  // namespace NextKey::Output::Internal
