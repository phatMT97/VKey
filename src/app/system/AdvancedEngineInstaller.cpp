// VKey - Advanced engine installer
// SPDX-License-Identifier: GPL-3.0-only

#include "AdvancedEngineInstaller.h"

#include "AdvancedEngineDownloadPolicy.h"
#include "AdvancedEngineStorage.h"
#include "UpdateChecker.h"
#include "core/CrashLog.h"
#include "core/Logger.h"
#include "core/Strings.h"
#include "core/Version.h"
#include "core/WinFileSystem.h"
#include "core/engine/RustEngineTrust.h"
#include "core/engine/RustInputEngine.h"

#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace NextKey {
namespace {

constexpr DWORD kConnectTimeoutMs = 5'000;
constexpr DWORD kSendTimeoutMs = 15'000;
constexpr DWORD kReceiveTimeoutMs = 15'000;
constexpr DWORD kMaxRedirectLocationBytes = 8 * 1024;
constexpr unsigned kMaxRedirects = 5;

enum class InstallResult : std::uint8_t {
    Installed,
    Cancelled,
    NetworkFailure,
    VerificationFailure,
    StorageFailure,
    ActivationFailure,
};

enum class InstallChoice : std::uint8_t {
    Automatic,
    Manual,
    Standard,
};

constexpr int kAutomaticButtonId = 1001;
constexpr int kManualButtonId = 1002;
constexpr int kStandardButtonId = 1003;
constexpr int kOpenFolderButtonId = 1004;

class UniqueInternet final {
public:
    explicit UniqueInternet(HINTERNET value = nullptr) noexcept : value_(value) {}
    ~UniqueInternet() {
        if (value_) {
            ::WinHttpCloseHandle(value_);
        }
    }

    UniqueInternet(const UniqueInternet&) = delete;
    UniqueInternet& operator=(const UniqueInternet&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept { return value_; }

private:
    HINTERNET value_;
};

struct ParsedUrl {
    std::wstring host;
    std::wstring resource;
};

std::wstring EnginePath(const std::wstring& directory) { return directory + L"\\" + kAdvancedEngineAssetName; }

std::wstring SignaturePath(const std::wstring& directory) { return EnginePath(directory) + L".sig"; }

bool IsTrustedInstalledEngine(const std::wstring& path) noexcept {
    UniqueFile file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    const std::wstring signature = path + L".sig";
    return file.valid() &&
           VerifyRustEngineFileHandle(file.get(), signature.c_str()) == RustEngineTrustStatus::Trusted;
}

bool ParseHttpsUrl(const std::wstring& url, ParsedUrl& parsed) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS || components.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
        !components.lpszHostName || components.dwHostNameLength == 0) {
        return false;
    }

    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.resource.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (parsed.resource.empty()) {
        parsed.resource = L"/";
    }
    if (components.lpszExtraInfo && components.dwExtraInfoLength != 0) {
        parsed.resource.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    return true;
}

bool QueryStatusCode(HINTERNET request, DWORD& status) noexcept {
    DWORD bytes = sizeof(status);
    return ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX) != FALSE;
}

bool QueryRedirectLocation(HINTERNET request, std::wstring& location) {
    DWORD bytes = 0;
    if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                              &bytes, WINHTTP_NO_HEADER_INDEX) ||
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 || bytes > kMaxRedirectLocationBytes) {
        return false;
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &bytes,
                               WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    location.assign(buffer.data(), bytes / sizeof(wchar_t));
    while (!location.empty() && location.back() == L'\0') {
        location.pop_back();
    }
    return !location.empty();
}

