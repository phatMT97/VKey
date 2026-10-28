// VKey - Engine Controller Header
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "stdafx.h"

#include <msctf.h>

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "CompositionManager.h"
#include "EditSession.h"
#include "LanguageBarButton.h"
#include "core/MacroContextMatch.h"
#include "core/MacroPrefix.h"
#include "core/TsfPromotionDecision.h"
#include "core/config/TypingConfig.h"
#include "core/engine/IInputEngine.h"
#include "core/engine/TypingAction.h"
#include "core/ipc/LexiconWireManager.h"
#include "core/ipc/SharedStateManager.h"

namespace NextKey {
#ifdef VKEY_USE_RUST_ENGINE
class RustUserDictionarySnapshot;
#endif
namespace TSF {

/// Controller bridging TSF events and the Telex engine
class EngineController {
public:
    enum class MacroResult : uint8_t {
        NoMatch,
        ExpandedEatTrigger,
        ExpandedPassTrigger,
    };

    explicit EngineController(ITfThreadMgr* pThreadMgr);
    ~EngineController();

    /// Set the TSF client ID for edit sessions
    void SetClientId(TfClientId clientId) { clientId_ = clientId; }

    /// Check if we want to handle this key
    bool WantKey(UINT vkCode, bool isKeyDown);

    /// Handle a key press
    bool HandleKey(ITfContext* pContext, UINT vkCode);

    /// Process backspace
    void ProcessBackspace(ITfContext* pContext);

    void TrackMacroCharacter(wchar_t ch);
    void TrackMacroBackspace() noexcept;
    void ClearMacroTracking() noexcept;
    [[nodiscard]] bool IsMacroCommitTrigger(UINT vkCode) const noexcept;
    [[nodiscard]] bool HasMacroCandidate() const noexcept;
    [[nodiscard]] bool IsEnglishMacroTrackingActive() const noexcept;
    [[nodiscard]] bool WouldExpandMacroTrigger(ITfContext* pContext,
                                               UINT vkCode,
                                               wchar_t triggerChar) const;
    [[nodiscard]] MacroResult HandleMacroTrigger(ITfContext* pContext,
                                                  UINT vkCode,
                                                  wchar_t triggerChar);

    /// Commit current composition
    void Commit(ITfContext* pContext);

    /// Commit with trailing character (e.g., space)
    void CommitWithChar(ITfContext* pContext, wchar_t appendChar);

    /// Esc-restore: end composition with the user's RAW keys (case-preserved),
    /// not the Vietnamese form. Returns false if engine has no raw input —
    /// caller should fall back to standard Commit() flow. Edit session
    /// failures are logged (via RequestEditSession) but not propagated;
    /// matches the existing Commit() / CommitWithChar() pattern.
    /// Resets engine state and digitLedWord_ on success.
    [[nodiscard]] bool CommitRawAndEnd(ITfContext* pContext);

    /// End composition with currently displayed verbatim text (Peek) without
    /// invoking engine commit or dictionary spell correction (e.g. on bare Esc).
    void EndCompositionVerbatim(ITfContext* pContext);

    /// Whether Esc-restore-raw is enabled in current config snapshot.
    /// Cheap getter — KeyEventSink uses this to gate the VK_ESCAPE branch.
    [[nodiscard]] bool IsEscRestoreRawEnabled() const noexcept {
        return activeConfig_.escRestoreRawEnabled;
    }

    /// Whether "BS keeps chars on suggest" is enabled in current config snapshot.
    [[nodiscard]] bool IsSuggestKeepCharsEnabled() const noexcept {
        return activeConfig_.suggestKeepChars;
    }

    [[nodiscard]] bool HasNonEmptySelection(ITfContext* pContext);
    /// Commit-undo state machine for ESC-restore-raw post-BS (design 2026-05-17).
    /// Mirrors HookEngine's state machine but lighter — single-entry cache, no replay.
    enum class CommitUndoState : uint8_t {
        Idle   = 0,
        Ready  = 1,  // Just CommitWithChar'd — waiting for first BS
        Primed = 2,  // BS happened in Ready — ESC will now restore from cache
    };

    [[nodiscard]] bool IsCommitUndoReady()  const noexcept { return commitUndoState_ == CommitUndoState::Ready; }
    [[nodiscard]] bool IsCommitUndoPrimed() const noexcept { return commitUndoState_ == CommitUndoState::Primed; }
    [[nodiscard]] bool WithinUndoWindow()   const noexcept;
    void TransitionUndoReadyToPrimed() noexcept;
    void ResetCommitUndo() noexcept;
    void OnNonRestoreKey() noexcept;  // Any key besides BS/ESC in Ready/Primed → Idle
    [[nodiscard]] bool TryRestoreLastCommitRaw(ITfContext* pContext);

