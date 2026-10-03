// VKey - Apply deferred TSF DLL swap at EXE startup
// SPDX-License-Identifier: GPL-3.0-only

#include "PendingDllApply.h"
#include "UpdateInstaller.h"              // TSF_DLL_FILENAME, constants, MakeParkedDllTimestamp
#include "UpdateSecurity.h"               // ComputeFileSha256
#include "core/security/ExtensionTrust.h"
#include "core/Debug.h"
#include "core/Strings.h"
#include "core/ipc/SharedState.h"          // SharedFlags
#include "core/ipc/SharedStateManager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace NextKey {

namespace {

/// Read a marker file produced by HandleTsfDllReplace. Content is a hex
/// SHA-256 string (64 chars) — we trim whitespace and lowercase for a stable
/// compare. Returns empty string on any failure.
std::string ReadMarkerHash(const std::filesystem::path& markerPath) noexcept {
    std::ifstream in(markerPath, std::ios::binary);
    if (!in.is_open()) return {};
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    // Trim leading/trailing non-hex characters (whitespace, newlines), keep the
    // hex run in the middle. std::isxdigit needs unsigned char to avoid UB
    // on values > 127.
    auto is_hex = [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    };
    auto begin = std::find_if(content.begin(), content.end(), is_hex);
    auto end = content.end();
    while (end > begin && !is_hex(*(end - 1))) --end;
    std::string hex(begin, end);
    for (auto& c : hex) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return hex.size() == 64 ? hex : std::string{};
}

std::wstring GetExeDirW() noexcept {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0) return L".";
    std::wstring full(buf, len);
    auto pos = full.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? full.substr(0, pos) : L".";
}

}  // namespace

PendingDllState ApplyPendingDllUpdate() noexcept {
    namespace fs = std::filesystem;

    std::wstring exeDir = GetExeDirW();
    fs::path pending = fs::path(exeDir) / (std::wstring(TSF_DLL_FILENAME) + TSF_DLL_PENDING_SUFFIX);
    fs::path live    = fs::path(exeDir) / TSF_DLL_FILENAME;
    fs::path marker  = fs::path(exeDir) / TSF_DLL_PENDING_MARKER;
    fs::path oldDir  = fs::path(exeDir) / OLD_VERSION_DIRNAME;

    std::error_code ec;

    if (!fs::exists(pending, ec)) {
        fs::remove(marker, ec);   // defensive: clean orphan markers
        return PendingDllState::None;
    }

    // SHA-256 gate — only apply .pending if the marker's recorded hash
    // matches the file's current hash. Protects against user-dropped or
    // corrupted .pending files, restoring parity with the %TEMP%+SHA256
    // guarantees of the --install-update flow.
    {
        std::string expected = ReadMarkerHash(marker);
        std::string actual   = ComputeFileSha256(pending);
        if (expected.empty() || actual.empty() || expected != actual ||
            !Security::IsExtensionTrusted(pending.wstring(), Security::ExtensionBinary::TsfDll)) {
            // Unauthenticated or corrupted pending — throw it away so the
            // next boot starts clean. Don't install it.
            fs::remove(pending, ec);
            fs::remove(marker, ec);
            return PendingDllState::None;
        }
    }

    fs::create_directories(oldDir, ec);
    fs::path parked = oldDir / (std::wstring(TSF_DLL_FILENAME)
                              + MakeParkedDllTimestamp(L"_pending"));

    fs::rename(live, parked, ec);
    if (ec) {
        return PendingDllState::SwapFailed;
    }

    std::error_code ec2;
    fs::rename(pending, live, ec2);
    if (ec2) {
        fs::rename(parked, live, ec);   // rollback
        return PendingDllState::SwapFailed;
    }

    fs::remove(marker, ec);
    return PendingDllState::SwapDoneNeedsReboot;
}

void RestartWindowsNow() noexcept {
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp{};
        if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (!AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr)
                || GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
                NEXTKEY_LOG(L"RestartWindowsNow: SE_SHUTDOWN_NAME not granted (err=%lu) — ExitWindowsEx will likely fail",
                            GetLastError());
            }
        } else {
            NEXTKEY_LOG(L"RestartWindowsNow: LookupPrivilegeValueW failed (err=%lu)", GetLastError());
        }
        CloseHandle(hToken);
    } else {
        NEXTKEY_LOG(L"RestartWindowsNow: OpenProcessToken failed (err=%lu)", GetLastError());
    }

    if (!ExitWindowsEx(EWX_REBOOT | EWX_RESTARTAPPS,
                       SHTDN_REASON_MAJOR_APPLICATION
                       | SHTDN_REASON_MINOR_UPGRADE
                       | SHTDN_REASON_FLAG_PLANNED)) {
        NEXTKEY_LOG(L"RestartWindowsNow: ExitWindowsEx failed (err=%lu)", GetLastError());
    }
}

void RestartWindowsWithPrompt(HWND owner) noexcept {
    if (MessageBoxW(owner, S(StringId::UPDATE_BANNER_CONFIRM), L"VKey",
                    MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK) {
        RestartWindowsNow();
    }
}

UpdateBannerState GetUpdateBannerState(const SharedStateManager& shared) noexcept {
    if (!shared.IsConnected()) return UpdateBannerState::Hide;
    const uint32_t flags = shared.ReadFlags();
    if (flags & SharedFlags::TSF_PENDING_DLL_SWAP) return UpdateBannerState::PendingSwap;
    // TSF_ENGINE_UNTRUSTED shares this banner deliberately: its cause is the same
    // stale-DLL skew, and UPDATE_BANNER_MISMATCH already tells the user to restart
    // Windows — which is exactly the fix.
    if (flags & (SharedFlags::TSF_POST_UPDATE_REBOOT | SharedFlags::TSF_ABI_MISMATCH |
                 SharedFlags::TSF_ENGINE_UNTRUSTED)) {
        return UpdateBannerState::Mismatch;
    }
    return UpdateBannerState::Hide;
}

}  // namespace NextKey
