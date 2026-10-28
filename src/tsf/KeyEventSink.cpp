// VKey - Key Event Sink Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "stdafx.h"

#include "KeyEventSink.h"

#include <cstdio>

#include "TextService.h"
#include "EngineController.h"
#include "CompositionEditSession.h"
#include "ComUtils.h"
#include "Define.h"
#include "Globals.h"
#include "core/CrashLog.h"
#include "core/MacroCase.h"
#include "core/TsfEditDecision.h"

namespace NextKey {
namespace TSF {

namespace {
// SEH filter for the TSF key-event entry points. Logs the structured-exception
// code (with app version, via CrashLog) then executes the handler. File scope,
// no C++ locals → safe to call from an __except filter expression. Runs in the
// host process — the crash log lands next to the module per CrashLog's path
// logic, giving a breadcrumb for an otherwise-invisible in-host DLL crash.
LONG LogTsfSeh(const wchar_t* where, unsigned long code) noexcept {
    char msg[64];
    _snprintf_s(msg, _TRUNCATE, "TSF SEH structured exception code=0x%08lX", code);
    ::NextKey::CrashLog(where, msg);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool WriteUnicodeClipboard(const std::wstring& text) noexcept {
    if (!OpenClipboard(nullptr)) return false;

    struct ClipboardCloser {
        ~ClipboardCloser() { CloseClipboard(); }
    } closer;

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) return false;

    void* destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        return false;
    }
    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);

    if (!EmptyClipboard() || SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        return false;
    }
    // SetClipboardData owns memory after success.
    return true;
}
}  // namespace

// Convert VK code + lParam to the Unicode character the active layout would produce.
// Uses ToUnicode so it respects US QWERTY, shift state, etc. Returns 0 if not printable.
static wchar_t VkToChar(UINT vk, LPARAM lParam) {
    BYTE keyState[256] = {};
    if (!GetKeyboardState(keyState)) return 0;
    UINT scanCode = (static_cast<UINT>(lParam) >> 16) & 0xFF;
    wchar_t buf[4] = {};
    int result = ToUnicode(vk, scanCode, keyState, buf, 4, 0);
    return (result == 1 && buf[0] != 0) ? buf[0] : 0;
}

// A bare Shift/CapsLock keydown produces no text and no navigation, so the
// "non-handled key with live buffer → commit" branches must skip it: committing
// there silently throws away the raw-keystroke state the Telex escape needs
// (#209 — "Tẻ" + Shift+R stayed "Tẻ" because Shift chopped the composition and
// the follow-up revive could only reseed from the rendered glyphs). Ctrl/Alt/Win
// keep their own deliberate chord commit earlier in both handlers.
static bool IsBareModifierKey(UINT vkCode) noexcept {
    return vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT ||
           vkCode == VK_CAPITAL;
}

// Deferred Enter repost — see the VK_RETURN branch in OnKeyDownImpl for why.
// A plain PostMessage was measured as too early: Chromium batches input per
// ~16ms frame, so the reposted Enter still reached the page in the same batch as
// compositionend. One frame of slack puts it in the next batch.
// ponytail: fixed 30ms, single pending key, no queue — Enter is not autorepeat
// -spammed in practice. If some host still swallows it, raise the constant
// before adding machinery.
constexpr UINT kEnterRepostDelayMs = 30;

namespace {
struct PendingEnterRepost {
    HWND hwnd = nullptr;
    LPARAM lParam = 0;
};
// Thread-local: the timer callback runs on the same UI thread that posted it.
thread_local PendingEnterRepost g_pendingEnterRepost;

VOID CALLBACK RepostEnterTimerProc(HWND, UINT, UINT_PTR idEvent, DWORD) {
    ::KillTimer(nullptr, idEvent);
    const HWND hwnd = g_pendingEnterRepost.hwnd;
    if (hwnd == nullptr) return;
    g_pendingEnterRepost.hwnd = nullptr;
    // The window can be gone by now (focus change, tab closed) — IsWindow keeps
    // the stale Enter from landing somewhere the user never pressed it.
    if (!::IsWindow(hwnd)) return;
    ::PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, g_pendingEnterRepost.lParam);
}
}  // namespace

// Helper function to check if a key is punctuation/number that should trigger commit
static bool IsPunctuationKey(UINT vkCode) {
    // Number keys (0-9)
    if (vkCode >= 0x30 && vkCode <= 0x39) return true;

    // Numpad keys
    if (vkCode >= VK_NUMPAD0 && vkCode <= VK_DIVIDE) return true;

    // OEM keys (punctuation on US keyboard)
    // VK_OEM_1 (;:), VK_OEM_PLUS (=+), VK_OEM_COMMA (,<), VK_OEM_MINUS (-_)
    // VK_OEM_PERIOD (.>), VK_OEM_2 (/?), VK_OEM_3 (`~), VK_OEM_4 ([{)
    // VK_OEM_5 (\|), VK_OEM_6 (]}), VK_OEM_7 ('")
    if (vkCode >= VK_OEM_1 && vkCode <= VK_OEM_3) return true;
    if (vkCode >= VK_OEM_4 && vkCode <= VK_OEM_8) return true;
    if (vkCode == VK_OEM_PLUS || vkCode == VK_OEM_COMMA ||
        vkCode == VK_OEM_MINUS || vkCode == VK_OEM_PERIOD) return true;

    return false;
}

KeyEventSink::KeyEventSink(TextService* pTextService, EngineController* pEngineController)
    : pTextService_(pTextService), pEngineController_(pEngineController) {
}

KeyEventSink::~KeyEventSink() {
    Unadvise();
}

void KeyEventSink::RememberClaimedSpaceKeyDown(
    UINT vk, LPARAM lParam) noexcept {
    constexpr uint32_t kPreviousKeyState = 1u << 30;
    const auto keyData =
        static_cast<uint32_t>(static_cast<uintptr_t>(lParam));
    if (vk == VK_SPACE && (keyData & kPreviousKeyState) == 0) {
        pendingClaimedSpaceVk_ = vk;
    }
}

