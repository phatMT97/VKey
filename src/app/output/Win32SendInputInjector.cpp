// src/app/output/Win32SendInputInjector.cpp
//
// D1 implementation. Batch SendInput. Stack-buffer std::array (no heap
// alloc on hot path). Bait-char prefix for Chromium autocomplete-dismiss
// quirk (replicates HookEngine::SendBackspaces line ~3121).
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

INPUT MakeKeyEvent(WORD vk, bool keyup,
                   ULONG_PTR extraInfo = Internal::kVKeyExtraInfo) noexcept {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = keyup ? KEYEVENTF_KEYUP : 0u;
    if (vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN) {
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    in.ki.dwExtraInfo = extraInfo;
    return in;
}

INPUT MakeUnicodeChar(WCHAR ch, bool keyup,
                      ULONG_PTR extraInfo = Internal::kVKeyExtraInfo) noexcept {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = static_cast<WORD>(ch);
    in.ki.dwFlags = KEYEVENTF_UNICODE | (keyup ? KEYEVENTF_KEYUP : 0u);
    in.ki.dwExtraInfo = extraInfo;
    return in;
}

}  // namespace

bool Win32SendInputInjector::Replace(std::size_t bsCount,
                                     std::wstring_view text,
                                     unsigned short reinjectVk) noexcept {
    std::array<INPUT, kMaxBatch> buf{};
    std::size_t i = 0;

    // Game-compat re-inject follows modifier releases in the batch (see IOutputInjector::Replace).
    // Key-down only: a sustained hold keeps sending downs to the game, and
    // the physical key-up passes through the hook on release.
    const std::size_t reinjectCount = (reinjectVk != 0) ? 1u : 0u;

    // Keep the entire synthetic replacement modifier-neutral: physical modifiers
    // (Shift, Ctrl, Alt, Win) turn synthetic VK_BACK into destructive editor
    // shortcuts (Shift+BS in Excel Web #225, Ctrl+BS word/line delete #267,
    // Alt+BS Undo). Release held modifiers at the front of the batch and restore
    // them at the tail before the hook resumes physical input.
    const auto heldMods = Internal::QueryHeldModifiers(
        bsCount, reinjectCount && text.empty() ? std::wstring_view{L"x"} : text);
    for (std::size_t k = 0; k < heldMods.count; ++k) {
        if (i + 1 > kMaxBatch) return false;
        buf[i++] = MakeKeyEvent(heldMods.vks[k], /*keyup=*/true);
    }
    // Bait-char prefix (Chromium suggest-dismiss): inserts U+202F + an
    // extra BS to delete it before the rest of the deletes/chars run.
    // Predicate in Internal::ShouldEmitBait — pure-BS only skips bait
    // when the user opts into the "BS giữ chữ khi có gợi ý" setting.
    if (reinjectCount != 0) {
        buf[i++] = MakeKeyEvent(static_cast<WORD>(reinjectVk), /*keyup=*/false);
    }
    const bool emitBait = Internal::ShouldEmitBait(
        needsBaitCharPrefix_, bsCount, text,
        suggestKeepChars_.load(std::memory_order_acquire));
    if (emitBait) {
        if (i + 2 > kMaxBatch) return false;
        buf[i++] = MakeUnicodeChar(0x202F, /*keyup=*/false);
        buf[i++] = MakeUnicodeChar(0x202F, /*keyup=*/true);
        ++bsCount;  // extra BS to delete the bait char
    }

    // Backspace events.
    for (std::size_t k = 0; k < bsCount; ++k) {
        if (i + 2 > kMaxBatch) return false;
        buf[i++] = MakeKeyEvent(VK_BACK, /*keyup=*/false);
        buf[i++] = MakeKeyEvent(VK_BACK, /*keyup=*/true);
    }

    // Char events.
    for (WCHAR ch : text) {
        if (i + 2 > kMaxBatch) return false;
        buf[i++] = MakeUnicodeChar(ch, /*keyup=*/false);
        buf[i++] = MakeUnicodeChar(ch, /*keyup=*/true);
    }
    for (std::size_t k = 0; k < heldMods.count; ++k) {
        if (i + 1 > kMaxBatch) return false;
        buf[i++] = MakeKeyEvent(heldMods.vks[k], /*keyup=*/false);
    }

    if (i == 0) return true;  // nothing to do (bsCount=0, text empty)

    UINT sent = 0;
    const bool delivered = Internal::TrackedSendInput(
        buf.data(), static_cast<UINT>(i), &sent);
    if (!delivered && heldMods.count > 0 && sent > 0) {
        Internal::RestoreModifiersAfterPartialSend(heldMods, sent, 0, i - heldMods.count);
    }
    return delivered;
}

void Win32SendInputInjector::SendKey(unsigned short vkCode) noexcept {
    INPUT events[2] = {
        MakeKeyEvent(static_cast<WORD>(vkCode), /*keyup=*/false),
        MakeKeyEvent(static_cast<WORD>(vkCode), /*keyup=*/true),
    };
    // Fire-and-forget; partial-send detection logged by TrackedSendInput
    // but not actionable for SendKey (re-inject single key has no clean
    // fallback — the original use case at HookEngine line ~905 is
    // re-inject after synth-pending, not a primary delivery).
    (void)Internal::TrackedSendInput(events, 2);
}

}  // namespace NextKey::Output
