// VKey - Key Event Sink Header
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "stdafx.h"
#include "QuickConvertEditSession.h"

namespace NextKey {
namespace TSF {

class TextService;
class EngineController;

/// Handles keyboard input events from TSF
class KeyEventSink : public ITfKeyEventSink {
public:
    KeyEventSink(TextService* pTextService, EngineController* pEngineController);
    virtual ~KeyEventSink();

    // Connect/disconnect from ITfKeystrokeMgr
    bool Advise(ITfThreadMgr* pThreadMgr);
    void Unadvise();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // ITfKeyEventSink
    IFACEMETHODIMP OnSetFocus(BOOL fForeground) override;
    IFACEMETHODIMP OnTestKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    IFACEMETHODIMP OnTestKeyUp(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    IFACEMETHODIMP OnKeyDown(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    IFACEMETHODIMP OnKeyUp(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    IFACEMETHODIMP OnPreservedKey(ITfContext* pContext, REFGUID rguid, BOOL* pfEaten) override;

private:
    // SEH-wrapped implementations for the two doc-mutating hot paths. The public
    // OnKeyDown / OnTestKeyDown are thin __try/__except wrappers (no unwindable
    // locals) that null-guard pEngineController_ and, on a structured or C++
    // exception in engine / edit-session code, log a crash breadcrumb and fail
    // safe (pass the key through) instead of taking down the host process
    // (Word / Chrome). __try and C++ unwinding can't share one function (C2712).
    HRESULT OnTestKeyDownImpl(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten);
    HRESULT OnKeyDownImpl(ITfContext* pContext, WPARAM wParam, LPARAM lParam, BOOL* pfEaten);
    HRESULT OnPreservedKeyImpl(ITfContext* pContext, REFGUID rguid, BOOL* pfEaten);
    void RememberClaimedSpaceKeyDown(UINT vk, LPARAM lParam) noexcept;
    void RefreshQuickConvertPreservedKey();
    void ClearQuickConvertPreservedKey() noexcept;
    void PublishQuickConvertCapability() noexcept;
    void CommitBeforeInactiveBypass(ITfContext* pContext);
    void ClearInactiveKeyState() noexcept;

    ULONG refCount_ = 1;
    TextService* pTextService_ = nullptr;
    EngineController* pEngineController_ = nullptr;
    ITfKeystrokeMgr* pKeystrokeMgr_ = nullptr;
    TF_PRESERVEDKEY quickConvertPreservedKey_{};
    HotkeyConfig registeredQuickConvertHotkey_{};
    bool quickConvertKeyRegistered_ = false;
    // Latched when the host refuses TF_ES_SYNC — see OnPreservedKeyImpl.
    bool quickConvertSyncUnsupported_ = false;
    bool isForeground_ = false;
    TsfQuickConvertSequenceState quickConvertSequence_;

    // Cached WantKey result from OnTestKeyDown to avoid double state-machine advance
    UINT lastTestedVk_ = 0;
    bool lastWantKeyResult_ = false;
    // Cached printable char from OnTestKeyDown's ToUnicode call. Avoids calling
    // ToUnicode twice per keystroke because it mutates kernel keyboard state.
    wchar_t lastTranslatedChar_ = 0;

    // English-mode macros pass ordinary keys through to the host. These caches
    // keep a TestKeyDown/KeyDown pair from processing the same key twice.
    UINT lastEnglishMacroObservedVk_ = 0;
    UINT lastPassthroughBsObservedVk_ = 0;
    UINT lastMacroHandledVk_ = 0;
    bool lastMacroHandledEat_ = false;

    // A Space committed into the composition is owned by the TIP until its
    // key-up. Some legacy hosts can repeat the initial keydown callback.
    // Two latches, not one: the key-down claim is dropped by the first key-up
    // phase to observe it, while the paired key-up eat lives on separately —
    // one latch would either leak into the next Space press or leave the host
    // seeing a key-up with no key-down.
    UINT pendingClaimedSpaceVk_ = 0;
    UINT claimedSpaceKeyUpVk_ = 0;
};

}  // namespace TSF
}  // namespace NextKey