bool KeyEventSink::Advise(ITfThreadMgr* pThreadMgr) {
    if (pThreadMgr == nullptr) return false;

    HRESULT hr = pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKeystrokeMgr_);
    if (FAILED(hr)) return false;

    hr = pKeystrokeMgr_->AdviseKeyEventSink(
        pTextService_->GetClientId(),
        static_cast<ITfKeyEventSink*>(this),
        TRUE  // Foreground
    );

    if (FAILED(hr)) {
        SafeRelease(pKeystrokeMgr_);
        return false;
    }

    RefreshQuickConvertPreservedKey();
    TSF_LOG(L"KeyEventSink advised");
    return true;
}

void KeyEventSink::Unadvise() {
    isForeground_ = false;
    PublishQuickConvertCapability();
    ClearQuickConvertPreservedKey();
    if (pKeystrokeMgr_) {
        pKeystrokeMgr_->UnadviseKeyEventSink(pTextService_->GetClientId());
        SafeRelease(pKeystrokeMgr_);
    }
    TSF_LOG(L"KeyEventSink unadvised");
}

void KeyEventSink::ClearQuickConvertPreservedKey() noexcept {
    if (pKeystrokeMgr_ != nullptr && quickConvertKeyRegistered_) {
        (void)pKeystrokeMgr_->UnpreserveKey(
            GUID_PreservedKey_QuickConvert, &quickConvertPreservedKey_);
    }
    quickConvertKeyRegistered_ = false;
    quickConvertPreservedKey_ = {};
    registeredQuickConvertHotkey_ = {};
    quickConvertSyncUnsupported_ = false;
    quickConvertSequence_.Reset();
}

void KeyEventSink::PublishQuickConvertCapability() noexcept {
    if (pEngineController_ == nullptr) return;
    const bool ready = isForeground_ && quickConvertKeyRegistered_
        && !quickConvertSyncUnsupported_
        && pEngineController_->IsEnabled() && pEngineController_->IsTsfActive();
    pEngineController_->SetTsfNativeConvertReady(ready);
}

void KeyEventSink::RefreshQuickConvertPreservedKey() {
    if (pKeystrokeMgr_ == nullptr || pEngineController_ == nullptr) return;

    const HotkeyConfig desired = pEngineController_->GetConvertConfig().hotkey;
    if (quickConvertKeyRegistered_ && desired == registeredQuickConvertHotkey_) {
        PublishQuickConvertCapability();
        return;
    }

    pEngineController_->SetTsfNativeConvertReady(false);
    ClearQuickConvertPreservedKey();

    // TSF preserved keys have no Windows-key modifier and require a concrete
    // virtual key. These bindings intentionally remain on the hook backend.
    if (desired.vk == 0 || desired.vk > 0xFFu || desired.win) return;

    TF_PRESERVEDKEY preserved{};
    preserved.uVKey = static_cast<UINT>(desired.vk);
    if (desired.alt) preserved.uModifiers |= TF_MOD_ALT;
    if (desired.ctrl) preserved.uModifiers |= TF_MOD_CONTROL;
    if (desired.shift) preserved.uModifiers |= TF_MOD_SHIFT;

    constexpr wchar_t description[] = L"VKey Quick Convert";
    const HRESULT hr = pKeystrokeMgr_->PreserveKey(
        pTextService_->GetClientId(),
        GUID_PreservedKey_QuickConvert,
        &preserved,
        description,
        static_cast<ULONG>(std::size(description) - 1));
    if (FAILED(hr)) {
        TSF_LOG(L"QuickConvert PreserveKey failed hr=0x%08X", hr);
        return;
    }

    quickConvertPreservedKey_ = preserved;
    registeredQuickConvertHotkey_ = desired;
    quickConvertKeyRegistered_ = true;
    PublishQuickConvertCapability();
}

IFACEMETHODIMP KeyEventSink::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) return E_INVALIDARG;
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfKeyEventSink)) {
        *ppvObj = static_cast<ITfKeyEventSink*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) KeyEventSink::AddRef() {
    return InterlockedIncrement(&refCount_);
}

IFACEMETHODIMP_(ULONG) KeyEventSink::Release() {
    ULONG count = InterlockedDecrement(&refCount_);
    // Note: Do NOT delete this here. Lifetime is managed by unique_ptr in TextService.
    // COM ref counting is maintained for contract compliance only.
    return count;
}

IFACEMETHODIMP KeyEventSink::OnSetFocus(BOOL fForeground) {
    isForeground_ = fForeground != FALSE;
    pendingClaimedSpaceVk_ = 0;
    claimedSpaceKeyUpVk_ = 0;
    if (fForeground) {
        TSF_LOG(L"OnSetFocus: foreground");
        // Re-read SharedState on focus to pick up ENGINE_ENABLED/VIETNAMESE_MODE changes
        if (pEngineController_) {
            pEngineController_->CheckConfigEvent(/*allowMacroDiskRead=*/true);
            pEngineController_->RefreshFlags();
            RefreshQuickConvertPreservedKey();
            // Publish TIP active state — EXE reads this for tray icon sync
            pEngineController_->SetTsfTipActive(true);
            pEngineController_->ClearMacroTracking();
            // Focus change invalidates the commit-undo window — cursor may have
            // moved arbitrarily relative to the cached lastCommit_ text.
            pEngineController_->ResetCommitUndo();
            pEngineController_->ResetMacroUndo();
        }
    } else {
        TSF_LOG(L"OnSetFocus: background");
        if (pEngineController_) {
            PublishQuickConvertCapability();
            pEngineController_->SetTsfTipActive(false);
            pEngineController_->ClearMacroTracking();
            pEngineController_->ResetCommitUndo();
        }
    }
    return S_OK;
}

