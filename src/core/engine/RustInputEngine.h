// VKey - Rust engine adapter
// Copyright (c) 2024-2026 PhatMT. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "IInputEngine.h"
#include "../config/TypingConfig.h"

#include <cstdint>
#include <memory>
#include <string>

namespace NextKey {

/// Immutable ABI-v8 exact-protection dictionary compiled off the key path.
/// The opaque Rust handle is shared by hosts and may be attached to any number
/// of empty Rust engine sessions in this process.
class RustUserDictionarySnapshot final {
public:
    ~RustUserDictionarySnapshot();

    RustUserDictionarySnapshot(const RustUserDictionarySnapshot&) = delete;
    RustUserDictionarySnapshot& operator=(const RustUserDictionarySnapshot&) = delete;

private:
    explicit RustUserDictionarySnapshot(void* handle) : handle_(handle) {}

    void* handle_ = nullptr;  // opaque VKeyUserDictionary*

    friend class RustInputEngine;
};

enum class RustUserDictionaryLoadStatus : uint8_t {
    Loaded,
    EngineUnavailable,
    IoError,
    InvalidUtf8,
    EngineRejected,
};

struct RustUserDictionaryLoadResult {
    std::shared_ptr<const RustUserDictionarySnapshot> snapshot;
    std::wstring path;
    RustUserDictionaryLoadStatus status = RustUserDictionaryLoadStatus::IoError;
    uint32_t engineStatus = 0;
    size_t errorLine = 0;
    bool created = false;

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == RustUserDictionaryLoadStatus::Loaded && snapshot != nullptr;
    }
};

/// IInputEngine adapter for the trusted, runtime-loaded vkey_engine C ABI.
class RustInputEngine : public IInputEngine {
public:
    explicit RustInputEngine(const TypingConfig& config);
    ~RustInputEngine() override;

    RustInputEngine(const RustInputEngine&) = delete;
    RustInputEngine& operator=(const RustInputEngine&) = delete;

    void PushChar(wchar_t c) override;
    void PushKey(wchar_t physicalChar, bool uppercase) override;
    void Backspace() override;
    [[nodiscard]] const std::wstring& Peek() const override { return peek_; }
    [[nodiscard]] std::wstring Commit() override;
    void Reset() override;
    void ClearContext() noexcept override;
    [[nodiscard]] size_t Count() const override { return count_; }
    [[nodiscard]] bool HasActiveQuickConsonant() const override;
    [[nodiscard]] bool SeedFromText(const std::wstring& text) override;
    [[nodiscard]] bool IsEnglishWord() const override;
    [[nodiscard]] bool IsToneEscaped() const override;
    [[nodiscard]] std::wstring PeekRaw() const override { return raw_; }
    [[nodiscard]] std::wstring_view PeekRawView() const noexcept override { return raw_; }
    [[nodiscard]] bool ShouldReplayCommittedKey(
        std::wstring_view rawInput, wchar_t key) const override;
    [[nodiscard]] bool LastCommitWasCorrected() const override;

    /// Whether the library passed artifact, ABI and runtime-identity checks.
    [[nodiscard]] static bool LibraryAvailable();

    /// Empty when LibraryAvailable(); otherwise a bounded diagnostic.
    [[nodiscard]] static std::wstring UnavailableReason();

    /// Resolve `user_dictionary.txt` beside config.toml and read/compile it.
    /// Missing and empty files both produce a valid empty snapshot WITHOUT
    /// creating the file on disk. Invalid/unreadable files return failure so
    /// callers can retain their last valid snapshot.
    /// Cold path only: performs file I/O, UTF-8 decoding and allocation.
    [[nodiscard]] static RustUserDictionaryLoadResult LoadUserDictionary(
        const std::wstring& configPath);

    /// Explicitly create the documented `user_dictionary.txt` template beside
    /// config.toml if it does not already exist. Idempotent and race-safe.
    /// Called only when the user explicitly triggers an Open/Create action from UI.
    static bool CreateUserDictionaryTemplate(
        const std::wstring& configPath, bool* created = nullptr);

    /// Directly compile a dictionary snapshot from a UTF-16 newline-delimited buffer (e.g. from LexiconWireView).
    /// Zero disk I/O, zero conversions, zero file locks.
    [[nodiscard]] static std::shared_ptr<const RustUserDictionarySnapshot>
    CreateUserDictionaryFromUtf16(const uint16_t* utf16Units, size_t count);

    /// Install process-global spell-check exclusions from a UTF-16 newline-delimited buffer (e.g. from LexiconWireView).
    /// Returns true on success. Sets outChanged to true if the exclusions differed from previously installed.
    static bool SetSpellExclusionsFromUtf16(
        const uint16_t* utf16Units, size_t count, bool* outChanged = nullptr);

    /// Attach a precompiled immutable snapshot. Null clears protection. Returns
    /// false without changing the engine if composition is active.
    [[nodiscard]] bool SetUserDictionary(
        const std::shared_ptr<const RustUserDictionarySnapshot>& snapshot);

private:
    void* handle_ = nullptr;  // opaque VKeyEngine*
    std::wstring peek_;
    std::wstring raw_;   // case-preserved physical keys, for PeekRaw/PeekRawView
    size_t count_ = 0;

    void Refresh();
};

}  // namespace NextKey