    // Macro undo on Backspace (issue #264)
    [[nodiscard]] bool HasMacroUndo() const noexcept {
        if (!macroUndoState_.active || !activeConfig_.macroUndoOnBackspace
            || !IsMacroTrackingEnabled()) return false;
        if ((GetTickCount() - macroUndoState_.timestamp) > kMacroUndoTimeoutMs) {
            return false;
        }
        return true;
    }
    void ResetMacroUndo() noexcept {
        macroUndoState_.active = false;
        macroUndoState_.shortcut.clear();
        macroUndoState_.expansion.clear();
        macroUndoState_.expansionLength = 0;
    }
    bool UndoMacro(ITfContext* pContext);

    /// Reset engine state
    void Reset();

    /// Check if there's pending composition (for Ctrl shortcuts)
    bool HasComposition() const { return engine_->Count() > 0; }

    /// Check if TSF composition is active
    bool IsComposing() const { return compositionMgr_.IsComposing(); }

    /// Whether this foreground application is configured for the TSF backend.
    [[nodiscard]] bool IsTsfActive() const noexcept { return tsfActive_; }

    /// Refresh the cheap SharedState routing bits used by key callbacks.
    /// Returns false when this TIP instance must be a strict passthrough.
    /// Unlike RefreshFlags(), this performs no process inspection or disk I/O.
    [[nodiscard]] bool RefreshKeyRouting() noexcept;

    /// Quick-convert config snapshot delivered by the main process.
    [[nodiscard]] const ConvertConfig& GetConvertConfig() const noexcept {
        return convertConfig_;
    }

    /// Check if engine has buffer (for sync check)
    bool HasEngineBuffer() const { return engine_->Count() > 0; }

    /// Check if vkCode is a digit key (0-9) that should be routed to the
    /// engine (and NOT trigger commit). VNI '0' is the clear-tone key;
    /// UserDefined may remap any digit via customKeyMap, so we route the
    /// full 0-9 range in those modes (unmapped digits fall through as
    /// ProcessChar literal). Telex/SimpleTelex don't claim digits.
    bool IsEngineDigitKey(UINT vkCode) const {
        return (activeConfig_.inputMethod == InputMethod::VNI ||
                activeConfig_.inputMethod == InputMethod::Combined ||
                activeConfig_.inputMethod == InputMethod::UserDefined) &&
               vkCode >= 0x30 && vkCode <= 0x39 &&
               !(GetKeyState(VK_SHIFT) & 0x8000);
    }

    /// Whether a physical [`[`/`]`] key is an engine action in the active
    /// method. Full Telex always owns both keys. UserDefined owns one when its
    /// exact shifted character (if configured) or base bracket has an action
    /// applicable to the current buffer.
    [[nodiscard]] bool IsEngineBracketKey(UINT vkCode) const;

    /// Reset only engine buffer (for sync recovery)
    void ResetEngine() { engine_->Reset(); }

    /// Check for config changes (call periodically, e.g., on focus).
    /// Disk-backed macro reload is allowed only outside key callbacks.
    /// Returns true if config was reloaded
    bool CheckConfigEvent(bool allowMacroDiskRead = false);

    /// Check if engine is enabled (app is running)
    [[nodiscard]] bool IsEnabled() const noexcept { return engineEnabled_; }

    /// Check if Vietnamese mode is active
    [[nodiscard]] bool IsVietnameseMode() const noexcept { return vietnameseMode_; }

    /// Toggle Vietnamese/English mode (atomic flag + icon refresh)
    void ToggleVietnameseMode();

    /// Get current code table
    [[nodiscard]] CodeTable GetCodeTable() const noexcept { return activeConfig_.codeTable; }

    /// Set code table (updates config, no persistence yet)
    void SetCodeTable(CodeTable ct) noexcept {
        activeConfig_.codeTable = ct;
        if (pendingConfig_.has_value()) {
            pendingConfig_->codeTable = ct;
        }
    }

    /// Promotion gate: check whether pendingConfig_ or pendingUserDictionary_ can be promoted
    [[nodiscard]] bool CanPromotePendingConfig() const noexcept;

    /// Attempt to promote pendingConfig_ to activeConfig_ and/or attach pendingUserDictionary_
    bool TryPromotePendingConfig();

    /// Const access to active config snapshot
    [[nodiscard]] const TypingConfig& GetActiveConfig() const noexcept { return activeConfig_; }

    /// Whether there is a staged configuration or engine recreation waiting for promotion gate
    [[nodiscard]] bool HasPendingConfig() const noexcept {
#ifdef VKEY_USE_RUST_ENGINE
        return pendingConfig_.has_value() || engineNeedsRecreate_ || (pendingUserDictionary_ != nullptr);
#else
        return pendingConfig_.has_value() || engineNeedsRecreate_;
#endif
    }