IFACEMETHODIMP KeyEventSink::OnTestKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (pfEaten == nullptr) return E_INVALIDARG;
    *pfEaten = FALSE;  // fail-safe default
    if (pEngineController_ == nullptr) return S_OK;  // init/deactivate race (C3)
    __try {
        return OnTestKeyDownImpl(pContext, wParam, lParam, pfEaten);
    } __except (LogTsfSeh(L"KeyEventSink::OnTestKeyDown", GetExceptionCode())) {
        *pfEaten = FALSE;  // recovered — pass key through untranslated, host survives
        return S_OK;
    }
}

HRESULT KeyEventSink::OnTestKeyDownImpl(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (pEngineController_->CheckConfigEvent()) {
        RefreshQuickConvertPreservedKey();
    }
    const bool routeToTsf = pEngineController_->RefreshKeyRouting();
    const UINT vk = static_cast<UINT>(wParam);
    if (vk != VK_BACK) pEngineController_->ResetMacroUndo();

    // Drop any translated char cached by a previous OnTestKeyDown whose
    // OnKeyDown pair never fired (rare TSF anomaly).
    lastTranslatedChar_ = 0;
    lastEnglishMacroObservedVk_ = 0;
    lastMacroHandledVk_ = 0;

    if (vk != pendingClaimedSpaceVk_) {
        pendingClaimedSpaceVk_ = 0;
        claimedSpaceKeyUpVk_ = 0;
    }
    if (ShouldSuppressClaimedKeyDown(
            pendingClaimedSpaceVk_, vk,
            static_cast<uint32_t>(static_cast<uintptr_t>(lParam)))) {
        *pfEaten = TRUE;
        return S_OK;
    }

    // The Windows input profile is session-wide, so this DLL is loaded into
    // passive TSF hosts even when only a small per-app list uses VKey's TSF
    // backend. Keep those hosts a true passthrough: context inspection can
    // request synchronous edit sessions and visibly stall shell/game keys
    // such as Win, Alt+Tab and Win+Tab (#250).
    // No Commit() here on purpose: a commit edit session in the test phase
    // races the passed-through printable key (CLAUDE.md "Chrome cursor race").
    // OnKeyDown closes the composition instead — it fires in every host,
    // including the Chromium ones that skip this phase entirely.
    if (!routeToTsf) {
        ClearInactiveKeyState();
        return S_OK;
    }

    // Check if this context blocks input (password, PIN, email fields)
    pEngineController_->CheckContextBlocked(pContext);
    if (pEngineController_->IsContextBlocked()) {
        pEngineController_->ClearMacroTracking();
        *pfEaten = FALSE;
        return S_OK;
    }

    // Intercept VK_BACK for autocomplete suggestion dismissal
    if (wParam == VK_BACK && pEngineController_->HasEngineBuffer() &&
        pEngineController_->IsSuggestKeepCharsEnabled() &&
        pEngineController_->HasNonEmptySelection(pContext)) {
        TSF_LOG(L"OnTestKeyDown: backspace autocomplete suggestion detected -> commit and pass through");
        pEngineController_->Commit(pContext);
        *pfEaten = FALSE;
        lastTestedVk_ = static_cast<UINT>(wParam);
        lastWantKeyResult_ = false;
        return S_OK;
    }

    // Commit-undo invalidation (design 2026-05-17): any key besides BS/ESC during
    // Ready/Primed means the user has moved on — drop the cache to prevent stale
    // restore on a later ESC. Modifier keys (Ctrl/Alt/Win/Shift) also reset; if
    // the user is starting a chord, the undo window is over.
    if (wParam != VK_BACK && wParam != VK_ESCAPE) {
        pEngineController_->OnNonRestoreKey();
    }

    // Safety check: recover from engine/composition desync.
    //
    // State A: engine has buffer but TSF composition was externally ended.
    //          TSF side is already gone — Reset() just drops our orphan
    //          composition pointer. No doc mutation.
    // State B: TSF composition active but our engine is empty. TSF host still
    //          has visible pending composition text. We must properly end the
    //          TSF composition (SetCompositionText + EndComposition via edit
    //          session) or the stale composition lingers until the host ends
    //          it. This does mutate the doc in the test phase — accepted
    //          tradeoff because TerminateComposition-only leaks the composition.
    bool isComposing = pEngineController_->IsComposing();
    bool hasBuffer = pEngineController_->HasEngineBuffer();
    if (!isComposing && hasBuffer) {
        TSF_LOG(L"OnTestKeyDown: desync A (composition gone, buffer=%d) → Reset",
                static_cast<int>(pEngineController_->HasEngineBuffer()));
        pEngineController_->Reset();
    } else if (isComposing && !hasBuffer) {
        TSF_LOG(L"OnTestKeyDown: desync B (composition live, buffer empty) → Commit");
        pEngineController_->Commit(pContext);
    }

    // Check modifiers
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    bool win = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;

    // Modifiers -> commit and pass through
    if (ctrl || alt || win) {
        pEngineController_->ResetMacroUndo();
        if (pEngineController_->HasEngineBuffer()) {
            TSF_LOG(L"OnTestKeyDown: modifier chord vk=0x%02X with live buffer → Commit",
                    static_cast<UINT>(wParam));
            pEngineController_->Commit(pContext);
        }
        *pfEaten = FALSE;
        return S_OK;
    }

    // Esc modal interrupt: two-way decision:
    // 1. If restore-raw is enabled and can restore (live buffer or post-BS primed undo, design 2026-05-17),
    //    restore the user's raw keys (víu → virus) and eat the key (pfEaten = TRUE).
    // 2. Otherwise, end composition verbatim with on-screen text without dictionary
    //    autocorrection (EndCompositionVerbatim) and pass Esc to the host (pfEaten = FALSE).
    // Committing in test phase is the right pattern for action keys (per CLAUDE.md
    // "TSF: commit-in-test-phase allowed for action keys") — no text-insert race.
    // The same branch in OnKeyDown handles Chromium hosts that skip OnTestKeyDown.
    if (wParam == VK_ESCAPE) {
        lastTestedVk_ = 0;
        bool restored = false;
        if (pEngineController_->IsEscRestoreRawEnabled()) {
            if (pEngineController_->HasEngineBuffer()) {
                restored = pEngineController_->CommitRawAndEnd(pContext);
            } else if (pEngineController_->IsCommitUndoPrimed()
                       && pEngineController_->WithinUndoWindow()) {
                restored = pEngineController_->TryRestoreLastCommitRaw(pContext);
            }
        }
        if (!restored && pEngineController_->HasEngineBuffer()) {
            pEngineController_->EndCompositionVerbatim(pContext);
        }
        *pfEaten = restored ? TRUE : FALSE;
        return S_OK;
    }

    // (VK_RETURN falls through to the generic non-handled-key branch below:
    // WantKey returns false for Enter, so with a live buffer the branch commits
    // the composition and passes Enter to the host — search submits, newline
    // inserts, form submits, all on a single press.)

    // Punctuation + active composition → eat the key; OnKeyDown will commit with
    // this char appended (atomic "abc," — no race between EndComposition and the
    // host's default key handling).
    //
    // We only eat when VkToChar succeeds (can reproduce the printable char). For
    // the rare case where it fails (dead key, non-printable punct mapping), fall
    // through without eating — TSF spec forbids mutating the document in the test
    // phase, so no commit here. Composition will resolve via normal flow in
    // OnKeyDown, which may not fire for an un-eaten key; worst case the user sees
    // a momentarily stale composition with an uneaten punct arriving at caret.
    // Acceptable for an edge case essentially never hit on US QWERTY.
    bool isPunctuation = IsPunctuationKey(static_cast<UINT>(wParam));
    if (isPunctuation && pEngineController_->HasEngineBuffer()) {
        bool isEngineDigit = pEngineController_->IsEngineDigitKey(static_cast<UINT>(wParam));
        bool isEngineBracket = pEngineController_->IsEngineBracketKey(static_cast<UINT>(wParam));
        if (!isEngineDigit && !isEngineBracket) {
            wchar_t ch = VkToChar(static_cast<UINT>(wParam), lParam);
            if (Macro::IsTextProducingTrigger(vk, ch)) {
                *pfEaten = TRUE;
                lastTestedVk_ = static_cast<UINT>(wParam);
                lastWantKeyResult_ = true;
                lastTranslatedChar_ = ch;
                return S_OK;
            }
            if (ch == 0) {
                TSF_LOG(L"OnTestKeyDown: VkToChar failed vk=0x%02X, passthrough (no eat)",
                        (UINT)wParam);
            }
            // Fall through to WantKey / normal flow — no doc mutation in test phase.
        }
    }

    bool wantKey = pEngineController_->WantKey(static_cast<UINT>(wParam), true);

    if (pEngineController_->IsEnglishMacroTrackingActive()) {
        if (vk >= 0x41 && vk <= 0x5A) {
            pEngineController_->TrackMacroCharacter(VkToChar(vk, lParam));
            lastEnglishMacroObservedVk_ = vk;
        } else if (vk == VK_BACK) {
            pEngineController_->TrackMacroBackspace();
            lastEnglishMacroObservedVk_ = vk;
        } else if (pEngineController_->IsMacroCommitTrigger(vk)) {
            const wchar_t triggerChar = VkToChar(vk, lParam);
            if (Macro::IsTextProducingTrigger(vk, triggerChar)
                && pEngineController_->WouldExpandMacroTrigger(pContext, vk, triggerChar)) {
                // Printable keys must not change document text during the test
                // phase. Claim the event and cache its translation so OnKeyDown
                // performs the edit without a second stateful ToUnicode call.
                lastTestedVk_ = vk;
                lastWantKeyResult_ = true;
                lastTranslatedChar_ = triggerChar;
                *pfEaten = TRUE;
                return S_OK;
            }
            const auto macroResult = pEngineController_->HandleMacroTrigger(
                pContext, vk, triggerChar);
            lastEnglishMacroObservedVk_ = vk;
            if (macroResult != EngineController::MacroResult::NoMatch) {
                lastMacroHandledVk_ = vk;
                lastMacroHandledEat_ =
                    macroResult == EngineController::MacroResult::ExpandedEatTrigger;
                *pfEaten = lastMacroHandledEat_ ? TRUE : FALSE;
                return S_OK;
            }
        } else {
            pEngineController_->ClearMacroTracking();
        }
    }

    if (wParam == VK_BACK) {
        bool suggestKeep = pEngineController_->IsSuggestKeepCharsEnabled();
        bool notEmpty = false;
        if (pContext != nullptr && suggestKeep) {
            auto* pSession = new SelectionCheckEditSession(pContext, &notEmpty);
            HRESULT hrSession = S_OK;
            HRESULT hr = pContext->RequestEditSession(
                pTextService_->GetClientId(), pSession, TF_ES_SYNC | TF_ES_READ, &hrSession);
            pSession->Release();
            if (SUCCEEDED(hr) && SUCCEEDED(hrSession) && notEmpty) {
                wantKey = false;
            }
        }
        TSF_LOG(L"OnTestKeyDown: VK_BACK wantKey=%d suggestKeep=%d notEmptySelection=%d",
                wantKey, suggestKeep, notEmpty);
    }

    // Commit-undo BS detection (design 2026-05-17): Ready → Primed.
    // When the user just CommitWithChar'd a word (space appended) and then
    // presses BS, they're undoing the just-committed word — not asking to
    // revive it back into composition. Transition state and let BS pass
    // through to the host (host deletes the space). ESC arriving next will
    // restore raw via TryRestoreLastCommitRaw. Second BS in Primed → cancel.
    // Must run BEFORE BackspaceRevive so revive doesn't compete with undo.
    if (wParam == VK_BACK && !pEngineController_->HasEngineBuffer()) {
        if (pEngineController_->HasMacroUndo()) {
            *pfEaten = TRUE;
            lastTestedVk_ = static_cast<UINT>(wParam);
            lastWantKeyResult_ = true;
            return S_OK;
        }
        if (pEngineController_->IsCommitUndoReady()
            && pEngineController_->WithinUndoWindow()) {
            pEngineController_->TransitionUndoReadyToPrimed();
            // Skip BackspaceRevive — user intent is undo, not revive.
            *pfEaten = FALSE;
            lastTestedVk_ = static_cast<UINT>(wParam);
            lastWantKeyResult_ = false;
            return S_OK;
        }
        if (pEngineController_->IsCommitUndoPrimed()) {
            // Second BS — user is now deleting committed body, drop the undo window.
            pEngineController_->ResetCommitUndo();
            // Fall through to BackspaceRevive (it may want to claim this BS).
        }
    }

    // Backspace revive: if engine is empty and cursor is right after a Vietnamese
    // word, claim the BS and re-enter composition in HandleKey. Pre-read the word
    // here (sync edit session) so we can decide whether to eat the key.
    if (!wantKey && wParam == VK_BACK && !pEngineController_->HasEngineBuffer()) {
        if (pEngineController_->PrepareBackspaceRevive(pContext)) {
            wantKey = true;
        }
    }

    // Cache result so OnKeyDown can reuse without calling WantKey again
    lastTestedVk_ = static_cast<UINT>(wParam);
    lastWantKeyResult_ = wantKey;

    // Enter, Tab, and navigation keys pass through, so expand before the
    // generic commit-and-pass branch when a composed macro is pending.
    if (!wantKey && !pEngineController_->IsEnglishMacroTrackingActive()
        && pEngineController_->HasMacroCandidate()
        && pEngineController_->IsMacroCommitTrigger(vk)) {
        const auto macroResult = pEngineController_->HandleMacroTrigger(
            pContext, vk, VkToChar(vk, lParam));
        if (macroResult != EngineController::MacroResult::NoMatch) {
            lastMacroHandledVk_ = vk;
            lastMacroHandledEat_ =
                macroResult == EngineController::MacroResult::ExpandedEatTrigger;
            *pfEaten = lastMacroHandledEat_ ? TRUE : FALSE;
            return S_OK;
        }
    }

    // Non-handled key with active buffer → commit and pass through. Action and
    // navigation keys (arrows, Escape, F-keys, Home/End, Delete) take effect on
    // a single press. Printable keys that could race with commit text are
    // handled by earlier branches (punct above, A-Z/space via HandleKey), so
    // they don't reach here. Bare Shift/CapsLock is excluded — see IsBareModifierKey.
    if (!wantKey && !IsBareModifierKey(vk) && pEngineController_->HasEngineBuffer()) {
        pEngineController_->Commit(pContext);
        *pfEaten = FALSE;
        return S_OK;
    }

    *pfEaten = wantKey ? TRUE : FALSE;
    return S_OK;
}

