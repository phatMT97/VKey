// VKey - Composition Edit Sessions
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

#include "CompositionManager.h"
#include "Define.h"
#include "EditSession.h"
#include "core/AutoCapDecision.h"
#include "core/TsfEditDecision.h"
#include "core/engine/CommittedTextRestore.h"
#include "core/engine/IInputEngine.h"
#include "core/engine/VietnameseTables.h"

namespace NextKey {
namespace TSF {

/// Edit session to start a new composition
class StartCompositionEditSession : public EditSession {
public:
    StartCompositionEditSession(ITfContext* pContext, CompositionManager* pMgr, const std::wstring& text)
        : EditSession(pContext), pMgr_(pMgr), text_(text) {}

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pMgr_ == nullptr || pContext_ == nullptr) return E_FAIL;

        if (!pMgr_->StartComposition(pContext_, ec, text_)) {
            TSF_LOG(L"StartCompositionEditSession: Failed to start composition");
            return E_FAIL;
        }

        TSF_LOG(L"StartCompositionEditSession: Success");
        return S_OK;
    }

private:
    CompositionManager* pMgr_;
    std::wstring text_;
};

/// Edit session to update composition text
class UpdateCompositionEditSession : public EditSession {
public:
    UpdateCompositionEditSession(ITfContext* pContext, CompositionManager* pMgr, const std::wstring& text)
        : EditSession(pContext), pMgr_(pMgr), text_(text) {}

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pMgr_ == nullptr) return E_FAIL;

        if (!pMgr_->IsComposing()) {
            TSF_LOG(L"UpdateCompositionEditSession: Not composing, starting new");
            if (!pMgr_->StartComposition(pContext_, ec, text_)) {
                return E_FAIL;
            }
            return S_OK;
        }

        if (!pMgr_->SetCompositionText(ec, text_)) return E_FAIL;

        TSF_LOG(L"UpdateCompositionEditSession: Success");
        return S_OK;
    }

private:
    CompositionManager* pMgr_;
    std::wstring text_;
};

/// Edit session to end composition
class EndCompositionEditSession : public EditSession {
public:
    EndCompositionEditSession(ITfContext* pContext, CompositionManager* pMgr)
        : EditSession(pContext), pMgr_(pMgr) {}

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pMgr_ == nullptr) return E_FAIL;

        pMgr_->EndComposition(ec);
        TSF_LOG(L"EndCompositionEditSession: Success");
        return S_OK;
    }

private:
    CompositionManager* pMgr_;
};

/// Edit session to commit with final text (update + end in one operation)
class CommitEditSession : public EditSession {
public:
    CommitEditSession(ITfContext* pContext, CompositionManager* pMgr, const std::wstring& finalText)
        : EditSession(pContext), pMgr_(pMgr), finalText_(finalText) {}

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pMgr_ == nullptr) return E_FAIL;

        if (pMgr_->IsComposing()) {
            if (!pMgr_->CurrentTextEquals(finalText_)
                && !pMgr_->SetCompositionText(ec, finalText_)) {
                return E_FAIL;
            }
            pMgr_->EndComposition(ec);
        } else {
            TSF_LOG(L"CommitEditSession: composition externally terminated, text dropped: %s",
                    finalText_.c_str());
        }

        return S_OK;
    }

private:
    CompositionManager* pMgr_;
    std::wstring finalText_;
};

/// Replace a known number of text units immediately before an empty selection.
class ReplacePrecedingTextEditSession : public EditSession {
public:
    ReplacePrecedingTextEditSession(ITfContext* pContext, std::size_t characterCount,
                                    const std::wstring& replacement, bool* replaced,
                                    const std::wstring& expected = {})
        : EditSession(pContext), characterCount_(characterCount), replacement_(replacement),
          replaced_(replaced), expected_(expected) {}

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (replaced_) *replaced_ = false;
        if (pContext_ == nullptr || characterCount_ == 0 || replaced_ == nullptr) return E_INVALIDARG;

        TF_SELECTION selection = {};
        ULONG fetched = 0;
        HRESULT hr = pContext_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (FAILED(hr) || fetched != 1 || selection.range == nullptr) return E_FAIL;

        CComPtr<ITfRange> range;
        range.Attach(selection.range);
        BOOL isEmpty = FALSE;
        if (FAILED(range->IsEmpty(ec, &isEmpty)) || !isEmpty) return E_FAIL;

