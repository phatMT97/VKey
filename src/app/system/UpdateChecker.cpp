// VKey - Update Checker Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "UpdateChecker.h"
#include "UpdateSecurity.h"
#include "CancelableBindStatusCallback.h"
#include "core/Version.h"
#include "core/Strings.h"
#include "core/WinStrings.h"
#include "core/CrashLog.h"
#include "core/config/ConfigManager.h"
#include "core/Debug.h"

#include <ole2.h>
#include <urlmon.h>
#include <CommCtrl.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <exception>
#include <thread>
#include <memory>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shlwapi.lib")

namespace NextKey {

namespace {

/// Get a temporary file path with the given filename
std::wstring GetTempFilePath(const wchar_t* filename) {
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + filename;
}

/// Compact a filesystem path for display, delegating to the Win32 API
/// which handles UNC, long, and drive-letter paths correctly.
std::wstring CompactPath(const std::wstring& path, UINT maxChars) {
    std::wstring buf(maxChars + 1, L'\0');
    if (PathCompactPathExW(buf.data(), path.c_str(), maxChars + 1, 0)) {
        buf.resize(wcslen(buf.c_str()));
        return buf;
    }
    return path;  // API failed — return original
}

HRESULT ShowTopmostTaskDialog(HWND parent, PCWSTR title, PCWSTR mainInstruction, PCWSTR content, TASKDIALOG_COMMON_BUTTON_FLAGS buttons, PCWSTR icon) {
    TASKDIALOGCONFIG tdc = {};
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = parent;
    tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tdc.pszWindowTitle = title;
    tdc.pszMainInstruction = mainInstruction;
    tdc.pszContent = content;
    tdc.dwCommonButtons = buttons;
    tdc.pszMainIcon = icon;
    tdc.pfCallback = [](HWND hwnd, UINT notification, WPARAM, LPARAM, LONG_PTR) -> HRESULT {
        if (notification == TDN_CREATED) {
            SetForegroundWindow(hwnd);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        return S_OK;
    };
    return TaskDialogIndirect(&tdc, nullptr, nullptr, nullptr);
}

}  // namespace


std::string UpdateChecker::DownloadToString(const std::wstring& url) noexcept {
    try {
        std::wstring tempFile = GetTempFilePath(L"vkey_update_check.tmp");

        // URLDownloadToFileW is the simplest WinAPI HTTP download
        HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), tempFile.c_str(), 0, nullptr);
        if (FAILED(hr)) {
            DeleteFileW(tempFile.c_str());
            return {};
        }

        // Read the temp file
        std::ifstream file(std::filesystem::path(tempFile), std::ios::binary);
        if (!file.is_open()) {
            DeleteFileW(tempFile.c_str());
            return {};
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        file.close();
        DeleteFileW(tempFile.c_str());

        return ss.str();
    } catch (...) {
        return {};
    }
}

std::string UpdateChecker::ExtractJsonString(const std::string& json, const std::string& key) {
    // Simple JSON string extraction: find "key":"value" or "key": "value"
    std::string searchKey = "\"" + key + "\"";
    auto pos = json.find(searchKey);
    if (pos == std::string::npos) return {};

    // Skip key and find colon
    pos += searchKey.size();
    pos = json.find(':', pos);
    if (pos == std::string::npos) return {};
    pos++;

    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) {
        pos++;
    }

    // Expect opening quote
    if (pos >= json.size() || json[pos] != '"') return {};
    pos++;

    // Find closing quote (handle escaped quotes)
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;  // Skip escape char
            if (json[pos] == 'n') result += '\n';
            else if (json[pos] == 't') result += '\t';
            else result += json[pos];
        } else {
            result += json[pos];
        }
        pos++;
    }

    return result;
}

std::string UpdateChecker::FindAssetUrl(const std::string& json, const std::string& assetName) {
    // Find the asset with matching name in the "assets" array
    // Look for "name":"assetName" then find nearest "browser_download_url"
    std::string searchName = "\"name\":\"" + assetName + "\"";

    // Also try with space after colon
    auto pos = json.find(searchName);
    if (pos == std::string::npos) {
        searchName = "\"name\": \"" + assetName + "\"";
        pos = json.find(searchName);
    }
    if (pos == std::string::npos) return {};

    // Search for browser_download_url starting from the asset name position
    // GitHub API usually puts browser_download_url after the name in the asset object.
    return ExtractJsonString(json.substr(pos), "browser_download_url");
}