bool IsRedirectStatus(DWORD status) noexcept {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

InstallResult StreamResponseToFile(HINTERNET request, HANDLE file, AdvancedEngineDownloadAsset asset,
                                   std::atomic<bool>& cancel) noexcept {
    AdvancedEngineDownloadBudget budget(asset, ExpectedRustEngineByteLength());
    const std::uint64_t expectedBytes = budget.ExpectedBytes();
    if (expectedBytes > (std::numeric_limits<DWORD>::max)()) {
        return InstallResult::VerificationFailure;
    }

    DWORD contentLength = 0;
    DWORD contentLengthBytes = sizeof(contentLength);
    if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthBytes,
                              WINHTTP_NO_HEADER_INDEX)) {
        if (!budget.MatchesContentLength(contentLength)) {
            return InstallResult::VerificationFailure;
        }
    } else if (::GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) {
        return InstallResult::NetworkFailure;
    }

    std::array<std::uint8_t, 64 * 1024> buffer{};
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) {
            return InstallResult::Cancelled;
        }

        DWORD bytesRead = 0;
        if (!::WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead)) {
            return InstallResult::NetworkFailure;
        }
        if (bytesRead == 0) {
            break;
        }
        if (!budget.AcceptChunk(bytesRead)) {
            return InstallResult::VerificationFailure;
        }

        DWORD offset = 0;
        while (offset < bytesRead) {
            DWORD bytesWritten = 0;
            if (!::WriteFile(file, buffer.data() + offset, bytesRead - offset, &bytesWritten, nullptr) ||
                bytesWritten == 0) {
                return InstallResult::StorageFailure;
            }
            offset += bytesWritten;
        }
    }

    return budget.Complete() ? InstallResult::Installed : InstallResult::VerificationFailure;
}