        TF_HALTCOND halt = {nullptr, TF_ANCHOR_START, TF_HF_OBJECT};
        LONG shifted = 0;
        hr = range->ShiftStart(ec, -static_cast<LONG>(characterCount_), &shifted, &halt);
        if (FAILED(hr) || shifted != -static_cast<LONG>(characterCount_)) return E_FAIL;

        // Undo may follow a mouse/caret move without a focus notification.
        // Verify the exact expansion under the same edit lock before replacing.
        if (!expected_.empty()) {
            std::wstring actual(expected_.size(), L'\0');
            ULONG read = 0;
            hr = range->GetText(ec, 0, actual.data(), static_cast<ULONG>(actual.size()), &read);
            if (FAILED(hr) || read != expected_.size() || actual != expected_) return E_FAIL;
        }

        hr = range->SetText(ec, 0, replacement_.c_str(), static_cast<LONG>(replacement_.size()));
        if (SUCCEEDED(hr)) {
            // SetText is irreversible within this edit session. Record the
            // document mutation even if the subsequent caret update fails so
            // the caller does not pass an already-consumed trigger to the host.
            *replaced_ = true;
            hr = range->Collapse(ec, TF_ANCHOR_END);
            if (SUCCEEDED(hr)) {
                selection.style.ase = TF_AE_END;
                selection.style.fInterimChar = FALSE;
                hr = pContext_->SetSelection(ec, 1, &selection);
            }
        }
        return hr;
    }

private:
    std::size_t characterCount_;
    std::wstring replacement_;
    bool* replaced_;
    std::wstring expected_;
};

/// Read-only edit session: fetch up to N chars preceding the caret.
/// No filtering — caller interprets the buffer (e.g. for auto-cap rules).
/// `AtDocStart()` is true when the caret can't be shifted back at all.
class ReadPrecedingCharsEditSession : public EditSession {
public:
    // Caller passes a compile-time max; buffer sized at template instantiation. 64 is
    // plenty for auto-cap (only need ~16 to skip trailing whitespace).
    static constexpr LONG MAX_CHARS = 64;

    explicit ReadPrecedingCharsEditSession(ITfContext* pContext, LONG maxChars = MAX_CHARS)
        : EditSession(pContext), maxChars_((std::min)(maxChars, MAX_CHARS)) {}

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pContext_ == nullptr) return E_FAIL;

        TF_SELECTION sel = {};
        ULONG fetched = 0;
        HRESULT hr = pContext_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
        if (FAILED(hr) || fetched != 1 || sel.range == nullptr) return S_OK;
        CComPtr<ITfRange> pSelRange;
        pSelRange.Attach(sel.range);

        BOOL selEmpty = FALSE;
        if (FAILED(pSelRange->IsEmpty(ec, &selEmpty)) || !selEmpty) return S_OK;

        CComPtr<ITfRange> pPeek;
        if (FAILED(pSelRange->Clone(&pPeek)) || !pPeek) return S_OK;

        TF_HALTCOND haltcond = { nullptr, TF_ANCHOR_START, TF_HF_OBJECT };
        LONG shifted = 0;
        if (FAILED(pPeek->ShiftStart(ec, -maxChars_, &shifted, &haltcond))) return S_OK;
        if (shifted == 0) {
            atDocStart_ = true;  // caret sits at document start — can't move back at all
            return S_OK;
        }
        if (shifted > 0) return S_OK;  // unexpected (shift-back should never go forward)

        WCHAR buf[MAX_CHARS] = {};
        ULONG retrieved = 0;
        if (FAILED(pPeek->GetText(ec, 0, buf, -shifted, &retrieved)) || retrieved == 0) {
            return S_OK;
        }
        text_.assign(buf, retrieved);
        return S_OK;
    }

    [[nodiscard]] const std::wstring& Text() const noexcept { return text_; }
    [[nodiscard]] bool AtDocStart() const noexcept { return atDocStart_; }

private:
    LONG maxChars_;
    std::wstring text_;
    bool atDocStart_ = false;
};

