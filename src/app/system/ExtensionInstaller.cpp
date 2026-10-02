// VKey - Extension Installer & Downloader
// SPDX-License-Identifier: GPL-3.0-only

#include "system/ExtensionInstaller.h"
#include "core/Version.h"
#include "core/Debug.h"
#include "core/security/ExtensionTrust.h"

#ifdef _WIN32
#include "system/StartupHelper.h"
#include <urlmon.h>
#include <shellapi.h>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace NextKey {

std::wstring ExtensionInstaller::GetExtensionExeName(ExtensionType type) noexcept {
    switch (type) {
        case ExtensionType::Watchdog:
            return L"VKeyWatchdog.exe";
        case ExtensionType::BrowserHost:
            return L"VKeyBrowserHost.exe";
        default:
            return L"";
    }
}

std::wstring ExtensionInstaller::GetExtensionPath(ExtensionType type) noexcept {
#ifdef _WIN32
    const std::wstring dir = GetInstallDirectory();
    if (dir.empty()) return GetExtensionExeName(type);
    return dir + L"\\" + GetExtensionExeName(type);
#else
    return GetExtensionExeName(type);
#endif
}

std::wstring ExtensionInstaller::GetExtensionDownloadUrl(ExtensionType type) {
    return L"https://github.com/phatMT97/VKey/releases/download/"
           L"v" VKEY_VERSION_WSTR L"/" + GetExtensionExeName(type);
}

std::wstring ExtensionInstaller::GetReleasePageUrl() {
    return L"https://github.com/phatMT97/VKey/releases/tag/v" VKEY_VERSION_WSTR;
}

bool ExtensionInstaller::IsInstalledAndTrusted(ExtensionType type) noexcept {
    const std::wstring path = GetExtensionPath(type);
    return Security::IsExtensionTrusted(path);
}

#ifdef _WIN32
bool ExtensionInstaller::EnsureInstalledWithUi(ExtensionType type, HWND parentHwnd) noexcept {
    const std::wstring exeName = GetExtensionExeName(type);
    const std::wstring path = GetExtensionPath(type);

    // 1. If already present and trusted, nothing more to do
    if (Security::IsExtensionTrusted(path)) {
        return true;
    }

    // 2. Check if file is present on disk but failed verification (tampered/fake)
    const bool fileExists = (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (fileExists) {
        std::wstring reason;
        Security::VerifyExtensionBinary(path, &reason);

        const std::wstring warnMsg = L"Tệp " + exeName +
            L" hiện có trong thư mục ứng dụng nhưng không vượt qua kiểm tra an toàn:\n" +
            reason + L"\n\nBạn có muốn tải lại phiên bản chính thức từ GitHub Release không?";

        int choice = MessageBoxW(parentHwnd, warnMsg.c_str(), L"Cảnh báo an toàn VKey",
                                 MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1);
        if (choice != IDYES) {
            return false;
        }
    } else {
        // File does not exist: prompt to download
        std::wstring featureName = (type == ExtensionType::Watchdog)
            ? L"Tự khởi động lại khi crash (Watchdog)"
            : L"Hỗ trợ trình duyệt (Browser Host)";

        std::wstring promptMsg = L"Tiện ích mở rộng \"" + featureName +
            L"\" chưa được cài đặt.\n\n"
            L"Bạn có muốn tải về tệp " + exeName + L" chính thức từ GitHub Release không?";

        int choice = MessageBoxW(parentHwnd, promptMsg.c_str(), L"Tiện ích mở rộng VKey",
                                 MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
        if (choice != IDYES) {
            return false;
        }
    }

    // 3. Download to staging file
    const std::wstring staging = path + L".download.tmp";
    const std::wstring url = GetExtensionDownloadUrl(type);

    NEXTKEY_LOG(L"ExtensionInstaller: downloading %ls from %ls to %ls",
                exeName.c_str(), url.c_str(), staging.c_str());

    HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), staging.c_str(), 0, nullptr);
    if (FAILED(hr)) {
        DeleteFileW(staging.c_str());
        NEXTKEY_LOG(L"ExtensionInstaller: download failed for %ls (hr=0x%lx)", exeName.c_str(), hr);

        int choice = MessageBoxW(parentHwnd,
            L"Không thể tải về tệp tiện ích mở rộng từ GitHub (lỗi kết nối hoặc phiên bản chưa phát hành).\n\n"
            L"Bạn có muốn mở trang GitHub Releases trên trình duyệt để kiểm tra không?",
            L"Tải tiện ích thất bại", MB_YESNO | MB_ICONERROR | MB_DEFBUTTON1);
        if (choice == IDYES) {
            ShellExecuteW(nullptr, L"open", GetReleasePageUrl().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return false;
    }

    // 4. Verify integrity and signature of downloaded staging file
    std::wstring reason;
    auto trust = Security::VerifyExtensionBinary(staging, &reason);
    if (trust != Security::ExtensionTrustResult::Trusted &&
        trust != Security::ExtensionTrustResult::DevBypass) {
        DeleteFileW(staging.c_str());
        NEXTKEY_LOG(L"ExtensionInstaller: staging verification failed for %ls: %ls",
                    exeName.c_str(), reason.c_str());

        MessageBoxW(parentHwnd,
            (L"Tệp tải về không vượt qua kiểm tra an toàn:\n" + reason +
             L"\n\nĐã hủy cài đặt để bảo vệ máy tính của bạn.").c_str(),
            L"Xác thực thất bại", MB_OK | MB_ICONERROR);
        return false;
    }

    // 5. Activate staging file into destination
    if (!MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        DeleteFileW(staging.c_str());
        NEXTKEY_LOG(L"ExtensionInstaller: MoveFileExW failed for %ls (err=%lu)",
                    path.c_str(), GetLastError());
        MessageBoxW(parentHwnd, L"Không thể ghi tệp vào thư mục ứng dụng (vui lòng kiểm tra quyền ghi).",
                    L"Lỗi cài đặt", MB_OK | MB_ICONERROR);
        return false;
    }

    NEXTKEY_LOG(L"ExtensionInstaller: %ls installed and verified successfully", exeName.c_str());
    return true;
}
#endif // _WIN32

}  // namespace NextKey
