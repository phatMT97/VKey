// VKey - Engine Controller Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "stdafx.h"

#include "EngineController.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "CompositionEditSession.h"
#include "Define.h"
#include "EscRestoreLastCommitSession.h"
#include "Globals.h"
#include "InputScopeChecker.h"
#include "core/DigitLedWordDecision.h"
#include "core/MacroCase.h"
#include "core/MacroContextMatch.h"
#include "core/MacroPrefix.h"
#include "core/MacroTableDecision.h"
#include "core/TsfEditDecision.h"
#include "core/TsfPromotionDecision.h"
#include "core/config/ConfigManager.h"
#include "core/config/LexiconTransaction.h"
#include "core/engine/EngineFactory.h"
#ifdef VKEY_USE_RUST_ENGINE
#include "core/engine/RustInputEngine.h"
#endif

namespace {

constexpr std::size_t kMaxRawMacroBuffer = 128;
constexpr std::size_t kMacroClipboardThreshold = 200;

bool IsCompartmentSet(ITfCompartmentMgr* manager, REFGUID guid) {
    if (manager == nullptr) return false;
    CComPtr<ITfCompartment> compartment;
    if (FAILED(manager->GetCompartment(guid, &compartment)) || !compartment) {
        return false;
    }
    CComVariant value;
    // S_FALSE/VT_EMPTY means unset, not a disabled keyboard. RAII also clears
    // unexpected variant types returned by a host.
    return compartment->GetValue(&value) == S_OK
        && value.vt == VT_I4 && value.lVal != 0;
}

class TsfCaseMapper final : public NextKey::Macro::CaseMapper {
public:
    void Upper(wchar_t* buffer, std::size_t count) const override {
        if (buffer != nullptr && count != 0) {
            CharUpperBuffW(buffer, static_cast<DWORD>(count));
        }
    }