/// Read-only edit session: find the Vietnamese word immediately before the caret.
/// Walks backward over up to MAX_CHARS chars, counting consecutive Vietnamese letters.
/// On success, DetachRange() transfers ownership of the word range to the caller.
class ReadPrecedingWordEditSession : public EditSession {
public:
    explicit ReadPrecedingWordEditSession(ITfContext* pContext)
        : EditSession(pContext) {}

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pContext_ == nullptr) return E_FAIL;

        constexpr LONG MAX_CHARS = 16;

        TF_SELECTION sel = {};
        ULONG fetched = 0;
        HRESULT hr = pContext_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
        if (FAILED(hr) || fetched != 1 || sel.range == nullptr) return S_OK;

        // GetSelection transfers a ref; Attach takes ownership without extra AddRef.
        CComPtr<ITfRange> pSelRange;
        pSelRange.Attach(sel.range);

        BOOL selEmpty = FALSE;
        if (FAILED(pSelRange->IsEmpty(ec, &selEmpty)) || !selEmpty) return S_OK;

        CComPtr<ITfRange> pPeek;
        if (FAILED(pSelRange->Clone(&pPeek)) || !pPeek) return S_OK;

        TF_HALTCOND haltcond = { nullptr, TF_ANCHOR_START, TF_HF_OBJECT };
        LONG shifted = 0;
        if (FAILED(pPeek->ShiftStart(ec, -MAX_CHARS, &shifted, &haltcond)) || shifted >= 0) {
            return S_OK;
        }
        LONG availChars = -shifted;

        WCHAR buf[MAX_CHARS] = {};
        ULONG retrieved = 0;
        HRESULT hrG = pPeek->GetText(ec, 0, buf, (std::min)(availChars, MAX_CHARS), &retrieved);
        if (FAILED(hrG) || retrieved == 0) return S_OK;

        LONG wordlen = 0;
        for (LONG i = static_cast<LONG>(retrieved) - 1; i >= 0; --i) {
            wchar_t base = 0;
            int modIdx = -1, toneIdx = -1;
            bool isUpper = false;
            if (DecomposeVietChar(buf[i], base, modIdx, toneIdx, isUpper)) {
                ++wordlen;
            } else {
                break;
            }
        }

        if (wordlen == 0) return S_OK;

        LONG shifted2 = 0;
        if (FAILED(pSelRange->ShiftStart(ec, -wordlen, &shifted2, &haltcond))
            || !IsExactBackwardRangeShift(wordlen, shifted2)) {
            return S_OK;
        }

        word_.assign(&buf[retrieved - wordlen], wordlen);
        pRange_ = pSelRange;  // CComPtr assignment — AddRefs for the member.
        TSF_LOG(L"ReadPrecedingWord: found '%ls' len=%ld", word_.c_str(), wordlen);
        return S_OK;
    }

    [[nodiscard]] bool Found() const noexcept { return pRange_ != nullptr; }
    [[nodiscard]] const std::wstring& Word() const noexcept { return word_; }

    /// Transfer ownership of the range to the caller. Returns nullptr if no word found.
    ITfRange* DetachRange() noexcept { return pRange_.Detach(); }

private:
    std::wstring word_;
    CComPtr<ITfRange> pRange_;
};

