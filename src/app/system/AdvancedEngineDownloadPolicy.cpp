// VKey - Advanced engine download policy
// SPDX-License-Identifier: GPL-3.0-only

#include "AdvancedEngineDownloadPolicy.h"

#include "core/Version.h"
#include "core/engine/EngineSignature.h"

#include <array>

namespace NextKey {
namespace {

bool EqualsAsciiIgnoreCase(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        wchar_t a = left[i];
        wchar_t b = right[i];
        if (a >= L'A' && a <= L'Z')
            a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z')
            b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) {
            return false;
        }
    }
    return true;
}

} // namespace

AdvancedEngineDownloadBudget::AdvancedEngineDownloadBudget(
    AdvancedEngineDownloadAsset asset, std::uint64_t engineBytes) noexcept
    : expected_(asset == AdvancedEngineDownloadAsset::Signature ? EngineSignature::kFileBytes : engineBytes) {}

bool AdvancedEngineDownloadBudget::AcceptChunk(std::uint64_t bytes) noexcept {
    if (bytes > expected_ - received_) return false;
    received_ += bytes;
    return true;
}

std::wstring BuildAdvancedEngineReleaseUrl() {
    return L"https://github.com/phatMT97/VKey/releases/download/"
           L"v" VKEY_VERSION_WSTR L"/vkey_engine.dll";
}

std::wstring BuildAdvancedEngineReleasePageUrl() {
    return L"https://github.com/phatMT97/VKey/releases/tag/"
           L"v" VKEY_VERSION_WSTR;
}

std::wstring BuildAdvancedEngineSignatureUrl() {
    return BuildAdvancedEngineReleaseUrl() + L".sig";
}

bool IsAllowedAdvancedEngineUrl(std::wstring_view url) noexcept {
    constexpr std::wstring_view scheme = L"https://";
    if (url.size() <= scheme.size() || !EqualsAsciiIgnoreCase(url.substr(0, scheme.size()), scheme)) {
        return false;
    }

    const size_t authorityEnd = url.find_first_of(L"/?#", scheme.size());
    const std::wstring_view authority =
        url.substr(scheme.size(),
                   authorityEnd == std::wstring_view::npos ? std::wstring_view::npos : authorityEnd - scheme.size());

    constexpr std::array<std::wstring_view, 3> allowedHosts = {
        L"github.com",
        L"objects.githubusercontent.com",
        L"release-assets.githubusercontent.com",
    };
    for (const std::wstring_view host : allowedHosts) {
        if (EqualsAsciiIgnoreCase(authority, host)) {
            return true;
        }
    }
    return false;
}

} // namespace NextKey