IFACEMETHODIMP KeyEventSink::OnTestKeyUp(ITfContext* /*pContext*/, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) {
    if (pfEaten == nullptr) return E_INVALIDARG;
    *pfEaten = FALSE;
    if (pEngineController_ == nullptr) return S_OK;  // init/deactivate race (C3)
    // The key-down claim ends here, not in OnKeyUp: a host that skips the
    // non-test key-up phase (mirror of Chromium skipping OnTestKeyDown) would
    // otherwise leave the latch armed and silently eat the next Space press.
    if (pendingClaimedSpaceVk_ == static_cast<UINT>(wParam)) {
        pendingClaimedSpaceVk_ = 0;
        claimedSpaceKeyUpVk_ = static_cast<UINT>(wParam);
        *pfEaten = TRUE;
        return S_OK;
    }
    if (!pEngineController_->RefreshKeyRouting()) return S_OK;
    // Eat keyup for A-Z and Backspace during active composition
    // (prevents apps from seeing keyup without corresponding keydown)
    // Do NOT call WantKey() here — it has side effects (auto-cap state machine)
    if (pEngineController_->IsComposing()) {
        UINT vk = static_cast<UINT>(wParam);
        *pfEaten = (vk >= 0x41 && vk <= 0x5A) || vk == VK_BACK ? TRUE : FALSE;
    } else {
        *pfEaten = FALSE;
    }
    return S_OK;
}