/// Combined read session: extracts Vietnamese word AND checks auto-cap in one pass.
/// Replaces separate ReadPrecedingWordEditSession + ReadPrecedingCharsEditSession calls
/// on the HandleKey A-Z path when engine buffer is empty.
class InspectPrecedingTextEditSession : public EditSession {
public:
    explicit InspectPrecedingTextEditSession(ITfContext* pContext)
        : EditSession(pContext) {}

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pContext_ == nullptr) return E_FAIL;

        constexpr LONG MAX_CHARS = 64;

        TF_SELECTION sel = {};
        ULONG fetched = 0;
        HRESULT hr = pContext_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
        CComPtr<ITfRange> pSelRange;
        pSelRange.Attach(sel.range);
        if (FAILED(hr) || fetched != 1 || !pSelRange) return S_OK;

        BOOL selEmpty = FALSE;
        if (FAILED(pSelRange->IsEmpty(ec, &selEmpty)) || !selEmpty) return S_OK;

        CComPtr<ITfRange> pPeek;
        if (FAILED(pSelRange->Clone(&pPeek)) || !pPeek) return S_OK;

        TF_HALTCOND haltcond = { nullptr, TF_ANCHOR_START, TF_HF_OBJECT };
        LONG shifted = 0;
        if (FAILED(pPeek->ShiftStart(ec, -MAX_CHARS, &shifted, &haltcond))) return S_OK;

        if (shifted == 0) {
            CComPtr<ITfRange> pDocumentStart;
            BOOL isDocumentStart = FALSE;
            if (SUCCEEDED(pContext_->GetStart(ec, &pDocumentStart)) && pDocumentStart
                && SUCCEEDED(pSelRange->IsEqualStart(
                    ec, pDocumentStart, TF_ANCHOR_START, &isDocumentStart))
                && isDocumentStart) {
                atDocStart_ = true;
                shouldAutoCap_ = true;
            }
            return S_OK;
        }
        if (shifted > 0) return S_OK;

        LONG availChars = -shifted;
        WCHAR buf[MAX_CHARS] = {};
        ULONG len = 0;
        if (FAILED(pPeek->GetText(ec, 0, buf, availChars, &len)) || len == 0) return S_OK;

        // Extract Vietnamese word (walk backward using DecomposeVietChar)
        LONG wordLen = 0;
        for (LONG i = static_cast<LONG>(len) - 1; i >= 0; --i) {
            wchar_t base = 0;
            int modIdx = -1, toneIdx = -1;
            bool isUpper = false;
            if (DecomposeVietChar(buf[i], base, modIdx, toneIdx, isUpper)) {
                ++wordLen;
            } else {
                break;
            }
        }

        if (wordLen > 0) {
            word_.assign(&buf[len - wordLen], wordLen);
            CComPtr<ITfRange> pWordRange;
            if (SUCCEEDED(pSelRange->Clone(&pWordRange)) && pWordRange) {
                LONG shifted2 = 0;
                if (SUCCEEDED(pWordRange->ShiftStart(ec, -wordLen, &shifted2, &haltcond))
                    && IsExactBackwardRangeShift(wordLen, shifted2)) {
                    wordRange_ = pWordRange;
                }
            }
        }

        // Auto-cap rule extracted to core/AutoCapDecision.h for Linux GTest
        // coverage (the buffer comes from a Win32 edit session here, but the
        // decision is pure CPU work over a wchar_t span). Reaching this point
        // means len != 0, so ComputeShouldAutoCap's empty-buffer=DocStart
        // convention can't fire off an unreadable range — a failed GetText
        // returned above with shouldAutoCap_ still false.
        shouldAutoCap_ = ComputeShouldAutoCap(buf, len);

        return S_OK;
    }

    [[nodiscard]] const std::wstring& Word() const noexcept { return word_; }
    [[nodiscard]] ITfRange* DetachWordRange() noexcept { return wordRange_.Detach(); }
    [[nodiscard]] bool ShouldAutoCap() const noexcept { return shouldAutoCap_; }
    [[nodiscard]] bool AtDocStart() const noexcept { return atDocStart_; }

private:
    std::wstring word_;
    CComPtr<ITfRange> wordRange_;
    bool shouldAutoCap_ = false;
    bool atDocStart_ = false;
};

/// Seeds `engine` to reproduce `word`, preferring an exact raw-keystroke replay
/// over SeedFromText's literal-glyph decomposition. `SeedFromText` restores enough
/// for the English-word check and continued typing/backspace, but can't recover
/// which raw key produced which diacritic — a modifier re-applied after reviving
/// from text alone silently fails to re-tone (see ADR: reopen-word raw replay).
/// When `rawInput` (the engine's own PeekRaw() snapshot at the moment this word
/// was committed) is available and still reproduces `word` exactly, replaying it
/// through the same PushChar path used for live typing is fully faithful. Falls
/// back to SeedFromText if there's no snapshot, or it no longer reproduces `word`
/// (e.g. input method/config changed between commit and revive).
/// ITfRange::SetText flags for the two revive sessions below. They replace text
/// that ALREADY EXISTS in the document (a previously committed word), unlike the
/// start/update paths which write fresh pre-edit. TF_ST_CORRECTION tells the
/// host "this is a correction of existing text", so rich-text hosts (Word,
/// WordPad, Outlook) preserve the replaced run's properties — bold, font,
/// colour — instead of resetting them.
///
/// Kept at 0 (2026-07-29, issue #234): the composition lifecycle was reworked
/// for plain-text hosts and the flag was not re-validated on this path, so
/// adding it now would change the freshly-fixed path untested. Notepad/Chrome/
/// Scintilla are plain text — no observable difference either way.
///
/// TO FIX (one edit): if reopening a *formatted* word loses its formatting —
/// WordPad, type a bold Vietnamese word, Space, then Backspace back into it —
/// set this to TF_ST_CORRECTION, then re-run the four #234 Notepad repros to
/// confirm the pre-edit range behaviour didn't regress.
constexpr DWORD kReviveSetTextFlags = 0;

