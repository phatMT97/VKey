// VKey - Advanced engine download policy
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace NextKey {

inline constexpr wchar_t kAdvancedEngineAssetName[] = L"vkey_engine.dll";

enum class AdvancedEngineDownloadAsset { Engine, Signature };

// Each response has its own byte budget. In particular the detached signature
// is 72 bytes, regardless of the DLL's locked byte length.
class AdvancedEngineDownloadBudget {
public:
    AdvancedEngineDownloadBudget(AdvancedEngineDownloadAsset asset, std::uint64_t engineBytes) noexcept;
    [[nodiscard]] std::uint64_t ExpectedBytes() const noexcept { return expected_; }
    [[nodiscard]] bool MatchesContentLength(std::uint64_t bytes) const noexcept { return bytes == expected_; }
    [[nodiscard]] bool AcceptChunk(std::uint64_t bytes) noexcept;
    [[nodiscard]] bool Complete() const noexcept { return received_ == expected_; }
private:
    std::uint64_t expected_;
    std::uint64_t received_ = 0;
};

[[nodiscard]] std::wstring BuildAdvancedEngineReleaseUrl();
[[nodiscard]] std::wstring BuildAdvancedEngineReleasePageUrl();

/// The detached signature published beside the engine. Release verifies this
/// instead of a baked hash, so an engine downloaded without it is unloadable.
[[nodiscard]] std::wstring BuildAdvancedEngineSignatureUrl();
[[nodiscard]] bool IsAllowedAdvancedEngineUrl(std::wstring_view url) noexcept;

} // namespace NextKey