    /// Combined serial of the latest pending configuration (epoch << 8 | configGeneration)
    [[nodiscard]] uint64_t GetPendingSnapshotSerial() const noexcept {
        return pendingSnapshotSerial_;
    }

    /// Initialize language bar button (call after SetClientId)
    bool InitLanguageBar(ITfThreadMgr* pThreadMgr);

    /// Cleanup language bar button
    void UninitLanguageBar();

    /// Re-read flags from SharedState (call on focus)
    void RefreshFlags();

    /// Publish TSF_TIP_ACTIVE flag to SharedState. Called by KeyEventSink::OnSetFocus
    /// (foreground/background) and TextService::Deactivate (layout switch-away).
    void SetTsfTipActive(bool active);

    /// Publish whether this foreground TIP instance has successfully preserved
    /// the native quick-convert hotkey. The EXE uses this as a routing handshake.
    void SetTsfNativeConvertReady(bool ready);

    /// Non-owning access to the SharedStateManager — shared with ReadonlyContextProvider
    /// so both can read/write the same memory-mapped region without duplicating the
    /// mapping handle.
    [[nodiscard]] SharedStateManager* GetSharedStateManager() noexcept { return &sharedState_; }

    /// Refresh dynamic input gates on each key, caching only input scopes.
    void CheckContextBlocked(ITfContext* pContext);

    /// Whether current context blocks Vietnamese input
    [[nodiscard]] bool IsContextBlocked() const noexcept { return contextBlocked_; }

    /// Try to prepare a Backspace revive: sync-read preceding Vietnamese word
    /// before caret. On success caches word + range for HandleKey(VK_BACK) to consume.
    /// Returns true if a Vietnamese word was found — caller should eat the BS key.
    bool PrepareBackspaceRevive(ITfContext* pContext);

    /// Discard any pending revive state (word + range). Safe to call at any time.
    void ClearPendingRevive();

    /// Whether a revive is queued (set by PrepareBackspaceRevive, consumed by HandleKey).
    [[nodiscard]] bool HasPendingRevive() const noexcept { return pendingReviveRange_ != nullptr; }

private:
    void RequestEditSession(ITfContext* pContext, EditSession* pEditSession);
    [[nodiscard]] BracketKey ResolveEngineBracketKey(UINT vkCode) const;
    [[nodiscard]] bool PushEngineKey(ITfContext* pContext, wchar_t ch, bool uppercase);

    void ReloadMacros(uint8_t generation);
#ifdef VKEY_USE_RUST_ENGINE
    void RefreshUserDictionarySnapshot(uint32_t epoch, uint8_t generation, bool allowDiskRead);
    [[nodiscard]] bool TryAttachUserDictionary();
#endif
    void ClearMacroTrackingAfterCommit() noexcept;
    [[nodiscard]] bool IsMacroTrackingEnabled() const noexcept;
    [[nodiscard]] bool ReplacePrecedingText(ITfContext* pContext,
                                            std::size_t characterCount,
                                            const std::wstring& replacement,
                                            const std::wstring& expected = {});
    [[nodiscard]] std::wstring ReadPrecedingTextFromContext(ITfContext* pContext,
                                                            LONG maxChars = 64) const;
    /// Plan against an explicitly assembled raw buffer. Callers that must not
    /// mutate state (WouldExpandMacroTrigger) pass a local copy.
    [[nodiscard]] Macro::MacroPlan EvaluateMacroPlan(const std::wstring& rawBuffer,
                                                    wchar_t triggerChar) const;
    /// Fallback for when rawMacroBuffer_ no longer mirrors the document.
    [[nodiscard]] std::optional<Macro::ContextMatch> LookupMacroInContext(
        ITfContext* pContext, wchar_t triggerChar) const;

    /// Detect if current app is Scintilla-based (cached, updated on context change)
    void DetectScintillaApp();

    /// Apply config from SharedState
    void ApplySharedState(const SharedState& state, bool allowMacroDiskRead);