inline bool SeedRevivedWord(IInputEngine* engine, const std::wstring& word,
                            const std::wstring& rawInput) {
    if (engine == nullptr) return false;
    return RestoreCommittedText(
               *engine, word, [&rawInput](IInputEngine& replayEngine) {
                   for (const wchar_t c : rawInput) replayEngine.PushChar(c);
               }) != CommittedTextRestoreResult::Failed;
}

/// Revive-composition edit session: starts a composition over an existing range that
/// covers a previously-committed Vietnamese word, seeds the engine from that word,
/// then deletes the last char (since this fires from VK_BACK). The resulting
/// composition text is the seeded word minus one char.
///
/// Expected caller flow:
///   1. ReadPrecedingWordEditSession returns (word, range covering word including the char to delete)
///   2. Engine state is empty
///   3. This session: compose covers range, SetText replaces range with word-minus-last-char
class ReviveCompositionEditSession : public EditSession {
public:
    ReviveCompositionEditSession(ITfContext* pContext, CompositionManager* pMgr,
                                 IInputEngine* pEngine, const std::wstring& word,
                                 ITfRange* pRange, const std::wstring& rawInput = std::wstring{})
        : EditSession(pContext), pMgr_(pMgr), pEngine_(pEngine), word_(word),
          pRange_(pRange), rawInput_(rawInput) {  // CComPtr assignment AddRefs automatically
    }

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pMgr_ == nullptr || pEngine_ == nullptr || pContext_ == nullptr || !pRange_) {
            return E_FAIL;
        }
        if (word_.empty()) return E_FAIL;

        // Seed engine_ fresh (English gate already passed upstream via a temp engine).
        if (!SeedRevivedWord(pEngine_, word_, rawInput_)) {
            TSF_LOG(L"ReviveCompositionEditSession: seeding failed on '%ls'",
                    word_.c_str());
            return E_FAIL;
        }

        if (!pMgr_->StartCompositionOnRange(pContext_, ec, pRange_, word_)) {
            // Host refused to compose over the committed word (#245). The key was
            // already eaten upstream, so do the deletion as a plain range edit —
            // the word stays committed instead of reopening, but the Backspace
            // still lands. ponytail: plain SetText, revisit if a host mangles
            // that too.
            TSF_LOG(L"ReviveCompositionEditSession: no range composition, plain-edit backspace");
            pEngine_->Backspace();
            const std::wstring& shortened = pEngine_->Peek();
            const HRESULT hr = pRange_->SetText(ec, 0, shortened.c_str(),
                                                static_cast<LONG>(shortened.size()));
            pEngine_->Reset();
            return hr;
        }

        pEngine_->Backspace();
        const std::wstring& composed = pEngine_->Peek();
        if (!pMgr_->SetCompositionText(ec, composed, kReviveSetTextFlags)) {
            pEngine_->Reset();
            pMgr_->EndComposition(ec);
            return E_FAIL;
        }

        if (pEngine_->Count() == 0) {
            pMgr_->EndComposition(ec);
        }

        TSF_LOG(L"ReviveCompositionEditSession: composition='%ls'", composed.c_str());
        return S_OK;
    }

private:
    CompositionManager* pMgr_;
    IInputEngine* pEngine_;
    std::wstring word_;
    CComPtr<ITfRange> pRange_;
    std::wstring rawInput_;
};