IFACEMETHODIMP KeyEventSink::OnKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (pfEaten == nullptr) return E_INVALIDARG;
    *pfEaten = FALSE;  // fail-safe default
    if (pEngineController_ == nullptr) return S_OK;  // init/deactivate race (C3)
    __try {
        return OnKeyDownImpl(pContext, wParam, lParam, pfEaten);
    } __except (LogTsfSeh(L"KeyEventSink::OnKeyDown", GetExceptionCode())) {
        *pfEaten = FALSE;  // recovered — pass key through untranslated, host survives
        return S_OK;
    }
}

HRESULT KeyEventSink::OnKeyDownImpl(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (pEngineController_->CheckConfigEvent()) {
        RefreshQuickConvertPreservedKey();
    }
    const bool routeToTsf = pEngineController_->RefreshKeyRouting();
    const UINT vk = static_cast<UINT>(wParam);
    // OnTestKeyDown may have just expanded this same trigger. Preserve that
    // snapshot until its cached result is returned below.
    if (vk != VK_BACK && vk != lastMacroHandledVk_) {
        pEngineController_->ResetMacroUndo();
    }

    if (vk != pendingClaimedSpaceVk_) {
        pendingClaimedSpaceVk_ = 0;
        claimedSpaceKeyUpVk_ = 0;
    }
    if (ShouldSuppressClaimedKeyDown(
            pendingClaimedSpaceVk_, vk,
            static_cast<uint32_t>(static_cast<uintptr_t>(lParam)))) {
        *pfEaten = TRUE;
        return S_OK;
    }

    if (!routeToTsf) {
        CommitBeforeInactiveBypass(pContext);
        return S_OK;
    }

    pEngineController_->CheckContextBlocked(pContext);
    if (pEngineController_->IsContextBlocked()) {
        pEngineController_->ClearMacroTracking();
        *pfEaten = FALSE;
        return S_OK;
    }

    // Desync recovery — mirrors OnTestKeyDown. Chromium hosts skip the test
    // phase, so OnKeyDown must self-heal too: every commit guard below checks
    // HasEngineBuffer(), so a stale composition with an empty engine (state B)
    // would survive all of them and the pre-edit gets stuck — Ctrl+A, Enter,
    // arrows all pass through against a live composition (#209 Ctrl+A report).
    // Idempotent in classic hosts: test phase already recovered, both checks
    // are consistent by the time we run.
    {
        bool isComposing = pEngineController_->IsComposing();
        bool hasBuffer = pEngineController_->HasEngineBuffer();
        if (!isComposing && hasBuffer) {
            TSF_LOG(L"OnKeyDown: desync A (composition gone, buffer live) → Reset");
            pEngineController_->Reset();
        } else if (isComposing && !hasBuffer) {
            TSF_LOG(L"OnKeyDown: desync B (composition live, buffer empty) → Commit");
            pEngineController_->Commit(pContext);
        }
    }

    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    bool win = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;

    // Modifiers -> commit and pass through. Mirrors OnTestKeyDown — Chromium
    // hosts skip the test phase, so without this Ctrl+A/C/X hit a live
    // composition (select-all breaks around the pre-edit word). Idempotent in
    // classic hosts: test phase already committed, buffer is empty.
    if (ctrl || alt || win) {
        pEngineController_->ResetMacroUndo();
        if (pEngineController_->HasEngineBuffer()) {
            TSF_LOG(L"OnKeyDown: modifier chord vk=0x%02X with live buffer → Commit",
                    static_cast<UINT>(wParam));
            pEngineController_->Commit(pContext);
        }
        *pfEaten = FALSE;
        return S_OK;
    }

    if (vk == lastMacroHandledVk_) {
        const bool eat = lastMacroHandledEat_;
        lastMacroHandledVk_ = 0;
        lastMacroHandledEat_ = false;
        lastEnglishMacroObservedVk_ = 0;
        if (eat) RememberClaimedSpaceKeyDown(vk, lParam);
        *pfEaten = eat ? TRUE : FALSE;
        return S_OK;
    }

    if (vk == VK_BACK && pEngineController_->HasMacroUndo()) {
        lastTestedVk_ = 0;
        lastTranslatedChar_ = 0;
        lastEnglishMacroObservedVk_ = 0;
        *pfEaten = pEngineController_->UndoMacro(pContext) ? TRUE : FALSE;
        return S_OK;
    }

    if (pEngineController_->IsEnglishMacroTrackingActive()) {
        const bool observedInTest = vk == lastEnglishMacroObservedVk_;
        lastEnglishMacroObservedVk_ = 0;
        if (vk >= 0x41 && vk <= 0x5A) {
            if (!observedInTest) {
                pEngineController_->TrackMacroCharacter(VkToChar(vk, lParam));
            }
            lastTestedVk_ = 0;
            lastTranslatedChar_ = 0;
            *pfEaten = FALSE;
            return S_OK;
        }
        if (vk == VK_BACK) {
            if (!observedInTest) pEngineController_->TrackMacroBackspace();
            lastTestedVk_ = 0;
            lastTranslatedChar_ = 0;
            *pfEaten = FALSE;
            return S_OK;
        }
        if (pEngineController_->IsMacroCommitTrigger(vk)) {
            if (!observedInTest) {
                const wchar_t triggerChar =
                    (vk == lastTestedVk_ && lastTranslatedChar_ != 0)
                        ? lastTranslatedChar_
                        : VkToChar(vk, lParam);
                lastTestedVk_ = 0;
                lastTranslatedChar_ = 0;
                const auto macroResult = pEngineController_->HandleMacroTrigger(
                    pContext, vk, triggerChar);
                if (macroResult != EngineController::MacroResult::NoMatch) {
                    const bool eat =
                        macroResult == EngineController::MacroResult::ExpandedEatTrigger;
                    if (eat) RememberClaimedSpaceKeyDown(vk, lParam);
                    *pfEaten = eat ? TRUE : FALSE;
                    return S_OK;
                }
            } else {
                lastTestedVk_ = 0;
                lastTranslatedChar_ = 0;
            }
            *pfEaten = FALSE;
            return S_OK;
        }
        pEngineController_->ClearMacroTracking();
    }

    // Intercept VK_BACK for autocomplete suggestion dismissal (Chromium fallback)
    if (vk == VK_BACK && pEngineController_->HasEngineBuffer() &&
        pEngineController_->IsSuggestKeepCharsEnabled() &&
        pEngineController_->HasNonEmptySelection(pContext)) {
        TSF_LOG(L"OnKeyDown: backspace autocomplete suggestion detected -> commit and pass through");
        pEngineController_->Commit(pContext);
        lastTestedVk_ = 0;
        lastTranslatedChar_ = 0;
        *pfEaten = FALSE;
        return S_OK;
    }

    // Esc modal interrupt: two-way decision (mirrors OnTestKeyDown):
    // 1. If restore-raw is enabled and can restore (live buffer or post-BS primed undo),
    //    restore raw keys (víu → virus) and eat the key (pfEaten = TRUE).
    // 2. Otherwise, finalize composition verbatim (EndCompositionVerbatim) without
    //    dictionary autocorrection and pass Esc through to the host app (pfEaten = FALSE).
    if (vk == VK_ESCAPE) {
        lastTestedVk_ = 0;
        bool restored = false;
        if (pEngineController_->IsEscRestoreRawEnabled()) {
            if (pEngineController_->HasEngineBuffer()) {
                restored = pEngineController_->CommitRawAndEnd(pContext);
            } else if (pEngineController_->IsCommitUndoPrimed()
                       && pEngineController_->WithinUndoWindow()) {
                restored = pEngineController_->TryRestoreLastCommitRaw(pContext);
            }
        }
        if (!restored && pEngineController_->HasEngineBuffer()) {
            pEngineController_->EndCompositionVerbatim(pContext);
        }
        *pfEaten = restored ? TRUE : FALSE;
        return S_OK;
    }

    // Space and printable punctuation are eaten by the TIP, so expand them
    // here and include any pass-through character in the committed text.
    if (pEngineController_->HasMacroCandidate()
        && pEngineController_->IsMacroCommitTrigger(vk)
        && !pEngineController_->IsEngineDigitKey(vk)
        && !pEngineController_->IsEngineBracketKey(vk)) {
        const wchar_t triggerChar =
            (vk == lastTestedVk_ && lastTranslatedChar_ != 0)
                ? lastTranslatedChar_
                : VkToChar(vk, lParam);
        const auto macroResult = pEngineController_->HandleMacroTrigger(
            pContext, vk, triggerChar);
        if (macroResult != EngineController::MacroResult::NoMatch) {
            const bool eat =
                macroResult == EngineController::MacroResult::ExpandedEatTrigger;
            if (eat) RememberClaimedSpaceKeyDown(vk, lParam);
            *pfEaten = eat ? TRUE : FALSE;
            return S_OK;
        }
    }

    // Punctuation: commit composition with this char appended (atomic, no race).
    // Prefer the char cached by OnTestKeyDown (avoids a second ToUnicode call that
    // could mutate kernel dead-key state on some layouts). Fall back to a direct
    // VkToChar when the cache is empty — some hosts (notably Chromium) skip
    // OnTestKeyDown entirely and route keystrokes straight to OnKeyDown, so the
    // cache never gets populated. Only one ToUnicode call per keystroke either way.
    if (IsPunctuationKey(vk) && pEngineController_->HasEngineBuffer()
        && !pEngineController_->IsEngineDigitKey(vk)
        && !pEngineController_->IsEngineBracketKey(vk)) {
        wchar_t ch = (vk == lastTestedVk_ && lastTranslatedChar_ != 0)
                       ? lastTranslatedChar_
                       : VkToChar(vk, lParam);
        if (Macro::IsTextProducingTrigger(vk, ch)) {
            pEngineController_->CommitWithChar(pContext, ch);
            lastTestedVk_ = 0;
            lastTranslatedChar_ = 0;
            *pfEaten = TRUE;
            return S_OK;
        }
        // Action keys such as Tab intentionally fall through. If VkToChar failed
        // for punctuation, normal flow likewise leaves the key to the host.
        if (ch == 0) {
            TSF_LOG(L"OnKeyDown: VkToChar failed vk=0x%02X, falling through", vk);
        }
    }

    bool wantKey = false;
    if (vk == lastTestedVk_) {
        wantKey = lastWantKeyResult_;
    } else {
        wantKey = pEngineController_->WantKey(vk, true);
        if (pContext != nullptr && wantKey && vk == VK_BACK && pEngineController_->IsSuggestKeepCharsEnabled()) {
            bool notEmpty = false;
            auto* pSession = new SelectionCheckEditSession(pContext, &notEmpty);
            HRESULT hrSession = S_OK;
            HRESULT hr = pContext->RequestEditSession(
                pTextService_->GetClientId(), pSession, TF_ES_SYNC | TF_ES_READ, &hrSession);
            pSession->Release();
            if (SUCCEEDED(hr) && SUCCEEDED(hrSession) && notEmpty) {
                wantKey = false;
            }
        }
    }
    if (vk == VK_BACK) {
        TSF_LOG(L"OnKeyDown: VK_BACK wantKey=%d lastTestedVk=%u lastWantKeyResult=%d",
                wantKey, lastTestedVk_, lastWantKeyResult_);
    }
    lastTestedVk_ = 0;  // Invalidate cache
    lastTranslatedChar_ = 0;

    // Non-handled key with active buffer → commit composition and pass the key
    // through so action keys (Enter submits, Escape cancels, F-keys, arrows,
    // Home/End) take effect on a single press. The "Chrome cursor race" only
    // affects printable keys racing with commit text; those are already handled
    // (punct via the branch above, A-Z/space via HandleKey), so nothing text-
    // inserting reaches this point. Works whether OnTestKeyDown fired or not.
    // Bare Shift/CapsLock is excluded — see IsBareModifierKey.
    if (!wantKey && !IsBareModifierKey(vk) && pEngineController_->HasEngineBuffer()) {
        pEngineController_->Commit(pContext);
        // Reaching here with a live buffer means the host skipped OnTestKeyDown
        // (Chromium family) — in every other host the test phase already
        // committed, so the buffer is empty and this branch never runs there.
        // Consequence for Enter: the composition ends inside the very key event
        // the page is about to receive. Web editors clear their own "composing"
        // flag one tick AFTER compositionend (the standard Safari workaround), so
        // an Enter landing in that same tick is skipped by the send handler and
        // falls through to the editor's default line break — Gemini's composer
        // types a newline instead of sending. Measured on Gemini: compositionend
        // and the Enter keydown were 1 ms apart, and Enter after any other commit
        // trigger (arrow key) sends correctly. Reposting Enter puts it in a later
        // message, i.e. a later renderer task, past that tick.
        // Same-process PostMessage: no UIPI, and low-level hooks don't see it, so
        // a concurrently running HookEngine can't process the key twice. No loop
        // either — the reposted key finds an empty buffer. Only the real WM_KEYUP
        // follows; the app sees one press either way.
        if (vk == VK_RETURN) {
            if (HWND hwndFocus = ::GetFocus()) {
                g_pendingEnterRepost = {hwndFocus, lParam};
                const UINT_PTR timer = ::SetTimer(
                    nullptr, 0, kEnterRepostDelayMs, RepostEnterTimerProc);
                if (timer != 0) {
                    *pfEaten = TRUE;
                    return S_OK;
                }
                g_pendingEnterRepost.hwnd = nullptr;  // timer failed → pass through
            }
        }
        *pfEaten = FALSE;
        return S_OK;
    }

    if (!wantKey) {
        *pfEaten = FALSE;
        return S_OK;
    }

    const bool handled = pEngineController_->HandleKey(pContext, vk);
    if (handled) RememberClaimedSpaceKeyDown(vk, lParam);
    *pfEaten = handled ? TRUE : FALSE;
    return S_OK;
}