uint32_t UpdateChecker::ParseVersion(const std::wstring& versionStr) noexcept {
    if (versionStr.empty()) return 0;

    std::wstring ver = versionStr;

    // Strip 'v' or 'V' prefix
    if (ver[0] == L'v' || ver[0] == L'V') {
        ver = ver.substr(1);
    }

    // Strip suffix like -rc1, -beta, etc.
    auto dashPos = ver.find(L'-');
    if (dashPos != std::wstring::npos) {
        ver = ver.substr(0, dashPos);
    }

    // Parse major.minor.patch
    unsigned int major = 0, minor = 0, patch = 0;
    if (swscanf_s(ver.c_str(), L"%u.%u.%u", &major, &minor, &patch) < 1) {
        return 0;
    }

    return (major << 16) | (minor << 8) | patch;
}

UpdateInfo UpdateChecker::CheckForUpdate() noexcept {
    UpdateInfo info;

    try {
        std::string response = DownloadToString(API_URL);
        if (response.empty()) return info;

        // Extract tag_name (version)
        std::string tagName = ExtractJsonString(response, "tag_name");
        if (tagName.empty()) return info;

        std::wstring version = Utf8ToWide(tagName);
        uint32_t remoteVersion = ParseVersion(version);
        uint32_t localVersion = VKEY_VERSION_PACKED;

        info.checkSucceeded = true;  // API call worked

        if (remoteVersion <= localVersion) return info;

        // Extract release page URL
        std::string htmlUrl = ExtractJsonString(response, "html_url");

        // Find release asset
#ifdef VKEY_LITE_MODE
        std::string assetUrl = FindAssetUrl(response, "VKeyClassic-x64.zip");
#else
        std::string assetUrl = FindAssetUrl(response, "VKey-x64.zip");
#endif

        if (assetUrl.empty()) return info;

        // SEC-003: Validate download URL points to allowed GitHub domain
        if (!IsAllowedDownloadUrl(assetUrl)) {
            return info;  // Reject non-GitHub URLs — possible API response tampering
        }

        info.available = true;
        info.version = version;
        // Strip 'v' prefix for display
        if (!info.version.empty() && (info.version[0] == L'v' || info.version[0] == L'V')) {
            info.version = info.version.substr(1);
        }
        info.downloadUrl = Utf8ToWide(assetUrl);
        info.changelogUrl = Utf8ToWide(htmlUrl);
        info.packedVersion = remoteVersion;
    } catch (...) {
        // Network or parsing error
    }

    return info;
}

bool UpdateChecker::DownloadFile(const std::wstring& url, const std::wstring& localPath, std::atomic<bool>& cancelFlag) noexcept {
    CancelableBindStatusCallback callback(cancelFlag);
    HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), localPath.c_str(), 0, &callback);
    return SUCCEEDED(hr);
}