/// Revive-and-type edit session: user typed a character while engine was empty
/// and cursor sat right after a Vietnamese word. Starts composition over the word,
/// seeds the engine, then PushChar'es the incoming char — so e.g. "gõ" + 'f'
/// re-enters composition and produces "gò" (grave tone replaces tilde).
///
/// KNOWN HOST LIMITATION, do not try to fix from here (#245, closed 2026-08-14):
/// Legcord/Electron duplicates the word instead of replacing it — "chuyen" +
/// Space + Left + 'e' lands "chuyenchuyên". Everything TSF can observe says the
/// composition is right: StartComposition on the range returns S_OK, reading the
/// composition's range back returns exactly the word, SetText returns S_OK, and
/// the pre-edit visibly wraps the correct text. The host's own layer inserts
/// rather than replaces. Two shapes were tried and neither helped: verifying the
/// range read-back (it always agreed) and splitting into two edit sessions so the
/// host establishes the composition before any text is written (Chromium gates
/// its SetCompositionFromExistingText path on the IME writing nothing in that
/// session — taking it only changed the corruption from dropping the word's last
/// character to dropping none). Both were reverted. Before touching this again,
/// get a log line proving VKey asked for the wrong thing — the four earlier
/// theories about *why* were all wrong, while "the host ignores the range" held
/// from the first report.
class ReviveAndTypeEditSession : public EditSession {
public:
    ReviveAndTypeEditSession(ITfContext* pContext, CompositionManager* pMgr,
                             IInputEngine* pEngine, const std::wstring& word,
                             ITfRange* pRange, wchar_t ch, bool uppercase,
                             const std::wstring& rawInput = std::wstring{})
        : EditSession(pContext), pMgr_(pMgr), pEngine_(pEngine),
          word_(word), pRange_(pRange), ch_(ch), uppercase_(uppercase), rawInput_(rawInput) {
    }

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pMgr_ == nullptr || pEngine_ == nullptr || pContext_ == nullptr
            || !pRange_ || word_.empty() || ch_ == 0) {
            return E_FAIL;
        }

        if (!SeedRevivedWord(pEngine_, word_, rawInput_)) {
            TSF_LOG(L"ReviveAndTypeEditSession: seeding failed on '%ls'",
                    word_.c_str());
            pEngine_->Reset();
            return S_OK;  // Revived() stays false — caller types the char normally
        }

        if (!pMgr_->StartCompositionOnRange(pContext_, ec, pRange_, word_)) {
            TSF_LOG(L"ReviveAndTypeEditSession: host refused range composition on '%ls'",
                    word_.c_str());
            pEngine_->Reset();
            return S_OK;
        }

        pEngine_->PushKey(ch_, uppercase_);
        const std::wstring& composed = pEngine_->Peek();
        if (!pMgr_->SetCompositionText(ec, composed, kReviveSetTextFlags)) {
            pEngine_->Reset();
            pMgr_->EndComposition(ec);
            return E_FAIL;
        }

        if (pEngine_->Count() == 0) {
            pMgr_->EndComposition(ec);
        }

        TSF_LOG(L"ReviveAndTypeEditSession: '%ls' + '%lc' → '%ls'",
                word_.c_str(), ch_, composed.c_str());
        revived_ = true;
        return S_OK;
    }

    /// False when the word was left committed and untouched — the caller still
    /// owes the document the character it ate, and must type it through the
    /// normal path so auto-cap and macro tracking still run.
    [[nodiscard]] bool Revived() const noexcept { return revived_; }

private:
    CompositionManager* pMgr_;
    IInputEngine* pEngine_;
    std::wstring word_;
    CComPtr<ITfRange> pRange_;
    wchar_t ch_;
    bool uppercase_;
    std::wstring rawInput_;
    bool revived_ = false;
};

/// Edit session to check if the selection is non-empty (for autocomplete detection)
class SelectionCheckEditSession : public EditSession {
public:
    SelectionCheckEditSession(ITfContext* pContext, bool* pHasSelection)
        : EditSession(pContext), pHasSelection_(pHasSelection) {
        if (pHasSelection_) *pHasSelection_ = false;
    }

    IFACEMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (pContext_ == nullptr || pHasSelection_ == nullptr) return E_FAIL;
        *pHasSelection_ = false;

        TF_SELECTION sel = {};
        ULONG fetched = 0;
        HRESULT hr = pContext_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
        if (SUCCEEDED(hr) && fetched == 1 && sel.range != nullptr) {
            CComPtr<ITfRange> pSelRange;
            pSelRange.Attach(sel.range);
            BOOL isEmpty = FALSE;
            if (SUCCEEDED(pSelRange->IsEmpty(ec, &isEmpty))) {
                *pHasSelection_ = (isEmpty == FALSE);
            }
        }
        return S_OK;
    }

private:
    bool* pHasSelection_;
};

}  // namespace TSF
}  // namespace NextKey