IFACEMETHODIMP KeyEventSink::OnKeyUp(ITfContext* /*pContext*/, WPARAM wParam, LPARAM /*lParam*/, BOOL* pfEaten) {
    if (pfEaten == nullptr) return E_INVALIDARG;
    *pfEaten = FALSE;
    if (pEngineController_ == nullptr) return S_OK;  // init/deactivate race (C3)
    const UINT upVk = static_cast<UINT>(wParam);
    if (pendingClaimedSpaceVk_ == upVk || claimedSpaceKeyUpVk_ == upVk) {
        pendingClaimedSpaceVk_ = 0;
        claimedSpaceKeyUpVk_ = 0;
        *pfEaten = TRUE;
        return S_OK;
    }
    if (!pEngineController_->RefreshKeyRouting()) return S_OK;
    if (pEngineController_->IsComposing()) {
        UINT vk = static_cast<UINT>(wParam);
        *pfEaten = (vk >= 0x41 && vk <= 0x5A) || vk == VK_BACK ? TRUE : FALSE;
    } else {
        *pfEaten = FALSE;
    }
    return S_OK;
}

void KeyEventSink::CommitBeforeInactiveBypass(ITfContext* pContext) {
    // Routing can drop mid-word (engine toggled off, app removed from the TSF
    // list, VKey.exe stopped). Before going passthrough, close whatever this
    // TIP still owns — the pre-bypass path used to commit via the
    // non-handled-key branch, and skipping it leaves a pre-edit stuck open
    // with no key able to reach it again. IsComposing() is a local bool, so
    // passive hosts (#250: Explorer, Start/Search, games) never composed and
    // still pay nothing here.
    if (pEngineController_->IsComposing() || pEngineController_->HasEngineBuffer()) {
        pEngineController_->Commit(pContext);
    }
    ClearInactiveKeyState();
}