InstallResult DownloadToFile(HANDLE file, std::wstring startUrl, AdvancedEngineDownloadAsset asset,
                             std::atomic<bool>& cancel) {
    UniqueInternet session(::WinHttpOpen(L"VKey/" VKEY_VERSION_WSTR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get() ||
        !::WinHttpSetTimeouts(session.get(), kConnectTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs)) {
        return InstallResult::NetworkFailure;
    }

    std::wstring url = std::move(startUrl);
    for (unsigned redirectCount = 0; redirectCount <= kMaxRedirects; ++redirectCount) {
        if (cancel.load(std::memory_order_relaxed)) {
            return InstallResult::Cancelled;
        }
        if (!IsAllowedAdvancedEngineUrl(url)) {
            return InstallResult::VerificationFailure;
        }

        ParsedUrl parsed;
        if (!ParseHttpsUrl(url, parsed)) {
            return InstallResult::VerificationFailure;
        }

        UniqueInternet connection(::WinHttpConnect(session.get(), parsed.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (!connection.get()) {
            return InstallResult::NetworkFailure;
        }

        UniqueInternet request(::WinHttpOpenRequest(connection.get(), L"GET", parsed.resource.c_str(), nullptr,
                                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    WINHTTP_FLAG_SECURE));
        if (!request.get()) {
            return InstallResult::NetworkFailure;
        }

        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!::WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                                sizeof(redirectPolicy)) ||
            !::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !::WinHttpReceiveResponse(request.get(), nullptr)) {
            return InstallResult::NetworkFailure;
        }

        DWORD status = 0;
        if (!QueryStatusCode(request.get(), status)) {
            return InstallResult::NetworkFailure;
        }
        if (IsRedirectStatus(status)) {
            if (redirectCount == kMaxRedirects || !QueryRedirectLocation(request.get(), url)) {
                return InstallResult::NetworkFailure;
            }
            continue;
        }
        if (status != HTTP_STATUS_OK) {
            return InstallResult::NetworkFailure;
        }
        return StreamResponseToFile(request.get(), file, asset, cancel);
    }
    return InstallResult::NetworkFailure;
}

InstallResult InstallEngine(std::atomic<bool>& cancel) noexcept {
    try {
        const std::wstring directory = ModuleDirectory(nullptr);
        if (directory.empty()) {
            return InstallResult::StorageFailure;
        }

        AdvancedEngineStagingFile staging;
        if (!staging.Create(directory)) {
            return InstallResult::StorageFailure;
        }

        const InstallResult download =
            DownloadToFile(staging.get(), BuildAdvancedEngineReleaseUrl(), AdvancedEngineDownloadAsset::Engine, cancel);
        if (download != InstallResult::Installed) {
            return download;
        }

        // The signature has to land before the engine is verified, and it has to
        // be the one published beside this engine — an installed pair from an
        // older release would verify each other but not this download.
        {
            AdvancedEngineStagingFile signatureStaging;
            if (!signatureStaging.Create(directory)) {
                return InstallResult::StorageFailure;
            }
            const InstallResult signatureDownload =
                DownloadToFile(signatureStaging.get(), BuildAdvancedEngineSignatureUrl(), AdvancedEngineDownloadAsset::Signature, cancel);
            if (signatureDownload != InstallResult::Installed) {
                return signatureDownload;
            }
            if (!::FlushFileBuffers(signatureStaging.get())) {
                return InstallResult::StorageFailure;
            }
            if (!signatureStaging.ActivateAs(SignaturePath(directory))) {
                return InstallResult::ActivationFailure;
            }
        } // Release the exclusive signature handle before verification opens it.

        if (VerifyRustEngineFileHandle(staging.get(), SignaturePath(directory).c_str()) !=
            RustEngineTrustStatus::Trusted) {
            return InstallResult::VerificationFailure;
        }
        if (!::FlushFileBuffers(staging.get())) {
            return InstallResult::StorageFailure;
        }
        if (cancel.load(std::memory_order_relaxed)) {
            return InstallResult::Cancelled;
        }
        if (!staging.ActivateAs(EnginePath(directory))) {
            return InstallResult::ActivationFailure;
        }
        return InstallResult::Installed;
    } catch (...) {
        return InstallResult::StorageFailure;
    }
}

[[nodiscard]] bool OpenShellTarget(HWND parent, LPCWSTR target) noexcept {
    if (!target || *target == L'\0') {
        return false;
    }
    const auto result = ::ShellExecuteW(parent, L"open", target, nullptr, nullptr, SW_SHOW);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

void ShowOpenTargetFailure(HWND parent, LPCWSTR target) noexcept {
    try {
        std::wstring content = S(StringId::SPELL_ADVANCED_OPEN_TARGET_FAILED);
        if (target && *target != L'\0') {
            content += L"\n\n";
            content += target;
        }
        ::MessageBoxW(parent, content.c_str(), L"VKey", MB_OK | MB_ICONWARNING);
    } catch (...) {
        ::MessageBoxW(parent, S(StringId::SPELL_ADVANCED_OPEN_TARGET_FAILED), L"VKey", MB_OK | MB_ICONWARNING);
    }
}

HRESULT CALLBACK AdvancedEngineDialogCallback(
    HWND hwnd, UINT notification, WPARAM, LPARAM lParam, LONG_PTR) noexcept {
    if (notification == TDN_CREATED) {
        ::SetForegroundWindow(hwnd);
        ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    } else if (notification == TDN_HYPERLINK_CLICKED) {
        const auto target = reinterpret_cast<LPCWSTR>(lParam);
        if (!OpenShellTarget(hwnd, target)) {
            ShowOpenTargetFailure(hwnd, target);
        }
    }
    return S_OK;
}

void OpenManualInstallFlow(HWND parent) {
    const std::wstring url = BuildAdvancedEngineReleasePageUrl();
    const std::wstring directory = ModuleDirectory(nullptr);

    // Delegates the request to the default browser. VKey performs no network
    // operation in the manual flow.
    if (!OpenShellTarget(parent, url.c_str())) {
        ShowOpenTargetFailure(parent, url.c_str());
    }

    std::wstring content = S(StringId::SPELL_ADVANCED_MANUAL_BODY);
    content += directory.empty() ? L"-" : directory;
    content += L"\n\n";
    content += S(StringId::SPELL_ADVANCED_MANUAL_FINISH);
    content += L"\n\nURL:\n";
    content += url;

    std::wstring footer = L"<a href=\"";
    footer += url;
    footer += L"\">";
    footer += S(StringId::SPELL_ADVANCED_MANUAL_LINK);
    footer += L"</a>";

    TASKDIALOG_BUTTON buttons[] = {
        {kOpenFolderButtonId, S(StringId::SPELL_ADVANCED_OPEN_FOLDER)},
    };

    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = parent;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS | TDF_USE_COMMAND_LINKS;
    dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    dialog.pszWindowTitle = L"VKey";
    dialog.pszMainIcon = TD_INFORMATION_ICON;
    dialog.pszMainInstruction = S(StringId::SPELL_ADVANCED_MANUAL_TITLE);
    dialog.pszContent = content.c_str();
    dialog.pszFooter = footer.c_str();
    dialog.pszFooterIcon = TD_INFORMATION_ICON;
    dialog.pButtons = buttons;
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.nDefaultButton = IDCLOSE;
    dialog.pfCallback = AdvancedEngineDialogCallback;

    int button = 0;
    if (FAILED(::TaskDialogIndirect(&dialog, &button, nullptr, nullptr))) {
        content += L"\n\n";
        content += S(StringId::SPELL_ADVANCED_OPEN_FOLDER);
        content += L"?";
        button = ::MessageBoxW(parent, content.c_str(), L"VKey", MB_YESNO | MB_ICONINFORMATION) == IDYES
            ? kOpenFolderButtonId
            : IDCLOSE;
    }
    if (button == kOpenFolderButtonId && !directory.empty()) {
        if (!OpenShellTarget(parent, directory.c_str())) {
            ShowOpenTargetFailure(parent, directory.c_str());
        }
    }
}

InstallChoice ShowInstallChoice(HWND parent) {
    TASKDIALOG_BUTTON buttons[] = {
        {kAutomaticButtonId, S(StringId::SPELL_ADVANCED_DOWNLOAD_AUTO)},
        {kManualButtonId, S(StringId::SPELL_ADVANCED_INSTALL_MANUAL)},
        {kStandardButtonId, S(StringId::SPELL_ADVANCED_USE_STANDARD)},
    };

    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = parent;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
    dialog.pszWindowTitle = L"VKey";
    dialog.pszMainIcon = TD_INFORMATION_ICON;
    dialog.pszMainInstruction = S(StringId::SPELL_ADVANCED_REQUIRED_TITLE);
    dialog.pszContent = S(StringId::SPELL_ADVANCED_DOWNLOAD_PROMPT);
    dialog.pButtons = buttons;
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.nDefaultButton = kAutomaticButtonId;
    dialog.pfCallback = AdvancedEngineDialogCallback;

    int button = 0;
    if (FAILED(::TaskDialogIndirect(&dialog, &button, nullptr, nullptr))) {
        std::wstring content = S(StringId::SPELL_ADVANCED_DOWNLOAD_PROMPT);
        content += L"\n\n";
        content += S(StringId::SPELL_ADVANCED_FALLBACK_CHOICE);
        const int fallback =
            ::MessageBoxW(parent, content.c_str(), L"VKey", MB_YESNOCANCEL | MB_ICONINFORMATION);
        if (fallback == IDYES) {
            return InstallChoice::Automatic;
        }
        if (fallback == IDNO) {
            return InstallChoice::Manual;
        }
        return InstallChoice::Standard;
    }
    if (button == kAutomaticButtonId) {
        return InstallChoice::Automatic;
    }
    if (button == kManualButtonId) {
        return InstallChoice::Manual;
    }
    return InstallChoice::Standard;
}

InstallChoice ShowInstallFailure(HWND parent, InstallResult result) noexcept {
    StringId message = StringId::SPELL_ADVANCED_INSTALL_FAILED;
    if (result == InstallResult::NetworkFailure) {
        message = StringId::SPELL_ADVANCED_NETWORK_FAILED;
    } else if (result == InstallResult::VerificationFailure) {
        message = StringId::SPELL_ADVANCED_VERIFY_FAILED;
    }

    TASKDIALOG_BUTTON buttons[] = {
        {kManualButtonId, S(StringId::SPELL_ADVANCED_INSTALL_MANUAL)},
        {kStandardButtonId, S(StringId::SPELL_ADVANCED_USE_STANDARD)},
    };

    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = parent;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
    dialog.pszWindowTitle = L"VKey";
    dialog.pszMainIcon = TD_WARNING_ICON;
    dialog.pszMainInstruction = S(StringId::SPELL_ADVANCED_NOT_ENABLED_TITLE);
    dialog.pszContent = S(message);
    dialog.pButtons = buttons;
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.nDefaultButton = kManualButtonId;
    dialog.pfCallback = AdvancedEngineDialogCallback;

    int button = 0;
    if (FAILED(::TaskDialogIndirect(&dialog, &button, nullptr, nullptr))) {
        try {
            std::wstring content = S(message);
            content += L"\n\n";
            content += S(StringId::SPELL_ADVANCED_FAILURE_FALLBACK_CHOICE);
            return ::MessageBoxW(parent, content.c_str(), L"VKey", MB_YESNO | MB_ICONWARNING) == IDYES
                ? InstallChoice::Manual
                : InstallChoice::Standard;
        } catch (...) {
            return ::MessageBoxW(parent, S(StringId::SPELL_ADVANCED_FAILURE_FALLBACK_CHOICE), L"VKey",
                                 MB_YESNO | MB_ICONWARNING) == IDYES
                ? InstallChoice::Manual
                : InstallChoice::Standard;
        }
    }
    return button == kManualButtonId ? InstallChoice::Manual : InstallChoice::Standard;
}

/// Trusted on disk is not the same as usable in this process: an ABI or symbol
/// mismatch — or a load failure this process already latched — leaves
/// EngineFactory on TypingEngine, so Advanced would read as enabled and do
/// nothing at all. Resolve the library here, while there is still UI to report
/// through, instead of failing silently on the first keystroke.
AdvancedEngineStatus ReadyIfLoadable(HWND parent) {
    if (RustInputEngine::LibraryAvailable()) {
        return AdvancedEngineStatus::Ready;
    }
    Logger::Log(L"[Engine] Advanced rejected: %ls", RustInputEngine::UnavailableReason().c_str());
    return ShowInstallFailure(parent, InstallResult::ActivationFailure) == InstallChoice::Manual
        ? AdvancedEngineStatus::ManualRequested
        : AdvancedEngineStatus::Unavailable;
}

} // namespace

AdvancedEngineStatus AdvancedEngineInstaller::EnsureInstalledWithUi(HWND parent) {
    try {
        const std::wstring directory = ModuleDirectory(nullptr);
        if (!directory.empty() && IsTrustedInstalledEngine(EnginePath(directory))) {
            return ReadyIfLoadable(parent);
        }

        const InstallChoice choice = ShowInstallChoice(parent);
        if (choice == InstallChoice::Manual) {
            return AdvancedEngineStatus::ManualRequested;
        }
        if (choice != InstallChoice::Automatic) {
            return AdvancedEngineStatus::Declined;
        }

        struct State {
            std::atomic<bool> done{false};
            std::atomic<bool> cancel{false};
            InstallResult result = InstallResult::StorageFailure;
        };
        auto state = std::make_shared<State>();
        // jthread, not thread: anything throwing between here and join() would
        // hit a joinable std::thread destructor and std::terminate the process.
        std::jthread worker([state]() {
            try {
                state->result = InstallEngine(state->cancel);
            } catch (const std::exception& error) {
                CrashLog(L"AdvancedEngineInstaller::thread", error.what());
                state->result = InstallResult::StorageFailure;
            } catch (...) {
                CrashLog(L"AdvancedEngineInstaller::thread", "(non-std exception)");
                state->result = InstallResult::StorageFailure;
            }
            state->done.store(true, std::memory_order_release);
        });

        const bool completed =
            UpdateChecker::ShowProgressDialog(parent, S(StringId::SPELL_ADVANCED_DOWNLOADING), state->done);
        if (!completed) {
            state->cancel.store(true, std::memory_order_release);
        }
        worker.join();
        // Result first: cancel is only polled at read-loop boundaries, so a
        // download that finished as the user dismissed the dialog is installed
        // and must not be thrown away.
        if (state->result == InstallResult::Installed) {
            return ReadyIfLoadable(parent);
        }
        if (!completed || state->result == InstallResult::Cancelled) {
            return AdvancedEngineStatus::Declined;
        }
        return ShowInstallFailure(parent, state->result) == InstallChoice::Manual
            ? AdvancedEngineStatus::ManualRequested
            : AdvancedEngineStatus::Unavailable;
    } catch (const std::exception& error) {
        CrashLog(L"AdvancedEngineInstaller::EnsureInstalledWithUi", error.what());
    } catch (...) {
        CrashLog(L"AdvancedEngineInstaller::EnsureInstalledWithUi", "(non-std exception)");
    }
    return ShowInstallFailure(parent, InstallResult::StorageFailure) == InstallChoice::Manual
        ? AdvancedEngineStatus::ManualRequested
        : AdvancedEngineStatus::Unavailable;
}

void AdvancedEngineInstaller::ShowManualInstallWithUi(HWND parent) {
    try {
        OpenManualInstallFlow(parent);
    } catch (const std::exception& error) {
        CrashLog(L"AdvancedEngineInstaller::ShowManualInstallWithUi", error.what());
        ::MessageBoxW(parent, S(StringId::SPELL_ADVANCED_INSTALL_FAILED), L"VKey", MB_OK | MB_ICONWARNING);
    } catch (...) {
        CrashLog(L"AdvancedEngineInstaller::ShowManualInstallWithUi", "(non-std exception)");
        ::MessageBoxW(parent, S(StringId::SPELL_ADVANCED_INSTALL_FAILED), L"VKey", MB_OK | MB_ICONWARNING);
    }
}

} // namespace NextKey
