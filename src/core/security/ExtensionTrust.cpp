// VKey - Extension Trust & Binary Integrity Verification
// SPDX-License-Identifier: GPL-3.0-only

#include "core/security/ExtensionTrust.h"
#include "core/Debug.h"
#include "core/Version.h"
#include "system/UpdateSecurity.h"

#include <vector>
#include <filesystem>
#include <string_view>

#ifdef _WIN32
#include "ExtensionHashes.generated.h"
#include <Windows.h>
#include <winver.h>
#include <wintrust.h>
#include <Softpub.h>
#include <wincrypt.h>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace {

constexpr DWORD kMaxExtensionBytes = 32 * 1024 * 1024; // 32 MiB ceiling

#ifdef VKEY_LITE_MODE
bool HasCurrentVersion(const std::wstring& filePath) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(filePath.c_str(), &ignored);
    if (size == 0 || size > 1024 * 1024) return false;
    std::vector<BYTE> versionData(size);
    if (!GetFileVersionInfoW(filePath.c_str(), 0, size, versionData.data())) return false;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(versionData.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) ||
        !info || length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE) {
        return false;
    }
    return HIWORD(info->dwFileVersionMS) == VKEY_VERSION_MAJOR &&
           LOWORD(info->dwFileVersionMS) == VKEY_VERSION_MINOR &&
           HIWORD(info->dwFileVersionLS) == VKEY_VERSION_PATCH &&
           LOWORD(info->dwFileVersionLS) == 0;
}

bool SignerSubjectMatches(const std::wstring& filePath) {
    HCERTSTORE hStore = nullptr;
    HCRYPTMSG hMsg = nullptr;
    bool matched = false;

    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, filePath.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr,
            &hStore, &hMsg, nullptr)) {
        return false;
    }

    DWORD cbSignerInfo = 0;
    if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &cbSignerInfo)
        && cbSignerInfo > 0) {
        std::vector<BYTE> buf(cbSignerInfo);
        if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, buf.data(), &cbSignerInfo)) {
            auto* signer = reinterpret_cast<CMSG_SIGNER_INFO*>(buf.data());
            CERT_INFO ci{};
            ci.Issuer = signer->Issuer;
            ci.SerialNumber = signer->SerialNumber;
            PCCERT_CONTEXT cert = CertFindCertificateInStore(hStore,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                CERT_FIND_SUBJECT_CERT, &ci, nullptr);
            if (cert) {
                wchar_t commonName[128]{};
                wchar_t organization[128]{};
                const DWORD cnLength = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                    0, nullptr, commonName, 128);
                const DWORD orgLength = CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE,
                    0, const_cast<char*>(szOID_ORGANIZATION_NAME), organization, 128);
                matched = cnLength > 1 && cnLength < 128 && orgLength > 1 && orgLength < 128
                    && std::wstring_view(commonName) == L"SignPath Foundation"
                    && std::wstring_view(organization) == L"SignPath Foundation";
                CertFreeCertificateContext(cert);
            }
        }
    }

    if (hMsg) CryptMsgClose(hMsg);
    if (hStore) CertCloseStore(hStore, 0);
    return matched;
}
#endif

} // namespace
#endif // _WIN32

namespace NextKey::Security {

ExtensionTrustResult VerifyExtensionBinary(
    const std::wstring& filePath,
    ExtensionBinary binary,
    std::wstring* reasonOut) noexcept {
    try {
#ifdef _WIN32
        // 1. Accessibility & Existence
        WIN32_FILE_ATTRIBUTE_DATA fileAttr{};
        if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &fileAttr)) {
            if (reasonOut) *reasonOut = L"Không tìm thấy tệp tiện ích mở rộng.";
            return ExtensionTrustResult::FileNotFound;
        }

        // 2. Size sanity check
        ULARGE_INTEGER fileSize{};
        fileSize.LowPart = fileAttr.nFileSizeLow;
        fileSize.HighPart = fileAttr.nFileSizeHigh;
        if (fileSize.QuadPart == 0 || fileSize.QuadPart > kMaxExtensionBytes) {
            if (reasonOut) *reasonOut = L"Kích thước tệp không hợp lệ.";
            return ExtensionTrustResult::SizeOutOfRange;
        }