bool UpdateChecker::ShowUpdateDialog(HWND parent, const UpdateInfo& info) {
    // Format the content string with version
    wchar_t content[1024];
    swprintf_s(content, S(StringId::UPDATE_AVAILABLE_BODY), info.version.c_str());

    // Build changelog link text
    std::wstring footerText;
    if (!info.changelogUrl.empty()) {
        footerText = L"<a href=\"";
        footerText += info.changelogUrl;
        footerText += L"\">";
        footerText += S(StringId::UPDATE_CHANGELOG);
        footerText += L"</a>";
    }

    TASKDIALOGCONFIG tdc = {};
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = parent;
    tdc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_COMMAND_LINKS;
    tdc.pszWindowTitle = L"VKey";
    tdc.pszMainIcon = TD_INFORMATION_ICON;
    tdc.pszMainInstruction = S(StringId::UPDATE_AVAILABLE_TITLE);
    tdc.pszContent = content;

    if (!footerText.empty()) {
        tdc.pszFooter = footerText.c_str();
        tdc.pszFooterIcon = TD_INFORMATION_ICON;
    }

    TASKDIALOG_BUTTON buttons[] = {
        { 1001, S(StringId::UPDATE_NOW) },
        { 1002, S(StringId::UPDATE_SKIP) },
    };
    tdc.pButtons = buttons;
    tdc.cButtons = 2;
    tdc.nDefaultButton = 1001;

    // Hyperlink callback
    tdc.pfCallback = [](HWND hwnd, UINT notification, WPARAM, LPARAM lParam, LONG_PTR) -> HRESULT {
        if (notification == TDN_CREATED) {
            SetForegroundWindow(hwnd);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        else if (notification == TDN_HYPERLINK_CLICKED) {
            ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lParam), nullptr, nullptr, SW_SHOW);
        }
        return S_OK;
    };

    int button = 0;
    HRESULT hr = TaskDialogIndirect(&tdc, &button, nullptr, nullptr);
    if (FAILED(hr)) return false;
    return button == 1001;
}