    void Lower(wchar_t* buffer, std::size_t count) const override {
        if (buffer != nullptr && count != 0) {
            CharLowerBuffW(buffer, static_cast<DWORD>(count));
        }
    }
};

bool DecodeWireExclusions(
    const NextKey::Wire::LexiconWireView& view,
    std::vector<std::wstring>& out) {
    out.clear();
    if (view.exclusionsUnits == 0) return view.exclusionsRowCount == 0;

    const auto* chars = reinterpret_cast<const char16_t*>(view.exclusionsBuf);
    size_t rowStart = 0;
    for (size_t i = 0; i < view.exclusionsUnits; ++i) {
        if (chars[i] != u'\n') continue;
        out.push_back(NextKey::Wire::U16ToWString(
            std::u16string_view(chars + rowStart, i - rowStart)));
        rowStart = i + 1;
    }
    return rowStart == view.exclusionsUnits && out.size() == view.exclusionsRowCount;
}

}  // namespace
namespace NextKey {
namespace TSF {

EngineController::EngineController(ITfThreadMgr* pThreadMgr) {
    // Context compartments are documented for these gates; Microsoft's
    // SampleIME also consults the thread manager. Honor either owner setting a
    // gate, and never let an unset/false value overwrite another owner's true.
    if (pThreadMgr != nullptr) {
        (void)pThreadMgr->QueryInterface(
            IID_ITfCompartmentMgr, reinterpret_cast<void**>(&threadCompartments_));
    }
    // Try to open SharedState from main app (read-write for flag toggling)
    if (sharedState_.OpenReadWrite()) {
        // Step 1: ABI check — seqlock-protected header read. CheckConfigEvent
        // re-evaluates a Retry/failed result because an older creator may have
        // exposed magic before finishing this header, or Create() may still be
        // mid-reinit.
        const auto abiResult = sharedState_.CheckAbiCompatibility();
        if (abiResult == SharedStateManager::AbiCheckResult::Incompatible) {
            abiOk_ = false;
            sharedState_.SetOrClearFlag(SharedFlags::TSF_ABI_MISMATCH, true);
            activeConfig_.inputMethod = InputMethod::Telex;
            activeConfig_.spellCheckEnabled = false;
            activeConfig_.optimizeLevel = 0;
            activeMethod_ = InputMethod::Telex;
            engine_ = EngineFactory::Create(activeConfig_);
            TSF_LOG(L"EngineController: SharedState ABI mismatch — passthrough");
        } else if (abiResult == SharedStateManager::AbiCheckResult::Retry) {
            // Seqlock contention, not a confirmed mismatch — do NOT raise the
            // sticky TSF_ABI_MISMATCH banner for this. CheckConfigEvent retries.
            abiOk_ = false;
            activeConfig_.inputMethod = InputMethod::Telex;
            activeConfig_.spellCheckEnabled = false;
            activeConfig_.optimizeLevel = 0;
            activeMethod_ = InputMethod::Telex;
            engine_ = EngineFactory::Create(activeConfig_);
            TSF_LOG(L"EngineController: SharedState ABI check contention, using defaults");
        } else {
            // Step 2: ABI OK; try a seqlock Read for the full config.
            abiOk_ = true;
            SharedState state = sharedState_.Read();
            if (state.IsValid()) {
                ApplySharedState(state, /*allowMacroDiskRead=*/true);
                lastEpoch_ = state.epoch;
                TSF_LOG(L"EngineController initialized from SharedState (epoch=%u, method=%d)",
                        state.epoch, state.inputMethod);
            } else {
                // Seqlock exhausted under contention — use defaults for now.
                // RefreshFlags / CheckConfigEvent will re-read on next focus.
                // Do NOT flip TSF_ABI_MISMATCH — ABI is fine.
                activeConfig_.inputMethod = InputMethod::Telex;
                activeConfig_.spellCheckEnabled = false;
                activeConfig_.optimizeLevel = 0;
                activeMethod_ = InputMethod::Telex;
                engine_ = EngineFactory::Create(activeConfig_);
                TSF_LOG(L"EngineController: SharedState read contention, using defaults");
            }
        }
    } else {
        // SharedState not available = EXE not running → disabled
        activeConfig_.inputMethod = InputMethod::Telex;
        activeConfig_.spellCheckEnabled = false;
        activeConfig_.optimizeLevel = 0;
        activeMethod_ = InputMethod::Telex;
        engine_ = EngineFactory::Create(activeConfig_);
        engineEnabled_ = false;
        TSF_LOG(L"EngineController: SharedState not available, engine disabled");
    }

    compositionMgr_.SetEngineController(this);
#ifdef VKEY_USE_RUST_ENGINE
    wireReaderActive_ = wireReader_.Open();
    if (wireReaderActive_) {
        TSF_LOG(L"EngineController: LexiconWireReader opened");
    }
#endif
}

EngineController::~EngineController() {
#ifdef VKEY_USE_RUST_ENGINE
    wireReader_.Close();
    wireReaderActive_ = false;
#endif
    if (lastContext_) {
        lastContext_->Release();
        lastContext_ = nullptr;
    }
    ClearPendingRevive();
    TSF_LOG(L"EngineController destroyed");
}

void EngineController::ClearPendingRevive() {
    pendingReviveRange_.Release();
    pendingReviveWord_.clear();
}

bool EngineController::PrepareBackspaceRevive(ITfContext* pContext) {
    ClearPendingRevive();
    if (pContext == nullptr) return false;

    // Gate: only when engine is fully enabled AND Vietnamese mode AND engine empty.
    if (!engineEnabled_ || !tsfActive_ || !vietnameseMode_) return false;
    if (contextBlocked_) return false;
    if (engine_->Count() > 0) return false;
    // Scintilla (Notepad++) doesn't support ITfRange backward scan — skip to avoid
    // a guaranteed-to-fail sync edit session per BS.
    if (isScintillaApp_) return false;

    // Step 1: READ preceding word via sync edit session.
    auto* pSession = new ReadPrecedingWordEditSession(pContext);
    HRESULT hrSession = S_OK;
    HRESULT hr = pContext->RequestEditSession(
        clientId_, pSession, TF_ES_SYNC | TF_ES_READ, &hrSession);

    std::wstring word;
    CComPtr<ITfRange> pRange;
    if (SUCCEEDED(hr) && SUCCEEDED(hrSession) && pSession->Found()) {
        word = pSession->Word();
        pRange.Attach(pSession->DetachRange());  // ownership transfer, no extra AddRef
    }
    pSession->Release();

    if (!pRange || word.empty()) return false;

    // Step 2: English-word gate via a throwaway engine (don't mutate engine_ —
    // safety resets at the top of each OnTestKeyDown would wipe it).
    auto tempEngine = EngineFactory::Create(activeConfig_);
    if (!tempEngine || !tempEngine->SeedFromText(word) || tempEngine->IsEnglishWord()) {
        TSF_LOG(L"PrepareBackspaceRevive: '%ls' rejected (not Vietnamese)", word.c_str());
        return false;  // pRange auto-Released
    }

    // Step 3: Cache {word, range} for HandleKey(VK_BACK). Engine_ stays empty.
    pendingReviveWord_ = std::move(word);
    pendingReviveRange_ = pRange;  // CComPtr = CComPtr → AddRefs (local copy stays valid)
    TSF_LOG(L"PrepareBackspaceRevive: armed for '%ls'", pendingReviveWord_.c_str());
    return true;
}

void EngineController::CheckContextBlocked(ITfContext* pContext) {
    static_assert(
        static_cast<uint32_t>(TF_SD_READONLY) == kTsfReadOnlyDocumentFlag,
        "pure TSF status decision must match the Windows SDK");
    static_assert(
        static_cast<uint32_t>(TF_SS_TRANSITORY) == kTsfTransitoryDocumentFlag,
        "pure TSF status decision must match the Windows SDK");

    const bool contextChanged = pContext != lastContext_;
    if (contextChanged) {
        // Cache scope inspection and compartment owners, not compartment
        // values: disabled/empty can change without a different ITfContext.
        contextCompartments_.Release();
        if (lastContext_) lastContext_->Release();
        lastContext_ = pContext;
        if (lastContext_) lastContext_->AddRef();
        scopeBlocked_ = false;

        if (pContext) {
            (void)pContext->QueryInterface(
                IID_ITfCompartmentMgr,
                reinterpret_cast<void**>(&contextCompartments_));
            auto* pSession = new InputScopeCheckSession(pContext, &scopeBlocked_);
            HRESULT hrSession = S_OK;
            HRESULT hr = pContext->RequestEditSession(
                clientId_, pSession, TF_ES_SYNC | TF_ES_READ, &hrSession);
            pSession->Release();

            if (FAILED(hr) || FAILED(hrSession)) {
                // If the scope cannot be checked, fail open so normal typing is
                // not disabled. The read-only status gate still runs below.
                scopeBlocked_ = false;
            }
        }

    }

    const bool contextDisabled = IsCompartmentSet(
        contextCompartments_, GUID_COMPARTMENT_KEYBOARD_DISABLED);
    const bool threadDisabled = IsCompartmentSet(
        threadCompartments_, GUID_COMPARTMENT_KEYBOARD_DISABLED);
    const bool contextEmpty = IsCompartmentSet(
        contextCompartments_, GUID_COMPARTMENT_EMPTYCONTEXT);
    const bool threadEmpty = IsCompartmentSet(
        threadCompartments_, GUID_COMPARTMENT_EMPTYCONTEXT);
    TF_STATUS status{};
    HRESULT hrStatus = E_POINTER;
    if (pContext) {
        hrStatus = pContext->GetStatus(&status);
        if (FAILED(hrStatus)) status = {};
    }
    const TsfContextInputState inputState{
        .hasContext = pContext != nullptr,
        .dynamicStatusFlags = status.dwDynamicFlags,
        .staticStatusFlags = status.dwStaticFlags,
        .keyboardDisabled = contextDisabled || threadDisabled,
        .emptyContext = contextEmpty || threadEmpty,
        .scopeBlocked = scopeBlocked_,
    };
    const bool wasBlocked = contextBlocked_;
    contextBlocked_ = ShouldBlockTsfContext(inputState);

    // Transitions and context switches only — this runs per keystroke, and a
    // line per key buries the one thing a reader needs. Both flags and the
    // focused window class go on the line that reports the verdict: which field
    // the user was in, what the host claimed about it, and what VKey did with
    // it, without needing a second line to correlate against. A host that
    // reports these flags differently than the ones checked in #242 shows up
    // as "typing stopped working" with no other trace.
    //
    // The `!verdictLogged_` arm is what makes a user-supplied log usable: a
    // reporter turns debug logging on from the tray while already focused in the
    // field they are about to type in, so the context never changes afterwards
    // and the verdict line would never appear (#242's Legcord log has zero of
    // them). Emit once for the context in hand, then go back to transitions.
    const bool logOn = ::NextKey::Logger::IsEnabled();
    if (!logOn) verdictLogged_ = false;
    if (logOn && (contextBlocked_ != wasBlocked || contextChanged || !verdictLogged_)) {
        verdictLogged_ = true;
        wchar_t focusClass[64] = {};
        ::GetClassNameW(::GetFocus(), focusClass, 64);
        TSF_LOG(L"Context %ls: focus='%ls' status=0x%08lX dyn=0x%08lX static=0x%08lX "
                L"context_disabled=%d thread_disabled=%d context_empty=%d thread_empty=%d",
                !contextBlocked_ ? L"open"
                : !inputState.hasContext ? L"blocked (no context)"
                : IsReadOnlyTsfDocument(status.dwDynamicFlags) ? L"blocked (read-only document)"
                : inputState.keyboardDisabled ? L"blocked (keyboard disabled)"
                : inputState.emptyContext ? L"blocked (empty context)"
                : scopeBlocked_  ? L"blocked (password/PIN/email field)"
                                 : L"blocked",
                focusClass, hrStatus, status.dwDynamicFlags, status.dwStaticFlags,
                contextDisabled, threadDisabled, contextEmpty, threadEmpty);
    }
}

bool EngineController::WantKey(UINT vkCode, bool /*isKeyDown*/) {
    if (!RefreshKeyRouting()) return false;
    if (!vietnameseMode_) return false;

    // Auto-cap is now driven by ShouldAutoCapitalize() which peeks the document
    // on each A-Z keystroke — no keystroke-history state machine needed.

    // 1. NEVER intercept if any modifier (except Shift) is down.
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    bool win = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;

    if (ctrl || alt || win) {
        return false;
    }

    bool engineHasComp = engine_->Count() > 0;

    // 1b. Digit-led word state machine — shared with HookEngine via
    // core/DigitLedWordDecision.h. Arms when a digit lands at word start
    // (VNI/Combined/UserDefined), bypasses subsequent keys, resets on
    // whitespace/nav/Esc/BS/Delete. Modifiers (Ctrl/Alt/Win) handled by
    // the early-return at line 192 above — never reach this state machine.
    {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        DigitLedInputs in{vkCode, shift, !engineHasComp, activeConfig_.inputMethod, digitLedWord_};
        switch (DecideDigitLed(in)) {
            case DigitLedDecision::Arm:    digitLedWord_ = true;  return false;
            case DigitLedDecision::Bypass:                        return false;
            case DigitLedDecision::Reset:  digitLedWord_ = false; return false;
            case DigitLedDecision::ResetAndContinue:
                digitLedWord_ = false;
                break;
            case DigitLedDecision::Continue: break;
        }
    }

    // 2. We want A-Z keys for typing processing
    if (vkCode >= 0x41 && vkCode <= 0x5A) {
        return true;
    }

    // 2b. Full Telex and applicable UserDefined bracket actions. Shift is an
    // uppercase request, not a reason to pass `{`/`}` through to the host.
    if (IsEngineBracketKey(vkCode)) {
        return true;
    }

    // 3. VNI/Combined/UserDefined: digit keys 0-9 for tone/modifier (only with pending composition)
    if (IsEngineDigitKey(vkCode) && engineHasComp) {
        return true;
    }

    // 4. We handle Backspace ONLY if we have internal content.
    if (vkCode == VK_BACK) {
        if (HasMacroUndo()) return true;
        return engineHasComp;
    }

    // 5. Space handling depends on the app
    if (vkCode == VK_SPACE) {
        if (isScintillaApp_) {
            // For Scintilla apps: don't claim space, let it trigger commit via "non-handled key" path
            // and pass through naturally
            return false;
        }
        // For other apps: claim space if we have composition
        return engineHasComp;
    }

    // 6. For Enter and all others, let the app handle it (we'll commit in OnTestKeyDown)
    return false;
}

bool EngineController::RefreshKeyRouting() noexcept {
    // ABI mismatch or an absent EXE mapping means strict passthrough. This is
    // intentionally only a shared-memory read: passive TSF hosts (Explorer,
    // Start/Search, games) call this on the input path and must not pay for
    // context inspection when they are not assigned to the TSF backend.
    if (!abiOk_ || !sharedState_.IsConnected()) return false;

    const uint32_t flags = sharedState_.ReadFlags();
    const bool newEngineEnabled = (flags & SharedFlags::ENGINE_ENABLED) != 0;
    if (newEngineEnabled != engineEnabled_) {
        engineEnabled_ = newEngineEnabled;
        ClearMacroTracking();
    }
    if (!engineEnabled_) return false;

    const bool newTsfActive = (flags & SharedFlags::TSF_ACTIVE) != 0;
    if (newTsfActive != tsfActive_) {
        tsfActive_ = newTsfActive;
        ClearMacroTracking();
        TSF_LOG(L"[Checkpoint] TSF_ACTIVE: %s",
                tsfActive_ ? L"ON (processing keys)" : L"OFF (passthrough)");
    }
    if (!tsfActive_) return false;

    const bool newVietnameseMode = (flags & SharedFlags::VIETNAMESE_MODE) != 0;
    if (newVietnameseMode != vietnameseMode_) {
        vietnameseMode_ = newVietnameseMode;
        ClearMacroTracking();
        if (langBarButton_) langBarButton_->Refresh();
    }
    return true;
}

void EngineController::RequestEditSession(ITfContext* pContext, EditSession* pEditSession) {
    if (pContext == nullptr || pEditSession == nullptr) return;

    HRESULT hrSession = S_OK;
    HRESULT hr = pContext->RequestEditSession(
        clientId_,
        pEditSession,
        TF_ES_SYNC | TF_ES_READWRITE,
        &hrSession
    );

    if (FAILED(hr)) {
        TSF_LOG(L"RequestEditSession request failed: request=0x%08X session=0x%08X",
                hr, hrSession);
    } else if (FAILED(hrSession)) {
        TSF_LOG(L"Edit session execution failed: request=0x%08X session=0x%08X",
                hr, hrSession);
    }
}

bool EngineController::IsEngineBracketKey(UINT vkCode) const {
    if (vkCode != VK_OEM_4 && vkCode != VK_OEM_6) return false;
    if (activeConfig_.inputMethod == InputMethod::Telex) return true;
    if (activeConfig_.inputMethod != InputMethod::UserDefined) return false;

    const wchar_t base = vkCode == VK_OEM_4 ? L'[' : L']';
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool capsLock = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
    const wchar_t physical = ResolveBracketKey(base, shift, capsLock).character;
    TypingAction action = activeConfig_.customKeyMap[static_cast<uint8_t>(physical)];
    if (action == TypingAction::None && physical != base) {
        action = activeConfig_.customKeyMap[static_cast<uint8_t>(base)];
    }
    return action != TypingAction::None &&
           (engine_->Count() > 0 || IsInsertTypeAction(action));
}

BracketKey EngineController::ResolveEngineBracketKey(UINT vkCode) const {
    const wchar_t base = vkCode == VK_OEM_4 ? L'[' : L']';
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool capsLock = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
    return ResolveBracketKey(base, shift, capsLock);
}

bool EngineController::PushEngineKey(ITfContext* pContext, wchar_t ch, bool uppercase) {
    engine_->PushKey(ch, uppercase);
    const std::wstring composition = engine_->Peek();

    if (!compositionMgr_.IsComposing()) {
        auto* pSession = new StartCompositionEditSession(
            pContext, &compositionMgr_, composition);
        RequestEditSession(pContext, pSession);
        pSession->Release();

        if (!compositionMgr_.IsComposing()) {
            TSF_LOG(L"PushEngineKey: composition failed to start, resetting engine");
            engine_->Reset();
            return false;
        }
    } else {
        auto* pSession = new UpdateCompositionEditSession(
            pContext, &compositionMgr_, composition);
        RequestEditSession(pContext, pSession);
        pSession->Release();
    }
    return true;
}

bool EngineController::HandleKey(ITfContext* pContext, UINT vkCode) {
    // 1. Handle Backspace (only if we have content, as decided by WantKey)
    if (vkCode == VK_BACK) {
        if (HasMacroUndo()) {
            return UndoMacro(pContext);
        }
        // Revive path: engine was pre-seeded in OnTestKeyDown via
        // PrepareBackspaceRevive (and passed the English-word gate). Wire up
        // composition covering the cached range, then apply backspace.
        if (HasPendingRevive()) {
            auto* pSession = new ReviveCompositionEditSession(
                pContext, &compositionMgr_, engine_.get(),
                pendingReviveWord_, pendingReviveRange_,
                MatchingRawForCommittedWord(pendingReviveWord_));
            RequestEditSession(pContext, pSession);
            pSession->Release();
            ClearPendingRevive();
            return true;
        }
        ProcessBackspace(pContext);
        return true;
    }

    // 2. Handle Space (only reaches here for non-Scintilla apps, as decided by WantKey)
    if (vkCode == VK_SPACE) {
        // For non-Scintilla apps: append space to committed text
        CommitWithChar(pContext, L' ');
        return true;  // Eat space
    }

    // 3. Text-producing engine keys. Brackets share the A-Z boundary path so
    // type-revive and sentence auto-cap apply consistently, but their physical
    // punctuation identity remains separate from output-case intent.
    const bool isBracket = IsEngineBracketKey(vkCode);
    if (isBracket || (vkCode >= 0x41 && vkCode <= 0x5A)) {
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool capsLock = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        bool upper = shift != capsLock;
        wchar_t ch = 0;
        if (isBracket) {
            const BracketKey key = ResolveEngineBracketKey(vkCode);
            ch = key.character;
            upper = key.uppercase;
        } else {
            ch = static_cast<wchar_t>(vkCode);
            if (!upper) ch = towlower(ch);
        }
        const wchar_t rawCh = ch;

        // At a new-composition boundary: first try Type-revive (extend an
        // existing Vietnamese word before caret), and if that doesn't fire,
        // apply auto-capitalize based on document state.
        //   "tét gõ|" + 'f' → "tét [gò]"       (revive wins, no auto-cap)
        //   "b.|" + 'a' → "b.A"                (auto-cap)
        //   empty doc + 'a' → "A"              (auto-cap: cursor at doc start)
        if (engine_->Count() == 0 && !compositionMgr_.IsComposing() && !isScintillaApp_) {
            // Single READ session for both revive check and auto-cap check
            auto* pInspect = new InspectPrecedingTextEditSession(pContext);
            HRESULT hrSession = S_OK;
            pContext->RequestEditSession(clientId_, pInspect, TF_ES_SYNC | TF_ES_READ, &hrSession);

            std::wstring word = pInspect->Word();
            CComPtr<ITfRange> wordRange;
            wordRange.Attach(pInspect->DetachWordRange());
            bool shouldAutoCap = pInspect->ShouldAutoCap();
            pInspect->Release();

            // Try revive if a seedable word is found. No English-word gate here
            // (unlike PrepareBackspaceRevive): continuing to type into an English
            // word is exactly when the engine needs the whole word in its buffer.
            // Blocking it left the tail alone in a fresh buffer, so English
            // protection couldn't see the prefix and the tone key landed on it —
            // "test" + "er" → "testẻ" instead of "tester". The BS path keeps its
            // gate: reviving there is followed by Backspace(), which re-renders a
            // raw replay as Vietnamese ("tester" → "tết").
            if (!word.empty() && wordRange) {
                auto tempEngine = EngineFactory::Create(activeConfig_);
                if (tempEngine && tempEngine->SeedFromText(word)) {
                    // Raw replay over glyph-seeding: SeedFromText loses which key
                    // produced which diacritic, so a tone key pressed right after
                    // the revive can't escape it (#209 Shift+R: "Tẻ" + R stayed
                    // "Tẻ" under the Rust engine instead of escaping to "TeR").
                    auto* pRevive = new ReviveAndTypeEditSession(
                        pContext, &compositionMgr_, engine_.get(), word, wordRange, ch, upper,
                        MatchingRawForCommittedWord(word));
                    RequestEditSession(pContext, pRevive);
                    // A refused revive (host wouldn't compose over committed text,
                    // or TF_ES_SYNC was denied) leaves the document untouched, so
                    // the eaten key still has to land. Falling through — rather
                    // than typing it inside the session — keeps auto-cap and
                    // macro tracking on the same code path as any other keystroke.
                    const bool revived = pRevive->Revived();
                    pRevive->Release();
                    if (revived) {
                        TSF_LOG(L"HandleKey: revive '%ls' + '%lc'", word.c_str(), ch);
                        return true;
                    }
                    TSF_LOG(L"HandleKey: revive of '%ls' refused, typing '%lc' alone",
                            word.c_str(), ch);
                }
            }

            // Auto-cap if revive didn't happen
            if (activeConfig_.autoCaps && shouldAutoCap) {
                if (rawMacroBuffer_.empty() && !upper) {
                    wasFirstCharAutoCapped_ = true;
                }
                upper = true;
                if (!isBracket) ch = towupper(ch);
                TSF_LOG(L"HandleKey: auto-cap → '%lc'", ch);
            }
            TrackMacroCharacter(rawCh);
        } else {
            TrackMacroCharacter(ch);
        }
        TSF_LOG(L"HandleKey: pushing char '%c' upper=%d", ch, upper);
        return PushEngineKey(pContext, ch, upper);
    }

    // 4. VNI/Combined/UserDefined: digit keys 0-9 → push to engine, update composition
    if (IsEngineDigitKey(vkCode) && engine_->Count() > 0) {
        wchar_t ch = static_cast<wchar_t>(vkCode);  // VK '0'-'9' = 0x30-0x39 = L'0'-L'9'
        TSF_LOG(L"HandleKey: pushing VNI digit '%c'", ch);
        engine_->PushChar(ch);

        std::wstring composition = engine_->Peek();
        if (compositionMgr_.IsComposing()) {
            auto* pSession = new UpdateCompositionEditSession(pContext, &compositionMgr_, composition);
            RequestEditSession(pContext, pSession);
            pSession->Release();
        }
        return true;
    }

#ifdef VKEY_USE_RUST_ENGINE
    if (engineNeedsRecreate_ || pendingUserDictionary_) {
#else
    if (engineNeedsRecreate_) {
#endif
        TryPromotePendingConfig();
    }
    return false;
}

void EngineController::ProcessBackspace(ITfContext* pContext) {
    TrackMacroBackspace();
    engine_->Backspace();

    if (engine_->Count() > 0) {
        std::wstring composition = engine_->Peek();
        auto* pSession = new UpdateCompositionEditSession(pContext, &compositionMgr_, composition);
        RequestEditSession(pContext, pSession);
        pSession->Release();
    } else {
        // Composition empty - clear text and end composition (delete all chars)
        auto* pSession = new CommitEditSession(pContext, &compositionMgr_, L"");
        RequestEditSession(pContext, pSession);
        pSession->Release();
        TSF_LOG(L"Backspace: composition cleared");
        TryPromotePendingConfig();
    }
}

void EngineController::Commit(ITfContext* pContext) {
    // VietType pattern: Get committed text, then request edit session, then reset engine.
    // This ensures TSF state and engine state are synchronized atomically.
    //
    // PeekRaw BEFORE engine_->Commit() — Commit() internally calls Reset() which
    // clears escRawHistory_ (see TelexEngineTest.EscRestoreRaw_PeekRawClearedByCommit).
    std::wstring rawSnapshot = engine_->PeekRaw();
    std::wstring committed = engine_->Commit();
    if (engine_->LastCommitWasCorrected()) {
        TSF_LOG(L"Commit: lexicon-corrected, text='%ls'", committed.c_str());
    }

    // Commit: set final text and end composition in one atomic operation
    auto* pSession = new CommitEditSession(pContext, &compositionMgr_, committed);
    RequestEditSession(pContext, pSession);
    pSession->Release();

    // Reset engine state AFTER the edit session completes (synchronous)
    engine_->Reset();
    digitLedWord_ = false;
    ClearMacroTracking();

    TSF_LOG(L"Commit called, text='%ls'", committed.c_str());

    // Plain Commit (no trailing char) → no undo window. Caller is Enter/arrow/F-key.
    RecordCommitSnapshot(std::move(committed), std::move(rawSnapshot), /*hasTrailingChar=*/false);
    TryPromotePendingConfig();
}

void EngineController::CommitWithChar(ITfContext* pContext, wchar_t appendChar) {
    // VietType pattern: Get committed text, then request edit session, then reset engine.
    //
    // PeekRaw BEFORE engine_->Commit() — see Commit() comment above.
    std::wstring rawSnapshot = engine_->PeekRaw();
    // Uncorrected commit == the engine's rendered text; Commit() clears it.
    const bool verbatim = Macro::IsVerbatimCommitTrigger(appendChar);
    std::wstring rendered = verbatim ? engine_->Peek() : std::wstring{};
    std::wstring committed = engine_->Commit();
    if (engine_->LastCommitWasCorrected()) {
        TSF_LOG(L"CommitWithChar: lexicon-corrected, text='%ls' verbatim=%d",
                committed.c_str(), verbatim ? 1 : 0);
        if (verbatim) committed = std::move(rendered);
    }

    // Append the commit character (e.g., space) if provided
    if (appendChar != L'\0') {
        committed += appendChar;
    }

    // Commit: set final text and end composition in one atomic operation
    auto* pSession = new CommitEditSession(pContext, &compositionMgr_, committed);
    RequestEditSession(pContext, pSession);
    pSession->Release();

    // Reset engine state AFTER the edit session completes (synchronous)
    engine_->Reset();
    digitLedWord_ = false;
    ClearMacroTrackingAfterCommit();

    TSF_LOG(L"CommitWithChar called, text='%ls'", committed.c_str());

    // CommitWithChar always appends a trigger (space) — opens undo window.
    // `committed` already includes the appended char (set above).
    const bool hasTrailing = (appendChar != L'\0');
    RecordCommitSnapshot(std::move(committed), std::move(rawSnapshot), hasTrailing);
    TryPromotePendingConfig();
}

bool EngineController::CommitRawAndEnd(ITfContext* pContext) {
    std::wstring raw = engine_->PeekRaw();
    if (raw.empty()) return false;

    auto* pSession = new CommitEditSession(pContext, &compositionMgr_, raw);
    RequestEditSession(pContext, pSession);
    pSession->Release();

    engine_->Reset();
    digitLedWord_ = false;
    ClearMacroTracking();

    TSF_LOG(L"CommitRawAndEnd: raw='%ls'", raw.c_str());
    TryPromotePendingConfig();
    return true;
}

void EngineController::EndCompositionVerbatim(ITfContext* pContext) {
    std::wstring verbatim = engine_->Peek();
    // CommitEditSession::DoEditSession skips SetCompositionText when
    // CurrentTextEquals, so it degrades to a pure EndComposition (preventing
    // #234 formatting loss) while safely repairing any Peek/composition text drift.
    auto* pSession = new CommitEditSession(pContext, &compositionMgr_, verbatim);
    RequestEditSession(pContext, pSession);
    pSession->Release();

    engine_->Reset();
    digitLedWord_ = false;
    ClearMacroTracking();
    ResetCommitUndo();

    TSF_LOG(L"EndCompositionVerbatim: verbatim='%ls'", verbatim.c_str());
    TryPromotePendingConfig();
}

bool EngineController::HasNonEmptySelection(ITfContext* pContext) {
    if (pContext == nullptr) return false;
    bool hasSelection = false;
    auto* pSession = new SelectionCheckEditSession(pContext, &hasSelection);
    HRESULT hrSession = S_OK;
    HRESULT hr = pContext->RequestEditSession(
        clientId_, pSession, TF_ES_SYNC | TF_ES_READ, &hrSession);
    pSession->Release();
    if (FAILED(hr) || FAILED(hrSession)) {
        return false;
    }
    return hasSelection;
}

void EngineController::Reset() {
    ClearMacroTracking();
    engine_->Reset();
    compositionMgr_.TerminateComposition();
    digitLedWord_ = false;
    TryPromotePendingConfig();
}

bool EngineController::IsMacroTrackingEnabled() const noexcept {
    if (!abiOk_ || contextBlocked_ || !activeConfig_.macroEnabled || macroTable_.empty()
        || !sharedState_.IsConnected()) {
        return false;
    }
    const uint32_t flags = sharedState_.ReadFlags();
    const bool liveVietnameseMode = (flags & SharedFlags::VIETNAMESE_MODE) != 0;
    return (flags & SharedFlags::ENGINE_ENABLED) != 0
        && (flags & SharedFlags::TSF_ACTIVE) != 0
        && (liveVietnameseMode || activeConfig_.macroInEnglish);
}

bool EngineController::IsEnglishMacroTrackingActive() const noexcept {
    if (!abiOk_ || contextBlocked_ || !activeConfig_.macroInEnglish
        || !activeConfig_.macroEnabled || macroTable_.empty()
        || !sharedState_.IsConnected()) {
        return false;
    }
    const uint32_t flags = sharedState_.ReadFlags();
    return (flags & SharedFlags::ENGINE_ENABLED) != 0
        && (flags & SharedFlags::TSF_ACTIVE) != 0
        && (flags & SharedFlags::VIETNAMESE_MODE) == 0;
}

bool EngineController::IsMacroCommitTrigger(UINT vkCode) const noexcept {
    return Macro::IsCommitTrigger(vkCode);
}

bool EngineController::HasMacroCandidate() const noexcept {
    return IsMacroTrackingEnabled()
        && (!rawMacroBuffer_.empty() || (engine_ && engine_->Count() > 0));
}

void EngineController::ClearMacroTracking() noexcept {
    ResetMacroUndo();
    tempMacroOff_ = false;
    rawMacroBuffer_.clear();
    wasFirstCharAutoCapped_ = false;
    macroCrossCommit_ = false;
}

void EngineController::ClearMacroTrackingAfterCommit() noexcept {
    if (!macroCrossCommit_) ClearMacroTracking();
}

void EngineController::TrackMacroCharacter(wchar_t ch) {
    if (!IsMacroTrackingEnabled() || ch == 0) return;

    rawMacroBuffer_ += ch;
    if (rawMacroBuffer_.size() > kMaxRawMacroBuffer) ClearMacroTracking();
}

void EngineController::TrackMacroBackspace() noexcept {
    if (!IsMacroTrackingEnabled() || rawMacroBuffer_.empty()) return;

    rawMacroBuffer_.pop_back();
    if (rawMacroBuffer_.empty()) ClearMacroTracking();
}

bool EngineController::ReplacePrecedingText(ITfContext* pContext,
                                             std::size_t characterCount,
                                             const std::wstring& replacement,
                                             const std::wstring& expected) {
    if (pContext == nullptr || characterCount == 0) return false;

    bool replaced = false;
    auto* pSession = new ReplacePrecedingTextEditSession(
        pContext, characterCount, replacement, &replaced, expected);
    HRESULT hrSession = S_OK;
    HRESULT hr = pContext->RequestEditSession(
        clientId_, pSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
    pSession->Release();

    if (!replaced) {
        TSF_LOG(L"ReplacePrecedingText: failed (request=0x%08X, session=0x%08X)",
                hr, hrSession);
        return false;
    }
    if (FAILED(hr) || FAILED(hrSession)) {
        // SetText already succeeded, so the trigger must stay consumed even if
        // collapsing the range or restoring the caret failed afterward.
        TSF_LOG(L"ReplacePrecedingText: text replaced; caret update failed "
                L"(request=0x%08X, session=0x%08X)", hr, hrSession);
    }
    return true;
}

std::wstring EngineController::ReadPrecedingTextFromContext(ITfContext* pContext,
                                                            LONG maxChars) const {
    if (pContext == nullptr || isScintillaApp_) return {};
    auto* pSession = new ReadPrecedingCharsEditSession(pContext, maxChars);
    HRESULT hrSession = S_OK;
    HRESULT hr = pContext->RequestEditSession(
        clientId_, pSession, TF_ES_SYNC | TF_ES_READ, &hrSession);
    std::wstring text;
    if (SUCCEEDED(hr) && SUCCEEDED(hrSession)) {
        text = pSession->Text();
    }
    pSession->Release();
    return text;
}

std::optional<Macro::ContextMatch> EngineController::LookupMacroInContext(
    ITfContext* pContext, wchar_t triggerChar) const {
    if (pContext == nullptr || macroTable_.empty() || maxMacroKeyLen_ == 0) {
        return std::nullopt;
    }
    // Reading one char more than the longest key lets the boundary guard see
    // the character in front of a full-length candidate.
    const LONG want = static_cast<LONG>(
        (std::min)(maxMacroKeyLen_ + 1, static_cast<std::size_t>(64)));
    const std::wstring text = ReadPrecedingTextFromContext(pContext, want);
    if (text.empty()) return std::nullopt;   // unsupported host, or caret at doc start

    const TsfCaseMapper caseMapper;
    return Macro::MatchInPrecedingText(text, triggerChar, macroTable_, maxMacroKeyLen_,
                                       activeConfig_.autoCapsMacro, kMacroClipboardThreshold,
                                       caseMapper, activeConfig_.macroTitleCase);
}

Macro::MacroPlan EngineController::EvaluateMacroPlan(const std::wstring& rawBuffer,
                                                    wchar_t triggerChar) const {
    const std::wstring previousComposition = engine_ ? engine_->Peek() : std::wstring{};
    if (rawBuffer.empty() && previousComposition.empty()) return {};

    const std::vector<uint8_t> encodedWidths;
    const TsfCaseMapper caseMapper;
    const Macro::PlanInputs inputs{
        .rawMacroBuffer = rawBuffer,
        .previousComposition = previousComposition,
        .previousEncodedWidths = encodedWidths,
        .macroTable = macroTable_,
        .macroCrossCommit = macroCrossCommit_,
        // TSF writes Unicode through ITfRange, so match document character counts.
        .currentCodeTable = CodeTable::Unicode,
        .autoCapsEnabled = activeConfig_.autoCapsMacro,
        .titleCaseEnabled = activeConfig_.macroTitleCase,
        .wasFirstCharAutoCapped = wasFirstCharAutoCapped_,
        .triggerChar = triggerChar,
        .clipboardThreshold = kMacroClipboardThreshold,
    };
    return Macro::Plan(inputs, caseMapper);
}

bool EngineController::WouldExpandMacroTrigger(ITfContext* pContext,
                                                UINT vkCode,
                                                wchar_t triggerChar) const {
    if (tempMacroOff_) return false;
    if (!Macro::IsCommitTrigger(vkCode) || !IsMacroTrackingEnabled()) return false;
    if (!Macro::ShouldTrigger(vkCode, activeConfig_.macroTriggerSpace, activeConfig_.macroTriggerEnter,
                              activeConfig_.macroTriggerTab, activeConfig_.macroTriggerDir)) {
        return false;
    }

    std::wstring candidate = rawMacroBuffer_;
    if (triggerChar > L' ') {
        candidate += triggerChar;
        if (candidate.size() > kMaxRawMacroBuffer) return false;
    }
    if (EvaluateMacroPlan(candidate, triggerChar).matched) return true;

    return LookupMacroInContext(pContext, triggerChar).has_value();
}

EngineController::MacroResult EngineController::HandleMacroTrigger(
    ITfContext* pContext, UINT vkCode, wchar_t triggerChar) {
    if (!Macro::IsCommitTrigger(vkCode)) return MacroResult::NoMatch;
    if (tempMacroOff_) {
        ClearMacroTracking();
        return MacroResult::NoMatch;
    }
    if (!IsMacroTrackingEnabled()) {
        ClearMacroTracking();
        return MacroResult::NoMatch;
    }

    if (!Macro::ShouldTrigger(vkCode, activeConfig_.macroTriggerSpace, activeConfig_.macroTriggerEnter,
                              activeConfig_.macroTriggerTab, activeConfig_.macroTriggerDir)) {
        if (vkCode == VK_SPACE && !rawMacroBuffer_.empty()) {
            rawMacroBuffer_.push_back(L' ');
            const bool keep = IsSpaceMacroPrefix(rawMacroBuffer_, macroPrefixIndex_);
            rawMacroBuffer_.pop_back();
            if (keep) {
                rawMacroBuffer_ += L' ';
                macroCrossCommit_ = true;
                return MacroResult::NoMatch;
            }
        }
        ClearMacroTracking();
        return MacroResult::NoMatch;
    }

    // The trigger character has to stay in the tracked buffer: punctuation can
    // be part of a shortcut, so a later boundary must still be able to match it.
    // Only extend an already-live candidate — a trigger pressed with nothing
    // tracked must not seed the buffer, or the context fallback below becomes
    // the only thing that can ever clear it again.
    const bool haveTrackedCandidate =
        !rawMacroBuffer_.empty() || (engine_ && engine_->Count() > 0);
    if (haveTrackedCandidate && triggerChar > L' ') {
        rawMacroBuffer_ += triggerChar;
        if (rawMacroBuffer_.size() > kMaxRawMacroBuffer) {
            ClearMacroTracking();
            return MacroResult::NoMatch;
        }
    }

    // Primary path: the tracked buffer still mirrors the document.
    Macro::MacroPlan plan = EvaluateMacroPlan(rawMacroBuffer_, triggerChar);

    // Fallback: rebuild the candidate from document text before the caret, for
    // when tracking drifted (composition interrupted, characters deleted,
    // shortcut re-typed). The shortcut is committed text at that point, so the
    // expansion must replace document characters rather than a composition.
    if (!plan.matched) {
        if (auto match = LookupMacroInContext(pContext, triggerChar)) {
            plan = match->plan;
            macroCrossCommit_ = true;
        }
    }

    if (!plan.matched) {
        if (vkCode == VK_SPACE && !rawMacroBuffer_.empty()) {
            rawMacroBuffer_.push_back(L' ');
            const bool keep = IsSpaceMacroPrefix(rawMacroBuffer_, macroPrefixIndex_);
            rawMacroBuffer_.pop_back();
            if (keep) {
                rawMacroBuffer_ += L' ';
                macroCrossCommit_ = true;
                return MacroResult::NoMatch;
            }
        } else if (triggerChar > L' '
                   && !(IsEngineDigitKey(vkCode) && engine_ && engine_->Count() > 0)) {
            // A later boundary can still match a punctuation-containing key.
            macroCrossCommit_ = true;
            return MacroResult::NoMatch;
        }

        ClearMacroTracking();
        return MacroResult::NoMatch;
    }

    const std::wstring previousComposition = engine_ ? engine_->Peek() : std::wstring{};
    std::wstring replacement = Macro::ExpandEscapesForClipboard(plan.expansion);
    bool eatTrigger = plan.isPartOfMacro;
    if (!eatTrigger && (vkCode == VK_SPACE || triggerChar > L' ')) {
        replacement += (vkCode == VK_SPACE) ? L' ' : triggerChar;
        eatTrigger = true;
    }

    bool replaced = false;
    if (macroCrossCommit_ || !compositionMgr_.IsComposing()) {
        if (compositionMgr_.IsComposing()) {
            auto* pSession = new CommitEditSession(pContext, &compositionMgr_, previousComposition);
            RequestEditSession(pContext, pSession);
            pSession->Release();
            engine_->Reset();
            digitLedWord_ = false;
        }
        replaced = ReplacePrecedingText(pContext, plan.bsCount, replacement);
    } else {
        auto* pSession = new CommitEditSession(pContext, &compositionMgr_, replacement);
        RequestEditSession(pContext, pSession);
        pSession->Release();
        replaced = !compositionMgr_.IsComposing();
    }

    if (!replaced) {
        TSF_LOG(L"HandleMacroTrigger: expansion edit failed for '%ls'", rawMacroBuffer_.c_str());
        ClearMacroTracking();
        return MacroResult::NoMatch;
    }

    engine_->Reset();
    digitLedWord_ = false;
    ClearMacroTracking();
    ResetCommitUndo();

    if (activeConfig_.macroUndoOnBackspace && !plan.shortcut.empty()
        && (vkCode == VK_SPACE || triggerChar > L' ')) {
        macroUndoState_.shortcut = std::move(plan.shortcut);
        macroUndoState_.expansion = replacement;
        macroUndoState_.expansionLength = replacement.size();
        macroUndoState_.timestamp = GetTickCount();
        macroUndoState_.active = true;
    } else {
        ResetMacroUndo();
    }

    TSF_LOG(L"HandleMacroTrigger: expanded macro");
    return eatTrigger ? MacroResult::ExpandedEatTrigger
                      : MacroResult::ExpandedPassTrigger;
}

bool EngineController::UndoMacro(ITfContext* pContext) {
    if (!HasMacroUndo()) return false;
    const std::size_t bsCount = macroUndoState_.expansionLength;
    std::wstring shortcut = std::move(macroUndoState_.shortcut);
    const std::wstring expansion = std::move(macroUndoState_.expansion);
    ResetMacroUndo();

    if (!ReplacePrecedingText(pContext, bsCount, shortcut, expansion)) {
        TSF_LOG(L"UndoMacro: ReplacePrecedingText failed");
        return false;
    }
    rawMacroBuffer_ = shortcut;
    macroCrossCommit_ = true;
    tempMacroOff_ = true;
    TSF_LOG(L"UndoMacro: restored shortcut '%ls'", shortcut.c_str());
    return true;
}

void EngineController::DetectScintillaApp() {
    // Cache Scintilla detection — called on context change, not per-keystroke.
    HWND hwnd = GetForegroundWindow();
    if (hwnd == nullptr) { isScintillaApp_ = false; return; }

    wchar_t className[256] = {0};
    if (GetClassNameW(hwnd, className, 256) > 0) {
        if (wcsstr(className, L"Notepad++") != nullptr) {
            isScintillaApp_ = true; return;
        }
    }

    HWND hwndFocus = GetFocus();
    if (hwndFocus != nullptr) {
        if (GetClassNameW(hwndFocus, className, 256) > 0) {
            if (wcsstr(className, L"Scintilla") != nullptr) {
                isScintillaApp_ = true; return;
            }
        }
    }

    isScintillaApp_ = false;
}

bool EngineController::CheckConfigEvent(bool allowMacroDiskRead) {
    TryPromotePendingConfig();

    if (!sharedState_.IsConnected()) {
        // Try to open SharedState if not connected
        if (!sharedState_.OpenReadWrite()) {
            return false;
        }
    }

    // A mapping becomes visible as soon as CreateFileMappingW succeeds. Older
    // creators wrote magic before structVersion/structSize, so a TSF instance
    // could observe a transient ABI mismatch during startup and latch abiOk_
    // false for its entire lifetime. Re-check before the epoch fast path so the
    // next key/focus self-heals once initialization completes.
    bool recoveredAbi = false;
    if (!abiOk_) {
        const auto abiResult = sharedState_.CheckAbiCompatibility();
        if (abiResult == SharedStateManager::AbiCheckResult::Incompatible) {
            // Genuinely still incompatible, not just transient contention —
            // SharedState::InitDefaults() resets flags (including
            // TSF_ABI_MISMATCH) on every Create(), so a VKeyApp.exe restart
            // (e.g. a version-skewed rebuild while this TSF host stayed
            // loaded) silently clears the banner even though THIS host is
            // still passthrough-only. Re-raise it so the UI reflects reality.
            sharedState_.SetOrClearFlag(SharedFlags::TSF_ABI_MISMATCH, true);
            return false;
        }
        if (abiResult != SharedStateManager::AbiCheckResult::Compatible) {
            return false;  // Retry — transient contention, try again next tick
        }
        abiOk_ = true;
        recoveredAbi = true;
        // Deliberately do NOT clear TSF_ABI_MISMATCH here. It is a single
        // process-shared bit, but every TSF host (one per document/thread,
        // possibly a different DLL version mid-update) has its own abiOk_.
        // Clearing it just because THIS host recovered could hide a genuine,
        // still-active mismatch in another host — this host already resumed
        // typing via its own abiOk_/recoveredAbi regardless of the shared
        // banner bit. The bit only clears on a fresh SharedStateManager::
        // Create() (VKeyApp.exe restart), which is the same action the
        // banner asks the user to take.
        TSF_LOG(L"EngineController: SharedState ABI recovered");
    }

    uint32_t currentEpoch = sharedState_.ReadEpoch();
    if (!recoveredAbi && currentEpoch == lastEpoch_) {
#ifdef VKEY_USE_RUST_ENGINE
        // Once the wire mapping has been observed, keep checking its tiny
        // seqlock-protected identity even when SharedState's epoch is stable.
        // The writer publishes wire contents before/around the generation
        // notification, and a reader can legitimately observe either side of
        // that handoff first.
        if (userDictionaryNeedsReload_ || wireReaderActive_) {
            RefreshUserDictionarySnapshot(currentEpoch, userDictionaryGeneration_, allowMacroDiskRead);
        }
        const bool dictionaryAttached = TryAttachUserDictionary();
#else
        constexpr bool dictionaryAttached = false;
#endif
        const bool promoted = TryPromotePendingConfig();
        if (allowMacroDiskRead && !macroConfigLoaded_) {
            ReloadMacros(macroGeneration_);
            return true;
        }
        return dictionaryAttached || promoted;
    }

    SharedState state = sharedState_.Read();
    if (!state.IsValid()) {
        // Epoch moved but Read() couldn't produce a valid snapshot. Two very
        // different causes look identical here: ordinary seqlock contention
        // (retry next tick, no action needed) vs. the EXE having recreated
        // the mapping with an incompatible layout while this host stayed
        // abiOk_==true from before (a version-skewed restart mid-update).
        // Only the latter is a confirmed mismatch — reuse the same
        // epoch-protected header check the constructor/recovery path already
        // trust, instead of silently keeping this host on stale config
        // forever with no user-visible signal.
        if (sharedState_.CheckAbiCompatibility() == SharedStateManager::AbiCheckResult::Incompatible) {
            abiOk_ = false;
            sharedState_.SetOrClearFlag(SharedFlags::TSF_ABI_MISMATCH, true);
            TSF_LOG(L"EngineController: SharedState ABI mismatch after epoch change — passthrough");
        }
        return false;
    }

    // Apply new config
    TSF_LOG(L"Config changed: epoch %u -> %u", lastEpoch_, state.epoch);
    lastEpoch_ = state.epoch;
    // ABI recovery is a rare, one-time event per TSF instance (not a per-key
    // occurrence) — worth the one disk read here so macros come back with
    // typing instead of staying empty until the next OnSetFocus.
    ApplySharedState(state, allowMacroDiskRead || recoveredAbi);

    return true;
}

#ifdef VKEY_USE_RUST_ENGINE
void EngineController::RefreshUserDictionarySnapshot(uint32_t epoch,
                                                     uint8_t generation,
                                                     bool allowDiskRead) {
    if (!userDictionaryGenerationKnown_ || userDictionaryGeneration_ != generation) {
        userDictionaryGenerationKnown_ = true;
        userDictionaryGeneration_ = generation;
        userDictionaryNeedsReload_ = true;
        const auto pendingDecision = DecidePendingTsfLexicon({
            .hasPendingConfig = pendingConfig_.has_value(),
            .lexiconSnapshotStaged = pendingLexiconSnapshot_,
        });
        if (!pendingDecision.keepPendingDictionary) pendingUserDictionary_.reset();
    }
    // Step 1: Zero-disk-I/O fast path via shared memory wire mapping (lock-free seqlock).
    bool wireUpdated = false;
    std::string wireErr;
    if (wireReader_.ReadSnapshotFast(wireLocalBuffer_, wireView_, epoch, &wireUpdated, &wireErr)) {
        wireReaderActive_ = true;
        if (!wireUpdated) {
            // Snapshot has not changed on the wire; cache hit.
            userDictionaryNeedsReload_ = false;
            return;
        }

        // Compile user dictionary snapshot first (no global side-effect).
        // This must happen BEFORE SetSpellExclusionsFromUtf16 because that call
        // mutates process-global state immediately. If we applied exclusions first
        // and the dict compile then failed, TSF would hold new exclusions + old
        // dictionary — a torn lexicon state violating the atomic-update contract.
        auto newSnapshot = RustInputEngine::CreateUserDictionaryFromUtf16(
            wireView_.userDictBuf,
            wireView_.userDictUnits);
        if (!newSnapshot) {
            // Rust compile failed (malformed wire data or OOM): keep reload pending (fail-stale).
            // Exclusions have NOT been touched yet — state remains fully consistent.
            TSF_LOG(L"UserDictionary: CreateUserDictionaryFromUtf16 returned nullptr — retaining reload flag");
            wireReader_.ResetCachedIdentity();
            return;
        }

        std::vector<std::wstring> wireExclusions;
        if (!DecodeWireExclusions(wireView_, wireExclusions)) {
            TSF_LOG(L"UserDictionary: exclusion decode failed — retaining reload flag");
            wireReader_.ResetCachedIdentity();
            return;
        }

        if (!pendingConfig_) pendingConfig_ = activeConfig_;
        const bool wireSpellSuggest =
            (wireView_.header->flags & Wire::LexiconWireFlags::SPELL_SUGGEST_ENABLED) != 0;
        if (pendingConfig_->spellSuggestEnabled != wireSpellSuggest) {
            pendingConfig_->spellSuggestEnabled = wireSpellSuggest;
            engineNeedsRecreate_ = true;
        }
        if (pendingConfig_->spellExclusions != wireExclusions) {
            pendingConfig_->spellExclusions = std::move(wireExclusions);
            engineNeedsRecreate_ = true;
        }

        // Both payloads are now staged. Engine construction applies exclusions
        // under RustInputEngine's process-global lock at the promotion boundary,
        // so it cannot overwrite the wire exclusions with stale config values.
        pendingUserDictionary_ = std::move(newSnapshot);
        pendingLexiconSnapshot_ = true;
        userDictionaryNeedsReload_ = false;
        TSF_LOG(L"UserDictionary: wire reload succeeded gen=%llu entries=%u units=%u",
                static_cast<unsigned long long>(wireView_.header->generation),
                static_cast<unsigned>(wireView_.header->userDictWordCount),
                static_cast<unsigned>(wireView_.header->userDictUtf16Units));
        return;
    }

    // Step 2: Legacy fallback to disk-based locked reading (Phase 1).
    // Invariant: NEVER touch disk on the typing hot path (!allowDiskRead).
    if (!allowDiskRead) {
        return;
    }

    const std::wstring configPath = ConfigManager::GetConfigPath(g_hInstance);
    LexiconSyncLock snapshotLock;
    if (!snapshotLock.IsLocked() || !LexiconRecovery::RecoverIfNeeded(configPath)) {
        return;
    }
    const auto diskConfig = ConfigManager::LoadFromFile(configPath);
    auto loaded = RustInputEngine::LoadUserDictionary(configPath);
    if (diskConfig && loaded.Succeeded()) {
        if (!pendingConfig_) pendingConfig_ = activeConfig_;
        if (pendingConfig_->spellSuggestEnabled != diskConfig->spellSuggestEnabled) {
            pendingConfig_->spellSuggestEnabled = diskConfig->spellSuggestEnabled;
            engineNeedsRecreate_ = true;
        }
        if (pendingConfig_->spellExclusions != diskConfig->spellExclusions) {
            pendingConfig_->spellExclusions = diskConfig->spellExclusions;
            engineNeedsRecreate_ = true;
        }
        pendingUserDictionary_ = std::move(loaded.snapshot);
        pendingLexiconSnapshot_ = true;
        userDictionaryNeedsReload_ = false;
        TSF_LOG(L"UserDictionary: disk locked reload succeeded path='%ls' generation=%u",
                configPath.c_str(), static_cast<unsigned>(generation));
    } else {
        // Fail-stale and retain the reload flag so a later disk-allowed tick
        // retries instead of accepting the cached wire identity as applied.
        TSF_LOG(L"UserDictionary: wire failed ('%ls') and disk reload rejected; retaining prior snapshot path='%ls'",
                std::wstring(wireErr.begin(), wireErr.end()).c_str(), configPath.c_str());
    }
}

bool EngineController::TryAttachUserDictionary() {
    if (!engine_ || !CanPromotePendingConfig() || !pendingUserDictionary_
        || !EngineFactory::WillUseRustEngine(activeConfig_)) {
        return false;
    }
    const bool attached = static_cast<RustInputEngine*>(engine_.get())
                              ->SetUserDictionary(pendingUserDictionary_);
    if (attached) {
        activeUserDictionary_ = std::move(pendingUserDictionary_);
        if (!pendingConfig_ && !userDictionaryNeedsReload_) {
            pendingLexiconSnapshot_ = false;
        }
        TSF_LOG(L"UserDictionary: attached at word boundary");
    }
    return attached;
}
#endif

void EngineController::ReloadMacros(uint8_t generation) {
    if (macroConfigLoaded_ && macroGeneration_ == generation) return;

    // g_hInstance (this DLL's own module handle) — NOT nullptr. TSF loads this
    // DLL in-process inside a foreign host (msedge.exe, notepad++.exe, ...);
    // GetConfigPath()'s default resolves nullptr to the CURRENT PROCESS's exe,
    // which inside a host means the host's install dir, not VKey's — silently
    // finding zero macros there (confirmed via TSF_LOG: "ReloadMacros: loaded 0
    // entries" while the real config.toml has entries). See LanguageBarButton.cpp
    // for the same g_hInstance-vs-nullptr fix applied to icon loading.
    const std::wstring configPath = ConfigManager::GetConfigPath(g_hInstance);
    const auto diskConfig = ConfigManager::LoadFromFile(configPath);
    if (!diskConfig) {
        // Read failed, not "no macros configured": a settings save renames a
        // temp file over config.toml, so a read racing that swap transiently
        // sees the file as missing/locked. Latching that as a successful load
        // pinned an empty table for the whole generation (observed in the wild
        // as "loaded 0 entries" right after a save, while a sibling host read
        // 3 entries at the same generation). Keep the current table and leave
        // macroConfigLoaded_ false so the next focus/init tick retries.
        TSF_LOG(L"ReloadMacros: config unreadable, keeping %zu entries (retry pending)",
                macroTable_.size());
        // Track the generation even though the read failed: the retry goes
        // through ReloadMacros(macroGeneration_), and the load guard keys off
        // macroConfigLoaded_, so this records what we're aiming at without
        // suppressing the retry.
        macroGeneration_ = generation;
        return;
    }

    macroConfigLoaded_ = true;
    macroGeneration_ = generation;
    ClearMacroTracking();
    macroTable_.clear();
    spaceMacroKeys_.clear();
    macroPrefixIndex_ = MacroPrefixIndex{};
    maxMacroKeyLen_ = 0;
    if (!activeConfig_.macroEnabled && (!pendingConfig_ || !pendingConfig_->macroEnabled)) return;

    // Trigger choices live only in TOML; SharedState carries feature bits.
    activeConfig_.macroTriggerSpace = diskConfig->macroTriggerSpace;
    activeConfig_.macroTriggerEnter = diskConfig->macroTriggerEnter;
    activeConfig_.macroTriggerTab = diskConfig->macroTriggerTab;
    activeConfig_.macroTriggerDir = diskConfig->macroTriggerDir;
    if (pendingConfig_.has_value()) {
        pendingConfig_->macroTriggerSpace = diskConfig->macroTriggerSpace;
        pendingConfig_->macroTriggerEnter = diskConfig->macroTriggerEnter;
        pendingConfig_->macroTriggerTab = diskConfig->macroTriggerTab;
        pendingConfig_->macroTriggerDir = diskConfig->macroTriggerDir;
    }

    macroTable_ = ConfigManager::LoadMacros(configPath);
    for (const auto& [key, value] : macroTable_) {
        (void)value;
        if (key.find(L' ') != std::wstring::npos) spaceMacroKeys_.insert(key);
    }
    macroPrefixIndex_ = MacroPrefixIndex(spaceMacroKeys_);
    maxMacroKeyLen_ = Macro::LongestMacroKeyLength(macroTable_);
    TSF_LOG(L"ReloadMacros: loaded %zu entries (generation=%u)",
            macroTable_.size(), static_cast<unsigned>(generation));
}

void EngineController::RefreshFlags() {
    DetectScintillaApp();
    if (!sharedState_.IsConnected()) {
        // Try to reconnect (EXE may have restarted)
        if (!sharedState_.OpenReadWrite()) {
            if (engineEnabled_ || tsfActive_) ClearMacroTracking();
            engineEnabled_ = false;
            tsfActive_ = false;
            return;
        }
        TSF_LOG(L"Reconnected to SharedState");
    }

    SharedState state = sharedState_.Read();
    if (state.IsValid()) {
        const bool wasEnabled = engineEnabled_;
        const bool wasTsfActive = tsfActive_;
        const bool wasVietnamese = vietnameseMode_;
        engineEnabled_ = (state.flags & SharedFlags::ENGINE_ENABLED) != 0;
        tsfActive_ = (state.flags & SharedFlags::TSF_ACTIVE) != 0;
        vietnameseMode_ = (state.flags & SharedFlags::VIETNAMESE_MODE) != 0;

        if (wasEnabled != engineEnabled_ || wasTsfActive != tsfActive_
            || wasVietnamese != vietnameseMode_) {
            ClearMacroTracking();
        }

        if (!wasEnabled && engineEnabled_) {
            TSF_LOG(L"Engine re-enabled (app started)");
            ApplySharedState(state, /*allowMacroDiskRead=*/true);
        } else if (wasEnabled && !engineEnabled_) {
            TSF_LOG(L"Engine disabled (app exited)");
        }

        // Refresh icon if Vietnamese mode changed (e.g. EXE hotkey toggled while bg)
        if (wasVietnamese != vietnameseMode_ && langBarButton_) {
            langBarButton_->Refresh();
        }
    } else {
        if (engineEnabled_ || tsfActive_) ClearMacroTracking();
        engineEnabled_ = false;
        tsfActive_ = false;
    }
}

void EngineController::SetTsfTipActive(bool active) {
    if (sharedState_.IsConnected()) {
        sharedState_.SetOrClearFlag(SharedFlags::TSF_TIP_ACTIVE, active);
        TSF_LOG(L"SetTsfTipActive: %s", active ? L"true" : L"false");
    }
}

void EngineController::SetTsfNativeConvertReady(bool ready) {
    if (sharedState_.IsConnected()) {
        sharedState_.PublishNativeConvertCapability(GetCurrentProcessId(), ready);
        TSF_LOG(L"SetTsfNativeConvertReady: %s", ready ? L"true" : L"false");
    }
}

void EngineController::ApplySharedState(const SharedState& state,
                                        bool allowMacroDiskRead) {
    const bool wasVietnameseMode = vietnameseMode_;
    const bool wasMacroEnabled = activeConfig_.macroEnabled;
    // Update runtime flags
    engineEnabled_ = (state.flags & SharedFlags::ENGINE_ENABLED) != 0;
    vietnameseMode_ = (state.flags & SharedFlags::VIETNAMESE_MODE) != 0;
    convertConfig_ = state.GetConvertConfig();

    // Validate inputMethod (valid range: 0–2) before casting to enum
    InputMethod newMethod = InputMethod::Telex;  // safe default
    if (state.inputMethod <= 2) {
        newMethod = static_cast<InputMethod>(state.inputMethod);
    } else {
        TSF_LOG(L"[EngineController] Invalid inputMethod %d from shared state, using Telex",
                state.inputMethod);
    }

    // Validate optimizeLevel (valid range: 0–2)
    uint8_t optimizeLevel = 0;
    if (state.optimizeLevel <= 2) {
        optimizeLevel = state.optimizeLevel;
    }

    TypingConfig newConfig = activeConfig_;
    newConfig.inputMethod = newMethod;
    newConfig.SetSpellCheckLevel(static_cast<SpellCheckLevel>(state.spellCheck));
    newConfig.optimizeLevel = optimizeLevel;
    DecodeFeatureFlags(state.GetFeatureFlags(), newConfig);

#ifdef VKEY_USE_RUST_ENGINE
    const auto pendingLexiconDecision = DecidePendingTsfLexicon({
        .hasPendingConfig = pendingConfig_.has_value(),
        .lexiconSnapshotStaged = pendingLexiconSnapshot_,
    });
    const TypingConfig& lexiconConfig = pendingLexiconDecision.preserveConfigFields
        ? *pendingConfig_
        : activeConfig_;
    // Advanced spell mode, exclusions and dictionary are one atomic lexicon
    // snapshot. SharedState can arrive before the wire publish, so retain the
    // last complete lexicon here; RefreshUserDictionarySnapshot replaces it
    // only after the corresponding wire/disk payload validates.
    newConfig.spellSuggestEnabled = lexiconConfig.spellSuggestEnabled;
    newConfig.spellExclusions = lexiconConfig.spellExclusions;
#endif

    pendingConfig_ = newConfig;
    engineNeedsRecreate_ = true;
    pendingSnapshotSerial_ = (static_cast<uint64_t>(state.epoch) << 8) | state.configGeneration;

#ifdef VKEY_USE_RUST_ENGINE
    RefreshUserDictionarySnapshot(state.epoch, state.configGeneration, allowMacroDiskRead);
#endif

    if (wasVietnameseMode != vietnameseMode_) ClearMacroTracking();
    if (wasMacroEnabled != newConfig.macroEnabled) macroConfigLoaded_ = false;

    // Runtime file-logger gate. SettingsDialog persists the bit into the
    // feature-flag bitmask via SharedState, so flipping the toggle in the
    // EXE reaches every TSF DLL instance on the next CheckConfigEvent tick.
    ::NextKey::Logger::SetEnabled(newConfig.debugLogEnabled);
    switch (DecideMacroTable({.loaded = macroConfigLoaded_,
                              .loadedGen = macroGeneration_,
                              .stateGen = state.configGeneration,
                              .allowDiskRead = allowMacroDiskRead})) {
        case MacroTableAction::None:
            break;
        case MacroTableAction::Reload:
            ReloadMacros(state.configGeneration);
            break;
        case MacroTableAction::KeepStale:
            // Reached only when this generation's read already failed from the
            // key path: macroConfigLoaded_/macroGeneration_ are set the way the
            // retry needs them, and the table we keep serving is the newest one
            // that ever loaded. Deliberately nothing to do — clearing it here is
            // what killed every macro for the rest of the session in hosts that
            // never fire another OnSetFocus (#227/#231).
            break;
    }

    // Promotion gate: if the engine is completely idle, promote immediately;
    // otherwise, defer promotion until the engine returns to word boundary.
    TryPromotePendingConfig();
}

bool EngineController::CanPromotePendingConfig() const noexcept {
    if (!engine_) return true;
    return CanPromoteTsfConfig({
        .engineBufferCount = engine_->Count(),
        .isComposing = compositionMgr_.IsComposing(),
        .rawMacroBufferEmpty = rawMacroBuffer_.empty(),
        .pendingReviveEmpty = pendingReviveWord_.empty()
    });
}

bool EngineController::TryPromotePendingConfig() {
#ifdef VKEY_USE_RUST_ENGINE
    if (!engineNeedsRecreate_ && !pendingUserDictionary_) {
        return false;
    }
#else
    if (!engineNeedsRecreate_) {
        return false;
    }
#endif
    if (!CanPromotePendingConfig()) {
        return false;
    }

    if (pendingConfig_.has_value()) {
        activeConfig_ = *pendingConfig_;
        activeMethod_ = activeConfig_.inputMethod;
        pendingConfig_.reset();
        compositionMgr_.SetHidePreeditUnderline(activeConfig_.hidePreeditUnderline);
    }

    if (engineNeedsRecreate_ || !engine_) {
        engine_ = EngineFactory::Create(activeConfig_);
        engineNeedsRecreate_ = false;

#ifdef VKEY_USE_RUST_ENGINE
        if (EngineFactory::WillUseRustEngine(activeConfig_)) {
            const auto& snapshot = pendingUserDictionary_ ? pendingUserDictionary_
                                                          : activeUserDictionary_;
            if (snapshot) {
                const bool attached = static_cast<RustInputEngine*>(engine_.get())
                                          ->SetUserDictionary(snapshot);
                if (attached && pendingUserDictionary_) {
                    activeUserDictionary_ = std::move(pendingUserDictionary_);
                }
                TSF_LOG(L"UserDictionary: engine-recreate attach %s",
                        attached ? L"ok" : L"failed");
            }
        } else {
            if (pendingUserDictionary_) {
                activeUserDictionary_ = std::move(pendingUserDictionary_);
            }
        }
#endif

        if (EngineFactory::RustEngineExpectedButUnavailable(activeConfig_)) {
            sharedState_.SetOrClearFlag(SharedFlags::TSF_ENGINE_UNTRUSTED, true);
        } else if (!activeConfig_.spellSuggestEnabled) {
            sharedState_.SetOrClearFlag(SharedFlags::TSF_ENGINE_UNTRUSTED, false);
        }
        TSF_LOG(L"Engine promoted & recreated (%s, modernOrtho=%d, allowZwjf=%d)",
                activeMethod_ == InputMethod::VNI ? L"VNI" : L"Telex",
                activeConfig_.modernOrtho ? 1 : 0, activeConfig_.allowZwjf ? 1 : 0);
    }
#ifdef VKEY_USE_RUST_ENGINE
    else if (pendingUserDictionary_) {
        static_cast<void>(TryAttachUserDictionary());
    }
    if (!pendingConfig_ && !pendingUserDictionary_ && !userDictionaryNeedsReload_) {
        pendingLexiconSnapshot_ = false;
    }
#endif

    return true;
}

void EngineController::ToggleVietnameseMode() {
    sharedState_.ToggleFlag(SharedFlags::VIETNAMESE_MODE);
    // Read back actual flag to stay in sync (avoids TOCTOU with EXE toggling)
    vietnameseMode_ = (sharedState_.ReadFlags() & SharedFlags::VIETNAMESE_MODE) != 0;
    digitLedWord_ = false;  // V/E switch ends any in-progress word
    ClearMacroTracking();
    if (langBarButton_) {
        langBarButton_->Refresh();
    }
    TSF_LOG(L"ToggleVietnameseMode: now %s", vietnameseMode_ ? L"Vietnamese" : L"English");
}

bool EngineController::InitLanguageBar(ITfThreadMgr* pThreadMgr) {
    if (!pThreadMgr) return false;

    ITfLangBarItemMgr* pLangBarItemMgr = nullptr;
    HRESULT hr = pThreadMgr->QueryInterface(
        IID_ITfLangBarItemMgr, reinterpret_cast<void**>(&pLangBarItemMgr));
    if (FAILED(hr) || !pLangBarItemMgr) {
        TSF_LOG(L"InitLanguageBar: failed to get ITfLangBarItemMgr");
        return false;
    }

    langBarButton_ = new LanguageBarButton();
    if (!langBarButton_->Initialize(this, pLangBarItemMgr)) {
        TSF_LOG(L"InitLanguageBar: button initialization failed");
        langBarButton_->Release();
        langBarButton_ = nullptr;
        pLangBarItemMgr->Release();
        return false;
    }

    pLangBarItemMgr->Release();
    TSF_LOG(L"InitLanguageBar: success");
    return true;
}

void EngineController::UninitLanguageBar() {
    if (langBarButton_) {
        langBarButton_->Uninitialize();
        langBarButton_->Release();
        langBarButton_ = nullptr;
    }
}

// ═══════════════════════════════════════════════════════════
// Commit-undo state machine (design 2026-05-17)
// ═══════════════════════════════════════════════════════════

void EngineController::RecordCommitSnapshot(std::wstring text,
                                            std::wstring rawInput,
                                            bool hasTrailingChar) noexcept {
    if (text.empty() || rawInput.empty()) {
        commitUndoState_ = CommitUndoState::Idle;
        lastCommit_ = {};
        return;
    }
    lastCommit_.text = std::move(text);
    lastCommit_.rawInput = std::move(rawInput);
    lastCommit_.hasTrailingChar = hasTrailingChar;
    lastCommit_.timestamp = GetTickCount();
    // Only CommitWithChar (trailing space) enters Ready. Plain Commit (Enter,
    // arrow, F-key) means cursor moves elsewhere — no undo window.
    commitUndoState_ = hasTrailingChar ? CommitUndoState::Ready : CommitUndoState::Idle;
    TSF_LOG(L"RecordCommitSnapshot: state=%d text='%ls' raw='%ls'",
            static_cast<int>(commitUndoState_), lastCommit_.text.c_str(), lastCommit_.rawInput.c_str());
}

std::wstring EngineController::MatchingRawForCommittedWord(const std::wstring& word) const {
    if (word.empty() || lastCommit_.text.empty() || lastCommit_.rawInput.empty()) {
        return {};
    }
    // Deliberately NOT age-gated (kCommitUndoTimeoutMs guards the undo window,
    // a different feature): a revive minutes later still needs the raw keys, and
    // an exact text match plus SeedRevivedWord's post-replay Peek()==word check
    // are the real guards. Worst case the raw came from another occurrence of the
    // same word — replaying it reproduces that word identically anyway.
    std::wstring body = lastCommit_.text;
    if (lastCommit_.hasTrailingChar && !body.empty()) {
        body.pop_back();
    }
    return body == word ? lastCommit_.rawInput : std::wstring{};
}

bool EngineController::WithinUndoWindow() const noexcept {
    if (commitUndoState_ == CommitUndoState::Idle) return false;
    return (GetTickCount() - lastCommit_.timestamp) <= kCommitUndoTimeoutMs;
}

void EngineController::TransitionUndoReadyToPrimed() noexcept {
    if (commitUndoState_ != CommitUndoState::Ready) return;
    commitUndoState_ = CommitUndoState::Primed;
    TSF_LOG(L"CommitUndo: Ready -> Primed");
}

void EngineController::ResetCommitUndo() noexcept {
    if (commitUndoState_ == CommitUndoState::Idle) return;
    TSF_LOG(L"CommitUndo: -> Idle (was state=%d)", static_cast<int>(commitUndoState_));
    commitUndoState_ = CommitUndoState::Idle;
    lastCommit_ = {};
}

void EngineController::OnNonRestoreKey() noexcept {
    if (commitUndoState_ != CommitUndoState::Idle) ResetCommitUndo();
    ResetMacroUndo();
}

bool EngineController::TryRestoreLastCommitRaw(ITfContext* pContext) {
    if (commitUndoState_ != CommitUndoState::Primed) return false;
    if (!WithinUndoWindow()) {
        TSF_LOG(L"TryRestoreLastCommitRaw: window expired (state=%d age=%ums)",
                static_cast<int>(commitUndoState_),
                GetTickCount() - lastCommit_.timestamp);
        ResetCommitUndo();
        return false;
    }
    if (lastCommit_.text.empty() || lastCommit_.rawInput.empty()) {
        ResetCommitUndo();
        return false;
    }
    // The Primed state means the user already deleted the trailing trigger (space).
    // We replace just the committed body. RecordCommitSnapshot stored the full
    // string including trailing char — strip it now.
    std::wstring body = lastCommit_.text;
    if (lastCommit_.hasTrailingChar && !body.empty()) {
        body.pop_back();
    }
    auto* pSession = new EscRestoreLastCommitSession(pContext, body, lastCommit_.rawInput);
    RequestEditSession(pContext, pSession);
    const bool replaced = pSession->Replaced();
    pSession->Release();

    // Reset state regardless of edit session outcome — session failure is logged
    // inside DoEditSession; caller falls back to pass-through ESC.
    ResetCommitUndo();
    return replaced;
}

}  // namespace TSF
}  // namespace NextKey