        // 3. Each VKey build embeds hashes of its own companions. Classic also
        // embeds hashes of the standard-release assets it may download later.
        const std::string_view bundledHash = binary == ExtensionBinary::Watchdog
            ? BuildPins::kWatchdogSha256
            : binary == ExtensionBinary::BrowserHost
                ? BuildPins::kBrowserHostSha256 : BuildPins::kTsfSha256;
        const std::string_view releaseHash = binary == ExtensionBinary::Watchdog
            ? BuildPins::kReleaseWatchdogSha256
            : binary == ExtensionBinary::BrowserHost
                ? BuildPins::kReleaseBrowserHostSha256 : std::string_view{};
        const std::string actualHash = ComputeFileSha256(filePath);
        if ((bundledHash.size() == 64 && actualHash == bundledHash) ||
            (releaseHash.size() == 64 && actualHash == releaseHash)) {
            return ExtensionTrustResult::Trusted;
        }

        // 4. Foundation signs Classic's watchdog and TSF DLL after the app
        // build, changing their hashes. Their signed version resources must
        // match the running Classic version.
        LONG status = TRUST_E_NOSIGNATURE;
#ifdef VKEY_LITE_MODE
        if ((binary == ExtensionBinary::Watchdog || binary == ExtensionBinary::TsfDll)
            && HasCurrentVersion(filePath)) {
            WINTRUST_FILE_INFO fileInfo{};
            fileInfo.cbStruct = sizeof(fileInfo);
            fileInfo.pcwszFilePath = filePath.c_str();

            GUID actionGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
            WINTRUST_DATA wtd{};
            wtd.cbStruct = sizeof(wtd);
            wtd.dwUIChoice = WTD_UI_NONE;
            wtd.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
            wtd.dwUnionChoice = WTD_CHOICE_FILE;
            wtd.pFile = &fileInfo;
            wtd.dwStateAction = WTD_STATEACTION_VERIFY;
            wtd.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN | WTD_SAFER_FLAG;

            status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),
                                    &actionGuid, &wtd);
            wtd.dwStateAction = WTD_STATEACTION_CLOSE;
            WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &actionGuid, &wtd);

            if (status == ERROR_SUCCESS && SignerSubjectMatches(filePath)) {
                return ExtensionTrustResult::Trusted;
            }
        }
#endif

        // Development bypass escape hatch
#if !defined(NDEBUG) || defined(VKEY_ALLOW_UNSIGNED_EXTENSIONS)
        NEXTKEY_LOG(L"[DEV] Extension %ls bypasses trust check (allowed in dev/debug build)",
                    filePath.c_str());
        if (reasonOut) *reasonOut = L"Chế độ phát triển (Development): Bỏ qua kiểm tra tiện ích.";
        return ExtensionTrustResult::DevBypass;
#else
        NEXTKEY_LOG(L"ExtensionTrust: verification failed for %ls (status=0x%lx)",
                    filePath.c_str(), status);
        if (reasonOut) *reasonOut = L"Tệp tiện ích không khớp phiên bản VKey hoặc đã bị thay đổi.";
        return ExtensionTrustResult::SignatureInvalid;
#endif

#else
        // Non-Windows stub (e.g. Linux unit tests)
        std::error_code ec;
        if (!std::filesystem::exists(filePath, ec)) {
            if (reasonOut) *reasonOut = L"File not found";
            return ExtensionTrustResult::FileNotFound;
        }
        return ExtensionTrustResult::Trusted;
#endif
    } catch (...) {
        if (reasonOut) *reasonOut = L"Lỗi ngoại lệ trong quá trình xác thực.";
        return ExtensionTrustResult::CannotAccess;
    }
}

bool IsExtensionTrusted(const std::wstring& filePath, ExtensionBinary binary) noexcept {
    const auto result = VerifyExtensionBinary(filePath, binary, nullptr);
    return result == ExtensionTrustResult::Trusted || result == ExtensionTrustResult::DevBypass;
}

}  // namespace NextKey::Security