void UpdateChecker::ShowUpToDateMessage(HWND parent) {
    TaskDialog(parent, nullptr, L"VKey", S(StringId::UPDATE_TITLE),
               S(StringId::UPDATE_LATEST), TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
}

void UpdateChecker::ShowCheckFailedMessage(HWND parent) {
    TaskDialog(parent, nullptr, L"VKey", S(StringId::UPDATE_TITLE),
               S(StringId::UPDATE_FAILED), TDCBF_OK_BUTTON, TD_WARNING_ICON, nullptr);
}

bool UpdateChecker::ShowProgressDialog(HWND parent, const wchar_t* message,
                                       std::atomic<bool>& doneFlag) {
    struct Ctx { std::atomic<bool>& done; };
    Ctx ctx{doneFlag};

    TASKDIALOGCONFIG tdc = {};
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = parent;
    tdc.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER;
    tdc.pszWindowTitle = L"VKey";
    tdc.pszMainInstruction = message;
    tdc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tdc.lpCallbackData = reinterpret_cast<LONG_PTR>(&ctx);
    tdc.pfCallback = [](HWND hwnd, UINT notification, WPARAM, LPARAM,
                        LONG_PTR refData) -> HRESULT {
        auto* c = reinterpret_cast<Ctx*>(refData);
        if (notification == TDN_CREATED) {
            SendMessageW(hwnd, TDM_SET_MARQUEE_PROGRESS_BAR, TRUE, 0);
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
            SetForegroundWindow(hwnd);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        if (notification == TDN_TIMER) {
            if (c->done.load(std::memory_order_acquire)) {
                PostMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
            }
        }
        return S_OK;
    };

    int button = 0;
    TaskDialogIndirect(&tdc, &button, nullptr, nullptr);
    return doneFlag.load(std::memory_order_acquire);
}

bool UpdateChecker::DownloadWithProgress(HWND parent, const std::wstring& downloadUrl, std::uint32_t expectedVersion) {
    struct State {
        std::atomic<bool> done{false};
        std::atomic<bool> cancel{false};
        bool success = false;
    };
    auto state = std::make_shared<State>();

    std::thread([state, downloadUrl, expectedVersion]() {
        try {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            state->success = DownloadAndLaunchInstaller(downloadUrl, state->cancel, expectedVersion);
            CoUninitialize();
        } catch (const std::exception& e) {
            CrashLog(L"UpdateChecker::DownloadWithProgress::thread", e.what());
        } catch (...) {
            CrashLog(L"UpdateChecker::DownloadWithProgress::thread", "(non-std exception)");
        }
        state->done.store(true, std::memory_order_release);
    }).detach();

    bool completed = ShowProgressDialog(parent, S(StringId::UPDATE_DOWNLOADING), state->done);

    if (!completed) {
        state->cancel.store(true, std::memory_order_release);
        return false;  // User cancelled
    }

    if (!state->success) {
        if (!state->cancel.load(std::memory_order_acquire)) {
            ShowTopmostTaskDialog(parent, L"VKey", S(StringId::UPDATE_TITLE),
                                  S(StringId::UPDATE_DOWNLOAD_FAILED), TDCBF_OK_BUTTON, TD_WARNING_ICON);
        }
        return false;
    }

    // Back up the current configuration file
    std::wstring backupPath;
    std::wstring activeConfig = ConfigManager::GetConfigPath();
    DWORD attrs = GetFileAttributesW(activeConfig.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring primaryBackup = activeConfig + L".bak";
        if (CopyFileW(activeConfig.c_str(), primaryBackup.c_str(), FALSE)) {
            backupPath = primaryBackup;
        } else {
            std::wstring appDataDir = ConfigManager::GetAppDataDirectory();
            CreateDirectoryW(appDataDir.c_str(), nullptr);  // ensure dir exists
            std::wstring fallbackBackup = appDataDir + L"\\config.toml.bak";
            if (CopyFileW(activeConfig.c_str(), fallbackBackup.c_str(), FALSE)) {
                backupPath = fallbackBackup;
            } else {
                NEXTKEY_LOG(L"[Update] Failed to copy config to primary or fallback backup path");
            }
        }
    }

    if (!backupPath.empty()) {
        wchar_t content[512] = {0};
        std::wstring compacted = CompactPath(backupPath, 60);
        swprintf_s(content, S(StringId::UPDATE_BACKUP_SUCCESS), compacted.c_str());
        ShowTopmostTaskDialog(parent, L"VKey", S(StringId::UPDATE_TITLE),
                              content, TDCBF_OK_BUTTON, TD_INFORMATION_ICON);
    }

    return true;
}

bool UpdateChecker::DownloadAndLaunchInstaller(const std::wstring& downloadUrl, std::atomic<bool>& cancelFlag, std::uint32_t expectedVersion) noexcept {
    try {
        if (cancelFlag.load(std::memory_order_relaxed)) return false;

        wchar_t tempDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempDir);
        wchar_t placeholder[MAX_PATH] = {};
        if (!GetTempFileNameW(tempDir, L"VKU", 0, placeholder)) return false;
        std::wstring zipPath = std::wstring(placeholder) + L".zip";
        DeleteFileW(placeholder);
        struct StagedPackage {
            std::wstring path;
            bool handedOff = false;
            ~StagedPackage() {
                if (!handedOff) {
                    DeleteFileW(path.c_str());
                    DeleteFileW((path + L".sig").c_str());
                }
            }
        } staged{zipPath};

        if (!DownloadFile(downloadUrl, zipPath, cancelFlag)) return false;

        if (cancelFlag.load(std::memory_order_relaxed)) {
            DeleteFileW(zipPath.c_str());
            return false;
        }

        // SEC-001: Verify ZIP hash against .sha256 sidecar
        if (!VerifyDownloadedZip(downloadUrl, zipPath, cancelFlag)) {
            DeleteFileW(zipPath.c_str());
            return false;
        }

        if (cancelFlag.load(std::memory_order_relaxed)) {
            DeleteFileW(zipPath.c_str());
            return false;
        }

        if (!DownloadFile(downloadUrl + L".sig", zipPath + L".sig", cancelFlag) ||
            cancelFlag.load(std::memory_order_acquire) ||
            !VerifySignedUpdateZip(zipPath, expectedVersion)) return false;

        // Launch updater: self --install-update <zip>
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        std::wstring cmdLine = L"\"";
        cmdLine += exePath;
        cmdLine += L"\" --install-update \"";
        cmdLine += zipPath;
        cmdLine += L"\"";

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        // CREATE_BREAKAWAY_FROM_JOB: installer must outlive the main process.
        // Without this, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE kills the installer
        // when the main process exits, preventing restart after update.
        if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                            CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi)) {
            // Fallback: some restricted job objects do not allow breakaway.
            // Launch without the flag to proceed with update, even if inside the same job.
            ZeroMemory(&pi, sizeof(pi));
            if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                                0, nullptr, nullptr, &si, &pi)) {
                return false;
            }
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        staged.handedOff = true;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace NextKey