    std::unique_ptr<IInputEngine> engine_;
    CompositionManager compositionMgr_;
    TypingConfig activeConfig_;
    std::optional<TypingConfig> pendingConfig_;
    uint64_t pendingSnapshotSerial_ = 0;
    bool engineNeedsRecreate_ = false;
    ConvertConfig convertConfig_;
    InputMethod activeMethod_ = InputMethod::Telex;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    SharedStateManager sharedState_; // For reading config from App
    uint32_t lastEpoch_ = 0;        // Last seen config epoch
    bool engineEnabled_ = true;     // ENGINE_ENABLED flag from SharedState
    bool tsfActive_ = false;        // TSF_ACTIVE flag from SharedState (foreground app in TSF list)
    bool vietnameseMode_ = true;    // VIETNAMESE_MODE flag from SharedState
    // true only once IsAbiCompatible() has actually been observed to pass —
    // NOT "assumed fine until proven otherwise". Defaulting true let an
    // instance that never got a chance to check (e.g. constructed while
    // SharedState was momentarily unavailable) skip validation forever,
    // since CheckConfigEvent's recovery path only runs when this is false.
    // Unchecked and confirmed-incompatible must both disable TSF the same
    // way, so they share the same falsy default.
    bool abiOk_ = false;
    LanguageBarButton* langBarButton_ = nullptr;  // Owned, Release'd in UninitLanguageBar
    ITfContext* lastContext_ = nullptr;   // Last seen context (AddRef'd for safe identity comparison)
    CComPtr<ITfCompartmentMgr> contextCompartments_;
    CComPtr<ITfCompartmentMgr> threadCompartments_;
    bool scopeBlocked_ = false;          // Cached password/PIN/email result for lastContext_
    bool contextBlocked_ = false;        // Combined current TSF input gates
    bool verdictLogged_ = false;         // Context verdict already logged for this enable-session
    bool isScintillaApp_ = false;        // Cached: current app is Scintilla-based (Notepad++, etc.)
    bool digitLedWord_ = false;          // True = current word started with a digit (VNI/Combined/UserDefined) → treat whole word as English (pass through; no composition)

    // Macros are not represented in SharedState because the table is variable
    // sized. TSF reloads this local snapshot whenever configGeneration changes.
    std::unordered_map<std::wstring, std::wstring> macroTable_;
    std::unordered_set<std::wstring> spaceMacroKeys_;
    MacroPrefixIndex macroPrefixIndex_;
    // Bounds the backward scan of document text in LookupMacroInContext.
    std::size_t maxMacroKeyLen_ = 0;
    std::wstring rawMacroBuffer_;
    bool wasFirstCharAutoCapped_ = false;
    uint8_t macroGeneration_ = 0;
    bool macroConfigLoaded_ = false;
    bool macroCrossCommit_ = false;
#ifdef VKEY_USE_RUST_ENGINE
    // The TSF key path reads dictionary snapshots directly from the wire mapping
    // without disk I/O. If wire mapping is unmapped or unavailable, focus/init
    // can fall back to disk-based locked reading.
    std::shared_ptr<const RustUserDictionarySnapshot> activeUserDictionary_;
    std::shared_ptr<const RustUserDictionarySnapshot> pendingUserDictionary_;
    uint8_t userDictionaryGeneration_ = 0;
    bool userDictionaryGenerationKnown_ = false;
    bool userDictionaryNeedsReload_ = false;
    // True while wire/disk lexicon fields and dictionary are staged but have
    // not all crossed the word-boundary promotion gate.
    bool pendingLexiconSnapshot_ = false;

    Wire::LexiconWireReader wireReader_;
    std::vector<uint8_t> wireLocalBuffer_;
    Wire::LexiconWireView wireView_{};
    bool wireReaderActive_ = false;
#endif

    // Pending Backspace revive — set by PrepareBackspaceRevive (called from OnTestKeyDown),
    // consumed by HandleKey(VK_BACK). CComPtr auto-manages ref count.
    std::wstring pendingReviveWord_;
    CComPtr<ITfRange> pendingReviveRange_;

    // Commit-undo cache for ESC restore-raw post-BS (design 2026-05-17).
    struct LastCommit {
        std::wstring text;       // What was written to document (including trailing char)
        std::wstring rawInput;   // engine_->PeekRaw() snapshot before Commit reset
        bool hasTrailingChar = false;
        DWORD timestamp = 0;     // GetTickCount() at commit
    };
    LastCommit lastCommit_;
    CommitUndoState commitUndoState_ = CommitUndoState::Idle;

    struct MacroUndoState {
        std::wstring shortcut;
        std::wstring expansion;
        std::size_t  expansionLength{0};
        DWORD        timestamp{0};
        bool         active{false};
    };
    MacroUndoState macroUndoState_{};
    bool tempMacroOff_{false};  // Do not immediately expand the restored shortcut.
    static constexpr DWORD kMacroUndoTimeoutMs = 4000;

    static constexpr DWORD kCommitUndoTimeoutMs = 1500;

    void RecordCommitSnapshot(std::wstring text, std::wstring rawInput, bool hasTrailingChar) noexcept;

    /// Returns the raw keystrokes that produced `word`, if `word` is exactly the
    /// most recently committed text (lastCommit_, trailing char stripped). Empty
    /// otherwise — callers should fall back to IInputEngine::SeedFromText, which
    /// can't re-tone on continued edit.
    [[nodiscard]] std::wstring MatchingRawForCommittedWord(const std::wstring& word) const;
};

}  // namespace TSF
}  // namespace NextKey