void KeyEventSink::ClearInactiveKeyState() noexcept {
    lastTestedVk_ = 0;
    lastWantKeyResult_ = false;
    lastTranslatedChar_ = 0;
    lastEnglishMacroObservedVk_ = 0;
    lastMacroHandledVk_ = 0;
    lastMacroHandledEat_ = false;
    pEngineController_->ClearMacroTracking();
}

IFACEMETHODIMP KeyEventSink::OnPreservedKey(ITfContext* pContext, REFGUID rguid, BOOL* pfEaten) {
    if (pfEaten == nullptr) return E_INVALIDARG;
    *pfEaten = FALSE;  // fail-safe default
    if (pEngineController_ == nullptr) return S_OK;
    __try {
        return OnPreservedKeyImpl(pContext, rguid, pfEaten);
    } __except (LogTsfSeh(L"KeyEventSink::OnPreservedKey", GetExceptionCode())) {
        *pfEaten = TRUE;  // the registered VKey chord must not leak to the host
        return S_OK;
    }
}

HRESULT KeyEventSink::OnPreservedKeyImpl(
    ITfContext* pContext, REFGUID rguid, BOOL* pfEaten) {
    if (!IsEqualGUID(rguid, GUID_PreservedKey_QuickConvert)) return S_OK;
    *pfEaten = TRUE;
    if (pContext == nullptr || !isForeground_) return S_OK;

    // A stale preserved-key callback can race a config change. Refreshing first
    // keeps the next press correctly routed; the old binding is consumed once.
    if (pEngineController_->CheckConfigEvent()) {
        RefreshQuickConvertPreservedKey();
        return S_OK;
    }
    if (!quickConvertKeyRegistered_ || !pEngineController_->IsEnabled()
        || !pEngineController_->IsTsfActive()) {
        PublishQuickConvertCapability();
        return S_OK;
    }

    pEngineController_->CheckContextBlocked(pContext);
    if (pEngineController_->IsContextBlocked()) return S_OK;
    if (pEngineController_->HasEngineBuffer()) pEngineController_->Commit(pContext);

    const ConvertConfig config = pEngineController_->GetConvertConfig();
    auto* session = new QuickConvertEditSession(
        pContext, config, &quickConvertSequence_);
    HRESULT sessionResult = E_FAIL;
    const HRESULT requestResult = pContext->RequestEditSession(
        pTextService_->GetClientId(), session,
        TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
    const bool hasResult = session->HasResult();
    const bool replaced = session->Replaced();
    const std::wstring result = session->Result();
    session->Release();

    if (FAILED(requestResult) || FAILED(sessionResult)) {
        TSF_LOG(L"QuickConvert edit session failed request=0x%08X session=0x%08X",
                requestResult, sessionResult);
        quickConvertSequence_.Reset();
        // A host that refuses a synchronous RW lock will keep refusing it, and
        // the chord is already eaten here — the hook can no longer take over on
        // this press. Latch the refusal and withdraw readiness so the next press
        // is routed to the clipboard backend instead of dying silently.
        if (requestResult == TF_E_SYNCHRONOUS) {
            quickConvertSyncUnsupported_ = true;
            PublishQuickConvertCapability();
        }
        return S_OK;
    }

    if (hasResult && !config.autoPaste && !WriteUnicodeClipboard(result)) {
        TSF_LOG(L"QuickConvert clipboard write failed");
    } else if (replaced) {
        TSF_LOG(L"QuickConvert replaced selected text (%zu chars)", result.size());
    }
    return S_OK;
}

}  // namespace TSF
}  // namespace NextKey
